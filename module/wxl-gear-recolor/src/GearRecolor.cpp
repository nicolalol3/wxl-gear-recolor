// wxl-gear-recolor: per-slot RGB colorize via addon ColorPicker.
// Copyright (C) 2026. GPLv3 (see WarcraftXL LICENSE).
//
// Lua API:
//   WXL_RecolorSetSlot(slot, r, g, b)   solid single rgb 0..1
//   WXL_RecolorSetSlotGradient(slot, nStops, fill, ...rgb)
//     nStops=2|3|5; fill=0 auto (1 rgb base) | 1 custom (nStops rgb)
//   WXL_RecolorSetSlotSelective(slot, sr,sg,sb, dr,dg,db, tol [, forceAppend])
//   WXL_RecolorGetSlot(slot) -> r,g,b,active,mode,sr,sg,sb,tol,ruleCount
//   WXL_RecolorGetSlotGradient(slot) -> active,nStops,fill, then nStops*rgb
//   WXL_RecolorBeginBatch / WXL_RecolorEndBatch([forceRebuild])
//   WXL_RecolorFlushTex()  — logout / leave-world TextureCache reset
//   WXL_RecolorOnUiReload() — /reload: drop stale tex handles, rebuild tinted sections
//   WXL_RecolorForceBodyRebuild / WXL_RecolorClearSlot / WXL_RecolorClearAll
//   WXL_RecolorCatchSlotColors(slot) -> n, then n*(r,g,b)  (selective Catch)
//   WXL_RecolorArmScreenSample / GetScreenSample / CancelScreenSample
//   WXL_RecolorSetRemote(guidStr, slot, mode, dataStr)  — observer sync from server
//   WXL_RecolorClearRemote(guidStr[, slot])
//   WXL_RecolorClearAllRemote()
// State: mode 0 solid single, 1 selective, 2 solid gradient
//   (legacy "slot r g b" still loads as solid single)
// Runtime draw key: local slots OR (ownerGuid, slot) for remotes — never path-only.

#include "core/Logger.hpp"
#include "core/Hook.hpp"
#include "events/EventScript.hpp"
#include "game/Binding.hpp"
#include "game/gx/Gx.hpp"
#include "game/m2/M2.hpp"
#include "game/unit/Unit.hpp"
#include "game/world/World.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/game/DB2.hpp"
#include "offsets/game/M2.hpp"
#include "runtime/LuaBindings.hpp"
#include "runtime/ModuleInstall.hpp"

#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    namespace ev = wxl::events;
    namespace m2 = wxl::game::m2;
    namespace gx = wxl::game::gx;
    namespace wlua = wxl::runtime::lua;

    constexpr size_t kOffInstModel = 0x2C;
    constexpr int kMaxEquipSlots = 19;

    // CCharacterComponent::RenderPrep @ 0x4F1520 (thiscall). Proven via NPC-flag test at +0x3C.
    constexpr uintptr_t kCharRenderPrep = 0x004F1520;
    // Retail ComponentData: model @ +0x38, flags @ +0x3C (bit0 = NPC).
    constexpr size_t kOffComponentModel = 0x38;
    constexpr size_t kOffComponentFlags = 0x3C;
    constexpr uint32_t kComponentFlagNpc = 0x1;

    float Clamp01(float v)
    {
        if (v < 0.f)
            return 0.f;
        if (v > 1.f)
            return 1.f;
        return v;
    }

    // #region agent log
    constexpr char kDbgLog615[] = "C:\\Azerothcore\\debug-615e3b.log";
    std::atomic<uint32_t> g_dbgPasteRemoteN{ 0 };
    std::atomic<uint32_t> g_dbgTryHslN{ 0 };
    std::atomic<uint32_t> g_dbgApplyRemoteN{ 0 };
    std::atomic<uint32_t> g_dbgNoteEquipN{ 0 };
    std::atomic<uint32_t> g_dbgForceRebuildN{ 0 };

    void DbgTexBasename615(const char* path, char* out, size_t n)
    {
        if (!out || n == 0)
            return;
        out[0] = '\0';
        if (!path)
            return;
        const char* base = path;
        for (const char* p = path; *p; ++p)
        {
            if (*p == '\\' || *p == '/')
                base = p + 1;
        }
        size_t len = std::strlen(base);
        if (len >= n)
            len = n - 1;
        std::memcpy(out, base, len);
        out[len] = '\0';
    }

    void DbgLog615(const char* hypothesisId, const char* location, const char* message,
        unsigned long long owner, int slot, int section, const char* texStem,
        int extraA, int extraB, uintptr_t extraPtr = 0)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, kDbgLog615, "a") != 0 || !f)
            return;
        const unsigned long long ts = static_cast<unsigned long long>(GetTickCount64());
        fprintf(f,
            "{\"sessionId\":\"615e3b\",\"runId\":\"no-remote-force\",\"hypothesisId\":\"%s\","
            "\"location\":\"%s\",\"message\":\"%s\",\"timestamp\":%llu,"
            "\"data\":{\"ownerLow\":%u,\"slot\":%d,\"section\":%d,"
            "\"a\":%d,\"b\":%d,\"ptr\":%llu,\"tex\":\"%s\"}}\n",
            hypothesisId, location, message, ts,
            static_cast<unsigned>(owner & 0xFFFFFFFFull), slot, section,
            extraA, extraB, static_cast<unsigned long long>(extraPtr),
            texStem ? texStem : "");
        fclose(f);
    }
    // #endregion

    // Fields named hue/sat/light for legacy call sites; values are RGB 0..1.
    // mode 0 = solid single (lum * dest)
    // mode 1 = selective (chained src→dst rules)
    // mode 2 = solid gradient (luminance samples 2/3/5 stop colors)
    constexpr int kMaxSelRules = 4;
    constexpr int kMaxGradStops = 5;
    struct SelRule
    {
        float sr = 0.8f, sg = 0.2f, sb = 0.2f;
        float dr = 0.2f, dg = 0.4f, db = 0.9f;
        float tol = 0.35f;
    };
    struct SlotHsl
    {
        bool active = false;
        uint8_t mode = 0; // 0 solid single, 1 selective, 2 solid gradient
        float hue = 1.f;   // solid dest / gradient base / last selective dest R
        float sat = 1.f;
        float light = 1.f;
        float srcR = 0.8f; // mirrors last selective src (GetSlot / UI)
        float srcG = 0.2f;
        float srcB = 0.2f;
        float tolerance = 0.35f;
        SelRule rules[kMaxSelRules] = {};
        uint8_t ruleCount = 0;
        uint8_t stopCount = 1; // gradient: 2, 3, or 5
        uint8_t gradFill = 0;  // 0 auto shades from hue/sat/light, 1 custom stops
        float stops[kMaxGradStops][3] = {};
    };

    void FillAutoStops(float br, float bg, float bb, int n, float outStops[kMaxGradStops][3])
    {
        if (n < 2)
            n = 2;
        if (n > kMaxGradStops)
            n = kMaxGradStops;
        const float darkR = Clamp01(br * 0.16f);
        const float darkG = Clamp01(bg * 0.16f);
        const float darkB = Clamp01(bb * 0.16f);
        const float litR = Clamp01(br + (1.f - br) * 0.78f);
        const float litG = Clamp01(bg + (1.f - bg) * 0.78f);
        const float litB = Clamp01(bb + (1.f - bb) * 0.78f);
        for (int i = 0; i < n; ++i)
        {
            const float t = (n <= 1) ? 0.5f
                : static_cast<float>(i) / static_cast<float>(n - 1);
            float r, g, b;
            if (t <= 0.5f)
            {
                const float u = t * 2.f;
                r = darkR + (br - darkR) * u;
                g = darkG + (bg - darkG) * u;
                b = darkB + (bb - darkB) * u;
            }
            else
            {
                const float u = (t - 0.5f) * 2.f;
                r = br + (litR - br) * u;
                g = bg + (litG - bg) * u;
                b = bb + (litB - bb) * u;
            }
            outStops[i][0] = Clamp01(r);
            outStops[i][1] = Clamp01(g);
            outStops[i][2] = Clamp01(b);
        }
    }

    void ResolveSolidStops(SlotHsl& h)
    {
        if (h.mode != 2)
        {
            h.stopCount = 1;
            h.stops[0][0] = h.hue;
            h.stops[0][1] = h.sat;
            h.stops[0][2] = h.light;
            return;
        }
        int n = h.stopCount;
        if (n != 2 && n != 3 && n != 5)
            n = 3;
        h.stopCount = static_cast<uint8_t>(n);
        if (h.gradFill == 0)
        {
            // hue/sat/light stay as the user base color.
            FillAutoStops(h.hue, h.sat, h.light, n, h.stops);
        }
        else
        {
            const int mid = n / 2;
            h.hue = h.stops[mid][0];
            h.sat = h.stops[mid][1];
            h.light = h.stops[mid][2];
        }
    }

    void SampleGradientStops(float lum, int n, const float stops[kMaxGradStops][3],
        float& r, float& g, float& b)
    {
        if (n <= 1)
        {
            r = Clamp01(stops[0][0]);
            g = Clamp01(stops[0][1]);
            b = Clamp01(stops[0][2]);
            return;
        }
        lum = Clamp01(lum);
        const float x = lum * static_cast<float>(n - 1);
        int i0 = static_cast<int>(x);
        if (i0 >= n - 1)
        {
            r = Clamp01(stops[n - 1][0]);
            g = Clamp01(stops[n - 1][1]);
            b = Clamp01(stops[n - 1][2]);
            return;
        }
        const float f = x - static_cast<float>(i0);
        r = Clamp01(stops[i0][0] + (stops[i0 + 1][0] - stops[i0][0]) * f);
        g = Clamp01(stops[i0][1] + (stops[i0 + 1][1] - stops[i0][1]) * f);
        b = Clamp01(stops[i0][2] + (stops[i0 + 1][2] - stops[i0][2]) * f);
    }

    void ApplySolidPixel(float& r, float& g, float& b, const SlotHsl& c)
    {
        const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
        if (c.mode == 2 && c.stopCount >= 2)
        {
            SampleGradientStops(lum, c.stopCount, c.stops, r, g, b);
            return;
        }
        r = Clamp01(lum * c.hue);
        g = Clamp01(lum * c.sat);
        b = Clamp01(lum * c.light);
    }

    void SyncSlotMirrorsFromLastRule(SlotHsl& h)
    {
        if (h.ruleCount == 0)
            return;
        const SelRule& r = h.rules[h.ruleCount - 1];
        h.srcR = r.sr;
        h.srcG = r.sg;
        h.srcB = r.sb;
        h.hue = r.dr;
        h.sat = r.dg;
        h.light = r.db;
        h.tolerance = r.tol;
    }

    bool SrcNear(float a, float b)
    {
        return std::fabs(a - b) < 0.045f;
    }

    bool SameSelectiveSrc(const SelRule& r, float sr, float sg, float sb)
    {
        return SrcNear(r.sr, sr) && SrcNear(r.sg, sg) && SrcNear(r.sb, sb);
    }


    std::mutex g_colorMutex;
    SlotHsl g_slotHsl[kMaxEquipSlots];
    // /recolor UI preview — never synced to server or remote observers until Apply.
    SlotHsl g_draftHsl[kMaxEquipSlots];
    std::atomic<bool> g_previewUiActive{ false };

    // Remote players' equipped tints (owner = full client GUID). Local keeps g_slotHsl
    // as hot cache for ActiveOwnerGuid(); both written via ApplyOwnerTint.
    using OwnerGuid = unsigned long long;
    using OwnerSlotHsl = std::array<SlotHsl, kMaxEquipSlots>;
    std::mutex g_remoteMu;
    std::unordered_map<OwnerGuid, OwnerSlotHsl> g_remoteTints;
    // CharacterComponent.model for remote owners (OC attaches here, not unit+0xB4).
    std::unordered_map<OwnerGuid, void*> g_remoteCcModels;
    std::unordered_map<OwnerGuid, void*> g_remoteCcComponents;
    // Every CharComponent RenderPrep: model → component (local + remote).
    std::unordered_map<void*, void*> g_modelToComponent;
    // Remote tint arrived before unit visible — section mask to Force on next prep.
    std::unordered_map<OwnerGuid, uint32_t> g_remotePendingDirty;
    // First CharComponent assemble for a remote must run untinted (face/skin).
    // Tinting during that pass paints item HSL onto face-adjacent composite layers.
    std::unordered_map<OwnerGuid, uint8_t> g_remoteNativeAssembleDone;
    // CharComponent assemble owner for the current paste window.
    std::atomic<OwnerGuid> g_prepOwnerGuid{ 0 };

    bool GuidSamePlayer(OwnerGuid a, OwnerGuid b)
    {
        if (!a || !b)
            return false;
        if (a == b)
            return true;
        return (a & 0xFFFFFFFFull) == (b & 0xFFFFFFFFull);
    }

    // ActivePlayerGuid() often returns 0 on the render thread. Cache from Lua.
    std::atomic<OwnerGuid> g_cachedSelfGuid{ 0 };

    std::mutex g_equipSnapMu;
    // owner -> per-slot item entry (0 = empty or unknown). Filled by Lua PUSH.
    std::unordered_map<OwnerGuid, std::array<uint32_t, kMaxEquipSlots>> g_equipSnap;

    // ItemDisplayInfo-backed identity for TextureComponents paste tint.
    // One stem per texture[0..7] (ArmUpper..Foot) — never ModelTexture tex1/tex2.
    constexpr size_t kStemCap = 64;
    struct DisplaySlotInfo
    {
        uint32_t displayId = 0;
        char componentStems[8][kStemCap] = {};
    };
    using OwnerDisplaySlots = std::array<DisplaySlotInfo, kMaxEquipSlots>;
    std::mutex g_displayMu;
    std::unordered_map<OwnerGuid, OwnerDisplaySlots> g_displaySnap;

    constexpr char kStateFile[] = "WarcraftXL_gear-recolor.state";

    // Per-character disk cache (shared Wow.exe folder — two clients must not share
    // one slot table or login sync copies the same tints onto both characters).
    void StateFileForOwner(OwnerGuid owner, char* out, size_t outN)
    {
        if (owner)
            sprintf_s(out, outN, "WarcraftXL_gear-recolor_%I64u.state",
                static_cast<unsigned long long>(owner));
        else
            sprintf_s(out, outN, "%s", kStateFile);
    }

    OwnerGuid ActiveOwnerGuid()
    {
        const OwnerGuid live = static_cast<OwnerGuid>(wxl::game::world::ActivePlayerGuid());
        if (live)
        {
            g_cachedSelfGuid.store(live, std::memory_order_relaxed);
            return live;
        }
        return g_cachedSelfGuid.load(std::memory_order_relaxed);
    }

    bool ParseGuidString(const char* s, OwnerGuid& out)
    {
        if (!s || !s[0])
            return false;
        while (*s == ' ' || *s == '\t')
            ++s;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
        char* end = nullptr;
        const unsigned long long v = _strtoui64(s, &end, 16);
        if (end == s)
            return false;
        out = static_cast<OwnerGuid>(v);
        return out != 0;
    }

    bool OwnerHasAnyTintLocked(const OwnerSlotHsl& slots)
    {
        for (int i = 0; i < kMaxEquipSlots; ++i)
        {
            if (slots[static_cast<size_t>(i)].active)
                return true;
        }
        return false;
    }

    bool OwnerHasAnyRemoteTint(OwnerGuid owner)
    {
        std::lock_guard<std::mutex> lock(g_remoteMu);
        auto it = g_remoteTints.find(owner);
        if (it != g_remoteTints.end() && OwnerHasAnyTintLocked(it->second))
            return true;
        for (const auto& kv : g_remoteTints)
        {
            if (GuidSamePlayer(kv.first, owner) && OwnerHasAnyTintLocked(kv.second))
                return true;
        }
        return false;
    }

    bool FormatTintData(const SlotHsl& h, char* out, size_t cap)
    {
        if (!out || cap < 8 || !h.active)
            return false;
        if (h.mode == 0)
        {
            return _snprintf_s(out, cap, _TRUNCATE, "%.6f %.6f %.6f",
                h.hue, h.sat, h.light) > 0;
        }
        if (h.mode == 2)
        {
            int n = _snprintf_s(out, cap, _TRUNCATE, "%u %u",
                static_cast<unsigned>(h.stopCount),
                static_cast<unsigned>(h.gradFill));
            if (n <= 0)
                return false;
            size_t used = static_cast<size_t>(n);
            if (h.gradFill == 0)
            {
                n = _snprintf_s(out + used, cap - used, _TRUNCATE,
                    " %.6f %.6f %.6f", h.hue, h.sat, h.light);
                return n > 0;
            }
            for (uint8_t s = 0; s < h.stopCount && s < kMaxGradStops; ++s)
            {
                n = _snprintf_s(out + used, cap - used, _TRUNCATE,
                    " %.6f %.6f %.6f",
                    h.stops[s][0], h.stops[s][1], h.stops[s][2]);
                if (n <= 0)
                    return false;
                used += static_cast<size_t>(n);
            }
            return true;
        }
        // Selective
        uint8_t nRules = h.ruleCount ? h.ruleCount : 1;
        int n = _snprintf_s(out, cap, _TRUNCATE, "%u", static_cast<unsigned>(nRules));
        if (n <= 0)
            return false;
        size_t used = static_cast<size_t>(n);
        if (h.ruleCount == 0)
        {
            n = _snprintf_s(out + used, cap - used, _TRUNCATE,
                " %.6f %.6f %.6f %.6f %.6f %.6f %.6f",
                h.srcR, h.srcG, h.srcB, h.hue, h.sat, h.light, h.tolerance);
            return n > 0;
        }
        for (uint8_t r = 0; r < nRules; ++r)
        {
            const SelRule& rule = h.rules[r];
            n = _snprintf_s(out + used, cap - used, _TRUNCATE,
                " %.6f %.6f %.6f %.6f %.6f %.6f %.6f",
                rule.sr, rule.sg, rule.sb, rule.dr, rule.dg, rule.db, rule.tol);
            if (n <= 0)
                return false;
            used += static_cast<size_t>(n);
        }
        return true;
    }

    // Resolve to the map key used by Lua PUSH / g_remoteTints (high bits may differ).
    OwnerGuid CanonicalTintOwner(OwnerGuid owner)
    {
        if (!owner)
            return 0;
        {
            std::lock_guard<std::mutex> lock(g_equipSnapMu);
            if (g_equipSnap.find(owner) != g_equipSnap.end())
                return owner;
            for (const auto& kv : g_equipSnap)
            {
                if (GuidSamePlayer(kv.first, owner))
                    return kv.first;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_remoteMu);
            if (g_remoteTints.find(owner) != g_remoteTints.end())
                return owner;
            for (const auto& kv : g_remoteTints)
            {
                if (GuidSamePlayer(kv.first, owner))
                    return kv.first;
            }
        }
        return owner;
    }

    uint32_t EquipSnapEntry(OwnerGuid owner, int slot)
    {
        if (!owner || slot < 0 || slot >= kMaxEquipSlots)
            return 0;
        owner = CanonicalTintOwner(owner);
        std::lock_guard<std::mutex> lock(g_equipSnapMu);
        auto it = g_equipSnap.find(owner);
        if (it == g_equipSnap.end())
            return 0;
        return it->second[static_cast<size_t>(slot)];
    }

    bool EquipSnapKnown(OwnerGuid owner)
    {
        if (!owner)
            return false;
        std::lock_guard<std::mutex> lock(g_equipSnapMu);
        if (g_equipSnap.find(owner) != g_equipSnap.end())
            return true;
        for (const auto& kv : g_equipSnap)
        {
            if (GuidSamePlayer(kv.first, owner))
                return true;
        }
        return false;
    }

    bool ParseTintData(uint8_t mode, const char* data, SlotHsl& out)
    {
        out = {};
        if (!data)
            return false;
        out.active = true;
        out.mode = mode;
        if (mode == 0)
        {
            float r = 0.f, g = 0.f, b = 0.f;
            if (sscanf_s(data, "%f %f %f", &r, &g, &b) != 3)
                return false;
            out.hue = Clamp01(r);
            out.sat = Clamp01(g);
            out.light = Clamp01(b);
            return true;
        }
        if (mode == 2)
        {
            unsigned nStops = 0, fill = 0;
            const char* p = data;
            if (sscanf_s(p, "%u %u", &nStops, &fill) != 2)
                return false;
            while (*p && *p != ' ')
                ++p;
            while (*p == ' ')
                ++p;
            while (*p && *p != ' ')
                ++p;
            while (*p == ' ')
                ++p;
            if (nStops != 2 && nStops != 3 && nStops != 5)
                nStops = 3;
            out.stopCount = static_cast<uint8_t>(nStops);
            out.gradFill = fill ? 1 : 0;
            if (out.gradFill == 0)
            {
                float r = 0.f, g = 0.f, b = 0.f;
                if (sscanf_s(p, "%f %f %f", &r, &g, &b) != 3)
                    return false;
                out.hue = Clamp01(r);
                out.sat = Clamp01(g);
                out.light = Clamp01(b);
                FillAutoStops(out.hue, out.sat, out.light, out.stopCount, out.stops);
                return true;
            }
            for (uint8_t s = 0; s < out.stopCount; ++s)
            {
                float r = 0.f, g = 0.f, b = 0.f;
                int consumed = 0;
                if (sscanf_s(p, "%f %f %f%n", &r, &g, &b, &consumed) < 3)
                    return false;
                out.stops[s][0] = Clamp01(r);
                out.stops[s][1] = Clamp01(g);
                out.stops[s][2] = Clamp01(b);
                p += consumed;
            }
            out.hue = out.stops[0][0];
            out.sat = out.stops[0][1];
            out.light = out.stops[0][2];
            return true;
        }
        // Selective
        unsigned nRules = 0;
        const char* p = data;
        int consumed = 0;
        if (sscanf_s(p, "%u%n", &nRules, &consumed) != 1 || nRules == 0 || nRules > kMaxSelRules)
            return false;
        p += consumed;
        out.ruleCount = static_cast<uint8_t>(nRules);
        for (uint8_t i = 0; i < out.ruleCount; ++i)
        {
            float sr, sg, sb, dr, dg, db, tol;
            consumed = 0;
            if (sscanf_s(p, "%f %f %f %f %f %f %f%n",
                &sr, &sg, &sb, &dr, &dg, &db, &tol, &consumed) < 7)
                return false;
            out.rules[i].sr = Clamp01(sr);
            out.rules[i].sg = Clamp01(sg);
            out.rules[i].sb = Clamp01(sb);
            out.rules[i].dr = Clamp01(dr);
            out.rules[i].dg = Clamp01(dg);
            out.rules[i].db = Clamp01(db);
            out.rules[i].tol = Clamp01(tol);
            p += consumed;
        }
        SyncSlotMirrorsFromLastRule(out);
        return true;
    }

    void SetRemoteSlotTint(OwnerGuid owner, int slot, const SlotHsl& h)
    {
        if (!owner || slot < 0 || slot >= kMaxEquipSlots)
            return;
        {
            std::lock_guard<std::mutex> lock(g_remoteMu);
            g_remoteTints[owner][static_cast<size_t>(slot)] = h;
        }
    }

    void ClearRemoteSlotTint(OwnerGuid owner, int slot)
    {
        if (!owner)
            return;
        {
            std::lock_guard<std::mutex> lock(g_remoteMu);
            auto it = g_remoteTints.find(owner);
            if (it != g_remoteTints.end())
            {
                if (slot < 0)
                {
                    g_remoteTints.erase(it);
                    g_remoteCcModels.erase(owner);
                    g_remoteCcComponents.erase(owner);
                    for (auto jt = g_remoteNativeAssembleDone.begin();
                        jt != g_remoteNativeAssembleDone.end(); )
                    {
                        if (GuidSamePlayer(jt->first, owner))
                            jt = g_remoteNativeAssembleDone.erase(jt);
                        else
                            ++jt;
                    }
                }
                else if (slot < kMaxEquipSlots)
                {
                    it->second[static_cast<size_t>(slot)] = {};
                    if (!OwnerHasAnyTintLocked(it->second))
                    {
                        g_remoteTints.erase(it);
                        g_remoteCcModels.erase(owner);
                        g_remoteCcComponents.erase(owner);
                        for (auto jt = g_remoteNativeAssembleDone.begin();
                            jt != g_remoteNativeAssembleDone.end(); )
                        {
                            if (GuidSamePlayer(jt->first, owner))
                                jt = g_remoteNativeAssembleDone.erase(jt);
                            else
                                ++jt;
                        }
                    }
                }
            }
        }
        // Keep equip snap aligned: empty/clear must not leave a stale entry that
        // lets paste re-tint feet after unequip (observer phantom boots).
        {
            std::lock_guard<std::mutex> lock(g_equipSnapMu);
            auto it = g_equipSnap.find(owner);
            if (it == g_equipSnap.end())
                return;
            if (slot < 0)
            {
                g_equipSnap.erase(it);
                return;
            }
            if (slot < kMaxEquipSlots)
                it->second[static_cast<size_t>(slot)] = 0;
        }
    }

    void ClearAllRemoteTints()
    {
        std::lock_guard<std::mutex> lock(g_remoteMu);
        g_remoteTints.clear();
        g_remoteCcModels.clear();
        g_remoteCcComponents.clear();
        g_remotePendingDirty.clear();
        g_remoteNativeAssembleDone.clear();
    }

    bool IsIdentityHsl(const SlotHsl& h)
    {
        return !h.active;
    }

    bool ModuleDisabled()
    {
        return GetFileAttributesA("WarcraftXL_gear-recolor.disable")
            != INVALID_FILE_ATTRIBUTES;
    }

    // Colorize: keep luminance (shading), apply picked RGB as chroma.
    void RgbToHsv(float r, float g, float b, float& h, float& s, float& v)
    {
        const float mx = (std::max)(r, (std::max)(g, b));
        const float mn = (std::min)(r, (std::min)(g, b));
        const float d = mx - mn;
        v = mx;
        s = (mx > 1e-6f) ? (d / mx) : 0.f;
        if (d < 1e-6f)
        {
            h = 0.f;
            return;
        }
        if (mx == r)
            h = (g - b) / d + (g < b ? 6.f : 0.f);
        else if (mx == g)
            h = (b - r) / d + 2.f;
        else
            h = (r - g) / d + 4.f;
        h *= (1.f / 6.f);
    }

    void HsvToRgb(float h, float s, float v, float& r, float& g, float& b)
    {
        if (s <= 1e-6f)
        {
            r = g = b = v;
            return;
        }
        h = h - std::floor(h);
        const float i = std::floor(h * 6.f);
        const float f = h * 6.f - i;
        const float p = v * (1.f - s);
        const float q = v * (1.f - f * s);
        const float t = v * (1.f - (1.f - f) * s);
        switch (static_cast<int>(i) % 6)
        {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
        }
    }

    float HueDistance(float a, float b)
    {
        float d = std::fabs(a - b);
        if (d > 0.5f)
            d = 1.f - d;
        return d;
    }

    float Smoothstep01(float edge0, float edge1, float x)
    {
        if (edge1 <= edge0)
            return x >= edge1 ? 1.f : 0.f;
        const float t = Clamp01((x - edge0) / (edge1 - edge0));
        return t * t * (3.f - 2.f * t);
    }

    // Lighting-invariant selective weight: compare normalized RGB (÷ max channel).
    // Speckles from shading drop a lot vs raw RGB; AA isolates need DespeckleWeights.
    float SelectiveWeight(float r, float g, float b, const SelRule& rule)
    {
        const float mx = (std::max)(r, (std::max)(g, b));
        const float mn = (std::min)(r, (std::min)(g, b));
        if (mx < 0.07f)
            return 0.f;
        const float sat = (mx - mn) / mx;
        if (sat < 0.10f)
            return 0.f;

        const float smx = (std::max)(rule.sr, (std::max)(rule.sg, rule.sb));
        if (smx < 0.07f)
            return 0.f;

        const float nr = r / mx, ng = g / mx, nb = b / mx;
        const float nsr = rule.sr / smx, nsg = rule.sg / smx, nsb = rule.sb / smx;
        const float dr = nr - nsr, dg = ng - nsg, db = nb - nsb;
        const float dist = std::sqrt(dr * dr + dg * dg + db * db);
        const float tol = (std::max)(0.02f, (std::min)(0.5f, rule.tol));
        // Normalized space: distances are typically ~0.05..0.6
        const float maxd = 0.045f + tol * 0.90f;
        if (dist >= maxd)
            return 0.f;

        float w = 1.f - Smoothstep01(maxd * 0.30f, maxd, dist);
        w *= Smoothstep01(0.10f, 0.22f, sat);
        const float glow = Smoothstep01(0.84f, 0.97f, mx) * Smoothstep01(0.35f, 0.70f, sat);
        w *= 1.f - glow * 0.92f;
        return Clamp01(w);
    }

    void ApplySelectiveWithWeight(float& r, float& g, float& b, const SelRule& rule, float w)
    {
        w = Clamp01(w);
        if (w <= 1e-4f)
            return;
        const float lum0 = 0.299f * r + 0.587f * g + 0.114f * b;
        const float dl = 0.299f * rule.dr + 0.587f * rule.dg + 0.114f * rule.db;
        float nr, ng, nb;
        if (dl > 1e-6f)
        {
            nr = Clamp01(rule.dr * (lum0 / dl));
            ng = Clamp01(rule.dg * (lum0 / dl));
            nb = Clamp01(rule.db * (lum0 / dl));
        }
        else
        {
            nr = Clamp01(rule.dr * lum0);
            ng = Clamp01(rule.dg * lum0);
            nb = Clamp01(rule.db * lum0);
        }
        r = Clamp01(r + (nr - r) * w);
        g = Clamp01(g + (ng - g) * w);
        b = Clamp01(b + (nb - b) * w);
    }

    // Kill isolated match pixels; lightly fill holes inside solid regions.
    void DespeckleWeights(float* w, uint32_t width, uint32_t height)
    {
        if (!w || width < 3 || height < 3)
            return;
        std::vector<float> tmp(static_cast<size_t>(width) * static_cast<size_t>(height));
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t i = static_cast<size_t>(y) * width + x;
                float sum = 0.f;
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (dx == 0 && dy == 0)
                            continue;
                        const int nx = static_cast<int>(x) + dx;
                        const int ny = static_cast<int>(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<int>(width)
                            || ny >= static_cast<int>(height))
                            continue;
                        sum += w[static_cast<size_t>(ny) * width + static_cast<size_t>(nx)];
                        ++n;
                    }
                }
                const float avg = (n > 0) ? (sum / static_cast<float>(n)) : 0.f;
                float v = w[i];
                if (v > 0.35f && avg < 0.16f)
                    v = 0.f; // isolated speck
                else if (v < 0.20f && avg > 0.55f)
                    v = avg * 0.70f; // fill small hole
                else
                    v *= (0.25f + 0.75f * Smoothstep01(0.12f, 0.42f, avg));
                tmp[i] = Clamp01(v);
            }
        }
        std::memcpy(w, tmp.data(), tmp.size() * sizeof(float));
    }

    // Selective: normalized-RGB soft mask (+ optional caller-side despeckle).
    void ApplyOneSelectiveRule(float& r, float& g, float& b, const SelRule& rule)
    {
        ApplySelectiveWithWeight(r, g, b, rule, SelectiveWeight(r, g, b, rule));
    }

    void ApplyHslPixel(float& r, float& g, float& b, const SlotHsl& c)
    {
        if (!c.active)
            return;
        if (c.mode == 0 || c.mode == 2)
        {
            ApplySolidPixel(r, g, b, c);
            return;
        }
        // Selective: apply chained rules in order (each sees prior output), so a
        // second Apply that targets the recolored color still works.
        if (c.ruleCount == 0)
        {
            // Legacy single-rule fields
            SelRule one{};
            one.sr = c.srcR;
            one.sg = c.srcG;
            one.sb = c.srcB;
            one.dr = c.hue;
            one.dg = c.sat;
            one.db = c.light;
            one.tol = c.tolerance;
            ApplyOneSelectiveRule(r, g, b, one);
            return;
        }
        for (uint8_t i = 0; i < c.ruleCount; ++i)
            ApplyOneSelectiveRule(r, g, b, c.rules[i]);
    }

    void SaveHslToDisk()
    {
        char path[128];
        StateFileForOwner(ActiveOwnerGuid(), path, sizeof(path));
        FILE* f = nullptr;
        if (fopen_s(&f, path, "w") != 0 || !f)
            return;
        std::lock_guard<std::mutex> lock(g_colorMutex);
        for (int i = 0; i < kMaxEquipSlots; ++i)
        {
            const SlotHsl& h = g_slotHsl[i];
            if (!h.active)
                continue;
            if (h.mode == 0)
            {
                fprintf(f, "%d 0 %.6f %.6f %.6f\n", i, h.hue, h.sat, h.light);
            }
            else if (h.mode == 2)
            {
                fprintf(f, "%d 2 %u %u", i,
                    static_cast<unsigned>(h.stopCount),
                    static_cast<unsigned>(h.gradFill));
                if (h.gradFill == 0)
                {
                    // Auto: persist base color only (rebuild stops on load).
                    fprintf(f, " %.6f %.6f %.6f", h.hue, h.sat, h.light);
                }
                else
                {
                    for (uint8_t s = 0; s < h.stopCount && s < kMaxGradStops; ++s)
                        fprintf(f, " %.6f %.6f %.6f",
                            h.stops[s][0], h.stops[s][1], h.stops[s][2]);
                }
                fprintf(f, "\n");
            }
            else
            {
                // Selective: slot 1 <count> then count×(sr sg sb dr dg db tol)
                uint8_t nRules = h.ruleCount;
                if (nRules == 0)
                {
                    fprintf(f, "%d 1 1 %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
                        i, h.srcR, h.srcG, h.srcB, h.hue, h.sat, h.light, h.tolerance);
                }
                else
                {
                    fprintf(f, "%d 1 %u", i, static_cast<unsigned>(nRules));
                    for (uint8_t r = 0; r < nRules; ++r)
                    {
                        const SelRule& rule = h.rules[r];
                        fprintf(f, " %.6f %.6f %.6f %.6f %.6f %.6f %.6f",
                            rule.sr, rule.sg, rule.sb,
                            rule.dr, rule.dg, rule.db, rule.tol);
                    }
                    fprintf(f, "\n");
                }
            }
        }
        fclose(f);
    }


    bool ContainsCI(const char* hay, const char* needle)
    {
        if (!hay || !needle || !*needle)
            return false;
        const size_t n = std::strlen(needle);
        for (const char* p = hay; *p; ++p)
        {
            size_t i = 0;
            while (i < n)
            {
                const unsigned char a = static_cast<unsigned char>(p[i]);
                const unsigned char b = static_cast<unsigned char>(needle[i]);
                if (!a)
                    break;
                if ((a | 32) != (b | 32) && !(a == '/' && b == '\\') && !(a == '\\' && b == '/'))
                    break;
                ++i;
            }
            if (i == n)
                return true;
        }
        return false;
    }

    // --- ItemDisplayInfo stem cache (TextureComponents identity) ---------------

    namespace db2 = wxl::offsets::game::db2;
    using wxl::game::Native;

    void CopyTexStem(const char* src, char* dst, size_t dstN)
    {
        if (!dst || dstN == 0)
            return;
        dst[0] = '\0';
        if (!src || !src[0])
            return;
        if (reinterpret_cast<uintptr_t>(src) < 0x10000u)
            return;
        const char* base = src;
        for (const char* p = src; *p; ++p)
        {
            if (*p == '\\' || *p == '/')
                base = p + 1;
        }
        size_t len = std::strlen(base);
        for (size_t i = 0; i < len; ++i)
        {
            if (base[i] == '.')
            {
                len = i;
                break;
            }
        }
        if (len < 2)
            return;
        if (len >= dstN)
            len = dstN - 1;
        std::memcpy(dst, base, len);
        dst[len] = '\0';
    }

    bool DisplaySlotHasComponentTextures(const DisplaySlotInfo& info)
    {
        for (int i = 0; i < 8; ++i)
        {
            if (info.componentStems[i][0])
                return true;
        }
        return false;
    }

    void DisplayInfoSetComponentStem(DisplaySlotInfo& info, int compIdx, const char* texName)
    {
        if (compIdx < 0 || compIdx >= 8)
            return;
        CopyTexStem(texName, info.componentStems[compIdx], kStemCap);
    }

    bool FillDisplayInfoFromId(uint32_t displayId, DisplaySlotInfo& out)
    {
        out = {};
        if (!displayId)
            return false;
        alignas(4) uint8_t dispBuf[db2::itemdisplayinfo::kRecordSize] = {};
        uint32_t ok = 0;
        __try
        {
            ok = Native<db2::itemdisplayinfo::LookupFn>(db2::itemdisplayinfo::kLookup)(
                reinterpret_cast<void*>(db2::itemdisplayinfo::kStorageObject),
                nullptr, displayId, dispBuf);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (!ok)
            return false;
        out.displayId = displayId;
        __try
        {
            for (size_t i = 0; i < db2::itemdisplayinfo::kComponentTexCount; ++i)
            {
                const char* tex = *reinterpret_cast<const char**>(
                    dispBuf + db2::itemdisplayinfo::kOffComponentTex0 + i * sizeof(void*));
                DisplayInfoSetComponentStem(out, static_cast<int>(i), tex);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = {};
            out.displayId = displayId;
        }
        return DisplaySlotHasComponentTextures(out) || out.displayId != 0;
    }

    uint32_t DisplayIdFromItemEntrySeh(uint32_t entry) noexcept
    {
        if (!entry)
            return 0;
        uint32_t displayId = 0;
        __try
        {
            const uint32_t low = *reinterpret_cast<uint32_t*>(db2::item::kMinId);
            const uint32_t high = *reinterpret_cast<uint32_t*>(db2::item::kMaxId);
            if (entry < low || entry > high)
                return 0;
            uint8_t* idTable = *reinterpret_cast<uint8_t**>(db2::item::kIdTable);
            if (!idTable)
                return 0;
            void* rec = *reinterpret_cast<void**>(idTable + (entry - low) * sizeof(void*));
            if (!rec)
                return 0;
            displayId = *reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(rec) + db2::item::kOffDisplayInfoId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return displayId;
    }

    void ClearDisplaySlot(OwnerGuid owner, int slot)
    {
        if (!owner || slot < 0 || slot >= kMaxEquipSlots)
            return;
        std::lock_guard<std::mutex> lock(g_displayMu);
        auto it = g_displaySnap.find(owner);
        if (it == g_displaySnap.end())
            return;
        it->second[static_cast<size_t>(slot)] = {};
    }

    void SetDisplaySlotFromDisplayId(OwnerGuid owner, int slot, uint32_t displayId)
    {
        if (!owner || slot < 0 || slot >= kMaxEquipSlots)
            return;
        DisplaySlotInfo info{};
        if (displayId)
            FillDisplayInfoFromId(displayId, info);
        std::lock_guard<std::mutex> lock(g_displayMu);
        g_displaySnap[owner][static_cast<size_t>(slot)] = info;
    }

    void SetDisplaySlotFromItemEntry(OwnerGuid owner, int slot, uint32_t entry)
    {
        if (!owner || slot < 0 || slot >= kMaxEquipSlots)
            return;
        if (entry == 0)
        {
            ClearDisplaySlot(owner, slot);
            return;
        }
        SetDisplaySlotFromDisplayId(owner, slot, DisplayIdFromItemEntrySeh(entry));
    }

    bool GetDisplaySlotInfo(OwnerGuid owner, int slot, DisplaySlotInfo& out)
    {
        out = {};
        if (!owner || slot < 0 || slot >= kMaxEquipSlots)
            return false;
        std::lock_guard<std::mutex> lock(g_displayMu);
        auto it = g_displaySnap.find(owner);
        if (it == g_displaySnap.end())
        {
            for (const auto& kv : g_displaySnap)
            {
                if (GuidSamePlayer(kv.first, owner))
                {
                    out = kv.second[static_cast<size_t>(slot)];
                    return out.displayId != 0 || DisplaySlotHasComponentTextures(out);
                }
            }
            return false;
        }
        out = it->second[static_cast<size_t>(slot)];
        return out.displayId != 0 || DisplaySlotHasComponentTextures(out);
    }

    // DBC texture[i] is a base name; runtime BLP adds _RaceGender (e.g. Chest_TU → Chest_TU_F).
    bool DbcStemMatchesPathStem(const char* pathStem, const char* dbcStem)
    {
        if (!pathStem || !dbcStem || !pathStem[0] || !dbcStem[0])
            return false;
        if (_stricmp(pathStem, dbcStem) == 0)
            return true;
        const size_t n = std::strlen(dbcStem);
        if (_strnicmp(pathStem, dbcStem, n) != 0)
            return false;
        const char next = pathStem[n];
        return next == '\0' || next == '_';
    }

    bool PathMatchesSlotComponent(const char* path, const DisplaySlotInfo& info, int compIdx)
    {
        if (compIdx < 0 || compIdx >= 8 || !path || !path[0])
            return false;
        const char* slotStem = info.componentStems[compIdx];
        if (!slotStem[0])
            return false;
        char pathStem[kStemCap] = {};
        CopyTexStem(path, pathStem, sizeof(pathStem));
        return pathStem[0] && DbcStemMatchesPathStem(pathStem, slotStem);
    }

    bool PathMatchesAnyStem(const char* path, const DisplaySlotInfo& info)
    {
        if (!path || !path[0])
            return false;
        for (int i = 0; i < 8; ++i)
        {
            if (PathMatchesSlotComponent(path, info, i))
                return true;
        }
        return false;
    }

    // ItemDisplayInfo texture[0..7] → composite paste section (CharComponent).
    int ComponentIndexFromTexturePath(const char* path)
    {
        if (!path)
            return -1;
        if (ContainsCI(path, "sleeve") || ContainsCI(path, "armupper"))
            return 0;
        if (ContainsCI(path, "bracer") || ContainsCI(path, "armlower"))
            return 1;
        if (ContainsCI(path, "glove_ha") || ContainsCI(path, "_ha_"))
            return 2;
        if (ContainsCI(path, "handtexture") || ContainsCI(path, "\\hand\\")
            || ContainsCI(path, "/hand/"))
            return 2;
        if (ContainsCI(path, "torsoupper") || ContainsCI(path, "chest_tu"))
            return 3;
        if (ContainsCI(path, "torsolower") || ContainsCI(path, "chest_tl"))
            return 4;
        if (ContainsCI(path, "legupper") || ContainsCI(path, "pant_lu")
            || ContainsCI(path, "robe_lu") || ContainsCI(path, "belt"))
            return 5;
        if (ContainsCI(path, "leglower") || ContainsCI(path, "pant_ll")
            || ContainsCI(path, "robe_ll") || ContainsCI(path, "boot_ll"))
            return 6;
        if (ContainsCI(path, "foottexture") || ContainsCI(path, "\\foot\\")
            || ContainsCI(path, "/foot/") || ContainsCI(path, "boot_fo"))
            return 7;
        if (ContainsCI(path, "glove") && ContainsCI(path, "_al_"))
            return 1;
        return -1;
    }

    void CandidateEquipSlotsForComponent(int compIdx, int* outSlots, int& outN)
    {
        outN = 0;
        if (!outSlots || compIdx < 0 || compIdx >= 8)
            return;
        auto push = [&](int s) {
            if (outN < 4)
                outSlots[outN++] = s;
        };
        switch (compIdx)
        {
        case 0: case 1: push(8); break;              // bracers
        case 2: push(9); break;                      // hands
        case 3: case 4: push(3); push(4); break;     // shirt + chest
        case 5: push(5); push(6); break;             // waist + legs
        case 6: push(6); break;
        case 7: push(7); break;                      // feet
        default: break;
        }
    }


    // Internal model slot (0-10 from OnItemSlotChange) → WoW EQUIPMENT_SLOT_*.
    int ModelSlotToEquipSlot(uint32_t modelSlot)
    {
        switch (modelSlot)
        {
        case 0:  return 0;
        case 1:  return 2;
        case 2:  return 3;
        case 3:  return 4;
        case 4:  return 5;
        case 5:  return 6;
        case 6:  return 7;
        case 7:  return 8;
        case 8:  return 9;
        case 9:  return 14;
        case 10: return 18;
        default: return -1;
        }
    }

    uint32_t GuardedDisplayIdFromItemData(void* itemDataPtr) noexcept
    {
        if (!itemDataPtr)
            return 0;
        uint32_t v = 0;
        __try { v = *static_cast<uint32_t*>(itemDataPtr); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return v;
    }

    bool PathLooksValid(const char* s)
    {
        if (!s || !s[0])
            return false;
        int n = 0;
        bool hasSlash = false;
        for (; s[n] && n < 260; ++n)
        {
            const unsigned char c = static_cast<unsigned char>(s[n]);
            if (c < 32 || c > 126)
                return false;
            if (c == '\\' || c == '/')
                hasSlash = true;
        }
        return n >= 6 && hasSlash;
    }

    bool PathIsRangedWeapon(const char* stem)
    {
        if (!stem)
            return false;
        static const char* kRanged[] = {
            "bow_", "bow1", "_bow", "gun_", "_gun", "rifle", "crossbow", "xbow_",
            "thrown", "wand_", "_wand", "firearm", "arrow_", "bullet_",
            nullptr
        };
        for (int i = 0; kRanged[i]; ++i)
        {
            if (ContainsCI(stem, kRanged[i]))
                return true;
        }
        return false;
    }

    void* ResolveM2Model(void* batchObj)
    {
        if (!batchObj)
            return nullptr;
        __try
        {
            void* model = *reinterpret_cast<void**>(static_cast<uint8_t*>(batchObj) + kOffInstModel);
            if (model && PathLooksValid(m2::PathStem(model)))
                return model;
            if (PathLooksValid(m2::PathStem(batchObj)))
                return batchObj;
            return nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    int CandidateSlotsForOcPath(const char* stem, int* outSlots, int maxSlots)
    {
        if (!stem || !outSlots || maxSlots <= 0)
            return 0;
        if (!ContainsCI(stem, "item\\objectcomponents\\")
            && !ContainsCI(stem, "item/objectcomponents/"))
            return 0;

        int n = 0;
        auto push = [&](int s) {
            if (s < 0 || n >= maxSlots)
                return;
            for (int i = 0; i < n; ++i)
                if (outSlots[i] == s)
                    return;
            outSlots[n++] = s;
        };

        if (ContainsCI(stem, "\\head\\") || ContainsCI(stem, "/head/"))
            push(0);
        else if (ContainsCI(stem, "\\shoulder\\") || ContainsCI(stem, "/shoulder/"))
            push(2);
        else if (ContainsCI(stem, "\\shield\\") || ContainsCI(stem, "/shield/")
            || ContainsCI(stem, "\\buckler\\") || ContainsCI(stem, "/buckler/"))
            push(16);
        else if (ContainsCI(stem, "\\weapon\\") || ContainsCI(stem, "/weapon/"))
        {
            if (PathIsRangedWeapon(stem))
                push(17);
            else
            {
                push(15);
                push(16);
            }
        }
        return n;
    }

    void ClearDraftSlot(int slot)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        std::lock_guard<std::mutex> lock(g_colorMutex);
        g_draftHsl[slot] = {};
    }

    void ClearAllDrafts()
    {
        std::lock_guard<std::mutex> lock(g_colorMutex);
        for (int i = 0; i < kMaxEquipSlots; ++i)
            g_draftHsl[i] = {};
    }

    void WriteLocalSlotHsl(int slot, const SlotHsl& h)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        std::lock_guard<std::mutex> lock(g_colorMutex);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            g_draftHsl[slot] = h;
        else
            g_slotHsl[slot] = h;
    }

    void ReadLocalSlotHsl(int slot, SlotHsl& out)
    {
        out = {};
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        std::lock_guard<std::mutex> lock(g_colorMutex);
        if (g_previewUiActive.load(std::memory_order_relaxed)
            && g_draftHsl[slot].active)
            out = g_draftHsl[slot];
        else
            out = g_slotHsl[slot];
    }

    void SaveLocalHslIfCommitted()
    {
        if (!g_previewUiActive.load(std::memory_order_relaxed))
            SaveHslToDisk();
    }

    // Transmog tints tab: preview-only draft until WXL_RecolorApplyDraft.
    void SetSlotDraftSolid(int slot, float r, float g, float b)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        SlotHsl h{};
        h.active = true;
        h.mode = 0;
        h.stopCount = 1;
        h.gradFill = 0;
        h.hue = Clamp01(r);
        h.sat = Clamp01(g);
        h.light = Clamp01(b);
        h.stops[0][0] = h.hue;
        h.stops[0][1] = h.sat;
        h.stops[0][2] = h.light;
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_draftHsl[slot] = h;
        }
    }

    void SetSlotDraftGradient(int slot, int nStops, int fill, const float* colors, int colorFloats)
    {
        if (slot < 0 || slot >= kMaxEquipSlots || !colors)
            return;
        if (nStops != 2 && nStops != 3 && nStops != 5)
            nStops = 3;
        SlotHsl h{};
        h.active = true;
        h.mode = 2;
        h.stopCount = static_cast<uint8_t>(nStops);
        h.gradFill = (fill != 0) ? 1 : 0;
        if (h.gradFill == 0)
        {
            if (colorFloats < 3)
                return;
            h.hue = Clamp01(colors[0]);
            h.sat = Clamp01(colors[1]);
            h.light = Clamp01(colors[2]);
        }
        else
        {
            if (colorFloats < nStops * 3)
                return;
            for (int s = 0; s < nStops; ++s)
            {
                h.stops[s][0] = Clamp01(colors[s * 3 + 0]);
                h.stops[s][1] = Clamp01(colors[s * 3 + 1]);
                h.stops[s][2] = Clamp01(colors[s * 3 + 2]);
            }
        }
        ResolveSolidStops(h);
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_draftHsl[slot] = h;
        }
    }

    void SetSlotDraftSelective(int slot, const SelRule* rules, int ruleCount)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        if (!rules || ruleCount <= 0)
        {
            ClearDraftSlot(slot);
            return;
        }
        SlotHsl h{};
        h.active = true;
        h.mode = 1;
        const int n = (std::min)(ruleCount, kMaxSelRules);
        h.ruleCount = static_cast<uint8_t>(n);
        for (int i = 0; i < n; ++i)
        {
            SelRule r = rules[i];
            r.sr = Clamp01(r.sr);
            r.sg = Clamp01(r.sg);
            r.sb = Clamp01(r.sb);
            r.dr = Clamp01(r.dr);
            r.dg = Clamp01(r.dg);
            r.db = Clamp01(r.db);
            r.tol = Clamp01(r.tol);
            if (r.tol < 0.02f)
                r.tol = 0.35f;
            h.rules[i] = r;
        }
        SyncSlotMirrorsFromLastRule(h);
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_draftHsl[slot] = h;
        }
    }

    bool IsBodyEquipSlot(int slot);
    void RequestBodyRebuildForSlot(int slot);

    void ApplySlotDraft(int slot)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        SlotHsl h{};
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            if (!g_draftHsl[slot].active)
                return;
            h = g_draftHsl[slot];
            g_slotHsl[slot] = h;
            g_draftHsl[slot] = {};
        }
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveHslToDisk();
    }

    bool TrySlotHsl(int slot, SlotHsl& out)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return false;
        std::lock_guard<std::mutex> lock(g_colorMutex);
        if (g_draftHsl[slot].active)
        {
            out = g_draftHsl[slot];
            return true;
        }
        if (!g_slotHsl[slot].active)
            return false;
        out = g_slotHsl[slot];
        return true;
    }

    // Server SET payload — draft wins while /recolor is open (Apply sends UI state).
    bool TrySlotHslCommittedOrDraft(int slot, SlotHsl& out)
    {
        return TrySlotHsl(slot, out);
    }

    bool TrySlotHslForOwner(OwnerGuid owner, int slot, SlotHsl& out)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return false;
        const OwnerGuid local = ActiveOwnerGuid();
        // STRICT: local table only for the local player. owner==0 must NOT fall through
        // to g_slotHsl while in world — that leaked local helm tint onto other PCs.
        if (local != 0)
        {
            if (GuidSamePlayer(owner, local))
                return TrySlotHsl(slot, out);
            // Remote (or unknown non-self): only remote map.
            std::lock_guard<std::mutex> lock(g_remoteMu);
            auto it = g_remoteTints.find(owner);
            if (it == g_remoteTints.end())
            {
                // PUSH / UnitGUID may disagree on high bits — match low 32.
                for (const auto& kv : g_remoteTints)
                {
                    if (GuidSamePlayer(kv.first, owner))
                    {
                        const SlotHsl& h = kv.second[static_cast<size_t>(slot)];
                        if (!h.active)
                            return false;
                        out = h;
                        return true;
                    }
                }
                return false;
            }
            const SlotHsl& h = it->second[static_cast<size_t>(slot)];
            if (!h.active)
                return false;
            out = h;
            return true;
        }
        return false;
    }

    bool AnyRemoteTintActive()
    {
        std::lock_guard<std::mutex> lock(g_remoteMu);
        for (const auto& kv : g_remoteTints)
        {
            if (OwnerHasAnyTintLocked(kv.second))
                return true;
        }
        return false;
    }

    bool TryHslForPathOwner(OwnerGuid owner, const char* stem, SlotHsl& out, int* outSlot = nullptr)
    {
        int cands[4] = { -1, -1, -1, -1 };
        const int nc = CandidateSlotsForOcPath(stem, cands, 4);
        if (nc <= 0)
            return false;
        for (int i = 0; i < nc; ++i)
        {
            if (TrySlotHslForOwner(owner, cands[i], out))
            {
                if (outSlot)
                    *outSlot = cands[i];
                return true;
            }
        }
        return false;
    }


    // --- In-world player scoping (no glue / paperdoll / DressUp clones) ---

    // OC live tint only during the world draw pass. Character panel (C), DressUp,
    // and other UI ModelFrames render AFTER OnWorldRenderEnd — must not get g_slotHsl.
    std::atomic<bool> g_ocWorldPass{ true };

    std::atomic<void*> g_localPlayerModel{ nullptr };
    std::atomic<void*> g_localPlayerComponent{ nullptr };
    std::atomic<bool> g_pendingEnterWorldRebuild{ true };
    std::atomic<int> g_rebuildBatchDepth{ 0 };
    std::atomic<uint32_t> g_deferredFullRebuildAt{ 0 };
    std::atomic<uint32_t> g_naturalTintPastes{ 0 };
    std::atomic<bool> g_forceAllowPaste{ false };
    std::atomic<bool> g_assemblingAllowed{ false };
    // Blocks orphan/force remote tint during deferred first-assemble (face bleed).
    // Outside that window orphan must work: most TextureComponents pastes have
    // prepOwner=0 and remotes only get color via ResolveOrphanPasteOwner.
    std::atomic<bool> g_suppressOrphanTint{ false };
    std::atomic<OwnerGuid> g_forceOwnerGuid{ 0 };
    std::atomic<void*> g_prepModel{ nullptr };
    std::atomic<uint32_t> g_prepReasonCode{ 0 };
    std::atomic<bool> g_playerModelLocked{ false };
    constexpr size_t kOffComponentSectionDirty = 0x0C;
    constexpr size_t kOffInstInitFlags = 0x10;

    void* ComponentModel(void* component);
    bool AnySlotActive();
    bool ComponentIsNpc(void* component);
    void FlushTexTintState(const char* reason);

    using CharRenderPrepFn = int(__thiscall*)(void* component, int a2);
    CharRenderPrepFn g_origRenderPrep = nullptr;

    void* SafeReadPtr(void* base, size_t off) noexcept
    {
        if (!base)
            return nullptr;
        __try
        {
            return *reinterpret_cast<void**>(static_cast<uint8_t*>(base) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    uint32_t SafeReadU32(void* base, size_t off) noexcept
    {
        if (!base)
            return 0;
        __try
        {
            return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(base) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void* LocalPlayerBodyModel()
    {
        const unsigned long long guid = ActiveOwnerGuid();
        if (!guid)
            return nullptr;
        void* unit = wxl::game::world::ResolveObject(
            guid, wxl::game::world::kTypeMaskUnit | wxl::game::world::kTypeMaskPlayer);
        void* model = wxl::game::unit::Model(unit);
        if (model)
            g_localPlayerModel.store(model, std::memory_order_relaxed);
        return model;
    }

    bool ComponentIsNpc(void* component)
    {
        return (SafeReadU32(component, kOffComponentFlags) & kComponentFlagNpc) != 0;
    }

    void* ComponentModel(void* component)
    {
        return SafeReadPtr(component, kOffComponentModel);
    }

    bool ModelIsUnderRoot(void* instance, void* root)
    {
        if (!instance || !root)
            return false;
        void* cur = instance;
        for (int depth = 0; depth < 24 && cur; ++depth)
        {
            if (cur == root)
                return true;
            cur = wxl::game::unit::ModelParent(cur);
        }
        return false;
    }

    bool BodyModelOwnerGuid(void* bodyModel, OwnerGuid& outGuid)
    {
        if (!bodyModel)
            return false;
        if (void* local = LocalPlayerBodyModel())
        {
            if (bodyModel == local)
            {
                outGuid = ActiveOwnerGuid();
                return outGuid != 0;
            }
        }
        std::vector<OwnerGuid> owners;
        {
            std::lock_guard<std::mutex> lock(g_remoteMu);
            owners.reserve(g_remoteTints.size());
            for (const auto& kv : g_remoteTints)
            {
                if (OwnerHasAnyTintLocked(kv.second))
                    owners.push_back(kv.first);
            }
            for (const auto& kv : g_remotePendingDirty)
            {
                if (kv.second)
                    owners.push_back(kv.first);
            }
            for (const auto& kv : g_remoteCcModels)
            {
                if (kv.second == bodyModel)
                {
                    outGuid = kv.first;
                    return true;
                }
            }
        }
        for (OwnerGuid guid : owners)
        {
            void* unit = wxl::game::world::ResolveObject(
                guid, wxl::game::world::kTypeMaskUnit | wxl::game::world::kTypeMaskPlayer);
            void* root = wxl::game::unit::Model(unit);
            if (root == bodyModel || (root && ModelIsUnderRoot(bodyModel, root)))
            {
                outGuid = guid;
                return true;
            }
        }
        return false;
    }

    // Live unit body only (self + remote players). No remembered CC / UI clone roots —
    // stale CC pointers were reusable by NPC/paperdoll parents and leaked slot tints.
    bool ResolveModelTintOwner(void* instance, OwnerGuid& outOwner)
    {
        if (!instance)
            return false;

        const OwnerGuid self = ActiveOwnerGuid();

        std::vector<OwnerGuid> owners;
        {
            std::lock_guard<std::mutex> lock(g_remoteMu);
            owners.reserve(g_remoteTints.size());
            for (const auto& kv : g_remoteTints)
            {
                if (OwnerHasAnyTintLocked(kv.second))
                    owners.push_back(kv.first);
            }
        }
        for (OwnerGuid guid : owners)
        {
            if (self != 0 && GuidSamePlayer(guid, self))
                continue;
            void* unit = wxl::game::world::ResolveObject(
                guid, wxl::game::world::kTypeMaskUnit | wxl::game::world::kTypeMaskPlayer);
            void* root = wxl::game::unit::Model(unit);
            if (root && ModelIsUnderRoot(instance, root))
            {
                outOwner = guid;
                return true;
            }
        }

        if (self != 0)
        {
            void* pm = LocalPlayerBodyModel();
            if (pm && ModelIsUnderRoot(instance, pm))
            {
                outOwner = self;
                return true;
            }
        }
        return false;
    }

    // CharModelObject → owner by sceneNode equality. Never pass CMO to BodyModelOwnerGuid
    // or ModelIsUnderRoot — those expect M2 render contexts (AV on DressUp / paperdoll).
    bool ResolveOwnerFromCmo(void* cmo, OwnerGuid& outOwner)
    {
        outOwner = 0;
        if (!cmo)
            return false;
        void* scene = SafeReadPtr(cmo, wxl::offsets::game::m2::kOffCmoSceneNode);
        if (!scene)
            return false;

        const OwnerGuid self = ActiveOwnerGuid();
        if (self)
        {
            void* pm = LocalPlayerBodyModel();
            if (pm && scene == pm)
            {
                outOwner = self;
                return true;
            }
        }

        std::vector<OwnerGuid> owners;
        {
            std::lock_guard<std::mutex> lock(g_remoteMu);
            owners.reserve(g_remoteTints.size() + g_remotePendingDirty.size());
            for (const auto& kv : g_remoteTints)
            {
                if (OwnerHasAnyTintLocked(kv.second))
                    owners.push_back(kv.first);
            }
            for (const auto& kv : g_remotePendingDirty)
            {
                if (kv.second)
                    owners.push_back(kv.first);
            }
        }
        for (OwnerGuid guid : owners)
        {
            if (self && GuidSamePlayer(guid, self))
                continue;
            void* unit = wxl::game::world::ResolveObject(
                guid, wxl::game::world::kTypeMaskUnit | wxl::game::world::kTypeMaskPlayer);
            void* root = wxl::game::unit::Model(unit);
            if (root && scene == root)
            {
                outOwner = guid;
                return true;
            }
        }
        return false;
    }

    void* OwnerBodyModel(OwnerGuid owner)
    {
        if (!owner)
            return nullptr;
        const OwnerGuid self = ActiveOwnerGuid();
        if (self && GuidSamePlayer(owner, self))
            return LocalPlayerBodyModel();
        void* unit = wxl::game::world::ResolveObject(
            owner, wxl::game::world::kTypeMaskUnit | wxl::game::world::kTypeMaskPlayer);
        return wxl::game::unit::Model(unit);
    }

    bool IsPasteTintAllowed()
    {
        const bool force = g_forceAllowPaste.load(std::memory_order_relaxed);
        const bool assembling = g_assemblingAllowed.load(std::memory_order_relaxed);
        if (!force && !assembling)
            return false;

        void* prep = g_prepModel.load(std::memory_order_relaxed);
        if (!prep)
            return false;

        const OwnerGuid prepOwner = g_prepOwnerGuid.load(std::memory_order_relaxed);
        const OwnerGuid self = ActiveOwnerGuid();
        if (!prepOwner)
            return false;

        const OwnerGuid forceOwn = g_forceOwnerGuid.load(std::memory_order_relaxed);
        const bool forceMatch = force && forceOwn != 0
            && GuidSamePlayer(forceOwn, prepOwner);
        if (!forceMatch)
        {
            void* ownerModel = OwnerBodyModel(prepOwner);
            if (!ownerModel || prep != ownerModel)
                return false;
        }
        if (self && GuidSamePlayer(prepOwner, self))
            return true;
        return OwnerHasAnyRemoteTint(prepOwner) || forceMatch;
    }

    void ClearPlayerScope()
    {
        g_localPlayerComponent.store(nullptr, std::memory_order_relaxed);
        g_localPlayerModel.store(nullptr, std::memory_order_relaxed);
        g_playerModelLocked.store(false, std::memory_order_relaxed);
        g_pendingEnterWorldRebuild.store(true, std::memory_order_relaxed);
        g_naturalTintPastes.store(0, std::memory_order_relaxed);
        g_deferredFullRebuildAt.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            for (int i = 0; i < kMaxEquipSlots; ++i)
            {
                g_slotHsl[static_cast<size_t>(i)] = {};
                g_draftHsl[static_cast<size_t>(i)] = {};
            }
        }
        FlushTexTintState("clear_scope");
    }

    bool IsBodyEquipSlot(int slot)
    {
        return slot == 3 || slot == 4 || slot == 5
            || slot == 6 || slot == 7 || slot == 8 || slot == 9;
    }

    // COMPONENT_SECTIONS bits used by RenderPrepSections @ 0x4EE0D0.
    uint32_t SectionMaskForEquipSlot(int slot)
    {
        switch (slot)
        {
        case 3: case 4: // shirt / chest — torso + sleeves/bracers from same item
            return (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4);
        case 5: // waist / belt lives on leg-upper
            return (1u << 5);
        case 6: // legs
            return (1u << 5) | (1u << 6);
        case 7: // feet (boot_ll on leg-lower + foot)
            return (1u << 6) | (1u << 7);
        case 8: // wrists / arms
            return (1u << 0) | (1u << 1);
        case 9: // hands
            return (1u << 1) | (1u << 2);
        case -1: // full body rebuild
            return 0x3FFu;
        default:
            return 0;
        }
    }

    bool PeekRemotePendingDirty(OwnerGuid owner)
    {
        std::lock_guard<std::mutex> lock(g_remoteMu);
        for (const auto& kv : g_remotePendingDirty)
        {
            if (kv.second && GuidSamePlayer(kv.first, owner))
                return true;
        }
        return false;
    }

    uint32_t TakeRemotePendingMask(OwnerGuid owner)
    {
        std::lock_guard<std::mutex> lock(g_remoteMu);
        for (auto it = g_remotePendingDirty.begin(); it != g_remotePendingDirty.end(); ++it)
        {
            if (!GuidSamePlayer(it->first, owner))
                continue;
            const uint32_t mask = it->second;
            g_remotePendingDirty.erase(it);
            return mask;
        }
        return 0;
    }

    void ClearRemotePendingDirty(OwnerGuid owner)
    {
        std::lock_guard<std::mutex> lock(g_remoteMu);
        for (auto it = g_remotePendingDirty.begin(); it != g_remotePendingDirty.end(); )
        {
            if (GuidSamePlayer(it->first, owner))
                it = g_remotePendingDirty.erase(it);
            else
                ++it;
        }
    }

    void OrRemotePendingMask(OwnerGuid owner, uint32_t sectionMask)
    {
        if (!owner || sectionMask == 0)
            return;
        std::lock_guard<std::mutex> lock(g_remoteMu);
        for (auto& kv : g_remotePendingDirty)
        {
            if (GuidSamePlayer(kv.first, owner))
            {
                kv.second |= sectionMask;
                return;
            }
        }
        g_remotePendingDirty[owner] |= sectionMask;
    }

    bool IsRemoteNativeAssembleDone(OwnerGuid owner)
    {
        if (!owner)
            return false;
        std::lock_guard<std::mutex> lock(g_remoteMu);
        for (const auto& kv : g_remoteNativeAssembleDone)
        {
            if (GuidSamePlayer(kv.first, owner) && kv.second)
                return true;
        }
        return false;
    }

    void MarkRemoteNativeAssembleDone(OwnerGuid owner)
    {
        if (!owner)
            return;
        std::lock_guard<std::mutex> lock(g_remoteMu);
        for (auto& kv : g_remoteNativeAssembleDone)
        {
            if (GuidSamePlayer(kv.first, owner))
            {
                kv.second = 1;
                return;
            }
        }
        g_remoteNativeAssembleDone[owner] = 1;
    }

    void RememberRemoteCc(OwnerGuid owner, void* component, void* model)
    {
        if (!owner || (!component && !model))
            return;
        std::lock_guard<std::mutex> lock(g_remoteMu);
        if (model)
            g_remoteCcModels[owner] = model;
        if (component)
            g_remoteCcComponents[owner] = component;
    }

    void RememberModelComponent(void* model, void* component)
    {
        if (!model || !component)
            return;
        std::lock_guard<std::mutex> lock(g_remoteMu);
        g_modelToComponent[model] = component;
    }

    void* LookupComponentForModel(void* model)
    {
        if (!model)
            return nullptr;
        std::lock_guard<std::mutex> lock(g_remoteMu);
        auto it = g_modelToComponent.find(model);
        return it != g_modelToComponent.end() ? it->second : nullptr;
    }

    void SetComponentSectionDirtySeh(void* component, uint32_t sectionMask) noexcept
    {
        if (!component || sectionMask == 0)
            return;
        __try
        {
            *reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(component) + kOffComponentSectionDirty)
                |= sectionMask;
            *reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(component) + 0x08) |= 0x1u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void DirtyComponentSections(void* component, uint32_t sectionMask)
    {
        // NEVER use full-body 0x3FF — that re-pastes every section and corrupts
        // composites. Callers must pass only the slot's section bits.
        if (!sectionMask)
            return;
        SetComponentSectionDirtySeh(component, sectionMask);
    }

    uint32_t ActiveRemoteSectionMask(OwnerGuid owner)
    {
        uint32_t mask = 0;
        if (!owner)
            return 0;
        std::lock_guard<std::mutex> lock(g_remoteMu);
        auto accumulate = [&](const OwnerSlotHsl& slots)
        {
            for (int s = 0; s < kMaxEquipSlots; ++s)
            {
                if (slots[static_cast<size_t>(s)].active)
                    mask |= SectionMaskForEquipSlot(s);
            }
        };
        auto it = g_remoteTints.find(owner);
        if (it != g_remoteTints.end())
        {
            accumulate(it->second);
            return mask;
        }
        for (const auto& kv : g_remoteTints)
        {
            if (GuidSamePlayer(kv.first, owner))
            {
                accumulate(kv.second);
                break;
            }
        }
        return mask;
    }

    int CallOrigRenderPrepSeh(void* component, int a2) noexcept
    {
        if (!component || !g_origRenderPrep)
            return 0;
        __try
        {
            return g_origRenderPrep(component, a2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // Verified CharComponent for a player: component.model must equal unit+0xB4.
    void* FindVerifiedOwnerComponent(OwnerGuid owner, void** outModel = nullptr)
    {
        if (!owner)
            return nullptr;
        void* unit = wxl::game::world::ResolveObject(
            owner, wxl::game::world::kTypeMaskUnit | wxl::game::world::kTypeMaskPlayer);
        void* unitModel = wxl::game::unit::Model(unit);
        if (!unitModel)
            return nullptr;
        if (outModel)
            *outModel = unitModel;

        void* remembered = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_remoteMu);
            auto it = g_remoteCcComponents.find(owner);
            if (it != g_remoteCcComponents.end())
                remembered = it->second;
        }
        if (remembered && ComponentModel(remembered) == unitModel)
            return remembered;

        void* mapped = LookupComponentForModel(unitModel);
        if (mapped && ComponentModel(mapped) == unitModel)
            return mapped;
        return nullptr;
    }


    // Identical rebuild for self and remote (the path that already works on the
    // local player). forceAllowPaste + restore-after-paste is safe per-component
    // because paste hooks always RestoreTextureCacheOrig.
    void ScheduleOwnerSectionRebuild(OwnerGuid owner, uint32_t sectionMask); // fwd
    int SetComponentSectionDirtyReplaceSeh(void* component, uint32_t sectionMask) noexcept; // fwd
    void ForceOwnerBodyRebuild(OwnerGuid owner, uint32_t sectionMask); // fwd
    void ForceOwnerPerSlotMasks(OwnerGuid owner, uint32_t sectionMask); // fwd
    void ScheduleDeferredRemoteClear(OwnerGuid owner, uint32_t sectionMask); // fwd
    void CancelDeferredRemoteClear(OwnerGuid owner, uint32_t sectionMask); // fwd
    void FlushDeferredRemoteClears(); // fwd

    void* FindOwnerCharComponent(OwnerGuid owner, void** outModel)
    {
        if (!owner)
            return nullptr;
        const OwnerGuid self = ActiveOwnerGuid();
        if (self && GuidSamePlayer(owner, self))
        {
            void* pm = LocalPlayerBodyModel();
            void* local = g_localPlayerComponent.load(std::memory_order_relaxed);
            if (pm && local && ComponentModel(local) == pm)
            {
                if (outModel)
                    *outModel = pm;
                return local;
            }
            if (pm)
            {
                void* mapped = LookupComponentForModel(pm);
                if (mapped && ComponentModel(mapped) == pm)
                {
                    if (outModel)
                        *outModel = pm;
                    return mapped;
                }
            }
            // No glue / char-select sticky — in-world body only.
            return nullptr;
        }
        return FindVerifiedOwnerComponent(owner, outModel);
    }

    // #region agent log — runtime RE probe (no tint behavior change)
    constexpr uintptr_t kWowLocalCharComponent = 0x00B6B1A0;
    constexpr uintptr_t kWowCharComponentPool = 0x00B6B240;
    constexpr size_t kWowCharComponentStride = 0x198;

    void ReProbeLog615(const char* message, void* component, void* model, int a2,
        uint32_t flags, const char* reason, int allowed, OwnerGuid owner,
        void* unitRoot, int modelEqRoot, int underRoot, int verifiedComp,
        int pasteStrictOk, int isSingleton, int poolIndex)
    {
        static std::atomic<int> s_n{ 0 };
        if (s_n.fetch_add(1, std::memory_order_relaxed) >= 500)
            return;
        FILE* f = nullptr;
        if (fopen_s(&f, kDbgLog615, "a") != 0 || !f)
            return;
        const unsigned long long ts = static_cast<unsigned long long>(GetTickCount64());
        fprintf(f,
            "{\"sessionId\":\"615e3b\",\"runId\":\"no-remote-force\",\"hypothesisId\":\"RE\","
            "\"location\":\"hkRenderPrep\",\"message\":\"%s\",\"timestamp\":%llu,"
            "\"data\":{\"reason\":\"%s\",\"allowed\":%d,\"a2\":%d,\"flags\":%u,"
            "\"comp\":%llu,\"model\":%llu,\"unitRoot\":%llu,"
            "\"ownerLow\":%u,\"modelEqRoot\":%d,\"underRoot\":%d,"
            "\"verifiedComp\":%d,\"pasteStrictOk\":%d,"
            "\"isSingleton\":%d,\"poolIndex\":%d}}\n",
            message, ts, reason ? reason : "", allowed, a2, flags,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(component)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(model)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(unitRoot)),
            static_cast<unsigned>(owner & 0xFFFFFFFFull),
            modelEqRoot, underRoot, verifiedComp, pasteStrictOk,
            isSingleton, poolIndex);
        fclose(f);
    }

    int CharComponentPoolIndex(void* component)
    {
        if (!component)
            return -1;
        void* pool = nullptr;
        __try { pool = *reinterpret_cast<void**>(kWowCharComponentPool); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        if (!pool)
            return -1;
        const uintptr_t base = reinterpret_cast<uintptr_t>(pool);
        const uintptr_t cur = reinterpret_cast<uintptr_t>(component);
        if (cur < base)
            return -1;
        const uintptr_t delta = cur - base;
        if (delta % kWowCharComponentStride != 0)
            return -1;
        return static_cast<int>(delta / kWowCharComponentStride);
    }

    void ReProbeRenderPrep(void* component, void* model, int a2, bool isNpc,
        const char* reason, bool allowed, OwnerGuid self, void* playerModel)
    {
        if (isNpc || !component || !model)
            return;

        OwnerGuid owner = 0;
        void* unitRoot = nullptr;
        const int isLocal = (playerModel && model == playerModel) ? 1 : 0;
        if (isLocal)
        {
            owner = self;
            unitRoot = playerModel;
        }
        else if (!BodyModelOwnerGuid(model, owner))
            return;

        if (!unitRoot)
            unitRoot = OwnerBodyModel(owner);
        if (!unitRoot)
            return;

        const int hasRemoteTint = OwnerHasAnyRemoteTint(owner) ? 1 : 0;
        if (!isLocal && !hasRemoteTint && !allowed)
            return;

        static std::atomic<uint32_t> s_localSample{ 0 };
        if (isLocal && !hasRemoteTint)
        {
            if ((s_localSample.fetch_add(1, std::memory_order_relaxed) % 40) != 0)
                return;
        }

        void* singleton = nullptr;
        __try { singleton = *reinterpret_cast<void**>(kWowLocalCharComponent); }
        __except (EXCEPTION_EXECUTE_HANDLER) { singleton = nullptr; }
        const int isSingleton = (singleton && component == singleton) ? 1 : 0;
        const int poolIndex = CharComponentPoolIndex(component);

        const uint32_t flags = SafeReadU32(component, kOffComponentFlags);
        const int modelEqRoot = (model == unitRoot) ? 1 : 0;
        const int underRoot = ModelIsUnderRoot(model, unitRoot) ? 1 : 0;
        void* verifiedModel = nullptr;
        void* verified = FindVerifiedOwnerComponent(owner, &verifiedModel);
        const int verifiedComp = verified ? 1 : 0;
        const char* tag = isLocal ? "self" : "remote";
        if (!allowed && hasRemoteTint)
            tag = "remote_miss";
        ReProbeLog615(tag, component, model, a2, flags, reason,
            allowed ? 1 : 0, owner, unitRoot, modelEqRoot, underRoot,
            verifiedComp, modelEqRoot, isSingleton, poolIndex);
    }
    // #endregion

    void DirtyOwnerSections(OwnerGuid owner, uint32_t sectionMask)
    {
        if (!owner || sectionMask == 0)
            return;
        const OwnerGuid self = ActiveOwnerGuid();
        if (self && GuidSamePlayer(owner, self))
            ScheduleOwnerSectionRebuild(owner, sectionMask);
        else
            OrRemotePendingMask(owner, sectionMask);
    }

    // Unified write: self → g_slotHsl, other → g_remoteTints, then DirtyOwnerSections.
    bool ApplyOwnerTint(OwnerGuid owner, int slot, const SlotHsl* hslOrNull)
    {
        if (!owner || slot < 0 || slot >= kMaxEquipSlots)
            return false;
        const OwnerGuid self = ActiveOwnerGuid();
        const bool isSelf = self && GuidSamePlayer(owner, self);
        if (isSelf)
        {
            if (!hslOrNull)
            {
                {
                    std::lock_guard<std::mutex> lock(g_colorMutex);
                    g_slotHsl[slot] = {};
                    g_draftHsl[slot] = {};
                }
                SaveHslToDisk();
            }
            else
            {
                {
                    std::lock_guard<std::mutex> lock(g_colorMutex);
                    g_slotHsl[slot] = *hslOrNull;
                    g_draftHsl[slot] = {};
                }
                SaveHslToDisk();
            }
            const uint32_t mask = SectionMaskForEquipSlot(slot);
            if (mask)
                DirtyOwnerSections(self, mask);
            return true;
        }
        if (!hslOrNull)
        {
            ClearRemoteSlotTint(owner, slot);
            const uint32_t mask = SectionMaskForEquipSlot(slot);
            // Defer Force: transmog visible-slot PUSH clear is often followed by
            // SET within the same tick — immediate Force pasted untinted feet
            // (logs: feet_paste empty/hasHsl:0) and wiped neighbours.
            if (mask)
                ScheduleDeferredRemoteClear(owner, mask);
            // Do NOT zero equip snap here — SET's NoteEquip refreshes entry.
            // Zeroing made empty:1 gate / diagnostics lie during the CLEAR window.
            // Head/shoulder/weapon (mask 0): OC path. NEVER fall back to
            // ActiveRemoteSectionMask — that Forced remotes with mask=255 and wiped
            // their composites (logs Asf L853/871/887 after weapon/helm unequip).
            return true;
        }

        // PUSH SET can arrive before equip snap if a transmog CLEAR storm zeroed
        // it — store tint and retry via pending mask instead of dropping.
        if (EquipSnapEntry(owner, slot) == 0)
        {
            SetRemoteSlotTint(owner, slot, *hslOrNull);
            const uint32_t mask = SectionMaskForEquipSlot(slot);
            if (mask)
                OrRemotePendingMask(owner, mask);
            return true;
        }

        // Skip Force when remote already has the identical HSL — transmog UI
        // re-PUSH SET storms were wiping composites (force_rebuild x300+).
        SlotHsl prev{};
        const bool hadPrev = TrySlotHslForOwner(owner, slot, prev);
        char prevData[384] = {};
        char nextData[384] = {};
        const bool same = hadPrev && prev.active && hslOrNull->active
            && prev.mode == hslOrNull->mode
            && FormatTintData(prev, prevData, sizeof(prevData))
            && FormatTintData(*hslOrNull, nextData, sizeof(nextData))
            && std::strcmp(prevData, nextData) == 0;
        SetRemoteSlotTint(owner, slot, *hslOrNull);
        const uint32_t mask = SectionMaskForEquipSlot(slot);
        if (mask)
            CancelDeferredRemoteClear(owner, mask);
        if (!same && mask)
            DirtyOwnerSections(owner, mask);

        // #region agent log
        if (g_dbgApplyRemoteN.fetch_add(1, std::memory_order_relaxed) < 40)
        {
            DbgLog615("H3", "ApplyOwnerTint", "remote_set",
                static_cast<unsigned long long>(owner), slot, static_cast<int>(mask),
                "", static_cast<int>(hslOrNull->mode), EquipSnapEntry(owner, slot),
                mask);
        }
        // #endregion
        return true;
    }


    int SetComponentSectionDirtyReplaceSeh(void* component, uint32_t sectionMask) noexcept
    {
        if (!component || sectionMask == 0)
            return -1;
        __try
        {
            *reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(component) + kOffComponentSectionDirty) = sectionMask;
            *reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(component) + 0x08) |= 0x1u;
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // Never Force a mega-mask (255 / multi-slot OR) — re-pastes whole composite.
    void ForceOwnerPerSlotMasks(OwnerGuid owner, uint32_t sectionMask)
    {
        if (!owner || sectionMask == 0)
            return;
        bool any = false;
        for (int s = 0; s < kMaxEquipSlots; ++s)
        {
            const uint32_t m = SectionMaskForEquipSlot(s);
            if (!m || (sectionMask & m) == 0)
                continue;
            ForceOwnerBodyRebuild(owner, m);
            any = true;
        }
        if (!any)
            ForceOwnerBodyRebuild(owner, sectionMask);
    }

    void ForceOwnerBodyRebuild(OwnerGuid owner, uint32_t sectionMask)
    {
        if (!owner || sectionMask == 0 || !g_origRenderPrep)
            return;
        void* unitModel = nullptr;
        void* component = FindOwnerCharComponent(owner, &unitModel);
        if (!component || !unitModel)
        {
            const OwnerGuid self = ActiveOwnerGuid();
            if (!(self && GuidSamePlayer(owner, self)))
                OrRemotePendingMask(owner, sectionMask);
            return;
        }
        const OwnerGuid self = ActiveOwnerGuid();
        if (!(self && GuidSamePlayer(owner, self)))
            RememberRemoteCc(owner, component, unitModel);

        if (SetComponentSectionDirtyReplaceSeh(component, sectionMask) < 0)
            return;

        // Identical to the local-player force path that already works.
        g_forceAllowPaste.store(true, std::memory_order_relaxed);
        g_assemblingAllowed.store(true, std::memory_order_relaxed);
        g_prepModel.store(unitModel, std::memory_order_relaxed);
        g_prepOwnerGuid.store(owner, std::memory_order_relaxed);
        g_forceOwnerGuid.store(owner, std::memory_order_relaxed);
        CallOrigRenderPrepSeh(component, 1);
        g_forceAllowPaste.store(false, std::memory_order_relaxed);
        g_assemblingAllowed.store(false, std::memory_order_relaxed);
        g_prepModel.store(nullptr, std::memory_order_relaxed);
        g_prepOwnerGuid.store(0, std::memory_order_relaxed);
        g_forceOwnerGuid.store(0, std::memory_order_relaxed);
        ClearRemotePendingDirty(owner);

        // #region agent log
        {
            const OwnerGuid self = ActiveOwnerGuid();
            const bool isRemote = self != 0 && !GuidSamePlayer(owner, self);
            if (isRemote && g_dbgForceRebuildN.fetch_add(1, std::memory_order_relaxed) < 40)
            {
                DbgLog615("H3", "ForceOwnerBodyRebuild", "done",
                    static_cast<unsigned long long>(owner), -1,
                    static_cast<int>(sectionMask), "", static_cast<int>(sectionMask), 0, 0);
            }
        }
        // #endregion
        // Do NOT RescheduleAllRemoteTints here. Logs (Asf L722/741/760): after every
        // self rebuild we ForceOwnerBodyRebuild(remote, mask=255) which wiped shaman
        // composites on the pala client ("To all" / equip feet → shaman tints vanish
        // or feet pick up a new colour) while g_remoteTints data stayed intact.
    }

    std::mutex g_ownerCoalesceMu;
    std::unordered_map<OwnerGuid, uint32_t> g_ownerCoalesceMask;
    std::atomic<uint32_t> g_ownerCoalesceDue{ 0 };

    // Remote PUSH clear from transmog visible-slot flicker: defer Force so a
    // following SET cancels the wipe (logs: CLEAR→feet_paste hasHsl:0→SET).
    std::mutex g_remoteClearMu;
    std::unordered_map<OwnerGuid, uint32_t> g_remoteDeferredClearMask;
    std::atomic<uint32_t> g_remoteClearDue{ 0 };

    void CancelDeferredRemoteClear(OwnerGuid owner, uint32_t sectionMask)
    {
        if (!owner || !sectionMask)
            return;
        std::lock_guard<std::mutex> lock(g_remoteClearMu);
        for (auto it = g_remoteDeferredClearMask.begin(); it != g_remoteDeferredClearMask.end(); )
        {
            if (!GuidSamePlayer(it->first, owner))
            {
                ++it;
                continue;
            }
            it->second &= ~sectionMask;
            if (!it->second)
                it = g_remoteDeferredClearMask.erase(it);
            else
                ++it;
        }
    }

    void ScheduleDeferredRemoteClear(OwnerGuid owner, uint32_t sectionMask)
    {
        if (!owner || !sectionMask)
            return;
        {
            std::lock_guard<std::mutex> lock(g_remoteClearMu);
            OwnerGuid key = owner;
            for (const auto& kv : g_remoteDeferredClearMask)
            {
                if (GuidSamePlayer(kv.first, owner))
                {
                    key = kv.first;
                    break;
                }
            }
            g_remoteDeferredClearMask[key] |= sectionMask;
        }
        g_remoteClearDue.store(GetTickCount() + 450u, std::memory_order_relaxed);
    }

    void FlushDeferredRemoteClears()
    {
        const uint32_t due = g_remoteClearDue.load(std::memory_order_relaxed);
        if (due == 0 || GetTickCount() < due)
            return;
        g_remoteClearDue.store(0, std::memory_order_relaxed);
        std::vector<std::pair<OwnerGuid, uint32_t>> jobs;
        {
            std::lock_guard<std::mutex> lock(g_remoteClearMu);
            jobs.assign(g_remoteDeferredClearMask.begin(), g_remoteDeferredClearMask.end());
            g_remoteDeferredClearMask.clear();
        }
        for (const auto& job : jobs)
        {
            if (!job.second)
                continue;
            for (int s = 0; s < kMaxEquipSlots; ++s)
            {
                const uint32_t m = SectionMaskForEquipSlot(s);
                if (!m || (job.second & m) == 0)
                    continue;
                DirtyOwnerSections(job.first, m);
            }
        }
    }

    void ScheduleOwnerSectionRebuild(OwnerGuid owner, uint32_t sectionMask)
    {
        if (!owner || sectionMask == 0)
            return;
        {
            std::lock_guard<std::mutex> lock(g_ownerCoalesceMu);
            g_ownerCoalesceMask[owner] |= sectionMask;
        }
        g_ownerCoalesceDue.store(GetTickCount() + 32u, std::memory_order_relaxed);
    }

    void FlushCoalescedOwnerRebuild()
    {
        const uint32_t due = g_ownerCoalesceDue.load(std::memory_order_relaxed);
        if (due == 0 || GetTickCount() < due)
            return;
        g_ownerCoalesceDue.store(0, std::memory_order_relaxed);
        std::vector<std::pair<OwnerGuid, uint32_t>> jobs;
        {
            std::lock_guard<std::mutex> lock(g_ownerCoalesceMu);
            jobs.assign(g_ownerCoalesceMask.begin(), g_ownerCoalesceMask.end());
            g_ownerCoalesceMask.clear();
        }
        const OwnerGuid self = ActiveOwnerGuid();
        for (const auto& job : jobs)
        {
            if (!self || !GuidSamePlayer(job.first, self))
                continue;
            ForceOwnerPerSlotMasks(job.first, job.second);
        }
    }

    void FlushPendingRemoteApplies()
    {
        // Login PUSH often arrives before CharComponent exists (force hasComp:0).
        // Once visible + first native assemble done: OR gear dirty bits only.
        // Never REPLACE (wipes face dirty). Never OR into a still-wide 0x3FF
        // first-assemble — that would re-enter tint on the same wide pass.
        std::vector<OwnerGuid> owners;
        {
            std::lock_guard<std::mutex> lock(g_remoteMu);
            owners.reserve(g_remotePendingDirty.size());
            for (const auto& kv : g_remotePendingDirty)
            {
                if (kv.second)
                    owners.push_back(kv.first);
            }
        }
        for (OwnerGuid owner : owners)
        {
            if (!IsRemoteNativeAssembleDone(owner))
                continue;
            void* unitModel = nullptr;
            void* component = FindOwnerCharComponent(owner, &unitModel);
            if (!component)
                continue;
            const uint32_t curDirty = SafeReadU32(component, kOffComponentSectionDirty);
            constexpr uint32_t kGearSectionBits = 0xFFu;
            if ((curDirty & ~kGearSectionBits) != 0)
                continue;
            const uint32_t pending = TakeRemotePendingMask(owner);
            if (!pending)
                continue;
            SetComponentSectionDirtySeh(component, pending);
        }
    }

    void FlushCoalescedLocalRebuild()
    {
        FlushCoalescedOwnerRebuild();
        FlushDeferredRemoteClears();
        FlushPendingRemoteApplies();
    }

    uint32_t ActiveLocalSectionMask()
    {
        uint32_t mask = 0;
        std::lock_guard<std::mutex> lock(g_colorMutex);
        for (int s = 0; s < kMaxEquipSlots; ++s)
        {
            if (g_slotHsl[static_cast<size_t>(s)].active)
                mask |= SectionMaskForEquipSlot(s);
        }
        return mask;
    }

    void ScheduleLocalSectionRebuild(uint32_t sectionMask)
    {
        if (sectionMask == 0)
            return;
        const OwnerGuid self = ActiveOwnerGuid();
        if (!self)
            return;
        ScheduleOwnerSectionRebuild(self, sectionMask);
    }

    void ForceAllowedBodyRebuildForSlot(int slot)
    {
        if (slot < 0)
            return;
        ScheduleLocalSectionRebuild(SectionMaskForEquipSlot(slot));
    }

    // During pushAll / multi-rule SetSlotSelective, skip per-slot rebuilds.
    void RequestBodyRebuildForSlot(int slot)
    {
        if (g_rebuildBatchDepth.load(std::memory_order_relaxed) > 0)
            return;
        ForceAllowedBodyRebuildForSlot(slot);
    }

    void BeginBodyRebuildBatch()
    {
        g_rebuildBatchDepth.fetch_add(1, std::memory_order_relaxed);
    }

    void EndBodyRebuildBatch(bool forceRebuild)
    {
        const int d = g_rebuildBatchDepth.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (d > 0)
            return;
        if (d < 0)
            g_rebuildBatchDepth.store(0, std::memory_order_relaxed);

        // Boot / enter-world sync: colors only. Forcing a full 0x3FF rebuild after the
        // engine already assembled with live paste tints is what made in-world look
        // worse than char-select Enter World.
        if (!forceRebuild)
            return;

        g_pendingEnterWorldRebuild.store(false, std::memory_order_relaxed);
        g_deferredFullRebuildAt.store(0, std::memory_order_relaxed);
        // Only sections with active local tint — never mask 1023.
        const uint32_t mask = ActiveLocalSectionMask();
        if (mask == 0)
            return;
        ScheduleLocalSectionRebuild(mask);
    }

    void EndBodyRebuildBatch()
    {
        EndBodyRebuildBatch(true);
    }

    // Back-compat wrapper used by Lua preview refresh — active tint sections only.
    void ForceAllowedBodyRebuild()
    {
        ScheduleLocalSectionRebuild(ActiveLocalSectionMask());
    }

    int __fastcall hkRenderPrep(void* component, void* /*edx*/, int a2)
    {
        bool allowed = false;
        void* model = ComponentModel(component);
        void* playerModel = LocalPlayerBodyModel();
        const bool isNpc = ComponentIsNpc(component);
        const char* reason = "deny";
        const OwnerGuid self = ActiveOwnerGuid();
        bool markRemoteNativeAfter = false;
        OwnerGuid markRemoteOwner = 0;

        if (component && model && !isNpc)
            RememberModelComponent(model, component);

        {
            static std::atomic<bool> s_wasWorldPlayer{ false };
            if (playerModel)
                s_wasWorldPlayer.store(true, std::memory_order_relaxed);
            else if (s_wasWorldPlayer.exchange(false, std::memory_order_relaxed))
                ClearPlayerScope();
        }

        if (component && !isNpc)
        {
            if (playerModel && model == playerModel)
            {
                allowed = true;
                reason = "local";
                g_localPlayerComponent.store(component, std::memory_order_relaxed);
                g_playerModelLocked.store(true, std::memory_order_relaxed);
                g_prepOwnerGuid.store(self, std::memory_order_relaxed);
            }
            else
            {
                OwnerGuid remoteOwner = 0;
                if (model && BodyModelOwnerGuid(model, remoteOwner)
                    && remoteOwner != 0 && !GuidSamePlayer(remoteOwner, self))
                {
                    const bool pending = PeekRemotePendingDirty(remoteOwner);
                    const bool hasTint = OwnerHasAnyRemoteTint(remoteOwner);
                    if (hasTint || pending)
                    {
                        // Gear sections use bits 0..7. First-create dirty is often
                        // 0x3FF (includes 8..9). Tinting during that pass paints
                        // item HSL onto face/skin composite (cold dual-login).
                        const uint32_t curDirty = SafeReadU32(
                            component, kOffComponentSectionDirty);
                        constexpr uint32_t kGearSectionBits = 0xFFu;
                        const bool wideAssemble =
                            (curDirty & ~kGearSectionBits) != 0;
                        const bool nativeDone =
                            IsRemoteNativeAssembleDone(remoteOwner);

                        if (wideAssemble || !nativeDone)
                        {
                            RememberRemoteCc(remoteOwner, component, model);
                            if (hasTint)
                            {
                                const uint32_t need =
                                    ActiveRemoteSectionMask(remoteOwner);
                                if (need)
                                    OrRemotePendingMask(remoteOwner, need);
                            }
                            markRemoteNativeAfter = true;
                            markRemoteOwner = remoteOwner;
                        }
                        else
                        {
                            allowed = true;
                            reason = "remote";
                            g_prepOwnerGuid.store(remoteOwner,
                                std::memory_order_relaxed);
                            RememberRemoteCc(remoteOwner, component, model);
                            if (pending)
                            {
                                const uint32_t mask =
                                    TakeRemotePendingMask(remoteOwner);
                                if (mask)
                                {
                                    // OR, never Replace — preserves non-gear dirty.
                                    SetComponentSectionDirtySeh(component, mask);
                                    g_forceAllowPaste.store(true,
                                        std::memory_order_relaxed);
                                }
                            }
                        }
                    }
                }
            }
        }
        else if (isNpc)
            reason = "npc_flag";

        if (!playerModel && g_playerModelLocked.load(std::memory_order_relaxed))
            ClearPlayerScope();

        if (!allowed)
        {
            const OwnerGuid fo = g_forceOwnerGuid.load(std::memory_order_relaxed);
            void* om = fo ? OwnerBodyModel(fo) : nullptr;
            if (fo && model && om && model == om
                && g_forceAllowPaste.load(std::memory_order_relaxed))
            {
                allowed = true;
                reason = "force_owner";
                g_prepOwnerGuid.store(fo, std::memory_order_relaxed);
                if (!GuidSamePlayer(fo, self))
                    RememberRemoteCc(fo, component, model);
            }
        }

        if (allowed)
            g_prepModel.store(model, std::memory_order_relaxed);
        else
            g_prepModel.store(nullptr, std::memory_order_relaxed);
        if (!allowed)
            g_prepOwnerGuid.store(0, std::memory_order_relaxed);

        uint32_t code = 0;
        if (allowed)
        {
            if (std::strcmp(reason, "local") == 0)
                code = 1;
            else if (std::strcmp(reason, "remote") == 0
                || std::strcmp(reason, "force_owner") == 0)
                code = 5;
        }
        g_prepReasonCode.store(code, std::memory_order_relaxed);
        g_assemblingAllowed.store(allowed, std::memory_order_relaxed);

        ReProbeRenderPrep(component, model, a2, isNpc, reason, allowed, self, playerModel);

        if (markRemoteNativeAfter)
            g_suppressOrphanTint.store(true, std::memory_order_relaxed);
        const int rc = CallOrigRenderPrepSeh(component, a2);
        if (markRemoteNativeAfter)
            g_suppressOrphanTint.store(false, std::memory_order_relaxed);
        g_forceAllowPaste.store(false, std::memory_order_relaxed);
        g_assemblingAllowed.store(false, std::memory_order_relaxed);
        g_prepModel.store(nullptr, std::memory_order_relaxed);
        g_prepOwnerGuid.store(0, std::memory_order_relaxed);
        g_prepReasonCode.store(0, std::memory_order_relaxed);

        if (markRemoteNativeAfter && markRemoteOwner)
            MarkRemoteNativeAssembleDone(markRemoteOwner);

        if (allowed && component && AnySlotActive()
            && g_pendingEnterWorldRebuild.load(std::memory_order_relaxed)
            && std::strcmp(reason, "local") == 0)
        {
            g_pendingEnterWorldRebuild.store(false, std::memory_order_relaxed);
            g_deferredFullRebuildAt.store(0, std::memory_order_relaxed);
        }

        FlushCoalescedLocalRebuild();
        return rc;
    }

    // --- Paste hooks: tint TextureComponents via TexTintCache orig backup ---

    std::mutex g_texMutex;
    std::unordered_map<void*, std::string> g_texNames;
    // Serialize all CharComponent pastes so a tinted shared TextureCache cannot be
    // sampled into another unit's composite mid tint→paste→restore (skin/bg leak).
    std::recursive_mutex g_pasteGateMu;

    struct TexTintCache
    {
        std::string path;
        std::vector<uint8_t> orig;      // BGRA mip0 OR palette (256*4) when paletted
        std::vector<uint8_t> origIdx;   // indexed mip0 when paletted (w*h)
        bool paletted = false;
        uint32_t w = 0;
        uint32_t h = 0;
    };
    std::unordered_map<void*, TexTintCache> g_texTint;
    // Canonical CLEAN pixels per texture path. TextureCacheCreate always returns the
    // shared handle (logs: same:1 on 238/238 clones) — player isolation = restore
    // from this map before every paste, tint briefly, restore after. Never trust
    // void*-only orig (rest0:0 always when path was never keyed).
    std::unordered_map<std::string, TexTintCache> g_pathOrig;
    wxl::offsets::engine::gx::CharPasteToSectionFn g_origPasteToSection = nullptr;
    wxl::offsets::engine::gx::CharPasteToSectionFn g_origPasteFromSkin = nullptr;
    wxl::offsets::engine::gx::TextureCacheGetPalFn g_origGetPal = nullptr;
    wxl::offsets::engine::gx::TextureCacheGetMipFn g_origGetMip = nullptr;

    // Private tint view for the duration of one native PasteToSection call.
    // GetPal/GetMip return these buffers so the shared TextureCache is never
    // written (fixes phantom + wipe from in-place mutate/restore races).
    struct PasteTintOverlay
    {
        void* tex = nullptr;
        bool active = false;
        bool paletted = false;
        uint32_t w = 0;
        uint32_t h = 0;
        alignas(16) uint8_t pal[256 * 4]{};
        std::vector<uint8_t> bgra;
    };
    thread_local PasteTintOverlay g_tlsPasteOverlay;

    void ClearPasteTintOverlay()
    {
        g_tlsPasteOverlay.active = false;
        g_tlsPasteOverlay.tex = nullptr;
        g_tlsPasteOverlay.paletted = false;
        g_tlsPasteOverlay.w = 0;
        g_tlsPasteOverlay.h = 0;
        g_tlsPasteOverlay.bgra.clear();
    }

    uint8_t* SafeOrigGetPal(void* tex) noexcept
    {
        if (!tex || !g_origGetPal)
            return nullptr;
        __try { return g_origGetPal(tex); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    uint8_t* SafeOrigGetMip(void* tex, uint32_t mip) noexcept
    {
        if (!tex || !g_origGetMip)
            return nullptr;
        __try { return g_origGetMip(tex, mip); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    uint8_t* __cdecl hkTextureCacheGetPal(void* tex)
    {
        if (!tex)
            return nullptr;
        if (g_tlsPasteOverlay.active && tex == g_tlsPasteOverlay.tex
            && g_tlsPasteOverlay.paletted)
            return g_tlsPasteOverlay.pal;
        return SafeOrigGetPal(tex);
    }

    uint8_t* __cdecl hkTextureCacheGetMip(void* tex, uint32_t mip)
    {
        if (!tex)
            return nullptr;
        if (g_tlsPasteOverlay.active && tex == g_tlsPasteOverlay.tex
            && !g_tlsPasteOverlay.paletted && mip == 0u
            && !g_tlsPasteOverlay.bgra.empty())
            return g_tlsPasteOverlay.bgra.data();
        return SafeOrigGetMip(tex, mip);
    }

    void* LoadTextureCacheByPath(const char* path) noexcept; // defined below
    int SafeGetInfo(void* tex, void* info, int flag) noexcept; // defined below

    // Durable Catch samples: captured while TextureCache/mip-table pixels are live.
    // Stale void* handles after unload return GetPal=null — never rely on them at Catch time.
    struct CatchSample
    {
        std::string path;
        bool paletted = false;
        uint32_t w = 0;
        uint32_t h = 0;
        std::vector<uint8_t> pixels; // 256*4 BGRX palette OR BGRA image
        uint32_t tick = 0;
    };
    std::mutex g_catchMu;
    CatchSample g_catchSample[kMaxEquipSlots];


    void ClearCatchSampleForSlot(int slot)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        std::lock_guard<std::mutex> lock(g_catchMu);
        g_catchSample[slot] = {};
    }

    // Local body slot unequip / replace: purge stale paste refs so section offsets
    // cannot keep painting the previous item after vanilla reuses the composite.
    void OnLocalBodySlotRefsCleared(int slot)
    {
        ClearCatchSampleForSlot(slot);
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
    }

    bool IsItemComponentTexture(const char* name)
    {
        if (!name || !name[0])
            return false;
        // Never touch character skin / face / hair caches (shared across units).
        if (ContainsCI(name, "character\\") || ContainsCI(name, "character/"))
            return false;
        return ContainsCI(name, "item\\texturecomponents")
            || ContainsCI(name, "item/texturecomponents");
    }

    bool IsObjectComponentTexture(const char* name)
    {
        if (!name || !name[0])
            return false;
        return ContainsCI(name, "item\\objectcomponents")
            || ContainsCI(name, "item/objectcomponents");
    }

    bool IsObjectComponentAlbedo(const char* name)
    {
        if (!IsObjectComponentTexture(name))
            return false;
        if (ContainsCI(name, "reflect") || ContainsCI(name, "dust1") || ContainsCI(name, "dust2"))
            return false;
        return true;
    }

    bool IsObjectComponentPathSane(const char* name)
    {
        if (!name)
            return false;
        // wxl-equip-extension can skip bad stems; never touch orphan OC paths here.
        if (ContainsCI(name, "\\glove\\") && ContainsCI(name, "shoulder"))
            return false;
        if (ContainsCI(name, "/glove/") && ContainsCI(name, "shoulder"))
            return false;
        return true;
    }

    std::atomic<uint32_t> g_ocUploadOk{ 0 };
    std::atomic<uint32_t> g_ocUploadFail{ 0 };
    std::atomic<int> g_hslPsState{ -1 }; // -1 unknown, 0 fail, 1 ok

    int SlotForComponentTexture(const char* name)
    {
        if (!name)
            return -1;
        // Filename wins over folder: boot_ll lives under LegLowerTexture but is feet.
        if (ContainsCI(name, "boot") || ContainsCI(name, "foottexture")
            || ContainsCI(name, "\\foot\\"))
            return 7;
        if (ContainsCI(name, "torsoupper") || ContainsCI(name, "torsolower")
            || ContainsCI(name, "chest"))
            return 4;
        if (ContainsCI(name, "legupper") || ContainsCI(name, "leglower")
            || ContainsCI(name, "pant") || ContainsCI(name, "belt"))
            return 6;
        if (ContainsCI(name, "handtexture") || ContainsCI(name, "\\hand\\") || ContainsCI(name, "/hand/")
            || ContainsCI(name, "glove"))
            return 9;
        if (ContainsCI(name, "armupper") || ContainsCI(name, "armlower")
            || ContainsCI(name, "sleeve") || ContainsCI(name, "bracer"))
            return 8;
        if (ContainsCI(name, "accessory"))
            return 5;
        return -1;
    }

    int ScoreComponentSlotMatch(int compIdx, int pathComp, int slot)
    {
        int score = 0;
        if (pathComp >= 0 && pathComp == compIdx)
            score += 8;
        switch (compIdx)
        {
        case 3: case 4:
            if (slot == 4)
                score += 4;
            else if (slot == 3)
                score += 2;
            break;
        case 5:
            if (slot == 5)
                score += 4;
            else if (slot == 6)
                score += 2;
            break;
        case 6:
            if (slot == 6)
                score += 4;
            break;
        case 7:
            if (slot == 7)
                score += 4;
            break;
        case 0: case 1:
            if (slot == 8)
                score += 4;
            break;
        case 2:
            if (slot == 9)
                score += 4;
            break;
        default:
            break;
        }
        return score;
    }

    // Stem match for one owner (no RenderPrep context required).
    // Same rules for self and remotes — tint + (display stem OR component→slot).
    // allowStemlessFallback: folder heuristics when display snap lags — ONLY safe
    // when caller already bound this owner (TryHsl with prepOwner). Orphan must
    // pass false or face/underwear layers pick up item HSL on cold dual-login.
    bool MatchComponentTextureForOwner(OwnerGuid owner, const char* name,
        int pasteSection, SlotHsl& out, int& outSlot,
        bool allowStemlessFallback = false)
    {
        owner = CanonicalTintOwner(owner);
        if (!owner || !name || !IsItemComponentTexture(name))
            return false;

        const int pathComp = ComponentIndexFromTexturePath(name);
        int compIdx = pathComp;
        if (compIdx < 0 && pasteSection >= 0 && pasteSection < 8)
            compIdx = pasteSection;
        if (compIdx < 0 || compIdx >= 8)
            return false;

        int bestSlot = -1;
        int bestScore = -1;
        auto consider = [&](int slot, int scoreBonus, bool requireStem)
        {
            if (slot < 0 || slot >= kMaxEquipSlots)
                return;
            SlotHsl hsl{};
            if (!TrySlotHslForOwner(owner, slot, hsl))
                return;
            if (requireStem)
            {
                if (EquipSnapEntry(owner, slot) == 0)
                    return;
                DisplaySlotInfo info{};
                if (!GetDisplaySlotInfo(owner, slot, info))
                    return;
                if (!PathMatchesSlotComponent(name, info, compIdx)
                    && !PathMatchesAnyStem(name, info))
                    return;
            }
            const int score = ScoreComponentSlotMatch(compIdx, pathComp, slot)
                + scoreBonus;
            if (score > bestScore)
            {
                bestScore = score;
                bestSlot = slot;
            }
        };

        // Primary: tinted slot whose ItemDisplayInfo owns this component stem.
        for (int slot = 0; slot < kMaxEquipSlots; ++slot)
            consider(slot, 8, true);

        // Fallback: component index → equip slot candidates (stem still required).
        if (bestSlot < 0)
        {
            int slotCands[4];
            int nSlotCands = 0;
            CandidateEquipSlotsForComponent(compIdx, slotCands, nSlotCands);
            for (int si = 0; si < nSlotCands; ++si)
                consider(slotCands[si], 0, true);
        }

        if (bestSlot < 0 && allowStemlessFallback)
        {
            int slotCands[4];
            int nSlotCands = 0;
            CandidateEquipSlotsForComponent(compIdx, slotCands, nSlotCands);
            for (int si = 0; si < nSlotCands; ++si)
                consider(slotCands[si], -2, false);
        }

        if (bestSlot < 0)
            return false;
        if (!TrySlotHslForOwner(owner, bestSlot, out))
            return false;
        outSlot = bestSlot;
        return true;
    }

    void CollectTintedOwners(std::vector<OwnerGuid>& out)
    {
        out.clear();
        const OwnerGuid self = ActiveOwnerGuid();
        if (self)
            out.push_back(self);
        std::lock_guard<std::mutex> lock(g_remoteMu);
        for (const auto& kv : g_remoteTints)
        {
            if (!OwnerHasAnyTintLocked(kv.second))
                continue;
            bool dup = false;
            for (OwnerGuid g : out)
            {
                if (GuidSamePlayer(g, kv.first))
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                out.push_back(kv.first);
        }
    }

    // Paste outside RenderPrep: resolve owner from texture stem + equip snap.
    // NEVER consider the local player here — self tints only apply when
    // g_prepOwnerGuid/self RenderPrep (TryHslForComponentTexture). Including self
    // painted local HSL into other units' composites on post-relog assemble
    // (faces / underwear / shared item paths looked "same color as my tint").
    bool ResolveOrphanPasteOwner(const char* name, int pasteSection,
        OwnerGuid& outOwner, SlotHsl& outHsl, int& outSlot)
    {
        outOwner = 0;
        outSlot = -1;
        if (!name || !IsItemComponentTexture(name))
            return false;

        const OwnerGuid self = ActiveOwnerGuid();
        std::vector<OwnerGuid> owners;
        CollectTintedOwners(owners);
        OwnerGuid bestOwner = 0;
        int bestSlot = -1;
        SlotHsl bestHsl{};
        int bestScore = -1;
        for (OwnerGuid owner : owners)
        {
            if (self && GuidSamePlayer(owner, self))
                continue;
            SlotHsl hsl{};
            int slot = -1;
            if (!MatchComponentTextureForOwner(owner, name, pasteSection, hsl, slot,
                false))
                continue;
            const int pathComp = ComponentIndexFromTexturePath(name);
            int compIdx = pathComp;
            if (compIdx < 0 && pasteSection >= 0 && pasteSection < 8)
                compIdx = pasteSection;
            const int score = ScoreComponentSlotMatch(compIdx, pathComp, slot);
            if (score > bestScore)
            {
                bestScore = score;
                bestOwner = owner;
                bestSlot = slot;
                bestHsl = hsl;
            }
        }
        if (!bestOwner || bestSlot < 0)
            return false;
        outOwner = bestOwner;
        outSlot = bestSlot;
        outHsl = bestHsl;
        return true;
    }

    // Same path for self + remote: stem must match ItemDisplayInfo for that slot.
    bool TryHslForComponentTexture(const char* name, int pasteSection, SlotHsl& out, int* outSlot)
    {
        if (!name || !IsItemComponentTexture(name))
            return false;

        OwnerGuid owner = CanonicalTintOwner(
            g_prepOwnerGuid.load(std::memory_order_relaxed));
        if (owner == 0)
        {
            void* prep = g_prepModel.load(std::memory_order_relaxed);
            void* pm = LocalPlayerBodyModel();
            if (!pm || prep != pm)
                return false;
            owner = ActiveOwnerGuid();
            if (!owner)
                return false;
        }

        int slot = -1;
        // Bound prepOwner: stemless fallback OK. Orphan path passes false.
        if (!MatchComponentTextureForOwner(owner, name, pasteSection, out, slot, true))
            return false;
        if (outSlot)
            *outSlot = slot;

        // #region agent log
        {
            const OwnerGuid self = ActiveOwnerGuid();
            const bool isRemoteCtx = self != 0 && !GuidSamePlayer(owner, self);
            if (isRemoteCtx && g_dbgTryHslN.fetch_add(1, std::memory_order_relaxed) < 60)
            {
                char stem[kStemCap] = {};
                DbgTexBasename615(name, stem, sizeof(stem));
                DisplaySlotInfo di{};
                uint32_t dispId = 0;
                if (GetDisplaySlotInfo(owner, slot, di))
                    dispId = di.displayId;
                DbgLog615("H1", "TryHslForComponentTexture",
                    "chosen", static_cast<unsigned long long>(owner), slot, pasteSection, stem,
                    ComponentIndexFromTexturePath(name), 0, dispId);
            }
        }
        // #endregion
        return true;
    }

    const char* TextureNameFromHandleField(void* handle) noexcept
    {
        if (!handle)
            return nullptr;
        __try
        {
            const char* n24 = reinterpret_cast<const char*>(static_cast<uint8_t*>(handle) + 0x24);
            if (n24 && n24[0] && (n24[0] == 'I' || n24[0] == 'i' || n24[0] == 'c' || n24[0] == 'C'))
                return n24;
            const char* n = reinterpret_cast<const char*>(
                static_cast<uint8_t*>(handle) + wxl::offsets::engine::gx::kTexHandleNameField);
            if (n && n[0])
                return n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    const char* TextureName(void* handle)
    {
        if (!handle)
            return nullptr;
        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            auto it = g_texNames.find(handle);
            if (it != g_texNames.end())
                return it->second.c_str();
        }
        return TextureNameFromHandleField(handle);
    }

    bool AnySlotActive()
    {
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            for (int i = 0; i < kMaxEquipSlots; ++i)
            {
                if (g_slotHsl[i].active || g_draftHsl[i].active)
                    return true;
            }
        }
        return AnyRemoteTintActive();
    }

    int SafeHasMips(void* tex) noexcept
    {
        auto fn = reinterpret_cast<wxl::offsets::engine::gx::TextureCacheHasMipsFn>(
            wxl::offsets::engine::gx::kTextureCacheHasMips);
        __try { return fn(tex); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    uint8_t* SafeGetPal(void* tex) noexcept
    {
        // Prefer trampoline so we never re-enter hk while building overlays.
        auto fn = g_origGetPal
            ? g_origGetPal
            : reinterpret_cast<wxl::offsets::engine::gx::TextureCacheGetPalFn>(
                wxl::offsets::engine::gx::kTextureCacheGetPal);
        __try { return fn(tex); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    uint8_t* SafeGetMip(void* tex, uint32_t mip) noexcept
    {
        auto fn = g_origGetMip
            ? g_origGetMip
            : reinterpret_cast<wxl::offsets::engine::gx::TextureCacheGetMipFn>(
                wxl::offsets::engine::gx::kTextureCacheGetMip);
        __try { return fn(tex, mip); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void FlushTexTintState(const char* reason)
    {
        size_t cacheN = 0;
        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            cacheN = g_texTint.size();
            // NEVER memcpy orig back through cached void* here.
            // On logout/leave-world those handles are often freed and the addresses
            // reused (sky, skin, other BLPs). Writing "palette orig" into the wrong
            // object permanently corrupts the shared TextureCache until client restart
            // — matches "other PCs + background exploded, still broken when I log them".
            // Live paste path already restores while the pointer is still valid.
            g_texTint.clear();
            g_texNames.clear();
            // Keep g_pathOrig — pure pixel bytes, not engine pointers.
        }
        (void)reason;
        (void)cacheN;
    }

    void InvalidatePathOrigForUiReload()
    {
        std::lock_guard<std::mutex> lock(g_texMutex);
        for (auto it = g_pathOrig.begin(); it != g_pathOrig.end(); )
        {
            if (IsItemComponentTexture(it->second.path.c_str()))
                it = g_pathOrig.erase(it);
            else
                ++it;
        }
    }

    void OnUiReload()
    {
        FlushTexTintState("ui_reload");
        InvalidatePathOrigForUiReload();
        ForceAllowedBodyRebuild();
    }

    std::string TexPathKey(const char* name)
    {
        std::string s = name ? name : "";
        for (char& c : s)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool WriteCacheOrigToTex(void* tex, const TexTintCache& st)
    {
        if (!tex || st.orig.empty())
            return false;
        if (st.paletted)
        {
            uint8_t* pal = SafeGetPal(tex);
            if (!pal || st.orig.size() < 256 * 4)
                return false;
            std::memcpy(pal, st.orig.data(), 256 * 4);
            return true;
        }
        uint8_t* mip0 = SafeGetMip(tex, 0);
        if (!mip0 || st.orig.size() < static_cast<size_t>(st.w) * st.h * 4u)
            return false;
        std::memcpy(mip0, st.orig.data(), st.orig.size());
        return true;
    }

    // Read-only: fill g_pathOrig from tex on first sight. Never writes back to the
    // shared TextureCache — overlay tinting reads from g_pathOrig only.
    bool CapturePathOrigForOverlay(void* tex, const char* name)
    {
        if (!tex || !name || !name[0])
            return false;
        const std::string key = TexPathKey(name);
        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            auto pit = g_pathOrig.find(key);
            if (pit != g_pathOrig.end() && !pit->second.orig.empty())
            {
                g_texTint[tex] = pit->second;
                return true;
            }
        }

        uint8_t* pal = SafeGetPal(tex);
        TexTintCache neu{};
        neu.path = name;
        if (pal)
        {
            uint8_t info[8] = {};
            uint32_t w = 0, h = 0;
            if (SafeGetInfo(tex, info, 1))
            {
                w = *reinterpret_cast<uint16_t*>(info + 0);
                h = *reinterpret_cast<uint16_t*>(info + 2);
            }
            uint8_t* mip0 = SafeGetMip(tex, 0);
            neu.paletted = true;
            neu.w = w;
            neu.h = h;
            neu.orig.assign(pal, pal + 256 * 4);
            if (mip0 && w && h && w <= 2048 && h <= 2048)
                neu.origIdx.assign(mip0, mip0 + static_cast<size_t>(w) * h);
        }
        else
        {
            uint8_t info[8] = {};
            if (!SafeGetInfo(tex, info, 1))
                return false;
            const uint32_t w = *reinterpret_cast<uint16_t*>(info + 0);
            const uint32_t h = *reinterpret_cast<uint16_t*>(info + 2);
            uint8_t* mip0 = SafeGetMip(tex, 0);
            if (!mip0 || !w || !h || w > 2048 || h > 2048)
                return false;
            neu.paletted = false;
            neu.w = w;
            neu.h = h;
            neu.orig.assign(mip0, mip0 + static_cast<size_t>(w) * h * 4u);
        }
        if (neu.orig.empty())
            return false;
        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            auto& slot = g_pathOrig[key];
            if (slot.orig.empty())
                slot = neu;
            g_texTint[tex] = slot;
        }
        return true;
    }

    // Capture clean pixels into g_pathOrig (once per path) and mirror onto g_texTint[tex].
    // If path already known, ALWAYS rewrite tex from canonical (scrub poison / foreign tint).
    bool EnsurePathOrig(void* tex, const char* name)
    {
        if (!tex || !name || !name[0])
            return false;
        const std::string key = TexPathKey(name);

        TexTintCache known{};
        bool haveKnown = false;
        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            auto pit = g_pathOrig.find(key);
            if (pit != g_pathOrig.end() && !pit->second.orig.empty())
            {
                known = pit->second;
                haveKnown = true;
            }
        }
        if (haveKnown)
        {
            if (!WriteCacheOrigToTex(tex, known))
                return false;
            std::lock_guard<std::mutex> lock(g_texMutex);
            g_texTint[tex] = known;
            return true;
        }

        // First sight of this path — capture from engine (should be clean BLP decode).
        uint8_t* pal = SafeGetPal(tex);
        TexTintCache neu{};
        neu.path = name;
        if (pal)
        {
            uint8_t info[8] = {};
            uint32_t w = 0, h = 0;
            if (SafeGetInfo(tex, info, 1))
            {
                w = *reinterpret_cast<uint16_t*>(info + 0);
                h = *reinterpret_cast<uint16_t*>(info + 2);
            }
            uint8_t* mip0 = SafeGetMip(tex, 0);
            neu.paletted = true;
            neu.w = w;
            neu.h = h;
            neu.orig.assign(pal, pal + 256 * 4);
            if (mip0 && w && h && w <= 2048 && h <= 2048)
                neu.origIdx.assign(mip0, mip0 + static_cast<size_t>(w) * h);
        }
        else
        {
            uint8_t info[8] = {};
            if (!SafeGetInfo(tex, info, 1))
                return false;
            const uint32_t w = *reinterpret_cast<uint16_t*>(info + 0);
            const uint32_t h = *reinterpret_cast<uint16_t*>(info + 2);
            uint8_t* mip0 = SafeGetMip(tex, 0);
            if (!mip0 || !w || !h || w > 2048 || h > 2048)
                return false;
            neu.paletted = false;
            neu.w = w;
            neu.h = h;
            neu.orig.assign(mip0, mip0 + static_cast<size_t>(w) * h * 4u);
        }
        if (neu.orig.empty())
            return false;
        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            // Another thread may have filled it — keep first writer.
            auto& slot = g_pathOrig[key];
            if (slot.orig.empty())
                slot = neu;
            g_texTint[tex] = slot;
        }
        return true;
    }

    int SafeGetInfo(void* tex, void* info, int flag) noexcept
    {
        auto fn = reinterpret_cast<wxl::offsets::engine::gx::TextureCacheGetInfoFn>(
            wxl::offsets::engine::gx::kTextureCacheGetInfo);
        __try { return fn(tex, info, flag); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    void StoreCatchSample(int slot, CatchSample&& sample)
    {
        if (slot < 0 || slot >= kMaxEquipSlots || sample.pixels.empty())
            return;
        sample.tick = GetTickCount();
        std::lock_guard<std::mutex> lock(g_catchMu);
        g_catchSample[slot] = std::move(sample);
    }

    // Capture while TextureCache CPU data is still mapped (during CharComponent paste).
    void CaptureCatchFromTextureCache(void* tex, int slot, const char* name)
    {
        if (!tex || slot < 0 || slot >= kMaxEquipSlots)
            return;
        uint8_t* pal = SafeGetPal(tex);
        uint8_t info[8] = {};
        uint32_t w = 0, h = 0;
        if (SafeGetInfo(tex, info, 1))
        {
            w = *reinterpret_cast<uint16_t*>(info + 0);
            h = *reinterpret_cast<uint16_t*>(info + 2);
        }
        uint8_t* mip0 = SafeGetMip(tex, 0);
        CatchSample s{};
        if (name)
            s.path = name;
        if (pal)
        {
            s.paletted = true;
            s.w = w;
            s.h = h;
            s.pixels.assign(pal, pal + 256 * 4);
            // Optional: keep usage-weighted idx by expanding a few samples into pixels later.
            // Palette alone is enough for distant-color Catch.
            (void)mip0;
        }
        else if (mip0 && w && h && w <= 2048 && h <= 2048)
        {
            s.paletted = false;
            s.w = w;
            s.h = h;
            const size_t n = static_cast<size_t>(w) * h;
            const size_t step = (n > 65536) ? (n / 65536) : 1;
            s.pixels.reserve((n / step) * 4);
            for (size_t i = 0; i < n; i += step)
            {
                const uint8_t* p = mip0 + i * 4;
                s.pixels.push_back(p[0]);
                s.pixels.push_back(p[1]);
                s.pixels.push_back(p[2]);
                s.pixels.push_back(p[3] ? p[3] : 255);
            }
            s.w = static_cast<uint32_t>(s.pixels.size() / 4);
            s.h = 1;
        }
        else
            return;
        StoreCatchSample(slot, std::move(s));
    }

    // OC / TextureCreate path: pixels live in kMipTablePtr only during OnTextureUpload
    // (emitted after native upload, before mip table clear).
    void CaptureCatchFromMipTable(int slot, const char* name, uint32_t width, uint32_t height)
    {
        if (slot < 0 || slot >= kMaxEquipSlots || !width || !height)
            return;
        if (width > 2048 || height > 2048)
            return;
        namespace gxoff = wxl::offsets::engine::gx;
        if (!*reinterpret_cast<uint32_t*>(gxoff::kMipTableValid))
            return;
        auto** tableHolder = reinterpret_cast<uint32_t**>(gxoff::kMipTablePtr);
        if (!tableHolder || !*tableHolder)
            return;
        uint32_t* table = *tableHolder;
        auto* pixels = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(table[0]));
        if (!pixels)
            return;
        CatchSample s{};
        if (name)
            s.path = name;
        s.paletted = false;
        const size_t n = static_cast<size_t>(width) * height;
        const size_t step = (n > 65536) ? (n / 65536) : 1;
        s.pixels.reserve((n / step) * 4);
        for (size_t i = 0; i < n; i += step)
        {
            const uint8_t* p = pixels + i * 4;
            s.pixels.push_back(p[0]);
            s.pixels.push_back(p[1]);
            s.pixels.push_back(p[2]);
            s.pixels.push_back(p[3] ? p[3] : 255);
        }
        s.w = static_cast<uint32_t>(s.pixels.size() / 4);
        s.h = 1;
        StoreCatchSample(slot, std::move(s));
    }

    // OC Catch: StretchRect cannot copy DXT albedos. Queue bound texture at
    // phase 0; at EndScene blit via textured quad into an RT, then read back.
    struct OcCatchPending
    {
        IDirect3DTexture9* tex = nullptr;
        int slot = -1;
        std::string path;
    };
    std::mutex g_ocCatchPendMu;
    std::vector<OcCatchPending> g_ocCatchPend;

    bool StoreCatchFromLockedRect(int slot, const char* path, const D3DLOCKED_RECT& lr,
                                  UINT w, UINT h)
    {
        if (!lr.pBits || !w || !h || lr.Pitch <= 0 || slot < 0 || slot >= kMaxEquipSlots)
            return false;
        CatchSample s{};
        if (path)
            s.path = path;
        s.paletted = false;
        const size_t n = static_cast<size_t>(w) * h;
        const size_t step = (n > 65536) ? (n / 65536) : 1;
        s.pixels.reserve((n / step) * 4);
        const auto* rows = static_cast<const uint8_t*>(lr.pBits);
        for (size_t i = 0; i < n; i += step)
        {
            const size_t y = i / w;
            const size_t x = i % w;
            const uint8_t* p = rows + y * static_cast<size_t>(lr.Pitch) + x * 4u;
            s.pixels.push_back(p[0]);
            s.pixels.push_back(p[1]);
            s.pixels.push_back(p[2]);
            s.pixels.push_back(p[3] ? p[3] : 255);
        }
        s.w = static_cast<uint32_t>(s.pixels.size() / 4);
        s.h = 1;
        StoreCatchSample(slot, std::move(s));
        return true;
    }

    bool ReadbackTextureToCatch(IDirect3DDevice9* dev, IDirect3DTexture9* tex,
                                int slot, const char* path)
    {
        if (!dev || !tex || slot < 0 || slot >= kMaxEquipSlots)
            return false;

        D3DSURFACE_DESC desc{};
        if (FAILED(tex->GetLevelDesc(0, &desc)) || !desc.Width || !desc.Height
            || desc.Width > 2048 || desc.Height > 2048)
            return false;


        if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8)
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
            {
                const bool ok = StoreCatchFromLockedRect(slot, path, lr, desc.Width, desc.Height);
                tex->UnlockRect(0);
                if (ok)
                    return true;
            }
        }

        // StretchRect cannot copy DXT→ARGB on this device (INVALIDCALL). Draw a
        // textured screen-space quad into an RT, then GetRenderTargetData.
        IDirect3DTexture9* rtTex = nullptr;
        HRESULT hr = dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtTex, nullptr);
        if (FAILED(hr) || !rtTex)
        {
            return false;
        }
        IDirect3DSurface9* rtSurf = nullptr;
        if (FAILED(rtTex->GetSurfaceLevel(0, &rtSurf)) || !rtSurf)
        {
            rtTex->Release();
            return false;
        }

        IDirect3DSurface9* oldRt = nullptr;
        IDirect3DSurface9* oldDepth = nullptr;
        dev->GetRenderTarget(0, &oldRt);
        dev->GetDepthStencilSurface(&oldDepth);

        IDirect3DStateBlock9* sb = nullptr;
        if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) && sb)
            sb->Capture();

        bool blitOk = false;
        if (SUCCEEDED(dev->SetRenderTarget(0, rtSurf)))
        {
            dev->SetDepthStencilSurface(nullptr);
            const D3DVIEWPORT9 vp{ 0, 0, desc.Width, desc.Height, 0.f, 1.f };
            dev->SetViewport(&vp);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.f, 0);

            dev->SetPixelShader(nullptr);
            dev->SetVertexShader(nullptr);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE,
                D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN
                | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
            dev->SetTexture(0, tex);
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

            struct BlitVtx { float x, y, z, rhw, u, v; };
            const float r = static_cast<float>(desc.Width) - 0.5f;
            const float b = static_cast<float>(desc.Height) - 0.5f;
            const BlitVtx quad[4] = {
                { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f },
                { r,     -0.5f, 0.f, 1.f, 1.f, 0.f },
                { -0.5f, b,     0.f, 1.f, 0.f, 1.f },
                { r,     b,     0.f, 1.f, 1.f, 1.f },
            };
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
            hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(BlitVtx));
            blitOk = SUCCEEDED(hr);
        }

        if (sb)
        {
            sb->Apply();
            sb->Release();
        }
        if (oldRt)
        {
            dev->SetRenderTarget(0, oldRt);
            oldRt->Release();
        }
        if (oldDepth)
        {
            dev->SetDepthStencilSurface(oldDepth);
            oldDepth->Release();
        }

        if (!blitOk)
        {
            rtSurf->Release();
            rtTex->Release();
            return false;
        }

        IDirect3DSurface9* staging = nullptr;
        hr = dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &staging, nullptr);
        if (FAILED(hr) || !staging)
        {
            rtSurf->Release();
            rtTex->Release();
            return false;
        }
        hr = dev->GetRenderTargetData(rtSurf, staging);
        rtSurf->Release();
        rtTex->Release();
        if (FAILED(hr))
        {
            staging->Release();
            return false;
        }

        D3DLOCKED_RECT lr{};
        if (FAILED(staging->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
        {
            staging->Release();
            return false;
        }
        const bool ok = StoreCatchFromLockedRect(slot, path, lr, desc.Width, desc.Height);
        staging->UnlockRect();
        staging->Release();
        return ok;
    }

    void FlushOcCatchPending(IDirect3DDevice9* dev)
    {
        if (!dev)
            return;
        std::vector<OcCatchPending> local;
        {
            std::lock_guard<std::mutex> lock(g_ocCatchPendMu);
            local.swap(g_ocCatchPend);
        }
        for (OcCatchPending& p : local)
        {
            if (p.tex)
            {
                ReadbackTextureToCatch(dev, p.tex, p.slot, p.path.empty() ? nullptr : p.path.c_str());
                p.tex->Release();
                p.tex = nullptr;
            }
        }
    }

    void QueueOcCatchFromDevice(IDirect3DDevice9* dev, int slot, const char* path)
    {
        if (!dev || slot < 0 || slot >= kMaxEquipSlots)
            return;
        {
            std::lock_guard<std::mutex> lock(g_catchMu);
            const CatchSample& cur = g_catchSample[slot];
            if (!cur.pixels.empty() && path && !cur.path.empty() && cur.path == path)
                return;
        }

        IDirect3DBaseTexture9* base = nullptr;
        if (FAILED(dev->GetTexture(0, &base)) || !base)
            return;

        IDirect3DTexture9* tex = nullptr;
        if (FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9),
                reinterpret_cast<void**>(&tex))) || !tex)
        {
            base->Release();
            return;
        }
        base->Release();

        D3DSURFACE_DESC desc{};
        if (FAILED(tex->GetLevelDesc(0, &desc)) || !desc.Width || !desc.Height
            || desc.Width > 2048 || desc.Height > 2048)
        {
            tex->Release();
            return;
        }

        // Try immediate lock (MANAGED); else defer to EndScene.
        if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8)
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
            {
                const bool ok = StoreCatchFromLockedRect(slot, path, lr, desc.Width, desc.Height);
                tex->UnlockRect(0);
                if (ok)
                {
                    tex->Release();
                    return;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_ocCatchPendMu);
            for (OcCatchPending& p : g_ocCatchPend)
            {
                if (p.slot == slot)
                {
                    if (p.tex)
                        p.tex->Release();
                    p.tex = tex;
                    p.path = path ? path : "";
                    return;
                }
            }
            if (g_ocCatchPend.size() >= 12)
            {
                OcCatchPending old = std::move(g_ocCatchPend.front());
                g_ocCatchPend.erase(g_ocCatchPend.begin());
                if (old.tex)
                    old.tex->Release();
            }
            OcCatchPending neu{};
            neu.tex = tex;
            neu.slot = slot;
            neu.path = path ? path : "";
            g_ocCatchPend.push_back(std::move(neu));
        }
    }

    void* LoadTextureCacheByPath(const char* path) noexcept
    {
        if (!path || !path[0])
            return nullptr;
        auto fn = reinterpret_cast<wxl::offsets::engine::gx::TextureCacheCreateFn>(
            wxl::offsets::engine::gx::kTextureCacheCreate);
        __try { return fn(path); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void HslBgraBuffer(uint8_t* pixels, size_t count, const SlotHsl& hsl, bool skipLowAlpha,
                       uint32_t width = 0, uint32_t height = 0)
    {
        if (!pixels || !count || !hsl.active || IsIdentityHsl(hsl))
            return;

        // Selective on a real 2D mip: weight → despeckle → apply (kills isolated pixels).
        const bool canDespeckle = (hsl.mode == 1) && width >= 3 && height >= 3
            && (static_cast<size_t>(width) * static_cast<size_t>(height) == count);
        if (canDespeckle)
        {
            std::vector<SelRule> rules;
            if (hsl.ruleCount == 0)
            {
                SelRule one{};
                one.sr = hsl.srcR; one.sg = hsl.srcG; one.sb = hsl.srcB;
                one.dr = hsl.hue; one.dg = hsl.sat; one.db = hsl.light;
                one.tol = hsl.tolerance;
                rules.push_back(one);
            }
            else
            {
                for (uint8_t ri = 0; ri < hsl.ruleCount; ++ri)
                    rules.push_back(hsl.rules[ri]);
            }

            std::vector<float> weights(count);
            for (const SelRule& rule : rules)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    uint8_t* p = pixels + i * 4;
                    const uint8_t a = p[3];
                    if (skipLowAlpha && a < 8)
                    {
                        weights[i] = 0.f;
                        continue;
                    }
                    if (p[0] < 8 && p[1] < 8 && p[2] < 8)
                    {
                        weights[i] = 0.f;
                        continue;
                    }
                    const float r = p[2] * (1.f / 255.f);
                    const float g = p[1] * (1.f / 255.f);
                    const float b = p[0] * (1.f / 255.f);
                    weights[i] = SelectiveWeight(r, g, b, rule);
                }
                DespeckleWeights(weights.data(), width, height);
                // Second pass tightens leftover speckles
                DespeckleWeights(weights.data(), width, height);

                for (size_t i = 0; i < count; ++i)
                {
                    if (weights[i] <= 1e-4f)
                        continue;
                    uint8_t* p = pixels + i * 4;
                    const uint8_t a = p[3];
                    float r = p[2] * (1.f / 255.f);
                    float g = p[1] * (1.f / 255.f);
                    float b = p[0] * (1.f / 255.f);
                    ApplySelectiveWithWeight(r, g, b, rule, weights[i]);
                    p[0] = static_cast<uint8_t>(b * 255.f + 0.5f);
                    p[1] = static_cast<uint8_t>(g * 255.f + 0.5f);
                    p[2] = static_cast<uint8_t>(r * 255.f + 0.5f);
                    p[3] = a;
                }
            }
            return;
        }

        for (size_t i = 0; i < count; ++i)
        {
            uint8_t* p = pixels + i * 4;
            const uint8_t a = p[3];
            // BLP palettes are often BGRX with A=0 (unused). Never skip on alpha alone
            // or body TextureComponents never tint (changed:0 in logs).
            if (skipLowAlpha && a < 8)
                continue;
            // Keep near-black punch-through keys.
            if (p[0] < 8 && p[1] < 8 && p[2] < 8)
                continue;
            float r = p[2] * (1.f / 255.f);
            float g = p[1] * (1.f / 255.f);
            float b = p[0] * (1.f / 255.f);
            ApplyHslPixel(r, g, b, hsl);
            p[0] = static_cast<uint8_t>(b * 255.f + 0.5f);
            p[1] = static_cast<uint8_t>(g * 255.f + 0.5f);
            p[2] = static_cast<uint8_t>(r * 255.f + 0.5f);
            p[3] = a;
        }
    }

    bool HslMip0Bgra(uint32_t width, uint32_t height, const SlotHsl& hsl)
    {
        namespace gxoff = wxl::offsets::engine::gx;
        if (!hsl.active || IsIdentityHsl(hsl))
            return false;
        if (!width || !height || width > 2048 || height > 2048)
            return false;
        if (!*reinterpret_cast<uint32_t*>(gxoff::kMipTableValid))
            return false;
        auto** tableHolder = reinterpret_cast<uint32_t**>(gxoff::kMipTablePtr);
        if (!tableHolder || !*tableHolder)
            return false;
        uint32_t* table = *tableHolder;
        auto* pixels = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(table[0]));
        if (!pixels)
            return false;
        HslBgraBuffer(pixels, static_cast<size_t>(width) * static_cast<size_t>(height),
            hsl, true, width, height);
        return true;
    }

    void FillMipIndicesFromMip0(void* tex, uint32_t w0, uint32_t h0, const uint8_t* idx0)
    {
        if (!tex || !idx0 || !w0 || !h0)
            return;
        for (uint32_t mip = 1; mip < 16; ++mip)
        {
            uint8_t* dst = SafeGetMip(tex, mip);
            if (!dst)
                break;
            const uint32_t mw = (w0 >> mip) ? (w0 >> mip) : 1u;
            const uint32_t mh = (h0 >> mip) ? (h0 >> mip) : 1u;
            for (uint32_t y = 0; y < mh; ++y)
            {
                const uint32_t sy = (y << mip);
                const uint32_t syClamped = sy < h0 ? sy : (h0 - 1);
                for (uint32_t x = 0; x < mw; ++x)
                {
                    const uint32_t sx = (x << mip);
                    const uint32_t sxClamped = sx < w0 ? sx : (w0 - 1);
                    dst[y * mw + x] = idx0[syClamped * w0 + sxClamped];
                }
            }
        }
    }

    // Build a private tinted view and arm TLS so GetPal/GetMip feed native paste
    // without mutating the shared TextureCache.
    bool ArmPasteTintOverlay(void* tex, const char* name, const SlotHsl& hsl)
    {
        ClearPasteTintOverlay();
        if (!tex || !hsl.active || IsIdentityHsl(hsl))
            return false;

        TexTintCache src{};
        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            if (name && name[0])
            {
                auto pit = g_pathOrig.find(TexPathKey(name));
                if (pit != g_pathOrig.end() && !pit->second.orig.empty())
                    src = pit->second;
            }
            if (src.orig.empty())
            {
                auto it = g_texTint.find(tex);
                if (it != g_texTint.end() && !it->second.orig.empty())
                    src = it->second;
            }
        }
        if (src.orig.empty())
            return false;

        if (src.paletted)
        {
            if (src.orig.size() < 256 * 4)
                return false;
            std::memcpy(g_tlsPasteOverlay.pal, src.orig.data(), 256 * 4);
            if (hsl.mode == 1)
            {
                // Reuse in-place selective path on the private palette copy.
                uint8_t* pal = g_tlsPasteOverlay.pal;
                for (int i = 0; i < 256; ++i)
                {
                    float r = pal[i * 4 + 2] * (1.f / 255.f);
                    float g = pal[i * 4 + 1] * (1.f / 255.f);
                    float b = pal[i * 4 + 0] * (1.f / 255.f);
                    float wmax = 0.f;
                    if (hsl.ruleCount == 0)
                    {
                        SelRule one{};
                        one.sr = hsl.srcR; one.sg = hsl.srcG; one.sb = hsl.srcB;
                        one.dr = hsl.hue; one.dg = hsl.sat; one.db = hsl.light;
                        one.tol = hsl.tolerance;
                        wmax = SelectiveWeight(r, g, b, one);
                        if (wmax < 0.45f)
                            continue;
                        ApplySelectiveWithWeight(r, g, b, one, wmax);
                    }
                    else
                    {
                        for (uint8_t ri = 0; ri < hsl.ruleCount; ++ri)
                            wmax = (std::max)(wmax, SelectiveWeight(r, g, b, hsl.rules[ri]));
                        if (wmax < 0.45f)
                            continue;
                        for (uint8_t ri = 0; ri < hsl.ruleCount; ++ri)
                        {
                            const float wi = SelectiveWeight(r, g, b, hsl.rules[ri]);
                            if (wi >= 0.45f)
                                ApplySelectiveWithWeight(r, g, b, hsl.rules[ri], wi);
                        }
                    }
                    pal[i * 4 + 2] = static_cast<uint8_t>(Clamp01(r) * 255.f + 0.5f);
                    pal[i * 4 + 1] = static_cast<uint8_t>(Clamp01(g) * 255.f + 0.5f);
                    pal[i * 4 + 0] = static_cast<uint8_t>(Clamp01(b) * 255.f + 0.5f);
                }
            }
            else
            {
                HslBgraBuffer(g_tlsPasteOverlay.pal, 256, hsl, false);
            }
            g_tlsPasteOverlay.paletted = true;
            g_tlsPasteOverlay.w = src.w;
            g_tlsPasteOverlay.h = src.h;
            g_tlsPasteOverlay.tex = tex;
            g_tlsPasteOverlay.active = true;
            return true;
        }

        const size_t bytes = src.orig.size();
        if (bytes < 4 || !src.w || !src.h)
            return false;
        g_tlsPasteOverlay.bgra = src.orig;
        HslBgraBuffer(g_tlsPasteOverlay.bgra.data(),
            static_cast<size_t>(src.w) * static_cast<size_t>(src.h),
            hsl, true, src.w, src.h);
        g_tlsPasteOverlay.paletted = false;
        g_tlsPasteOverlay.w = src.w;
        g_tlsPasteOverlay.h = src.h;
        g_tlsPasteOverlay.tex = tex;
        g_tlsPasteOverlay.active = true;
        return true;
    }


    bool RestoreTextureCacheOrig(void* tex, const char* name = nullptr)
    {
        if (!tex)
            return false;
        TexTintCache st{};
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            auto it = g_texTint.find(tex);
            if (it != g_texTint.end() && !it->second.orig.empty())
            {
                st = it->second;
                have = true;
            }
            else if (name && name[0])
            {
                auto pit = g_pathOrig.find(TexPathKey(name));
                if (pit != g_pathOrig.end() && !pit->second.orig.empty())
                {
                    st = pit->second;
                    g_texTint[tex] = st;
                    have = true;
                }
            }
        }
        if (!have)
            return false;
        return WriteCacheOrigToTex(tex, st);
    }


    bool SafeCallPaste(wxl::offsets::engine::gx::CharPasteToSectionFn fn,
                       int section, void* src, void* dst) noexcept
    {
        if (!fn)
            return false;
        __try
        {
            fn(section, src, dst);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }


    void __cdecl hkPasteSkinLayout(int section, void* srcTexture, void* dstMips)
    {
        // 0x4F07D0: base skin laid into every body section — NEVER tint here.
        std::lock_guard<std::recursive_mutex> gate(g_pasteGateMu);
        if (g_origPasteToSection)
            SafeCallPaste(g_origPasteToSection, section, srcTexture, dstMips);
    }

    void __cdecl hkPasteToSection(int section, void* srcTexture, void* dstMips)
    {
        // 0x4F08A0: item TextureComponents pasted into one composite layer.
        std::lock_guard<std::recursive_mutex> gate(g_pasteGateMu);
        bool allow = IsPasteTintAllowed();
        OwnerGuid effectiveOwner = CanonicalTintOwner(
            g_prepOwnerGuid.load(std::memory_order_relaxed));
        if (srcTexture)
        {
            const char* name = TextureName(srcTexture);
            if (IsItemComponentTexture(name))
            {
                SlotHsl hsl{};
                int slot = -1;
                bool hasHsl = TryHslForComponentTexture(name, section, hsl, &slot);
                bool orphanTint = false;
                // Orphan never uses self. Most remote pastes have prepOwner=0 — orphan
                // is required for observer tint. Suppress only during deferred first
                // assemble (g_suppressOrphanTint) so face/skin stay clean.
                if (!hasHsl
                    && !g_suppressOrphanTint.load(std::memory_order_relaxed))
                {
                    OwnerGuid found = 0;
                    SlotHsl foundHsl{};
                    int foundSlot = -1;
                    if (ResolveOrphanPasteOwner(name, section, found, foundHsl, foundSlot)
                        && (effectiveOwner == 0 || GuidSamePlayer(effectiveOwner, found)))
                    {
                        effectiveOwner = found;
                        hsl = foundHsl;
                        slot = foundSlot;
                        hasHsl = true;
                        orphanTint = !allow;
                        if (orphanTint)
                            allow = true;
                    }
                }
                else if (hasHsl && effectiveOwner == 0)
                {
                    effectiveOwner = CanonicalTintOwner(
                        g_prepOwnerGuid.load(std::memory_order_relaxed));
                    if (!effectiveOwner)
                    {
                        // TryHsl resolved via local prepModel==player without prepOwner.
                        const OwnerGuid selfGuid = ActiveOwnerGuid();
                        if (selfGuid)
                            effectiveOwner = selfGuid;
                    }
                }

                const OwnerGuid self = ActiveOwnerGuid();
                if (!g_suppressOrphanTint.load(std::memory_order_relaxed)
                    && hasHsl && !allow && effectiveOwner != 0
                    && !(self && GuidSamePlayer(effectiveOwner, self)))
                {
                    // Remote tint matched but IsPasteTintAllowed failed — same
                    // force path self gets via assembling RenderPrep. Blocked
                    // during deferred first-assemble via g_suppressOrphanTint.
                    orphanTint = true;
                    allow = true;
                }

                const uint32_t equipEntry = (slot >= 0)
                    ? EquipSnapEntry(effectiveOwner, slot) : 0;
                const int equipKnown = EquipSnapKnown(effectiveOwner) ? 1 : 0;
                const bool isSelf = self != 0 && GuidSamePlayer(effectiveOwner, self);

                // Same gate for everyone: need owner + hsl. Self still blocks known-empty
                // slots (phantoms). Remotes may tint when entry snap lagged behind PUSH.
                const bool emptySelf = isSelf && slot >= 0 && equipKnown && equipEntry == 0;
                const bool doTint = allow && hasHsl && slot >= 0 && effectiveOwner != 0
                    && !emptySelf;
                bool pathOk = false;

                // #region agent log
                if (!isSelf && g_dbgPasteRemoteN.fetch_add(1, std::memory_order_relaxed) < 80)
                {
                    char stem[kStemCap] = {};
                    DbgTexBasename615(name, stem, sizeof(stem));
                    const int suspicious = (ContainsCI(name, "head") || ContainsCI(name, "face")
                        || ContainsCI(name, "hair") || ContainsCI(name, "skin")
                        || section <= 2) ? 1 : 0;
                    const char* msg = doTint ? "remote_tint"
                        : (hasHsl ? "remote_skip" : "remote_no_hsl");
                    if (orphanTint && doTint)
                        msg = "orphan_tint";
                    else if (orphanTint && hasHsl && !doTint)
                        msg = "orphan_skip";
                    DbgLog615(doTint ? "H1" : (hasHsl ? "H4" : "H2"),
                        "hkPasteToSection", msg,
                        static_cast<unsigned long long>(effectiveOwner), slot, section, stem,
                        static_cast<int>(equipEntry), suspicious,
                        reinterpret_cast<uintptr_t>(srcTexture));
                }
                // #endregion

                if (doTint)
                {
                    struct OrphanPasteScope
                    {
                        bool active = false;
                        OwnerGuid prevOwner = 0;
                        void* prevModel = nullptr;
                        bool prevAssembling = false;
                        bool prevForce = false;
                        OwnerGuid prevForceOwner = 0;

                        void Enter(OwnerGuid owner)
                        {
                            if (!owner)
                                return;
                            prevOwner = g_prepOwnerGuid.exchange(owner, std::memory_order_relaxed);
                            prevModel = g_prepModel.exchange(
                                OwnerBodyModel(owner), std::memory_order_relaxed);
                            prevAssembling = g_assemblingAllowed.exchange(
                                true, std::memory_order_relaxed);
                            prevForce = g_forceAllowPaste.exchange(
                                true, std::memory_order_relaxed);
                            prevForceOwner = g_forceOwnerGuid.exchange(
                                owner, std::memory_order_relaxed);
                            active = true;
                        }

                        ~OrphanPasteScope()
                        {
                            if (!active)
                                return;
                            g_prepOwnerGuid.store(prevOwner, std::memory_order_relaxed);
                            g_prepModel.store(prevModel, std::memory_order_relaxed);
                            g_assemblingAllowed.store(prevAssembling, std::memory_order_relaxed);
                            g_forceAllowPaste.store(prevForce, std::memory_order_relaxed);
                            g_forceOwnerGuid.store(prevForceOwner, std::memory_order_relaxed);
                        }
                    } orphanScope;
                    if (orphanTint)
                        orphanScope.Enter(effectiveOwner);

                    pathOk = CapturePathOrigForOverlay(srcTexture, name);
                    const bool ok = pathOk && ArmPasteTintOverlay(srcTexture, name, hsl);
                    // #region agent log
                    if (!isSelf && ok
                        && g_dbgPasteRemoteN.load(std::memory_order_relaxed) < 100)
                    {
                        char stem[kStemCap] = {};
                        DbgTexBasename615(name, stem, sizeof(stem));
                        DbgLog615("H5", "hkPasteToSection", "overlay_ok",
                            static_cast<unsigned long long>(effectiveOwner), slot, section, stem,
                            pathOk ? 1 : 0, hsl.mode,
                            reinterpret_cast<uintptr_t>(srcTexture));
                    }
                    // #endregion
                    if (ok)
                    {
                        SafeCallPaste(g_origPasteFromSkin, section, srcTexture, dstMips);
                        ClearPasteTintOverlay();
                        if (allow)
                        {
                            void* prep = g_prepModel.load(std::memory_order_relaxed);
                            void* pm = LocalPlayerBodyModel();
                            const bool playerCatch = (pm && prep == pm) || (!pm);
                            if (playerCatch)
                                CaptureCatchFromTextureCache(srcTexture, slot, name);
                        }
                        return;
                    }
                }
            }
        }
        if (!srcTexture || !dstMips)
        {
            if (g_origPasteFromSkin)
                SafeCallPaste(g_origPasteFromSkin, section, srcTexture, dstMips);
            return;
        }
        if (g_origPasteFromSkin)
            SafeCallPaste(g_origPasteFromSkin, section, srcTexture, dstMips);
    }

    void SetSlotHsl(int slot, float r, float g, float b)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        SlotHsl h{};
        h.active = true;
        h.mode = 0;
        h.stopCount = 1;
        h.gradFill = 0;
        h.hue = Clamp01(r);
        h.sat = Clamp01(g);
        h.light = Clamp01(b);
        h.stops[0][0] = h.hue;
        h.stops[0][1] = h.sat;
        h.stops[0][2] = h.light;
        WriteLocalSlotHsl(slot, h);
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveLocalHslIfCommitted();
    }

    void ResetSlotTint(int slot)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        ClearDraftSlot(slot);
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = {};
        }
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveLocalHslIfCommitted();
    }

    // nStops: 2, 3, or 5.
    // colors layout: RGB triples packed flat (max 5*3).
    void SetSlotGradient(int slot, int nStops, int fill, const float* colors, int colorFloats)
    {
        if (slot < 0 || slot >= kMaxEquipSlots || !colors)
            return;
        if (nStops != 2 && nStops != 3 && nStops != 5)
            nStops = 3;
        SlotHsl h{};
        h.active = true;
        h.mode = 2;
        h.stopCount = static_cast<uint8_t>(nStops);
        h.gradFill = (fill != 0) ? 1 : 0;
        if (h.gradFill == 0)
        {
            if (colorFloats < 3)
                return;
            h.hue = Clamp01(colors[0]);
            h.sat = Clamp01(colors[1]);
            h.light = Clamp01(colors[2]);
        }
        else
        {
            if (colorFloats < nStops * 3)
                return;
            for (int s = 0; s < nStops; ++s)
            {
                h.stops[s][0] = Clamp01(colors[s * 3 + 0]);
                h.stops[s][1] = Clamp01(colors[s * 3 + 1]);
                h.stops[s][2] = Clamp01(colors[s * 3 + 2]);
            }
        }
        ResolveSolidStops(h);
        WriteLocalSlotHsl(slot, h);
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveLocalHslIfCommitted();
    }

    void SetSlotSelective(int slot, float sr, float sg, float sb,
                          float dr, float dg, float db, float tol,
                          bool forceAppend = false)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        SelRule neu{};
        neu.sr = Clamp01(sr);
        neu.sg = Clamp01(sg);
        neu.sb = Clamp01(sb);
        neu.dr = Clamp01(dr);
        neu.dg = Clamp01(dg);
        neu.db = Clamp01(db);
        neu.tol = Clamp01(tol);
        if (neu.tol < 0.02f)
            neu.tol = 0.35f;

        const char* action = "new";
        SlotHsl h{};
        ReadLocalSlotHsl(slot, h);
        // Switching from solid / empty → start a fresh selective chain.
        if (!h.active || h.mode != 1)
        {
            h = {};
            h.active = true;
            h.mode = 1;
            h.rules[0] = neu;
            h.ruleCount = 1;
            action = "new";
        }
        else if (!forceAppend && h.ruleCount > 0 &&
            SameSelectiveSrc(h.rules[h.ruleCount - 1], neu.sr, neu.sg, neu.sb))
        {
            h.rules[h.ruleCount - 1] = neu;
            action = "update_last";
        }
        else if (h.ruleCount < kMaxSelRules)
        {
            h.rules[h.ruleCount++] = neu;
            action = forceAppend ? "force_append" : "append";
        }
        else
        {
            for (int i = 0; i < kMaxSelRules - 1; ++i)
                h.rules[i] = h.rules[i + 1];
            h.rules[kMaxSelRules - 1] = neu;
            h.ruleCount = kMaxSelRules;
            action = "shift_append";
        }
        SyncSlotMirrorsFromLastRule(h);
        (void)action;
        WriteLocalSlotHsl(slot, h);
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveLocalHslIfCommitted();
    }

    void ClearSlotHsl(int slot)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        ClearDraftSlot(slot);
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = {};
        }
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveLocalHslIfCommitted();
    }

    void ClearAllHsl()
    {
        ClearAllDrafts();
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            for (int i = 0; i < kMaxEquipSlots; ++i)
                g_slotHsl[i] = {};
        }
        // Do NOT RequestBodyRebuildForSlot(-1) — full 0x3FF rebuild on login
        // (SetSelfGuid) re-pastes the entire body and bugs composites. Per-slot
        // ApplyLocalPayload / SetSlot rebuilds only the sections that need it.
        SaveLocalHslIfCommitted();
    }

    // Login PUSH often arrives before SetSelfGuid — ApplyOwnerTint stored them as
    // remotes. Local paste only reads g_slotHsl for self → invisible until
    // unequip/reequip. Pull matching remote rows into the local table.
    void PromoteRemoteTintsToLocal(OwnerGuid self)
    {
        if (!self)
            return;
        OwnerSlotHsl promoted{};
        bool any = false;
        {
            std::lock_guard<std::mutex> lock(g_remoteMu);
            for (auto it = g_remoteTints.begin(); it != g_remoteTints.end(); )
            {
                if (!GuidSamePlayer(it->first, self))
                {
                    ++it;
                    continue;
                }
                for (int s = 0; s < kMaxEquipSlots; ++s)
                {
                    const SlotHsl& h = it->second[static_cast<size_t>(s)];
                    if (h.active)
                    {
                        promoted[static_cast<size_t>(s)] = h;
                        any = true;
                    }
                }
                g_remoteCcModels.erase(it->first);
                g_remoteCcComponents.erase(it->first);
                it = g_remoteTints.erase(it);
            }
        }
        if (!any)
            return;
        uint32_t mask = 0;
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            for (int s = 0; s < kMaxEquipSlots; ++s)
            {
                if (!promoted[static_cast<size_t>(s)].active)
                    continue;
                g_slotHsl[s] = promoted[static_cast<size_t>(s)];
                g_draftHsl[s] = {};
                mask |= SectionMaskForEquipSlot(s);
            }
        }
        if (mask)
            DirtyOwnerSections(self, mask);
    }

    // Solid OC PS — single color (byte-stable lighting path).
    constexpr char kSolidPsHlsl[] = R"(
sampler2D s0 : register(s0);
float4 c0 : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 diff : COLOR0) : COLOR0
{
    float4 t = tex2D(s0, uv);
    float lum = dot(t.rgb, float3(0.299, 0.587, 0.114));
    float3 colored = saturate(lum * c0.rgb);
    float3 outRgb = saturate(colored * diff.rgb);
    return float4(outRgb, saturate(t.a * diff.a));
}
)";

    // Solid gradient OC PS: luminance samples up to 5 stop colors (c1..c5), count in c0.a.
    constexpr char kSolidGradPsHlsl[] = R"(
sampler2D s0 : register(s0);
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 c4 : register(c4);
float4 c5 : register(c5);

float3 stopAt(float idx)
{
    if (idx < 0.5) return c1.rgb;
    if (idx < 1.5) return c2.rgb;
    if (idx < 2.5) return c3.rgb;
    if (idx < 3.5) return c4.rgb;
    return c5.rgb;
}

float4 main(float2 uv : TEXCOORD0, float4 diff : COLOR0) : COLOR0
{
    float4 t = tex2D(s0, uv);
    float lum = saturate(dot(t.rgb, float3(0.299, 0.587, 0.114)));
    float n = max(2.0, min(5.0, c0.a));
    float x = lum * (n - 1.0);
    float i0 = floor(x);
    float f = saturate(x - i0);
    float3 a = stopAt(i0);
    float3 b = stopAt(min(i0 + 1.0, n - 1.0));
    float3 colored = saturate(lerp(a, b, f));
    float3 outRgb = saturate(colored * diff.rgb);
    return float4(outRgb, saturate(t.a * diff.a));
}
)";

    // Selective OC PS v10: lighting-robust chromaticity match (norm-RGB).
    // Evidence: v9 compiled+ran (usePs:1) but weapons looked unchanged — screen
    // eyedropper src includes lighting; absolute RGB dist never matched albedo.
    constexpr char kSelectivePsHlsl[] = R"(
sampler2D s0 : register(s0);
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 c4 : register(c4);
float4 c5 : register(c5);
float4 c6 : register(c6);
float4 c7 : register(c7);
float4 c8 : register(c8);

float3 applyRule(float3 c, float3 src, float3 dst, float tol)
{
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    if (mx < 0.04) return c;
    float sat = (mx - mn) / mx;
    if (sat < 0.05) return c;
    float smx = max(src.r, max(src.g, src.b));
    if (smx < 0.04) return c;
    float3 n = c / mx;
    float3 ns = src / smx;
    float dist = sqrt(dot(n - ns, n - ns));
    // Wide gate: eyedropper src is lit; norm-RGB still drifts.
    float maxd = 0.12 + saturate(tol) * 1.35;
    if (dist >= maxd) return c;
    float t = saturate(dist / maxd);
    float w = 1.0 - (t * t * (3.0 - 2.0 * t));
    float lum = dot(c, float3(0.299, 0.587, 0.114));
    float dl = dot(dst, float3(0.299, 0.587, 0.114));
    float3 repl = (dl > 0.001) ? saturate(dst * (lum / dl)) : saturate(dst * lum);
    return saturate(lerp(c, repl, w));
}

float4 main(float2 uv : TEXCOORD0, float4 diff : COLOR0) : COLOR0
{
    float4 t = tex2D(s0, uv);
    float3 colored = t.rgb;
    if (c0.a >= 0.5) colored = applyRule(colored, c1.rgb, c2.rgb, c1.a);
    if (c0.a >= 1.5) colored = applyRule(colored, c3.rgb, c4.rgb, c3.a);
    if (c0.a >= 2.5) colored = applyRule(colored, c5.rgb, c6.rgb, c5.a);
    float3 outRgb = saturate(colored * diff.rgb);
    return float4(outRgb, saturate(t.a * diff.a));
}
)";

    IDirect3DPixelShader9* EnsureSolidPs(void* deviceRaw)
    {
        static IDirect3DPixelShader9* ps = nullptr;
        static int compiledVer = 0;
        constexpr int kPsVer = 2; // locked to proven solid+diffuse
        if (compiledVer == kPsVer)
            return ps;
        compiledVer = kPsVer;
        if (ps)
        {
            ps->Release();
            ps = nullptr;
        }
        ps = static_cast<IDirect3DPixelShader9*>(
            gx::CompilePixelShader(gx::Device9(deviceRaw), kSolidPsHlsl, "ps_2_0"));
        g_hslPsState.store(ps ? 1 : 0, std::memory_order_relaxed);
        if (ps)
            WLOG_INFO("gear-recolor: solid OC PS v%d ready", kPsVer);
        else
            WLOG_WARN("gear-recolor: solid OC PS compile failed");
        return ps;
    }

    IDirect3DPixelShader9* EnsureSolidGradPs(void* deviceRaw)
    {
        static IDirect3DPixelShader9* ps = nullptr;
        static int compiledVer = 0;
        constexpr int kPsVer = 1;
        if (compiledVer == kPsVer)
            return ps;
        compiledVer = kPsVer;
        if (ps)
        {
            ps->Release();
            ps = nullptr;
        }
        ps = static_cast<IDirect3DPixelShader9*>(
            gx::CompilePixelShader(gx::Device9(deviceRaw), kSolidGradPsHlsl, "ps_2_0"));
        if (ps)
            WLOG_INFO("gear-recolor: solid gradient OC PS v%d ready", kPsVer);
        else
            WLOG_WARN("gear-recolor: solid gradient OC PS compile failed");
        return ps;
    }

    IDirect3DPixelShader9* EnsureSelectivePs(void* deviceRaw)
    {
        static IDirect3DPixelShader9* ps = nullptr;
        static int compiledVer = 0;
        constexpr int kPsVer = 10; // v10: norm-RGB match (eyedropper lighting-safe)
        if (compiledVer == kPsVer)
            return ps;
        compiledVer = kPsVer;
        if (ps)
        {
            ps->Release();
            ps = nullptr;
        }
        ps = static_cast<IDirect3DPixelShader9*>(
            gx::CompilePixelShader(gx::Device9(deviceRaw), kSelectivePsHlsl, "ps_2_0"));
        int profile = 0;
        if (ps)
            profile = 20;
        if (!ps)
        {
            ps = static_cast<IDirect3DPixelShader9*>(
                gx::CompilePixelShader(gx::Device9(deviceRaw), kSelectivePsHlsl, "ps_2_a"));
            if (ps)
                profile = 21;
        }
        if (ps)
            WLOG_INFO("gear-recolor: selective OC PS v%d ready (profile=%d)", kPsVer, profile);
        else
            WLOG_WARN("gear-recolor: selective OC PS compile failed");
        return ps;
    }

    // --- Screen eyedropper (sample backbuffer under cursor) ---
    std::atomic<bool> g_sampleArmed{ false };
    std::atomic<bool> g_sampleReady{ false };
    std::atomic<bool> g_samplePending{ false };
    float g_sampleRgb[3] = { 0.f, 0.f, 0.f };
    int g_sampleClientX = 0;
    int g_sampleClientY = 0;
    std::mutex g_sampleMu;

    bool SampleBackbufferAt(IDirect3DDevice9* dev, int cx, int cy, float outRgb[3])
    {
        if (!dev || !outRgb)
            return false;
        IDirect3DSurface9* bb = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
            return false;

        D3DSURFACE_DESC desc{};
        bb->GetDesc(&desc);
        IDirect3DSurface9* staging = nullptr;
        HRESULT hr = dev->CreateOffscreenPlainSurface(
            desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr);
        if (FAILED(hr) || !staging)
        {
            bb->Release();
            return false;
        }
        hr = dev->GetRenderTargetData(bb, staging);
        bb->Release();
        if (FAILED(hr))
        {
            staging->Release();
            return false;
        }

        if (cx < 0)
            cx = 0;
        if (cy < 0)
            cy = 0;
        if (cx >= static_cast<int>(desc.Width))
            cx = static_cast<int>(desc.Width) - 1;
        if (cy >= static_cast<int>(desc.Height))
            cy = static_cast<int>(desc.Height) - 1;

        D3DLOCKED_RECT lr{};
        RECT rc{ cx, cy, cx + 1, cy + 1 };
        hr = staging->LockRect(&lr, &rc, D3DLOCK_READONLY);
        if (FAILED(hr))
        {
            staging->Release();
            return false;
        }

        const uint8_t* p = static_cast<const uint8_t*>(lr.pBits);
        float r = 0.f, g = 0.f, b = 0.f;
        // Common WoW backbuffer: A8R8G8B8 / X8R8G8B8 (BGRA in memory).
        if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8
            || desc.Format == D3DFMT_A8B8G8R8 || desc.Format == D3DFMT_X8B8G8R8)
        {
            if (desc.Format == D3DFMT_A8B8G8R8 || desc.Format == D3DFMT_X8B8G8R8)
            {
                r = p[0] / 255.f;
                g = p[1] / 255.f;
                b = p[2] / 255.f;
            }
            else
            {
                b = p[0] / 255.f;
                g = p[1] / 255.f;
                r = p[2] / 255.f;
            }
        }
        else
        {
            // Fallback: treat as BGRA8
            b = p[0] / 255.f;
            g = p[1] / 255.f;
            r = p[2] / 255.f;
        }
        staging->UnlockRect();
        staging->Release();
        outRgb[0] = Clamp01(r);
        outRgb[1] = Clamp01(g);
        outRgb[2] = Clamp01(b);
        return true;
    }

    void ArmScreenSample()
    {
        g_sampleReady.store(false, std::memory_order_relaxed);
        g_samplePending.store(false, std::memory_order_relaxed);
        g_sampleArmed.store(true, std::memory_order_relaxed);
    }

    void DisarmScreenSample()
    {
        g_sampleArmed.store(false, std::memory_order_relaxed);
        g_samplePending.store(false, std::memory_order_relaxed);
    }

    constexpr char kPreviewTgaPath0[] =
        "Interface\\AddOns\\HorizontalTools\\Recolor\\ht_prev0.tga";
    constexpr char kPreviewTgaPath1[] =
        "Interface\\AddOns\\HorizontalTools\\Recolor\\ht_prev1.tga";
    std::atomic<int> g_previewFlip{ 0 };

    bool EnsurePreviewDir()
    {
        CreateDirectoryA("Interface", nullptr);
        CreateDirectoryA("Interface\\AddOns", nullptr);
        CreateDirectoryA("Interface\\AddOns\\HorizontalTools", nullptr);
        CreateDirectoryA("Interface\\AddOns\\HorizontalTools\\Recolor", nullptr);
        // Old green BLP previews confuse SetTexture basename resolution — remove them.
        DeleteFileA("Interface\\AddOns\\HorizontalTools\\Recolor\\preview0.blp");
        DeleteFileA("Interface\\AddOns\\HorizontalTools\\Recolor\\preview1.blp");
        DeleteFileA("Interface\\AddOns\\HorizontalTools\\Recolor\\preview0.tga");
        DeleteFileA("Interface\\AddOns\\HorizontalTools\\Recolor\\preview1.tga");
        return true;
    }

    // WotLK SetTexture: 24-bit uncompressed TGA, bottom-left origin. 32-bit / top-left
    // often fails to load → solid green missing-texture.
    bool WriteTgaBgr24(const char* path, uint32_t w, uint32_t h, const uint8_t* bgra)
    {
        if (!path || !bgra || !w || !h || w > 2048 || h > 2048)
            return false;
        EnsurePreviewDir();
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") != 0 || !f)
            return false;
        uint8_t hdr[18] = {};
        hdr[2] = 2; // uncompressed true-color
        hdr[12] = static_cast<uint8_t>(w & 0xFF);
        hdr[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
        hdr[14] = static_cast<uint8_t>(h & 0xFF);
        hdr[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
        hdr[16] = 24;
        hdr[17] = 0x00; // bottom-left origin
        fwrite(hdr, 1, 18, f);
        std::vector<uint8_t> row(static_cast<size_t>(w) * 3u);
        for (int y = static_cast<int>(h) - 1; y >= 0; --y)
        {
            const uint8_t* src = bgra + static_cast<size_t>(y) * w * 4u;
            for (uint32_t x = 0; x < w; ++x)
            {
                row[x * 3u + 0] = src[x * 4u + 0]; // B
                row[x * 3u + 1] = src[x * 4u + 1]; // G
                row[x * 3u + 2] = src[x * 4u + 2]; // R
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
        return true;
    }

    bool BuildTintedBgraPreview(const TexTintCache& st, const SlotHsl& hsl,
                                std::vector<uint8_t>& outBgra, uint32_t& outW, uint32_t& outH)
    {
        if (!st.paletted || st.orig.size() < 256 * 4 || st.origIdx.empty() || !st.w || !st.h)
            return false;
        outW = st.w;
        outH = st.h;
        const size_t n = static_cast<size_t>(st.w) * st.h;
        if (st.origIdx.size() < n)
            return false;
        // Same pipeline as body: expand indices via ORIGINAL palette, YIQ each pixel.
        uint8_t tintPal[256 * 4];
        std::memcpy(tintPal, st.orig.data(), 256 * 4);
        HslBgraBuffer(tintPal, 256, hsl, false);
        outBgra.resize(n * 4);
        for (size_t i = 0; i < n; ++i)
        {
            const uint8_t* s = tintPal + static_cast<size_t>(st.origIdx[i]) * 4u;
            uint8_t* d = outBgra.data() + i * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3] ? s[3] : 255;
        }
        return true;
    }

    int ScoreTexPathForSlot(const char* path, int slot)
    {
        if (!path)
            return -1000;
        int score = 0;
        if (slot == 4)
        {
            if (ContainsCI(path, "chest") || ContainsCI(path, "torso"))
                score += 20;
            if (ContainsCI(path, "sleeve") || ContainsCI(path, "arm")
                || ContainsCI(path, "hand") || ContainsCI(path, "glove")
                || ContainsCI(path, "bracer"))
                score -= 12;
        }
        else if (slot == 6)
        {
            if (ContainsCI(path, "pant") && !ContainsCI(path, "belt"))
                score += 25;
            if (ContainsCI(path, "belt"))
                score -= 8;
            if (ContainsCI(path, "boot") || ContainsCI(path, "foot"))
                score -= 15;
        }
        else if (slot == 7)
        {
            if (ContainsCI(path, "boot") || ContainsCI(path, "foot"))
                score += 20;
            if (ContainsCI(path, "pant"))
                score -= 10;
        }
        return score;
    }

    bool FindCacheForSlot(int slot, TexTintCache& outSt, std::string& outPath, void** outTex)
    {
        if (outTex)
            *outTex = nullptr;

        void* bestTex = nullptr;
        TexTintCache bestSt{};
        std::string bestPath;
        int bestScore = -100000;

        auto consider = [&](void* tex, const char* name, const TexTintCache& st) {
            if (st.orig.empty() || !st.paletted)
                return;
            const char* p = name && name[0] ? name : (st.path.empty() ? nullptr : st.path.c_str());
            if (!p)
                return;
            DisplaySlotInfo dinfo{};
            const OwnerGuid self = ActiveOwnerGuid();
            const bool stemOk = self && GetDisplaySlotInfo(self, slot, dinfo)
                && DisplaySlotHasComponentTextures(dinfo) && PathMatchesAnyStem(p, dinfo);
            // Preview bake: prefer ItemDisplayInfo component stems; folder heuristic only
            // when stems are unknown (UI swatch, not in-world paste tint).
            const int mapped = SlotForComponentTexture(p);
            if (!stemOk)
            {
                if (DisplaySlotHasComponentTextures(dinfo))
                    return; // display known but this BLP is not this slot's item
                if (mapped != slot)
                    return;
            }
            int score = ScoreTexPathForSlot(p, slot);
            if (stemOk)
                score += 40;
            if (mapped == slot)
                score += 5;
            if (score > bestScore)
            {
                bestScore = score;
                bestTex = tex;
                bestSt = st;
                bestPath = p;
            }
        };
        {
            std::lock_guard<std::mutex> tlock(g_texMutex);
            for (auto& kv : g_texTint)
            {
                const char* p = kv.second.path.empty() ? nullptr : kv.second.path.c_str();
                if (!p)
                {
                    auto nit = g_texNames.find(kv.first);
                    if (nit != g_texNames.end())
                        p = nit->second.c_str();
                }
                consider(kv.first, p, kv.second);
            }
        }

        if (bestScore > -100000)
        {
            outSt = bestSt;
            outPath = bestPath;
            if (outTex)
                *outTex = bestTex;
            return true;
        }

        std::lock_guard<std::mutex> tlock(g_texMutex);
        DisplaySlotInfo dinfo{};
        const OwnerGuid self = ActiveOwnerGuid();
        const bool haveStems = self && GetDisplaySlotInfo(self, slot, dinfo)
            && DisplaySlotHasComponentTextures(dinfo);
        for (auto& kv : g_texNames)
        {
            if (!IsItemComponentTexture(kv.second.c_str()))
                continue;
            if (haveStems)
            {
                if (!PathMatchesAnyStem(kv.second.c_str(), dinfo))
                    continue;
            }
            else if (SlotForComponentTexture(kv.second.c_str()) != slot)
            {
                continue;
            }
            outPath = kv.second;
            return false;
        }
        return false;
    }

    bool BakeSlotPreview(int slot, const SlotHsl& hsl, std::string& pathOut)
    {
        TexTintCache st{};
        std::string srcPath;
        void* tex = nullptr;
        const bool haveCache = FindCacheForSlot(slot, st, srcPath, &tex);
        const int flip = g_previewFlip.fetch_xor(1, std::memory_order_relaxed) & 1;
        pathOut = flip ? kPreviewTgaPath1 : kPreviewTgaPath0;
        if (!haveCache)
        {
            return false;
        }
        if (st.paletted && st.origIdx.empty() && tex)
        {
            uint8_t info[8] = {};
            if (SafeGetInfo(tex, info, 1))
            {
                st.w = *reinterpret_cast<uint16_t*>(info + 0);
                st.h = *reinterpret_cast<uint16_t*>(info + 2);
            }
            uint8_t* mip0 = SafeGetMip(tex, 0);
            if (mip0 && st.w && st.h)
                st.origIdx.assign(mip0, mip0 + static_cast<size_t>(st.w) * st.h);
        }

        std::vector<uint8_t> bgra;
        uint32_t pw = 0, ph = 0;
        if (!BuildTintedBgraPreview(st, hsl, bgra, pw, ph))
            return false;

        const bool ok = WriteTgaBgr24(pathOut.c_str(), pw, ph, bgra.data());
        const int score = ScoreTexPathForSlot(srcPath.c_str(), slot);
        return ok;
    }

    // Quantize + farthest-point: 3 distant colors from the worn item's texture.
    // Prefer durable CatchSample (captured at paste/upload). Stale TextureCache
    // handles return GetPal=null after unload — never rely on them alone.
    bool CatchDistantColors(int slot, float outRgb[9], int* outCount)
    {
        if (outCount)
            *outCount = 0;
        if (slot < 0 || slot >= kMaxEquipSlots || !outRgb)
            return false;

        struct Bucket
        {
            double r = 0, g = 0, b = 0;
            int n = 0;
        };
        std::unordered_map<uint32_t, Bucket> buckets;
        buckets.reserve(512);

        auto addPixel = [&](float r, float g, float b, float a) {
            if (a < 0.08f)
                return;
            const float mx = (std::max)(r, (std::max)(g, b));
            const float mn = (std::min)(r, (std::min)(g, b));
            if (mx < 0.06f)
                return;
            if (mx > 0.97f && (mx - mn) < 0.06f)
                return;
            const float sat = (mx > 1e-6f) ? ((mx - mn) / mx) : 0.f;
            if (sat < 0.06f)
                return;
            const int qr = (std::min)(31, static_cast<int>(Clamp01(r) * 31.f + 0.5f));
            const int qg = (std::min)(31, static_cast<int>(Clamp01(g) * 31.f + 0.5f));
            const int qb = (std::min)(31, static_cast<int>(Clamp01(b) * 31.f + 0.5f));
            const uint32_t key = (static_cast<uint32_t>(qr) << 10)
                | (static_cast<uint32_t>(qg) << 5)
                | static_cast<uint32_t>(qb);
            Bucket& bk = buckets[key];
            bk.r += r;
            bk.g += g;
            bk.b += b;
            ++bk.n;
        };

        auto ingestSample = [&](const CatchSample& s) {
            if (s.pixels.empty())
                return;
            if (s.paletted && s.pixels.size() >= 256 * 4)
            {
                for (int i = 0; i < 256; ++i)
                {
                    const uint8_t* p = s.pixels.data() + static_cast<size_t>(i) * 4u;
                    if (p[0] < 4 && p[1] < 4 && p[2] < 4)
                        continue;
                    addPixel(p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, 1.f);
                }
            }
            else
            {
                const size_t n = s.pixels.size() / 4;
                for (size_t i = 0; i < n; ++i)
                {
                    const uint8_t* p = s.pixels.data() + i * 4;
                    const float a = (p[3] > 0) ? (p[3] / 255.f) : 1.f;
                    addPixel(p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, a);
                }
            }
        };

        int bankOk = 0;
        int reloadOk = 0;
        std::string bankPath;
        {
            std::lock_guard<std::mutex> lock(g_catchMu);
            const CatchSample& s = g_catchSample[slot];
            if (!s.pixels.empty())
            {
                bankOk = 1;
                bankPath = s.path;
                ingestSample(s);
            }
        }

        // Reload TextureComponents BLP by path if bank empty (body slots).
        if (buckets.empty())
        {
            std::vector<std::string> paths;
            DisplaySlotInfo dinfo{};
            const OwnerGuid self = ActiveOwnerGuid();
            const bool haveStems = self && GetDisplaySlotInfo(self, slot, dinfo)
                && DisplaySlotHasComponentTextures(dinfo);
            {
                std::lock_guard<std::mutex> lock(g_texMutex);
                for (auto& kv : g_texNames)
                {
                    const char* p = kv.second.c_str();
                    if (!IsItemComponentTexture(p))
                        continue;
                    if (haveStems)
                    {
                        if (!PathMatchesAnyStem(p, dinfo))
                            continue;
                    }
                    else if (SlotForComponentTexture(p) != slot)
                    {
                        continue;
                    }
                    paths.push_back(kv.second);
                }
            }
            if (paths.empty() && !bankPath.empty())
                paths.push_back(bankPath);
            for (const std::string& path : paths)
            {
                void* tex = LoadTextureCacheByPath(path.c_str());
                if (!tex)
                    continue;
                CaptureCatchFromTextureCache(tex, slot, path.c_str());
                std::lock_guard<std::mutex> lock(g_catchMu);
                if (!g_catchSample[slot].pixels.empty())
                {
                    reloadOk = 1;
                    ingestSample(g_catchSample[slot]);
                    break;
                }
            }
        }

        // Still empty: try loose palette ingest from bank/reload without sat gate.
        if (buckets.empty())
        {
            CatchSample s{};
            {
                std::lock_guard<std::mutex> lock(g_catchMu);
                s = g_catchSample[slot];
            }
            if (s.paletted && s.pixels.size() >= 256 * 4)
            {
                for (int i = 0; i < 256; ++i)
                {
                    const uint8_t* p = s.pixels.data() + static_cast<size_t>(i) * 4u;
                    if (p[0] < 3 && p[1] < 3 && p[2] < 3)
                        continue;
                    const float r = p[2] / 255.f, g = p[1] / 255.f, b = p[0] / 255.f;
                    const float mx = (std::max)(r, (std::max)(g, b));
                    if (mx < 0.04f)
                        continue;
                    const int qr = (std::min)(31, static_cast<int>(r * 31.f + 0.5f));
                    const int qg = (std::min)(31, static_cast<int>(g * 31.f + 0.5f));
                    const int qb = (std::min)(31, static_cast<int>(b * 31.f + 0.5f));
                    const uint32_t key = (static_cast<uint32_t>(qr) << 10)
                        | (static_cast<uint32_t>(qg) << 5)
                        | static_cast<uint32_t>(qb);
                    Bucket& bk = buckets[key];
                    bk.r += r; bk.g += g; bk.b += b; ++bk.n;
                }
            }
        }

        struct Cand
        {
            float r, g, b;
            int n;
            float sat;
        };
        std::vector<Cand> cands;
        cands.reserve(buckets.size());
        for (auto& kv : buckets)
        {
            if (kv.second.n <= 0)
                continue;
            const float inv = 1.f / static_cast<float>(kv.second.n);
            Cand c{};
            c.r = Clamp01(static_cast<float>(kv.second.r * inv));
            c.g = Clamp01(static_cast<float>(kv.second.g * inv));
            c.b = Clamp01(static_cast<float>(kv.second.b * inv));
            c.n = kv.second.n;
            const float mx = (std::max)(c.r, (std::max)(c.g, c.b));
            const float mn = (std::min)(c.r, (std::min)(c.g, c.b));
            c.sat = (mx > 1e-6f) ? ((mx - mn) / mx) : 0.f;
            cands.push_back(c);
        }
        if (cands.empty())
        {
            return false;
        }

        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            const float sa = a.sat * std::sqrt(static_cast<float>(a.n));
            const float sb = b.sat * std::sqrt(static_cast<float>(b.n));
            return sa > sb;
        });
        if (cands.size() > 96)
            cands.resize(96);

        auto dist2 = [](const Cand& a, const Cand& b) {
            const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
            return dr * dr + dg * dg + db * db;
        };

        int picked[3] = { -1, -1, -1 };
        int nPick = 0;
        picked[0] = 0;
        nPick = 1;
        while (nPick < 3 && nPick < static_cast<int>(cands.size()))
        {
            int best = -1;
            float bestMin = -1.f;
            for (int i = 0; i < static_cast<int>(cands.size()); ++i)
            {
                bool used = false;
                for (int p = 0; p < nPick; ++p)
                    if (picked[p] == i)
                        used = true;
                if (used)
                    continue;
                float mind = 1e9f;
                for (int p = 0; p < nPick; ++p)
                    mind = (std::min)(mind, dist2(cands[i], cands[picked[p]]));
                if (mind > bestMin)
                {
                    bestMin = mind;
                    best = i;
                }
            }
            if (best < 0 || bestMin < 0.0025f)
                break;
            picked[nPick++] = best;
        }

        for (int i = 0; i < nPick; ++i)
        {
            outRgb[i * 3 + 0] = cands[picked[i]].r;
            outRgb[i * 3 + 1] = cands[picked[i]].g;
            outRgb[i * 3 + 2] = cands[picked[i]].b;
        }
        if (outCount)
            *outCount = nPick;

        return nPick > 0;
    }

    int __cdecl LuaRecolorCatchSlotColors(void* state)
    {
        if (!state || wlua::GetTop(state) < 1)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        float rgb[9] = {};
        int n = 0;
        if (!CatchDistantColors(slot, rgb, &n) || n <= 0)
        {
            wlua::PushNumber(state, 0.0);
            return 1;
        }
        wlua::PushNumber(state, static_cast<double>(n));
        for (int i = 0; i < n * 3; ++i)
            wlua::PushNumber(state, static_cast<double>(rgb[i]));
        return 1 + n * 3;
    }

    int __cdecl LuaRecolorSetSlot(void* state)
    {
        if (!state || wlua::GetTop(state) < 4)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        const float r = static_cast<float>(wlua::ToNumber(state, 2));
        const float g = static_cast<float>(wlua::ToNumber(state, 3));
        const float b = static_cast<float>(wlua::ToNumber(state, 4));
        SetSlotHsl(slot, r, g, b);
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorSetSlotDraft(void* state)
    {
        if (!state || wlua::GetTop(state) < 4)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        const float r = static_cast<float>(wlua::ToNumber(state, 2));
        const float g = static_cast<float>(wlua::ToNumber(state, 3));
        const float b = static_cast<float>(wlua::ToNumber(state, 4));
        SetSlotDraftSolid(slot, r, g, b);
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorSetSlotGradientDraft(void* state)
    {
        if (!state || wlua::GetTop(state) < 5)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        const int nStops = static_cast<int>(wlua::ToNumber(state, 2));
        const int fill = static_cast<int>(wlua::ToNumber(state, 3));
        const int top = wlua::GetTop(state);
        float colors[kMaxGradStops * 3] = {};
        int nFloats = 0;
        for (int i = 4; i <= top && nFloats < kMaxGradStops * 3; ++i)
            colors[nFloats++] = static_cast<float>(wlua::ToNumber(state, i));
        SetSlotDraftGradient(slot, nStops, fill, colors, nFloats);
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorSetSlotSelectiveDraft(void* state)
    {
        if (!state || wlua::GetTop(state) < 2)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        const int nRules = static_cast<int>(wlua::ToNumber(state, 2));
        if (nRules <= 0)
        {
            SetSlotDraftSelective(slot, nullptr, 0);
            wlua::PushBoolean(state, 1);
            return 1;
        }
        SelRule rules[kMaxSelRules] = {};
        const int top = wlua::GetTop(state);
        int arg = 3;
        int count = 0;
        for (int i = 0; i < nRules && i < kMaxSelRules; ++i)
        {
            if (arg + 6 > top)
                break;
            rules[count].sr = static_cast<float>(wlua::ToNumber(state, arg++));
            rules[count].sg = static_cast<float>(wlua::ToNumber(state, arg++));
            rules[count].sb = static_cast<float>(wlua::ToNumber(state, arg++));
            rules[count].dr = static_cast<float>(wlua::ToNumber(state, arg++));
            rules[count].dg = static_cast<float>(wlua::ToNumber(state, arg++));
            rules[count].db = static_cast<float>(wlua::ToNumber(state, arg++));
            rules[count].tol = static_cast<float>(wlua::ToNumber(state, arg++));
            ++count;
        }
        SetSlotDraftSelective(slot, rules, count);
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorApplyDraft(void* state)
    {
        if (!state || wlua::GetTop(state) < 1)
            return 0;
        ApplySlotDraft(static_cast<int>(wlua::ToNumber(state, 1)));
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorResetTint(void* state)
    {
        if (!state || wlua::GetTop(state) < 1)
            return 0;
        ResetSlotTint(static_cast<int>(wlua::ToNumber(state, 1)));
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorSetSlotGradient(void* state)
    {
        if (!state || wlua::GetTop(state) < 5)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        const int nStops = static_cast<int>(wlua::ToNumber(state, 2));
        const int fill = static_cast<int>(wlua::ToNumber(state, 3));
        const int top = wlua::GetTop(state);
        float colors[kMaxGradStops * 3] = {};
        int nFloats = 0;
        for (int i = 4; i <= top && nFloats < kMaxGradStops * 3; ++i)
            colors[nFloats++] = static_cast<float>(wlua::ToNumber(state, i));
        SetSlotGradient(slot, nStops, fill, colors, nFloats);
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorGetSlotGradient(void* state)
    {
        if (!state || wlua::GetTop(state) < 1)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        SlotHsl h{};
        if (slot >= 0 && slot < kMaxEquipSlots)
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            h = g_slotHsl[slot];
        }
        if (!h.active || h.mode != 2)
        {
            wlua::PushNumber(state, 0.0);
            return 1;
        }
        wlua::PushNumber(state, 1.0);
        wlua::PushNumber(state, static_cast<double>(h.stopCount));
        wlua::PushNumber(state, static_cast<double>(h.gradFill));
        int n = 3;
        for (uint8_t s = 0; s < h.stopCount && s < kMaxGradStops; ++s)
        {
            wlua::PushNumber(state, static_cast<double>(h.stops[s][0]));
            wlua::PushNumber(state, static_cast<double>(h.stops[s][1]));
            wlua::PushNumber(state, static_cast<double>(h.stops[s][2]));
            n += 3;
        }
        // Also push base (hue/sat/light) for auto UI edit.
        wlua::PushNumber(state, static_cast<double>(h.hue));
        wlua::PushNumber(state, static_cast<double>(h.sat));
        wlua::PushNumber(state, static_cast<double>(h.light));
        return n + 3;
    }

    int __cdecl LuaRecolorSetSlotSelective(void* state)
    {
        if (!state || wlua::GetTop(state) < 8)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        const float sr = static_cast<float>(wlua::ToNumber(state, 2));
        const float sg = static_cast<float>(wlua::ToNumber(state, 3));
        const float sb = static_cast<float>(wlua::ToNumber(state, 4));
        const float dr = static_cast<float>(wlua::ToNumber(state, 5));
        const float dg = static_cast<float>(wlua::ToNumber(state, 6));
        const float db = static_cast<float>(wlua::ToNumber(state, 7));
        const float tol = static_cast<float>(wlua::ToNumber(state, 8));
        const bool forceAppend = (wlua::GetTop(state) >= 9)
            && (wlua::ToNumber(state, 9) != 0.0);
        SetSlotSelective(slot, sr, sg, sb, dr, dg, db, tol, forceAppend);
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorGetSlotTexPath(void* state)
    {
        if (!state || wlua::GetTop(state) < 1)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        TexTintCache st{};
        std::string path;
        void* tex = nullptr;
        FindCacheForSlot(slot, st, path, &tex);
        if (path.empty())
            return 0;
        wlua::PushString(state, path.c_str());
        return 1;
    }

    int __cdecl LuaRecolorBakePreview(void* state)
    {
        if (!state || wlua::GetTop(state) < 1)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        SlotHsl hsl{};
        hsl.active = true;
        if (wlua::GetTop(state) >= 4)
        {
            hsl.hue = Clamp01(static_cast<float>(wlua::ToNumber(state, 2)));
            hsl.sat = Clamp01(static_cast<float>(wlua::ToNumber(state, 3)));
            hsl.light = Clamp01(static_cast<float>(wlua::ToNumber(state, 4)));
        }
        else if (!TrySlotHsl(slot, hsl))
        {
            return 0;
        }
        std::string path;
        if (!BakeSlotPreview(slot, hsl, path))
            return 0;
        wlua::PushString(state, path.c_str());
        return 1;
    }

    int __cdecl LuaRecolorGetSlot(void* state)
    {
        if (!state || wlua::GetTop(state) < 1)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        SlotHsl h{};
        if (slot >= 0 && slot < kMaxEquipSlots)
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            h = g_slotHsl[slot];
        }
        wlua::PushNumber(state, static_cast<double>(h.hue));
        wlua::PushNumber(state, static_cast<double>(h.sat));
        wlua::PushNumber(state, static_cast<double>(h.light));
        wlua::PushNumber(state, h.active ? 1.0 : 0.0);
        wlua::PushNumber(state, static_cast<double>(h.mode));
        wlua::PushNumber(state, static_cast<double>(h.srcR));
        wlua::PushNumber(state, static_cast<double>(h.srcG));
        wlua::PushNumber(state, static_cast<double>(h.srcB));
        wlua::PushNumber(state, static_cast<double>(h.tolerance));
        wlua::PushNumber(state, static_cast<double>(h.ruleCount));
        return 10;
    }

    int __cdecl LuaRecolorClearSlot(void* state)
    {
        if (!state || wlua::GetTop(state) < 1)
            return 0;
        ClearSlotHsl(static_cast<int>(wlua::ToNumber(state, 1)));
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorClearAll(void* state)
    {
        ClearAllHsl();
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorApplyOwnerTint(void* state)
    {
        // WXL_RecolorApplyOwnerTint(ownerGuid, slot, "clear"|mode, [data])
        // Unified self + remote write path (same mental model).
        try
        {
            if (!state || wlua::GetTop(state) < 3)
                return 0;
            const char* guidStr = wlua::ToString(state, 1, nullptr);
            const int slot = static_cast<int>(wlua::ToNumber(state, 2));
            OwnerGuid owner = 0;
            if (!ParseGuidString(guidStr, owner) || slot < 0 || slot >= kMaxEquipSlots)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            const char* modeOrClear = wlua::ToString(state, 3, nullptr);
            if (!modeOrClear)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            if (_stricmp(modeOrClear, "clear") == 0)
            {
                const bool ok = ApplyOwnerTint(owner, slot, nullptr);
                wlua::PushBoolean(state, ok ? 1 : 0);
                return 1;
            }
            if (wlua::GetTop(state) < 4)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            const int mode = static_cast<int>(wlua::ToNumber(state, 3));
            const char* data = wlua::ToString(state, 4, nullptr);
            SlotHsl h{};
            if (mode < 0 || mode > 2 || !data
                || !ParseTintData(static_cast<uint8_t>(mode), data, h))
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            const bool ok = ApplyOwnerTint(owner, slot, &h);
            wlua::PushBoolean(state, ok ? 1 : 0);
            return 1;
        }
        catch (...)
        {
            if (state)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            return 0;
        }
    }

    int __cdecl LuaRecolorSetRemote(void* state)
    {
        // Compat wrapper → ApplyOwnerTint (never skips self; Apply routes correctly).
        try
        {
            if (!state || wlua::GetTop(state) < 4)
                return 0;
            const char* guidStr = wlua::ToString(state, 1, nullptr);
            const int slot = static_cast<int>(wlua::ToNumber(state, 2));
            const int mode = static_cast<int>(wlua::ToNumber(state, 3));
            const char* data = wlua::ToString(state, 4, nullptr);
            OwnerGuid owner = 0;
            if (!ParseGuidString(guidStr, owner) || slot < 0 || slot >= kMaxEquipSlots
                || mode < 0 || mode > 2 || !data)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            SlotHsl h{};
            if (!ParseTintData(static_cast<uint8_t>(mode), data, h))
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            const bool ok = ApplyOwnerTint(owner, slot, &h);
            wlua::PushBoolean(state, ok ? 1 : 0);
            return 1;
        }
        catch (...)
        {
            if (state)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            return 0;
        }
    }

    // Compat: apply server PUSH to local slot table without GUID arg.
    int __cdecl LuaRecolorApplyLocalPayload(void* state)
    {
        try
        {
            if (!state || wlua::GetTop(state) < 2)
                return 0;
            const int slot = static_cast<int>(wlua::ToNumber(state, 1));
            if (slot < 0 || slot >= kMaxEquipSlots)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            OwnerGuid self = ActiveOwnerGuid();
            if (!self)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            const char* modeOrClear = wlua::ToString(state, 2, nullptr);
            if (!modeOrClear)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            if (_stricmp(modeOrClear, "clear") == 0)
            {
                const bool ok = ApplyOwnerTint(self, slot, nullptr);
                wlua::PushBoolean(state, ok ? 1 : 0);
                return 1;
            }
            if (wlua::GetTop(state) < 3)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            const int mode = static_cast<int>(wlua::ToNumber(state, 2));
            const char* data = wlua::ToString(state, 3, nullptr);
            SlotHsl h{};
            if (mode < 0 || mode > 2 || !data
                || !ParseTintData(static_cast<uint8_t>(mode), data, h))
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            const bool ok = ApplyOwnerTint(self, slot, &h);
            wlua::PushBoolean(state, ok ? 1 : 0);
            return 1;
        }
        catch (...)
        {
            if (state)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            return 0;
        }
    }

    int __cdecl LuaRecolorSetSelfGuid(void* state)
    {
        if (!state || wlua::GetTop(state) < 1)
            return 0;
        const char* guidStr = wlua::ToString(state, 1, nullptr);
        OwnerGuid owner = 0;
        if (!ParseGuidString(guidStr, owner) || !owner)
        {
            wlua::PushBoolean(state, 0);
            return 1;
        }
        const OwnerGuid prev = g_cachedSelfGuid.load(std::memory_order_relaxed);
        g_cachedSelfGuid.store(owner, std::memory_order_relaxed);
        // Character switch only — /reload keeps same guid and g_slotHsl in DLL.
        if (!prev || !GuidSamePlayer(prev, owner))
            ClearAllHsl();
        // Adopt early login PUSHes that landed as remotes (self was still 0).
        PromoteRemoteTintsToLocal(owner);
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorOnUiReload(void* state)
    {
        OnUiReload();
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorBeginEquipSnap(void* state)
    {
        // Only refresh LOCAL equip snap. Remotes are filled from server PUSH
        // entry — GetInventoryItemID on other units returns 0 here and was
        // wiping real entries → every remote slot flagged phantom (feet etc.).
        {
            std::lock_guard<std::mutex> lock(g_equipSnapMu);
            const OwnerGuid self = ActiveOwnerGuid();
            if (self)
                g_equipSnap.erase(self);
            else
                g_equipSnap.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_displayMu);
            const OwnerGuid self = ActiveOwnerGuid();
            if (self)
                g_displaySnap.erase(self);
            else
                g_displaySnap.clear();
        }
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorNoteEquip(void* state)
    {
        // WXL_RecolorNoteEquip(ownerGuid, slot, entry)
        // Marks owner as "equipKnown". entry=0 means slot empty.
        if (!state || wlua::GetTop(state) < 3)
            return 0;
        const char* guidStr = wlua::ToString(state, 1, nullptr);
        const int slot = static_cast<int>(wlua::ToNumber(state, 2));
        const uint32_t entry = static_cast<uint32_t>(wlua::ToNumber(state, 3));
        OwnerGuid owner = 0;
        if (!ParseGuidString(guidStr, owner) || slot < 0 || slot >= kMaxEquipSlots)
        {
            wlua::PushBoolean(state, 0);
            return 1;
        }
        {
            std::lock_guard<std::mutex> lock(g_equipSnapMu);
            g_equipSnap[owner][static_cast<size_t>(slot)] = entry;
        }
        SetDisplaySlotFromItemEntry(owner, slot, entry);
        const OwnerGuid self = ActiveOwnerGuid();

        // #region agent log
        {
            const bool isRemote = self != 0 && !GuidSamePlayer(owner, self);
            if (isRemote && g_dbgNoteEquipN.fetch_add(1, std::memory_order_relaxed) < 40)
            {
                DisplaySlotInfo di{};
                uint32_t dispId = 0;
                if (GetDisplaySlotInfo(owner, slot, di))
                    dispId = di.displayId;
                DbgLog615("H4", "LuaRecolorNoteEquip", "remote_snap",
                    static_cast<unsigned long long>(owner), slot, 0, "",
                    static_cast<int>(entry), static_cast<int>(dispId), 0);
            }
        }
        // #endregion

        if (self && GuidSamePlayer(owner, self))
        {
            // Unequip OR item swap: drop stale paste refs for this slot.
            ClearCatchSampleForSlot(slot);
            if (entry == 0 && IsBodyEquipSlot(slot))
                RequestBodyRebuildForSlot(slot);
        }
        // Remote tint clear on unequip is handled by applyPush → ApplyOwnerTint
        // (deferred). Do NOT ClearRemoteSlotTint here — transmog visible-slot
        // flicker sends NoteEquip(0) for every slot and was wiping live tints.
        wlua::PushBoolean(state, 1);
        return 1;
    }

    int __cdecl LuaRecolorClearRemote(void* state)
    {
        try
        {
            if (!state || wlua::GetTop(state) < 1)
                return 0;
            const char* guidStr = wlua::ToString(state, 1, nullptr);
            OwnerGuid owner = 0;
            if (!ParseGuidString(guidStr, owner))
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            const int top = wlua::GetTop(state);
            const int slot = (top >= 2) ? static_cast<int>(wlua::ToNumber(state, 2)) : -1;
            if (slot >= 0 && slot < kMaxEquipSlots)
            {
                ApplyOwnerTint(owner, slot, nullptr);
            }
            else
            {
                // Clear-all: Force per body slot that had tint — never OR into 255.
                OwnerSlotHsl before{};
                {
                    std::lock_guard<std::mutex> lock(g_remoteMu);
                    auto it = g_remoteTints.find(owner);
                    if (it != g_remoteTints.end())
                        before = it->second;
                }
                ClearRemoteSlotTint(owner, -1);
                for (int s = 0; s < kMaxEquipSlots; ++s)
                {
                    if (!before[static_cast<size_t>(s)].active)
                        continue;
                    const uint32_t m = SectionMaskForEquipSlot(s);
                    if (m)
                        DirtyOwnerSections(owner, m);
                }
            }
            wlua::PushBoolean(state, 1);
            return 1;
        }
        catch (...)
        {
            if (state)
            {
                wlua::PushBoolean(state, 0);
                return 1;
            }
            return 0;
        }
    }

    int __cdecl LuaRecolorClearAllRemote(void* state)
    {
        ClearAllRemoteTints();
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorGetSlotPayload(void* state)
    {
        if (!state || wlua::GetTop(state) < 1)
            return 0;
        const int slot = static_cast<int>(wlua::ToNumber(state, 1));
        SlotHsl h{};
        if (!TrySlotHslCommittedOrDraft(slot, h) || !h.active)
        {
            wlua::PushBoolean(state, 0);
            return 1;
        }
        char data[384];
        if (!FormatTintData(h, data, sizeof(data)))
        {
            wlua::PushBoolean(state, 0);
            return 1;
        }
        wlua::PushBoolean(state, 1);
        wlua::PushNumber(state, static_cast<double>(h.mode));
        wlua::PushString(state, data);
        return 3;
    }

    int __cdecl LuaRecolorSetPreviewActive(void* state)
    {
        const bool active = state && wlua::ToNumber(state, 1) != 0.0;
        g_previewUiActive.store(active, std::memory_order_relaxed);
        if (!active)
            ClearAllDrafts();
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorBeginBatch(void* state)
    {
        BeginBodyRebuildBatch();
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorEndBatch(void* state)
    {
        // arg1 optional: 0 = sync colors only (enter-world), 1/default = force full rebuild
        bool forceRebuild = true;
        if (state && wlua::GetTop(state) >= 1)
            forceRebuild = (wlua::ToNumber(state, 1) != 0.0);
        EndBodyRebuildBatch(forceRebuild);
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorFlushTex(void* state)
    {
        FlushTexTintState("lua_leaving");
        g_pendingEnterWorldRebuild.store(true, std::memory_order_relaxed);
        g_naturalTintPastes.store(0, std::memory_order_relaxed);
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorForceBodyRebuild(void* state)
    {
        g_pendingEnterWorldRebuild.store(false, std::memory_order_relaxed);
        g_deferredFullRebuildAt.store(0, std::memory_order_relaxed);
        ForceAllowedBodyRebuild();
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorArmScreenSample(void* state)
    {
        ArmScreenSample();
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorCancelScreenSample(void* state)
    {
        DisarmScreenSample();
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorGetScreenSample(void* state)
    {
        if (!state)
            return 0;
        const bool ready = g_sampleReady.load(std::memory_order_relaxed);
        float rgb[3] = {};
        if (ready)
        {
            std::lock_guard<std::mutex> lock(g_sampleMu);
            rgb[0] = g_sampleRgb[0];
            rgb[1] = g_sampleRgb[1];
            rgb[2] = g_sampleRgb[2];
            g_sampleReady.store(false, std::memory_order_relaxed);
        }
        wlua::PushNumber(state, static_cast<double>(rgb[0]));
        wlua::PushNumber(state, static_cast<double>(rgb[1]));
        wlua::PushNumber(state, static_cast<double>(rgb[2]));
        wlua::PushNumber(state, ready ? 1.0 : 0.0);
        return 4;
    }

    class GearRecolor final : public ev::EventScript
    {
    public:
        GearRecolor()
        {
            if (ModuleDisabled())
            {
                WLOG_INFO("gear-recolor: disabled");
                return;
            }

            wlua::RegisterFunction("WXL_RecolorSetSlot", &LuaRecolorSetSlot);
            wlua::RegisterFunction("WXL_RecolorSetSlotDraft", &LuaRecolorSetSlotDraft);
            wlua::RegisterFunction("WXL_RecolorSetSlotGradient", &LuaRecolorSetSlotGradient);
            wlua::RegisterFunction("WXL_RecolorSetSlotGradientDraft", &LuaRecolorSetSlotGradientDraft);
            wlua::RegisterFunction("WXL_RecolorResetTint", &LuaRecolorResetTint);
            wlua::RegisterFunction("WXL_RecolorGetSlotGradient", &LuaRecolorGetSlotGradient);
            wlua::RegisterFunction("WXL_RecolorSetSlotSelective", &LuaRecolorSetSlotSelective);
            wlua::RegisterFunction("WXL_RecolorSetSlotSelectiveDraft", &LuaRecolorSetSlotSelectiveDraft);
            wlua::RegisterFunction("WXL_RecolorApplyDraft", &LuaRecolorApplyDraft);
            wlua::RegisterFunction("WXL_RecolorCatchSlotColors", &LuaRecolorCatchSlotColors);
            wlua::RegisterFunction("WXL_RecolorGetSlot", &LuaRecolorGetSlot);
            wlua::RegisterFunction("WXL_RecolorGetSlotTexPath", &LuaRecolorGetSlotTexPath);
            wlua::RegisterFunction("WXL_RecolorBakePreview", &LuaRecolorBakePreview);
            wlua::RegisterFunction("WXL_RecolorSetPreviewActive", &LuaRecolorSetPreviewActive);
            wlua::RegisterFunction("WXL_RecolorClearSlot", &LuaRecolorClearSlot);
            wlua::RegisterFunction("WXL_RecolorClearAll", &LuaRecolorClearAll);
            wlua::RegisterFunction("WXL_RecolorSetRemote", &LuaRecolorSetRemote);
            wlua::RegisterFunction("WXL_RecolorApplyOwnerTint", &LuaRecolorApplyOwnerTint);
            wlua::RegisterFunction("WXL_RecolorApplyLocalPayload", &LuaRecolorApplyLocalPayload);
            wlua::RegisterFunction("WXL_RecolorSetSelfGuid", &LuaRecolorSetSelfGuid);
            wlua::RegisterFunction("WXL_RecolorBeginEquipSnap", &LuaRecolorBeginEquipSnap);
            wlua::RegisterFunction("WXL_RecolorNoteEquip", &LuaRecolorNoteEquip);
            wlua::RegisterFunction("WXL_RecolorClearRemote", &LuaRecolorClearRemote);
            wlua::RegisterFunction("WXL_RecolorClearAllRemote", &LuaRecolorClearAllRemote);
            wlua::RegisterFunction("WXL_RecolorGetSlotPayload", &LuaRecolorGetSlotPayload);
            wlua::RegisterFunction("WXL_RecolorForceBodyRebuild", &LuaRecolorForceBodyRebuild);
            wlua::RegisterFunction("WXL_RecolorBeginBatch", &LuaRecolorBeginBatch);
            wlua::RegisterFunction("WXL_RecolorEndBatch", &LuaRecolorEndBatch);
            wlua::RegisterFunction("WXL_RecolorFlushTex", &LuaRecolorFlushTex);
            wlua::RegisterFunction("WXL_RecolorOnUiReload", &LuaRecolorOnUiReload);
            wlua::RegisterFunction("WXL_RecolorArmScreenSample", &LuaRecolorArmScreenSample);
            wlua::RegisterFunction("WXL_RecolorCancelScreenSample", &LuaRecolorCancelScreenSample);
            wlua::RegisterFunction("WXL_RecolorGetScreenSample", &LuaRecolorGetScreenSample);

            // Do not LoadHslFromDisk here — no self guid yet; shared state file leaked
            // across dual-client characters. Slots come from server PUSH after SetSelfGuid.

            on<&GearRecolor::OnBatchDraw>(ev::Event::OnM2BatchDraw);
            on<&GearRecolor::OnBlpLoad>(ev::Event::OnBlpLoad);
            on<&GearRecolor::OnTextureUpload>(ev::Event::OnTextureUpload);
            on<&GearRecolor::OnInput>(ev::Event::OnInput);
            on<&GearRecolor::OnEndScene>(ev::Event::OnEndScene);
            on<&GearRecolor::OnWorldRenderEnd>(ev::Event::OnWorldRenderEnd);
            on<&GearRecolor::OnItemSlotChange>(ev::Event::OnItemSlotChange);
            on<&GearRecolor::OnItemSlotClear>(ev::Event::OnItemSlotClear);
            WLOG_INFO("gear-recolor: events bound (world-only OC, displayInfo body tint)");
        }

        void OnItemSlotChange(const ev::ItemSlotChangeArgs& a)
        {
            const int equipSlot = ModelSlotToEquipSlot(a.modelSlot);
            if (equipSlot < 0)
                return;

            OwnerGuid owner = 0;
            if (!ResolveOwnerFromCmo(a.charModelObj, owner))
                return;

            const uint32_t displayId = GuardedDisplayIdFromItemData(a.itemDataPtr);
            const OwnerGuid self = ActiveOwnerGuid();
            const bool isSelf = self && GuidSamePlayer(owner, self);

            // Drop catch sample on equip identity change (unequip OR swap).
            if (isSelf)
            {
                ClearCatchSampleForSlot(equipSlot);
            }

            if (displayId == 0)
            {
                ClearDisplaySlot(owner, equipSlot);
                if (isSelf && IsBodyEquipSlot(equipSlot))
                    RequestBodyRebuildForSlot(equipSlot);
            }
            else
            {
                SetDisplaySlotFromDisplayId(owner, equipSlot, displayId);
            }
        }

        void OnItemSlotClear(const ev::ItemSlotClearArgs& a)
        {
            if (a.equipSlotWow >= static_cast<uint32_t>(kMaxEquipSlots))
                return;
            OwnerGuid owner = 0;
            if (!ResolveOwnerFromCmo(a.charModelObj, owner))
                return;
            const int equipSlot = static_cast<int>(a.equipSlotWow);
            ClearDisplaySlot(owner, equipSlot);
            const OwnerGuid self = ActiveOwnerGuid();
            if (self && GuidSamePlayer(owner, self))
                OnLocalBodySlotRefsCleared(equipSlot);
        }

        void OnWorldRenderEnd(const ev::WorldRenderEndArgs&)
        {
            // World finished — UI 3D (paperdoll C, DressUp, etc.) draws next.
            g_ocWorldPass.store(false, std::memory_order_relaxed);
        }

        void OnInput(const ev::InputArgs& a)
        {
            // WM_LBUTTONDOWN = 0x0201
            if (!g_sampleArmed.load(std::memory_order_relaxed) || a.message != 0x0201u)
                return;
            POINT pt{};
            GetCursorPos(&pt);
            HWND hwnd = GetForegroundWindow();
            if (hwnd)
                ScreenToClient(hwnd, &pt);
            g_sampleClientX = pt.x;
            g_sampleClientY = pt.y;
            g_samplePending.store(true, std::memory_order_relaxed);
            g_sampleArmed.store(false, std::memory_order_relaxed);
            if (a.handled)
                *a.handled = true;
        }

        void OnEndScene(const ev::EndSceneArgs& a)
        {
            // Arm OC tint for the next frame's world pass (UI of this frame is done).
            g_ocWorldPass.store(true, std::memory_order_relaxed);

            auto* dev = static_cast<IDirect3DDevice9*>(a.device);
            FlushOcCatchPending(dev);

            if (!g_samplePending.exchange(false, std::memory_order_relaxed))
                return;
            if (!dev)
                return;

            float rgb[3] = {};
            if (!SampleBackbufferAt(dev, g_sampleClientX, g_sampleClientY, rgb))
                return;
            {
                std::lock_guard<std::mutex> lock(g_sampleMu);
                g_sampleRgb[0] = rgb[0];
                g_sampleRgb[1] = rgb[1];
                g_sampleRgb[2] = rgb[2];
            }
            g_sampleReady.store(true, std::memory_order_relaxed);
        }

        void OnBatchDraw(const ev::M2BatchDrawArgs& a)
        {
            if (!a.model || !a.device)
                return;

            // No OC work during UI ModelFrame draws (Character C, DressUp, glue).
            if (!g_ocWorldPass.load(std::memory_order_relaxed))
                return;

            if (a.phase == 0)
            {
                OwnerGuid owner = 0;
                if (!ResolveModelTintOwner(a.model, owner))
                    return;
                void* model = ResolveM2Model(a.model);
                const char* stem = model ? m2::PathStem(model) : nullptr;
                if (!PathLooksValid(stem))
                    return;
                int cands[4] = {};
                const int nc = CandidateSlotsForOcPath(stem, cands, 4);
                if (nc <= 0)
                    return;
                if (owner != 0 && owner != ActiveOwnerGuid())
                    return;
                auto* dev = static_cast<IDirect3DDevice9*>(a.device);
                for (int i = 0; i < nc; ++i)
                    QueueOcCatchFromDevice(dev, cands[i], stem);
                return;
            }

            if (a.phase != 1)
                return;
            if (!AnySlotActive() && !AnyRemoteTintActive())
                return;

            OwnerGuid owner = 0;
            if (!ResolveModelTintOwner(a.model, owner))
                return;

            void* model = ResolveM2Model(a.model);
            const char* stem = model ? m2::PathStem(model) : nullptr;
            if (!PathLooksValid(stem))
                return;

            SlotHsl hsl{};
            if (!TryHslForPathOwner(owner, stem, hsl, nullptr))
                return;

            // Absolute: self tint only on live unit body tree. Remotes already
            // resolved via live unit Model() in ResolveModelTintOwner.
            void* body = OwnerBodyModel(owner);
            if (!body || !ModelIsUnderRoot(a.model, body))
                return;

            RedrawTinted(a, hsl);
        }

        void OnBlpLoad(const ev::BlpLoadArgs& a)
        {
            if (!a.name || !a.handle)
                return;

            if (!IsItemComponentTexture(a.name) && !IsObjectComponentAlbedo(a.name))
                return;

            {
                std::lock_guard<std::mutex> lock(g_texMutex);
                g_texNames[a.handle] = a.name;
                // Fresh BLP bytes — drop any stale orig keyed by this handle so we
                // never restore a previous item's palette into a reused pointer.
                g_texTint.erase(a.handle);
                // Drop pathOrig for this path so the next paste re-captures from
                // live TextureCache (never call EnsurePathOrig here — mips are
                // often not mapped yet during OnBlpLoad → AV at login).
                if (IsItemComponentTexture(a.name))
                    g_pathOrig.erase(TexPathKey(a.name));
            }
        }

        void OnTextureUpload(const ev::TextureUploadArgs& a)
        {
            // Capture OC albedo pixels from the live mip table (TextureCreate path —
            // SafeGetPal does not work on these handles). Fired AFTER native upload.
            if (!a.texture || !a.width || !a.height)
                return;
            const char* name = TextureName(a.texture);
            if (!IsObjectComponentAlbedo(name) || !IsObjectComponentPathSane(name))
                return;
            int cands[4] = {};
            const int nc = CandidateSlotsForOcPath(name, cands, 4);
            for (int i = 0; i < nc; ++i)
                CaptureCatchFromMipTable(cands[i], name, a.width, a.height);
        }

    private:
        void RedrawTinted(const ev::M2BatchDrawArgs& a, const SlotHsl& hsl)
        {
            auto* dev = static_cast<IDirect3DDevice9*>(a.device);
            if (!dev || !hsl.active || IsIdentityHsl(hsl))
                return;

            IDirect3DPixelShader9* oldPs = nullptr;
            DWORD oldZFunc = 0, oldZWrite = 0, oldTf = 0;
            DWORD oldColorOp = 0, oldArg1 = 0, oldArg2 = 0;
            DWORD oldAlphaOp = 0, oldAArg1 = 0, oldAArg2 = 0;

            dev->GetPixelShader(&oldPs);
            dev->GetRenderState(D3DRS_ZFUNC, &oldZFunc);
            dev->GetRenderState(D3DRS_ZWRITEENABLE, &oldZWrite);
            dev->GetRenderState(D3DRS_TEXTUREFACTOR, &oldTf);
            dev->GetTextureStageState(0, D3DTSS_COLOROP, &oldColorOp);
            dev->GetTextureStageState(0, D3DTSS_COLORARG1, &oldArg1);
            dev->GetTextureStageState(0, D3DTSS_COLORARG2, &oldArg2);
            dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &oldAlphaOp);
            dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldAArg1);
            dev->GetTextureStageState(0, D3DTSS_ALPHAARG2, &oldAArg2);

            DWORD blend = 0, srcBlend = 0, dstBlend = 0, alphaTest = 0;
            dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
            dev->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
            dev->GetRenderState(D3DRS_DESTBLEND, &dstBlend);
            dev->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);

            IDirect3DPixelShader9* tintPs = nullptr;
            const bool selective = (hsl.mode == 1);
            const bool solidGrad = (hsl.mode == 2 && hsl.stopCount >= 2);
            if (selective)
                tintPs = EnsureSelectivePs(a.device);
            else if (solidGrad)
                tintPs = EnsureSolidGradPs(a.device);
            else
                tintPs = EnsureSolidPs(a.device);
            const bool usePs = (tintPs != nullptr);


            // Selective without PS must NOT fall back to full-mesh TF modulate
            // (that recolors the wrong parts / whole weapon).
            if (selective && !usePs)
            {
                if (oldPs)
                    oldPs->Release();
                return;
            }

            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

            if (usePs)
            {
                if (selective)
                {
                    float consts[9][4] = {};
                    uint8_t nRules = hsl.ruleCount;
                    if (nRules == 0)
                    {
                        // Legacy mirrors → single rule
                        nRules = 1;
                        consts[1][0] = hsl.srcR;
                        consts[1][1] = hsl.srcG;
                        consts[1][2] = hsl.srcB;
                        consts[1][3] = hsl.tolerance > 0.f ? hsl.tolerance : 0.35f;
                        consts[2][0] = hsl.hue;
                        consts[2][1] = hsl.sat;
                        consts[2][2] = hsl.light;
                    }
                    else
                    {
                        for (uint8_t i = 0; i < nRules && i < kMaxSelRules; ++i)
                        {
                            const SelRule& r = hsl.rules[i];
                            consts[1 + i * 2][0] = r.sr;
                            consts[1 + i * 2][1] = r.sg;
                            consts[1 + i * 2][2] = r.sb;
                            consts[1 + i * 2][3] = r.tol > 0.f ? r.tol : 0.35f;
                            consts[2 + i * 2][0] = r.dr;
                            consts[2 + i * 2][1] = r.dg;
                            consts[2 + i * 2][2] = r.db;
                        }
                    }
                    consts[0][3] = static_cast<float>(nRules);
                    float oldConsts[9][4] = {};
                    dev->GetPixelShaderConstantF(0, &oldConsts[0][0], 9);
                    dev->SetPixelShader(tintPs);
                    dev->SetPixelShaderConstantF(0, &consts[0][0], 9);
                    gx::Device9(a.device).DrawIndexedPrimitive(
                        a.primType, a.baseVertex, a.minIndex, a.numVerts,
                        a.startIndex, a.primCount);
                    // Critical: leaked c0..c8 corrupt later world/other-unit draws
                    // (skin/background "exploded") until client restart.
                    dev->SetPixelShaderConstantF(0, &oldConsts[0][0], 9);
                }
                else if (solidGrad)
                {
                    float consts[6][4] = {};
                    consts[0][3] = static_cast<float>(hsl.stopCount);
                    for (uint8_t s = 0; s < hsl.stopCount && s < kMaxGradStops; ++s)
                    {
                        consts[1 + s][0] = hsl.stops[s][0];
                        consts[1 + s][1] = hsl.stops[s][1];
                        consts[1 + s][2] = hsl.stops[s][2];
                    }
                    float oldConsts[6][4] = {};
                    dev->GetPixelShaderConstantF(0, &oldConsts[0][0], 6);
                    dev->SetPixelShader(tintPs);
                    dev->SetPixelShaderConstantF(0, &consts[0][0], 6);
                    gx::Device9(a.device).DrawIndexedPrimitive(
                        a.primType, a.baseVertex, a.minIndex, a.numVerts,
                        a.startIndex, a.primCount);
                    dev->SetPixelShaderConstantF(0, &oldConsts[0][0], 6);
                }
                else
                {
                    const float c0[4] = { hsl.hue, hsl.sat, hsl.light, 1.f };
                    float oldC0[4] = {};
                    dev->GetPixelShaderConstantF(0, oldC0, 1);
                    dev->SetPixelShader(tintPs);
                    dev->SetPixelShaderConstantF(0, c0, 1);
                    gx::Device9(a.device).DrawIndexedPrimitive(
                        a.primType, a.baseVertex, a.minIndex, a.numVerts,
                        a.startIndex, a.primCount);
                    dev->SetPixelShaderConstantF(0, oldC0, 1);
                }
            }
            else
            {
                const int rr = static_cast<int>(Clamp01(hsl.hue) * 255.f + 0.5f);
                const int gg = static_cast<int>(Clamp01(hsl.sat) * 255.f + 0.5f);
                const int bb = static_cast<int>(Clamp01(hsl.light) * 255.f + 0.5f);
                dev->SetPixelShader(nullptr);
                dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, rr, gg, bb));
                dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
                dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                gx::Device9(a.device).DrawIndexedPrimitive(
                    a.primType, a.baseVertex, a.minIndex, a.numVerts,
                    a.startIndex, a.primCount);
            }

            if (!usePs)
            {
                dev->SetTextureStageState(0, D3DTSS_COLOROP, oldColorOp);
                dev->SetTextureStageState(0, D3DTSS_COLORARG1, oldArg1);
                dev->SetTextureStageState(0, D3DTSS_COLORARG2, oldArg2);
                dev->SetTextureStageState(0, D3DTSS_ALPHAOP, oldAlphaOp);
                dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldAArg1);
                dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, oldAArg2);
                dev->SetRenderState(D3DRS_TEXTUREFACTOR, oldTf);
            }
            dev->SetRenderState(D3DRS_ZWRITEENABLE, oldZWrite);
            dev->SetRenderState(D3DRS_ZFUNC, oldZFunc);
            dev->SetPixelShader(oldPs);
            if (oldPs)
                oldPs->Release();
        }
    };

    void InstallPasteHooks()
    {
        const bool okSkinLayout = wxl::core::hook::Install("GearRecolor_PasteSkinLayout",
            wxl::offsets::engine::gx::kCharPasteToSection,
            reinterpret_cast<void*>(&hkPasteSkinLayout),
            reinterpret_cast<void**>(&g_origPasteToSection));
        const bool okItemPaste = wxl::core::hook::Install("GearRecolor_PasteToSection",
            wxl::offsets::engine::gx::kCharPasteFromSkin,
            reinterpret_cast<void*>(&hkPasteToSection),
            reinterpret_cast<void**>(&g_origPasteFromSkin));
        const bool okRenderPrep = wxl::core::hook::Install("GearRecolor_RenderPrep",
            kCharRenderPrep,
            reinterpret_cast<void*>(&hkRenderPrep),
            reinterpret_cast<void**>(&g_origRenderPrep));
        const bool okGetPal = wxl::core::hook::Install("GearRecolor_TexCacheGetPal",
            wxl::offsets::engine::gx::kTextureCacheGetPal,
            reinterpret_cast<void*>(&hkTextureCacheGetPal),
            reinterpret_cast<void**>(&g_origGetPal));
        const bool okGetMip = wxl::core::hook::Install("GearRecolor_TexCacheGetMip",
            wxl::offsets::engine::gx::kTextureCacheGetMip,
            reinterpret_cast<void*>(&hkTextureCacheGetMip),
            reinterpret_cast<void**>(&g_origGetMip));
        WLOG_INFO("gear-recolor: paste hooks skinLayout=%d itemPaste=%d renderPrep=%d "
            "getPal=%d getMip=%d",
            okSkinLayout ? 1 : 0, okItemPaste ? 1 : 0, okRenderPrep ? 1 : 0,
            okGetPal ? 1 : 0, okGetMip ? 1 : 0);
    }

    struct PasteHookInstaller
    {
        PasteHookInstaller()
        {
            wxl::runtime::modules::Register("wxl-gear-recolor-paste", &InstallPasteHooks);
        }
    };

    PasteHookInstaller g_pasteHookInstaller;
    GearRecolor g_gearRecolor;
}
