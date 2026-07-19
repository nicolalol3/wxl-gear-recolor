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
//   WXL_RecolorForceBodyRebuild / WXL_RecolorClearSlot / WXL_RecolorClearAll
//   WXL_RecolorCatchSlotColors(slot) -> n, then n*(r,g,b)  (selective Catch)
//   WXL_RecolorArmScreenSample / GetScreenSample / CancelScreenSample
// State: mode 0 solid single, 1 selective, 2 solid gradient
//   (legacy "slot r g b" still loads as solid single)

#include "core/Logger.hpp"
#include "core/Hook.hpp"
#include "events/EventScript.hpp"
#include "game/gx/Gx.hpp"
#include "game/m2/M2.hpp"
#include "game/unit/Unit.hpp"
#include "game/world/World.hpp"
#include "offsets/engine/Gx.hpp"
#include "runtime/LuaBindings.hpp"
#include "runtime/ModuleInstall.hpp"

#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <string>
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
    // Draft tints: paperdoll/DressUp only until Lua Apply commits to g_slotHsl.
    SlotHsl g_draftHsl[kMaxEquipSlots];
    std::atomic<bool> g_hslPreferDraft{ false };

    constexpr char kStateFile[] = "WarcraftXL_gear-recolor.state";

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
        FILE* f = nullptr;
        if (fopen_s(&f, kStateFile, "w") != 0 || !f)
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

    void LoadHslFromDisk()
    {
        FILE* f = nullptr;
        if (fopen_s(&f, kStateFile, "r") != 0 || !f)
            return;
        int n = 0;
        char line[320];
        while (fgets(line, sizeof(line), f))
        {
            int slot = 0;
            float t0 = 0.f, t1 = 0.f, t2 = 0.f, t3 = 0.f;
            const int nProbe = sscanf_s(line, "%d %f %f %f %f", &slot, &t0, &t1, &t2, &t3);
            if (slot < 0 || slot >= kMaxEquipSlots)
                continue;

            SlotHsl h{};
            h.active = true;

            if (nProbe == 4)
            {
                // Legacy: slot r g b
                if (t0 > 1.01f || t1 > 1.01f || t2 > 1.01f || t2 < -0.01f)
                    continue;
                h.mode = 0;
                h.hue = Clamp01(t0);
                h.sat = Clamp01(t1);
                h.light = Clamp01(t2);
            }
            else
            {
                int mode = 0;
                int count = 0;
                // Chained selective: "slot 1 <count:1..4> sr sg sb dr dg db tol ..."
                // (old "slot 1 destR ..." yields count=0 via %d truncation → legacy path)
                const int nHead = sscanf_s(line, "%d %d %d", &slot, &mode, &count);
                if (nHead == 3 && mode == 2 && (count == 2 || count == 3 || count == 5))
                {
                    // Solid gradient: slot 2 <nStops> <fill> then colors
                    int fill = 0;
                    const char* p = line;
                    for (int tok = 0; tok < 2 && p && *p; ++tok)
                    {
                        while (*p == ' ' || *p == '\t')
                            ++p;
                        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
                            ++p;
                    }
                    int nReadHead = 0;
                    if (sscanf_s(p, " %d %d%n", &count, &fill, &nReadHead) < 2 || nReadHead <= 0)
                        continue;
                    p += nReadHead;
                    h.mode = 2;
                    h.stopCount = static_cast<uint8_t>(count);
                    h.gradFill = (fill != 0) ? 1 : 0;
                    if (h.gradFill == 0)
                    {
                        float br = 0.f, bg = 0.f, bb = 0.f;
                        if (sscanf_s(p, " %f %f %f", &br, &bg, &bb) < 3)
                            continue;
                        h.hue = Clamp01(br);
                        h.sat = Clamp01(bg);
                        h.light = Clamp01(bb);
                    }
                    else
                    {
                        for (int s = 0; s < count; ++s)
                        {
                            float sr = 0.f, sg = 0.f, sb = 0.f;
                            int nRead = 0;
                            if (sscanf_s(p, " %f %f %f%n", &sr, &sg, &sb, &nRead) < 3
                                || nRead <= 0)
                            {
                                h.active = false;
                                break;
                            }
                            p += nRead;
                            h.stops[s][0] = Clamp01(sr);
                            h.stops[s][1] = Clamp01(sg);
                            h.stops[s][2] = Clamp01(sb);
                        }
                        if (!h.active)
                            continue;
                    }
                    ResolveSolidStops(h);
                }
                else if (nHead == 3 && mode == 1 && count >= 1 && count <= kMaxSelRules)
                {
                    h.mode = 1;
                    h.ruleCount = 0;
                    const char* p = line;
                    // skip slot, mode, count tokens
                    for (int tok = 0; tok < 3 && p && *p; ++tok)
                    {
                        while (*p == ' ' || *p == '\t')
                            ++p;
                        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
                            ++p;
                    }
                    for (int r = 0; r < count; ++r)
                    {
                        float sr = 0.f, sg = 0.f, sb = 0.f;
                        float dr = 0.f, dg = 0.f, db = 0.f, tol = 0.35f;
                        int nRead = 0;
                        if (sscanf_s(p, " %f %f %f %f %f %f %f%n",
                                &sr, &sg, &sb, &dr, &dg, &db, &tol, &nRead) < 7 || nRead <= 0)
                            break;
                        p += nRead;
                        SelRule rule{};
                        rule.sr = Clamp01(sr);
                        rule.sg = Clamp01(sg);
                        rule.sb = Clamp01(sb);
                        rule.dr = Clamp01(dr);
                        rule.dg = Clamp01(dg);
                        rule.db = Clamp01(db);
                        rule.tol = Clamp01(tol);
                        if (rule.tol < 0.02f)
                            rule.tol = 0.35f;
                        h.rules[h.ruleCount++] = rule;
                    }
                    if (h.ruleCount == 0)
                        continue;
                    SyncSlotMirrorsFromLastRule(h);
                }
                else
                {
                    float a = 1.f, b = 1.f, c = 1.f, d = 0.f, e = 0.f, f2 = 0.f, tol = 0.35f;
                    const int nNew = sscanf_s(line, "%d %d %f %f %f %f %f %f %f",
                        &slot, &mode, &a, &b, &c, &d, &e, &f2, &tol);
                    if (nNew < 5 || slot < 0 || slot >= kMaxEquipSlots)
                        continue;
                    if (a > 1.01f || b > 1.01f || c > 1.01f)
                        continue;
                    h.mode = (mode == 1) ? 1 : 0;
                    h.hue = Clamp01(a);
                    h.sat = Clamp01(b);
                    h.light = Clamp01(c);
                    if (h.mode == 1)
                    {
                        if (nNew < 9)
                            continue;
                        // Legacy single-rule: dest then src then tol
                        h.srcR = Clamp01(d);
                        h.srcG = Clamp01(e);
                        h.srcB = Clamp01(f2);
                        h.tolerance = Clamp01(tol);
                        if (h.tolerance < 0.02f)
                            h.tolerance = 0.35f;
                        h.rules[0].sr = h.srcR;
                        h.rules[0].sg = h.srcG;
                        h.rules[0].sb = h.srcB;
                        h.rules[0].dr = h.hue;
                        h.rules[0].dg = h.sat;
                        h.rules[0].db = h.light;
                        h.rules[0].tol = h.tolerance;
                        h.ruleCount = 1;
                    }
                }
            }

            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = h;
            ++n;
        }
        fclose(f);
        if (n > 0)
            WLOG_INFO("gear-recolor: loaded {} slot tint(s) from {}", n, kStateFile);
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

    bool TrySlotHsl(int slot, SlotHsl& out)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return false;
        std::lock_guard<std::mutex> lock(g_colorMutex);
        // g_hslPreferDraft is set by preview OC draw / preview CharComponent paste.
        if (g_hslPreferDraft.load(std::memory_order_relaxed) && g_draftHsl[slot].active)
        {
            out = g_draftHsl[slot];
            return true;
        }
        if (!g_slotHsl[slot].active)
            return false;
        out = g_slotHsl[slot];
        return true;
    }

    bool AnyDraftActive()
    {
        std::lock_guard<std::mutex> lock(g_colorMutex);
        for (int i = 0; i < kMaxEquipSlots; ++i)
        {
            if (g_draftHsl[i].active)
                return true;
        }
        return false;
    }

    bool TryHslForPath(const char* stem, SlotHsl& out, int* outSlot = nullptr)
    {
        int cands[4] = { -1, -1, -1, -1 };
        const int nc = CandidateSlotsForOcPath(stem, cands, 4);
        if (nc <= 0)
            return false;
        for (int i = 0; i < nc; ++i)
        {
            if (TrySlotHsl(cands[i], out))
            {
                if (outSlot)
                    *outSlot = cands[i];
                return true;
            }
        }
        return false;
    }

    // --- Player / preview scoping (tint must NOT leak to other units) ---

    std::atomic<void*> g_localPlayerModel{ nullptr };
    std::atomic<void*> g_previewModel{ nullptr };
    std::atomic<void*> g_localPlayerComponent{ nullptr };
    std::atomic<void*> g_previewComponent{ nullptr };
    std::atomic<bool> g_previewUiActive{ false };
    std::atomic<bool> g_pendingEnterWorldRebuild{ true };
    std::atomic<bool> g_insideForcedRebuild{ false };
    std::atomic<int> g_rebuildBatchDepth{ 0 };
    std::atomic<uint32_t> g_deferredFullRebuildAt{ 0 };
    // Natural paste tints since last ClearPlayerScope (char-select / world assemble).
    std::atomic<uint32_t> g_naturalTintPastes{ 0 };
    std::atomic<uint32_t> g_previewCaptureUntil{ 0 };
    std::atomic<bool> g_forceAllowPaste{ false };
    std::atomic<bool> g_assemblingAllowed{ false };
    // Set for the duration of hkRenderPrep / forced rebuild — paste tint must
    // only run when this matches the local player model (never other PCs).
    std::atomic<void*> g_prepModel{ nullptr };
    std::atomic<uint32_t> g_prepReasonCode{ 0 }; // 0 deny, 1 player, 2 sticky, 3 other-allow
    // Once we have matched unit->model == component.model, never use login optimism.
    std::atomic<bool> g_playerModelLocked{ false };
    // Retail: section loop at 0x4EE0D0 does `test [edi+0x0C], (1<<section)`.
    // (whoa +0x04 is wrong here — +0x04 is a list link; +0x08 is m_flags.)
    constexpr size_t kOffComponentSectionDirty = 0x0C;
    // M2Instance+0x10 init flags (WXL M2.hpp); logged on UI root capture.
    constexpr size_t kOffInstInitFlags = 0x10;

    void* ComponentModel(void* component);
    bool AnySlotActive();
    bool ComponentIsNpc(void* component);
    void FlushTexTintState(const char* reason);
    // Char-select one-shot dirty; reset on scope flush so logout→select re-dirties.
    std::atomic<void*> g_charselectDirtyModel{ nullptr };

    // Extra OC roots for paperdoll / char-select / DressUp clones (not world unit+0xB4).
    constexpr int kMaxUiOcRoots = 12;
    std::mutex g_uiRootMu;
    void* g_uiOcRoots[kMaxUiOcRoots] = {};
    int g_uiOcRootN = 0;
    std::atomic<uint32_t> g_uiCaptureUntil{ 0 };
    std::atomic<bool> g_characterUiActive{ false };
    std::atomic<bool> g_pendingPreviewForce{ false };

    void RegisterUiOcRoot(void* root)
    {
        if (!root)
            return;
        std::lock_guard<std::mutex> lock(g_uiRootMu);
        for (int i = 0; i < g_uiOcRootN; ++i)
        {
            if (g_uiOcRoots[i] == root)
                return;
        }
        if (g_uiOcRootN < kMaxUiOcRoots)
            g_uiOcRoots[g_uiOcRootN++] = root;
        else
            g_uiOcRoots[0] = root;
    }

    void ClearUiOcRoots()
    {
        std::lock_guard<std::mutex> lock(g_uiRootMu);
        g_uiOcRootN = 0;
        for (int i = 0; i < kMaxUiOcRoots; ++i)
            g_uiOcRoots[i] = nullptr;
    }

    bool IsUiCaptureArmed()
    {
        return GetTickCount() <= g_uiCaptureUntil.load(std::memory_order_relaxed);
    }

    void ArmUiCapture(uint32_t ms)
    {
        g_uiCaptureUntil.store(GetTickCount() + ms, std::memory_order_relaxed);
    }

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
        const unsigned long long guid = wxl::game::world::ActivePlayerGuid();
        if (!guid)
            return nullptr;
        void* unit = wxl::game::world::ResolveObject(
            guid, wxl::game::world::kTypeMaskUnit | wxl::game::world::kTypeMaskPlayer);
        void* model = wxl::game::unit::Model(unit);
        g_localPlayerModel.store(model, std::memory_order_relaxed);
        return model;
    }

    bool ModelInAllowedTree(void* instance)
    {
        if (!instance)
            return false;

        // World: prefer live local player model root. Never tint OC for units
        // that merely share item paths (same helm/shoulder/weapon as you).
        // Glue / char-select: ActivePlayerGuid is null — still allow the sticky
        // CharacterComponent model so helm/shoulder/weapon tint on enter-world.
        // (Body paste already uses sticky; OC used to miss it after world gate.)
        void* playerModel = LocalPlayerBodyModel();

        // Roots that may own ObjectComponents (helm/shoulder/weapon):
        // unit+0xB4, sticky CharacterComponent.model, DressUp preview, and UI
        // paperdoll / char-select clones captured into g_uiOcRoots.
        void* roots[16] = {};
        int n = 0;
        auto addRoot = [&](void* p) {
            if (!p || n >= 16)
                return;
            for (int i = 0; i < n; ++i)
            {
                if (roots[i] == p)
                    return;
            }
            roots[n++] = p;
        };
        if (playerModel)
        {
            addRoot(playerModel);
            addRoot(g_localPlayerModel.load(std::memory_order_relaxed));
            addRoot(ComponentModel(g_localPlayerComponent.load(std::memory_order_relaxed)));
        }
        else
        {
            // Char-select / enter-world screen only (no world unit yet).
            addRoot(g_localPlayerModel.load(std::memory_order_relaxed));
            addRoot(ComponentModel(g_localPlayerComponent.load(std::memory_order_relaxed)));
        }
        addRoot(g_previewModel.load(std::memory_order_relaxed));
        {
            std::lock_guard<std::mutex> lock(g_uiRootMu);
            for (int i = 0; i < g_uiOcRootN; ++i)
                addRoot(g_uiOcRoots[i]);
        }
        if (n == 0)
            return false;

        void* cur = instance;
        for (int depth = 0; depth < 24 && cur; ++depth)
        {
            for (int i = 0; i < n; ++i)
            {
                if (cur == roots[i])
                    return true;
            }
            cur = wxl::game::unit::ModelParent(cur);
        }
        return false;
    }

    // Paperdoll / shallow UI clones: parent chain often ends at p0 with p1==null.
    bool TryCaptureUiOcRoot(void* instance)
    {
        if (!instance)
            return false;
        const bool uiOpen = g_characterUiActive.load(std::memory_order_relaxed)
            || g_previewUiActive.load(std::memory_order_relaxed)
            || IsUiCaptureArmed();
        if (!uiOpen)
            return false;
        void* p0 = wxl::game::unit::ModelParent(instance);
        if (!p0)
            return false;
        // Only shallow UI trees (paperdoll / DressUp), not world unit graphs.
        if (wxl::game::unit::ModelParent(p0) != nullptr)
            return false;
        void* player = LocalPlayerBodyModel();
        if (player && p0 == player)
            return false;
        RegisterUiOcRoot(p0);
        return true;
    }

    bool ComponentIsNpc(void* component)
    {
        return (SafeReadU32(component, kOffComponentFlags) & kComponentFlagNpc) != 0;
    }

    void* ComponentModel(void* component)
    {
        return SafeReadPtr(component, kOffComponentModel);
    }

    bool IsPasteTintAllowed()
    {
        const bool force = g_forceAllowPaste.load(std::memory_order_relaxed);
        const bool assembling = g_assemblingAllowed.load(std::memory_order_relaxed);
        if (!force && !assembling)
            return false;

        // Hard world gate: even if sticky/optimism wrongly allowed another unit's
        // CharComponent, never mutate TextureCache for non-local assemblies.
        void* pm = LocalPlayerBodyModel();
        void* prep = g_prepModel.load(std::memory_order_relaxed);
        void* previewModel = g_previewModel.load(std::memory_order_relaxed);
        if (!pm)
            return true;
        if (!prep)
            return false;
        if (prep == pm)
            return true;
        // DressUp / transmog preview rebuild while world player exists.
        if (previewModel && prep == previewModel
            && g_previewUiActive.load(std::memory_order_relaxed))
            return true;
        return false;
    }

    void ClearPlayerScope()
    {
        g_localPlayerComponent.store(nullptr, std::memory_order_relaxed);
        g_localPlayerModel.store(nullptr, std::memory_order_relaxed);
        g_previewComponent.store(nullptr, std::memory_order_relaxed);
        g_previewModel.store(nullptr, std::memory_order_relaxed);
        g_playerModelLocked.store(false, std::memory_order_relaxed);
        g_previewCaptureUntil.store(0, std::memory_order_relaxed);
        g_pendingPreviewForce.store(false, std::memory_order_relaxed);
        // World re-entry: prefer natural paste tint (char-select quality), not Force.
        g_pendingEnterWorldRebuild.store(true, std::memory_order_relaxed);
        g_naturalTintPastes.store(0, std::memory_order_relaxed);
        g_deferredFullRebuildAt.store(0, std::memory_order_relaxed);
        ClearUiOcRoots();
        // Logout/relog: TextureCache pointers are freed and reused. Stale g_texTint
        // "orig" + live pastes corrupt the next char-select until client restart.
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
        case 3: case 4: // shirt / chest → torso upper+lower
            return (1u << 3) | (1u << 4);
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

    bool ComponentBelongsToLocalPlayer(void* component)
    {
        if (!component)
            return false;
        void* playerModel = LocalPlayerBodyModel();
        if (!playerModel)
            return false;
        void* cm = ComponentModel(component);
        return cm && cm == playerModel;
    }

    void ArmPreviewCapture(uint32_t ms)
    {
        const uint32_t now = GetTickCount();
        g_previewCaptureUntil.store(now + ms, std::memory_order_relaxed);
        g_previewUiActive.store(true, std::memory_order_relaxed);
    }

    void ForceComponentRebuild(void* component, uint32_t sectionMask)
    {
        if (!component || !g_origRenderPrep || sectionMask == 0)
            return;
        // Stale pointer after logout/relog → native RenderPrep crashes.
        // Char-select has no ActivePlayerGuid — allow sticky local / preview.
        const bool isPreview = (component == g_previewComponent.load(std::memory_order_relaxed));
        const bool isStickyLocal = (component == g_localPlayerComponent.load(std::memory_order_relaxed));
        if (!ComponentBelongsToLocalPlayer(component) && !isPreview && !isStickyLocal)
        {
            return;
        }
        // Preview component may outlive its model after DressUp teardown, or Dress
        // may replace the model pointer — refresh sticky instead of wiping preview.
        if (component == g_previewComponent.load(std::memory_order_relaxed))
        {
            void* cm = ComponentModel(component);
            if (!cm)
            {
                g_previewComponent.store(nullptr, std::memory_order_relaxed);
                g_previewModel.store(nullptr, std::memory_order_relaxed);
                return;
            }
            g_previewModel.store(cm, std::memory_order_relaxed);
        }
        uint32_t dirtyBefore = 0;
        __try
        {
            // GOOD PATH (keep): dirty section bits @ +0x0C then RenderPrep(a2=1).
            // Use a per-slot mask — dirtying 0xFFFFFFFF re-pasted every body piece and
            // caused cross-slot texture influence.
            dirtyBefore = *reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(component) + kOffComponentSectionDirty);
            *reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(component) + kOffComponentSectionDirty) = sectionMask;
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(component) + 0x08) |= 0x1u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        g_forceAllowPaste.store(true, std::memory_order_relaxed);
        void* prepNow = ComponentModel(component);
        g_prepModel.store(prepNow, std::memory_order_relaxed);
        // Preview rebuilds must resolve draft HSL (paperdoll-only until Apply).
        g_hslPreferDraft.store(isPreview, std::memory_order_relaxed);
        __try
        {
            g_origRenderPrep(component, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ClearPlayerScope();
        }
        // Always drop force-allow — a stuck true tints every subsequent paste
        // (other players / skin overlays sharing TextureCache).
        g_forceAllowPaste.store(false, std::memory_order_relaxed);
        g_assemblingAllowed.store(false, std::memory_order_relaxed);
        g_prepModel.store(nullptr, std::memory_order_relaxed);
        g_hslPreferDraft.store(false, std::memory_order_relaxed);
    }

    void ForceAllowedBodyRebuildForSlot(int slot)
    {
        const uint32_t mask = SectionMaskForEquipSlot(slot);
        if (mask == 0)
            return;

        void* playerModel = LocalPlayerBodyModel();
        void* local = g_localPlayerComponent.load(std::memory_order_relaxed);
        void* preview = g_previewComponent.load(std::memory_order_relaxed);

        if (!playerModel)
        {
            // Char-select / glue: keep sticky component — do NOT ClearPlayerScope.
            if (local && ComponentModel(local))
            {
                ForceComponentRebuild(local, mask);
            }
            if (preview && preview != local)
                ForceComponentRebuild(preview, mask);
            if (!local && !preview)
                g_pendingPreviewForce.store(true, std::memory_order_relaxed);
            return;
        }

        if (local && !ComponentBelongsToLocalPlayer(local))
        {
            g_localPlayerComponent.store(nullptr, std::memory_order_relaxed);
            local = nullptr;
        }
        if (local)
            ForceComponentRebuild(local, mask);
        if (preview && preview != local)
            ForceComponentRebuild(preview, mask);
        else if (!preview && g_previewUiActive.load(std::memory_order_relaxed))
            g_pendingPreviewForce.store(true, std::memory_order_relaxed);
    }

    void ForcePreviewOnlyRebuildForSlot(int slot)
    {
        const uint32_t mask = SectionMaskForEquipSlot(slot);
        if (mask == 0)
            return;
        void* preview = g_previewComponent.load(std::memory_order_relaxed);
        if (preview)
            ForceComponentRebuild(preview, mask);
        else if (g_previewUiActive.load(std::memory_order_relaxed))
            g_pendingPreviewForce.store(true, std::memory_order_relaxed);
        ArmPreviewCapture(800);
    }

    // During pushAll / multi-rule SetSlotSelective, skip per-slot rebuilds — they
    // re-paste partial section masks and destroy the clean char-select composite.
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

        const uint32_t nat = g_naturalTintPastes.load(std::memory_order_relaxed);
        const bool hasPlayer = LocalPlayerBodyModel() != nullptr;

        // Boot / enter-world sync: colors only. Forcing a full 0x3FF rebuild after the
        // engine already assembled with live paste tints is what made in-world look
        // worse than char-select Enter World.
        if (!forceRebuild)
            return;

        g_pendingEnterWorldRebuild.store(false, std::memory_order_relaxed);
        g_deferredFullRebuildAt.store(0, std::memory_order_relaxed);
        ForceAllowedBodyRebuildForSlot(-1);
    }

    void EndBodyRebuildBatch()
    {
        EndBodyRebuildBatch(true);
    }

    // Back-compat wrapper used by Lua preview refresh (full body).
    void ForceAllowedBodyRebuild()
    {
        ForceAllowedBodyRebuildForSlot(-1);
    }

    int __fastcall hkRenderPrep(void* component, void* /*edx*/, int a2)
    {
        bool allowed = false;
        void* model = ComponentModel(component);
        void* playerModel = LocalPlayerBodyModel();
        const bool isNpc = ComponentIsNpc(component);
        const uint32_t flags = SafeReadU32(component, kOffComponentFlags);
        void* sticky = g_localPlayerComponent.load(std::memory_order_relaxed);
        const bool locked = g_playerModelLocked.load(std::memory_order_relaxed);
        const char* reason = "deny";

        // Logout / return to glue: ActivePlayerGuid clears. Always flush tex cache
        // here — waiting only on `locked` missed some transitions and left stale
        // g_texTint keyed by freed pointers (restart was the only recovery).
        {
            static std::atomic<bool> s_wasWorldPlayer{ false };
            if (playerModel)
                s_wasWorldPlayer.store(true, std::memory_order_relaxed);
            else if (s_wasWorldPlayer.exchange(false, std::memory_order_relaxed))
                ClearPlayerScope();
        }

        if (component && !isNpc)
        {
            if (playerModel && model && model == playerModel)
            {
                allowed = true;
                reason = "player_model";
                g_localPlayerComponent.store(component, std::memory_order_relaxed);
                g_playerModelLocked.store(true, std::memory_order_relaxed);
            }
            else if (sticky && component == sticky)
            {
                // Stale after logout: sticky must still belong to the live player.
                // Char-select / glue: no ActivePlayerGuid — keep optimism sticky.
                if (playerModel && ComponentModel(sticky) == playerModel)
                {
                    allowed = true;
                    reason = "sticky_component";
                }
                else if (!playerModel && model && model == ComponentModel(sticky))
                {
                    allowed = true;
                    reason = "charselect_sticky";
                    g_localPlayerModel.store(model, std::memory_order_relaxed);
                }
                else if (!playerModel && model)
                {
                    // Component model moved (char re-selected) — retarget sticky.
                    allowed = true;
                    reason = "charselect_retarget";
                    g_localPlayerComponent.store(component, std::memory_order_relaxed);
                    g_localPlayerModel.store(model, std::memory_order_relaxed);
                }
                else
                {
                    g_localPlayerComponent.store(nullptr, std::memory_order_relaxed);
                    reason = "sticky_stale";
                }
            }
            else if (!locked && !playerModel && model && !sticky)
            {
                // Glue one-shot ONLY when we have no sticky yet. Never re-enter
                // optimism for other CharComponents — that tinted every loading
                // unit's TextureComponents (other PCs / shared cache corruption).
                // Logs: localOk:2 reason:3 on full body pastes = this path.
                allowed = true;
                reason = "login_optimism";
                g_localPlayerComponent.store(component, std::memory_order_relaxed);
                g_localPlayerModel.store(model, std::memory_order_relaxed);
            }
            else if (!locked && !playerModel && model && sticky && component != sticky)
            {
                reason = "glue_other_deny";
            }
            else
            {
                const uint32_t now = GetTickCount();
                const uint32_t until = g_previewCaptureUntil.load(std::memory_order_relaxed);
                if (g_previewUiActive.load(std::memory_order_relaxed) && now <= until && model)
                {
                    const void* prevModel = g_previewModel.load(std::memory_order_relaxed);
                    g_previewModel.store(model, std::memory_order_relaxed);
                    g_previewComponent.store(component, std::memory_order_relaxed);
                    allowed = true;
                    reason = "preview_capture";
                    RegisterUiOcRoot(model);
                    const bool pending = g_pendingPreviewForce.exchange(
                        false, std::memory_order_relaxed);
                    if ((pending || model != prevModel) && AnySlotActive())
                    {
                        // Dress often calls RenderPrep(a2=0) with clean sections — force
                        // a body paste when preview model is (re)captured.
                        __try
                        {
                            *reinterpret_cast<uint32_t*>(
                                static_cast<uint8_t*>(component) + kOffComponentSectionDirty)
                                |= SectionMaskForEquipSlot(-1);
                            *reinterpret_cast<uint32_t*>(
                                static_cast<uint8_t*>(component) + 0x08) |= 0x1u;
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER)
                        {
                        }
                    }
                }
                else if (model && model == g_previewModel.load(std::memory_order_relaxed))
                {
                    allowed = true;
                    reason = "preview_model";
                    g_previewComponent.store(component, std::memory_order_relaxed);
                    RegisterUiOcRoot(model);
                }
                else if (!playerModel)
                    reason = "no_player_model";
                else if (!model)
                    reason = "no_comp_model";
                else
                    reason = "model_mismatch";
            }
        }
        else if (!component)
            reason = "no_component";
        else if (isNpc)
            reason = "npc_flag";

        if (!playerModel && locked)
            ClearPlayerScope();

        // Char-select: assembled models often arrive with clean section bits — dirty
        // once per sticky model so body TextureComponents pick up saved colors.
        if (allowed && !playerModel && AnySlotActive() && component && model)
        {
            void* prevDirty = g_charselectDirtyModel.load(std::memory_order_relaxed);
            if (model != prevDirty)
            {
                g_charselectDirtyModel.store(model, std::memory_order_relaxed);
                __try
                {
                    *reinterpret_cast<uint32_t*>(
                        static_cast<uint8_t*>(component) + kOffComponentSectionDirty)
                        |= SectionMaskForEquipSlot(-1);
                    *reinterpret_cast<uint32_t*>(
                        static_cast<uint8_t*>(component) + 0x08) |= 0x1u;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
        }

        // Preview body refresh is driven by capture dirty + WXL_RecolorForceBodyRebuild.


        g_prepModel.store(model, std::memory_order_relaxed);
        {
            uint32_t code = 0;
            if (allowed)
            {
                if (std::strcmp(reason, "player_model") == 0)
                    code = 1;
                else if (std::strcmp(reason, "sticky_component") == 0
                    || std::strcmp(reason, "charselect_sticky") == 0
                    || std::strcmp(reason, "charselect_retarget") == 0)
                    code = 2;
                else if (std::strcmp(reason, "login_optimism") == 0)
                    code = 4;
                else
                    code = 3; // preview / other
            }
            g_prepReasonCode.store(code, std::memory_order_relaxed);
        }
        g_assemblingAllowed.store(allowed, std::memory_order_relaxed);
        const int rc = g_origRenderPrep
            ? g_origRenderPrep(component, a2)
            : 0;
        g_assemblingAllowed.store(false, std::memory_order_relaxed);
        g_prepModel.store(nullptr, std::memory_order_relaxed);
        g_prepReasonCode.store(0, std::memory_order_relaxed);

        // After world player locks: if natural paste already tinted (char-select
        // quality path), do NOT ForceComponentRebuild — that was the "adjustment"
        // that looked worse than Enter World. Only arm a deferred force when zero
        // natural tints were seen (colors applied too late).
        if (allowed && component && AnySlotActive()
            && g_pendingEnterWorldRebuild.load(std::memory_order_relaxed)
            && (std::strcmp(reason, "player_model") == 0
                || std::strcmp(reason, "sticky_component") == 0))
        {
            g_pendingEnterWorldRebuild.store(false, std::memory_order_relaxed);
            const uint32_t nat = g_naturalTintPastes.load(std::memory_order_relaxed);
            if (nat == 0)
                g_deferredFullRebuildAt.store(GetTickCount() + 600u, std::memory_order_relaxed);
            else
                g_deferredFullRebuildAt.store(0, std::memory_order_relaxed);
        }

        const uint32_t due = g_deferredFullRebuildAt.load(std::memory_order_relaxed);
        if (due != 0 && GetTickCount() >= due
            && allowed && component
            && !g_insideForcedRebuild.load(std::memory_order_relaxed)
            && g_rebuildBatchDepth.load(std::memory_order_relaxed) == 0
            && AnySlotActive()
            && (std::strcmp(reason, "player_model") == 0
                || std::strcmp(reason, "sticky_component") == 0))
        {
            g_deferredFullRebuildAt.store(0, std::memory_order_relaxed);
            g_insideForcedRebuild.store(true, std::memory_order_relaxed);
            ForceComponentRebuild(component, SectionMaskForEquipSlot(-1));
            g_insideForcedRebuild.store(false, std::memory_order_relaxed);
        }

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
    wxl::offsets::engine::gx::CharPasteToSectionFn g_origPasteToSection = nullptr;
    wxl::offsets::engine::gx::CharPasteToSectionFn g_origPasteFromSkin = nullptr;

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

    // Remember recent item pastes so HSL slider changes can re-tint composites live
    // (DressUpModel preview + world body) without equip/unequip.
    struct LivePaste
    {
        int section = 0;
        void* src = nullptr;
        void* dst = nullptr;
        int slot = -1;
    };
    constexpr int kMaxLivePastes = 96;
    std::mutex g_livePasteMu;
    LivePaste g_livePastes[kMaxLivePastes];
    int g_livePasteN = 0;

    void RememberLivePaste(int section, void* src, void* dst, int slot)
    {
        // slot >= 0: item component; slot == -2: base skin layout layer
        if (!src || !dst || slot < -2 || slot == -1)
            return;
        std::lock_guard<std::mutex> lock(g_livePasteMu);
        // One entry per (section, src, dst) layer. NEVER collapse by section alone —
        // CharAssemble pastes pant+belt into the same section; keeping only the last
        // made slider replay replace pants with boots (etc.).
        for (int i = 0; i < g_livePasteN; ++i)
        {
            if (g_livePastes[i].dst == dst
                && g_livePastes[i].section == section
                && g_livePastes[i].src == src)
            {
                g_livePastes[i].slot = slot;
                return;
            }
        }
        if (g_livePasteN < kMaxLivePastes)
        {
            g_livePastes[g_livePasteN++] = { section, src, dst, slot };
            return;
        }
        static int s_rot = 0;
        g_livePastes[s_rot % kMaxLivePastes] = { section, src, dst, slot };
        ++s_rot;
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

    std::atomic<bool> g_ocPixelTintLive{ false }; // unused: mesh always used for OC live
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

    bool TryHslForComponentTexture(const char* name, SlotHsl& out, int* outSlot)
    {
        const int mapped = SlotForComponentTexture(name);
        if (mapped >= 0 && TrySlotHsl(mapped, out))
        {
            if (outSlot)
                *outSlot = mapped;
            return true;
        }
        // Shirt (3) may drive chest (4) components when chest HSL is inactive.
        if (mapped == 4 && TrySlotHsl(3, out))
        {
            if (outSlot)
                *outSlot = 3;
            return true;
        }
        return false;
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
        std::lock_guard<std::mutex> lock(g_colorMutex);
        for (int i = 0; i < kMaxEquipSlots; ++i)
        {
            if (g_slotHsl[i].active || g_draftHsl[i].active)
                return true;
        }
        return false;
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
        auto fn = reinterpret_cast<wxl::offsets::engine::gx::TextureCacheGetPalFn>(
            wxl::offsets::engine::gx::kTextureCacheGetPal);
        __try { return fn(tex); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    uint8_t* SafeGetMip(void* tex, uint32_t mip) noexcept
    {
        auto fn = reinterpret_cast<wxl::offsets::engine::gx::TextureCacheGetMipFn>(
            wxl::offsets::engine::gx::kTextureCacheGetMip);
        __try { return fn(tex, mip); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void FlushTexTintState(const char* reason)
    {
        size_t cacheN = 0;
        int liveN = 0;
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
        }
        {
            std::lock_guard<std::mutex> lock(g_livePasteMu);
            liveN = g_livePasteN;
            g_livePasteN = 0;
        }
        g_charselectDirtyModel.store(nullptr, std::memory_order_relaxed);
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

    bool TintTextureCacheForPaste(void* tex, const char* name, const SlotHsl& hsl,
                                  int slot, const char** outMode)
    {
        if (outMode)
            *outMode = "none";
        if (!tex || !hsl.active || IsIdentityHsl(hsl))
            return false;

        const int has = SafeHasMips(tex);
        if (has <= 0)
        {
            if (outMode)
                *outMode = (has < 0) ? "hasmips_fault" : "no_mips";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            TexTintCache& st = g_texTint[tex];
            if (st.path.empty() && name)
                st.path = name;
        }

        uint8_t* pal = SafeGetPal(tex);
        if (pal)
        {
            // Keep mip indices; tint the 256 palette from the cached ORIGINAL only.
            // (Spatial expand/despeckle+index-remap was REJECTED: remapped thousands of
            // pixels per paste and caused severe graphic corruption — see body-path logs.)
            if (outMode)
                *outMode = "palette_rgb";
            uint8_t info[8] = {};
            uint32_t w = 0, h = 0;
            if (SafeGetInfo(tex, info, 1))
            {
                w = *reinterpret_cast<uint16_t*>(info + 0);
                h = *reinterpret_cast<uint16_t*>(info + 2);
            }
            uint8_t* mip0 = SafeGetMip(tex, 0);
            {
                std::lock_guard<std::mutex> lock(g_texMutex);
                TexTintCache& st = g_texTint[tex];
                // Pointer reuse after logout: same void* may now be a different BLP.
                if (!st.orig.empty() && name && !st.path.empty()
                    && _stricmp(st.path.c_str(), name) != 0)
                {
                    st = {};
                }
                if (st.path.empty() && name)
                    st.path = name;
                if (st.orig.empty())
                {
                    st.paletted = true;
                    st.w = w;
                    st.h = h;
                    st.orig.assign(pal, pal + 256 * 4);
                    if (mip0 && w && h && w <= 2048 && h <= 2048)
                        st.origIdx.assign(mip0, mip0 + static_cast<size_t>(w) * h);
                }
                else if (st.origIdx.empty() && mip0 && w && h && w <= 2048 && h <= 2048)
                {
                    st.w = w;
                    st.h = h;
                    st.origIdx.assign(mip0, mip0 + static_cast<size_t>(w) * h);
                }
                if (st.orig.size() < 256 * 4)
                    return false;
                // Always re-tint from orig (never stack on already-tinted pal).
                // Do NOT rewrite mip indices.
                std::memcpy(pal, st.orig.data(), 256 * 4);
                // Selective on palette: slightly harder gate than OC to limit fringe
                // speckles without touching indices (soft despeckle cannot run on 256).
                if (hsl.mode == 1)
                {
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
                        }
                        else
                        {
                            for (uint8_t ri = 0; ri < hsl.ruleCount; ++ri)
                                wmax = (std::max)(wmax, SelectiveWeight(r, g, b, hsl.rules[ri]));
                        }
                        // Harder than mesh PS: skip weak palette fringe entries.
                        if (wmax < 0.45f)
                            continue;
                        if (hsl.ruleCount == 0)
                        {
                            SelRule one{};
                            one.sr = hsl.srcR; one.sg = hsl.srcG; one.sb = hsl.srcB;
                            one.dr = hsl.hue; one.dg = hsl.sat; one.db = hsl.light;
                            one.tol = hsl.tolerance;
                            ApplySelectiveWithWeight(r, g, b, one, wmax);
                        }
                        else
                        {
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
                    HslBgraBuffer(pal, 256, hsl, false);
                }
            }
            return true;
        }

        uint8_t info[8] = {};
        if (!SafeGetInfo(tex, info, 1))
        {
            if (outMode)
                *outMode = "no_info";
            return false;
        }
        const uint32_t w = *reinterpret_cast<uint16_t*>(info + 0);
        const uint32_t h = *reinterpret_cast<uint16_t*>(info + 2);
        uint8_t* mip0 = SafeGetMip(tex, 0);
        if (!mip0 || !w || !h || w > 2048 || h > 2048)
        {
            if (outMode)
                *outMode = "no_mip0";
            return false;
        }
        if (outMode)
            *outMode = "bgra";
        const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
        std::lock_guard<std::mutex> lock(g_texMutex);
        TexTintCache& st = g_texTint[tex];
        if (!st.orig.empty() && name && !st.path.empty()
            && _stricmp(st.path.c_str(), name) != 0)
        {
            st = {};
        }
        if (st.path.empty() && name)
            st.path = name;
        if (st.orig.empty())
        {
            st.paletted = false;
            st.w = w;
            st.h = h;
            st.orig.assign(mip0, mip0 + bytes);
        }
        if (st.orig.size() < bytes)
            return false;
        std::memcpy(mip0, st.orig.data(), bytes);
        HslBgraBuffer(mip0, static_cast<size_t>(w) * static_cast<size_t>(h), hsl, true, w, h);
        return true;
    }

    bool RestoreTextureCacheOrig(void* tex)
    {
        if (!tex)
            return false;
        std::lock_guard<std::mutex> lock(g_texMutex);
        auto it = g_texTint.find(tex);
        if (it == g_texTint.end() || it->second.orig.empty())
            return false;
        TexTintCache& st = it->second;
        if (st.paletted)
        {
            uint8_t* pal = SafeGetPal(tex);
            if (!pal || st.orig.size() < 256 * 4)
                return false;
            std::memcpy(pal, st.orig.data(), 256 * 4);
            return true;
        }
        uint8_t* mip0 = SafeGetMip(tex, 0);
        if (!mip0 || st.orig.empty())
            return false;
        std::memcpy(mip0, st.orig.data(), st.orig.size());
        return true;
    }

    bool SlotMatchesLivePaste(int wantSlot, int pasteSlot)
    {
        if (wantSlot < 0)
            return true;
        if (pasteSlot == wantSlot)
            return true;
        // Shirt/chest only — never pull sleeves (8) or gloves into chest replay.
        if ((wantSlot == 4 || wantSlot == 3) && (pasteSlot == 4 || pasteSlot == 3))
            return true;
        return false;
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

    void ReplayLivePastesForSlot(int slot)
    {
        // Body sections are multi-layer (pant+belt, bracer+glove_al, pant_ll+boot_ll).
        // Replaying only the changed slot left other layers missing → wrong texture
        // appearing in that section. Always rebuild the full ordered paste list.
        const bool bodySlot = (slot < 0)
            || slot == 3 || slot == 4 || slot == 5
            || slot == 6 || slot == 7 || slot == 8 || slot == 9;
        if (!bodySlot)
            return;

        // Replay runs outside RenderPrep — force-allow so tint applies, then restore
        // TextureCache so world units never see our modified shared sources.
        std::lock_guard<std::recursive_mutex> gate(g_pasteGateMu);
        void* pm = LocalPlayerBodyModel();
        if (!pm)
            pm = ComponentModel(g_localPlayerComponent.load(std::memory_order_relaxed));
        g_prepModel.store(pm, std::memory_order_relaxed);
        g_forceAllowPaste.store(true, std::memory_order_relaxed);

        LivePaste copy[kMaxLivePastes];
        int n = 0;
        {
            std::lock_guard<std::mutex> lock(g_livePasteMu);
            for (int i = 0; i < g_livePasteN; ++i)
                copy[n++] = g_livePastes[i];
        }

        int ok = 0;
        int fail = 0;
        for (int i = 0; i < n; ++i)
        {
            // Skin layout (-2): paste untinted to reset the section base.
            if (copy[i].slot == -2)
            {
                if (SafeCallPaste(g_origPasteToSection, copy[i].section, copy[i].src, copy[i].dst))
                    ++ok;
                else
                    ++fail;
                continue;
            }

            const char* name = TextureName(copy[i].src);
            SlotHsl hsl{};
            int mapped = -1;
            const bool has = name && TryHslForComponentTexture(name, hsl, &mapped);
            RestoreTextureCacheOrig(copy[i].src);
            if (has)
                TintTextureCacheForPaste(copy[i].src, name, hsl, mapped, nullptr);

            if (SafeCallPaste(g_origPasteFromSkin, copy[i].section, copy[i].src, copy[i].dst))
                ++ok;
            else
                ++fail;

            // Unconditional restore after every tinted replay paste.
            RestoreTextureCacheOrig(copy[i].src);
        }

        g_forceAllowPaste.store(false, std::memory_order_relaxed);
        g_assemblingAllowed.store(false, std::memory_order_relaxed);
        g_prepModel.store(nullptr, std::memory_order_relaxed);
    }

    void __cdecl hkPasteSkinLayout(int section, void* srcTexture, void* dstMips)
    {
        // 0x4F07D0: base skin laid into every body section — NEVER tint here.
        // Only track layers while assembling the local player / DressUp preview.
        std::lock_guard<std::recursive_mutex> gate(g_pasteGateMu);
        if (srcTexture && dstMips && IsPasteTintAllowed())
            RememberLivePaste(section, srcTexture, dstMips, -2);
        if (g_origPasteToSection)
            g_origPasteToSection(section, srcTexture, dstMips);
    }

    void __cdecl hkPasteToSection(int section, void* srcTexture, void* dstMips)
    {
        // 0x4F08A0: items / face / hair overlays — tint TextureComponents only for
        // the local player (or armed DressUp preview). Always restore the shared
        // TextureCache after paste so other characters never inherit our palette.
        std::lock_guard<std::recursive_mutex> gate(g_pasteGateMu);
        const bool allow = IsPasteTintAllowed();
        bool didTint = false;
        const char* tintName = nullptr;
        if (srcTexture)
        {
            const char* name = TextureName(srcTexture);
            if (IsItemComponentTexture(name))
            {
                SlotHsl hsl{};
                int slot = -1;
                const bool hasHsl = TryHslForComponentTexture(name, hsl, &slot);
                if (slot < 0)
                    slot = SlotForComponentTexture(name);
                if (allow && slot >= 0)
                {
                    RememberLivePaste(section, srcTexture, dstMips, slot);
                    // Catch bank must match /recolor: only local-player pastes.
                    // DressUp / transmog preview rebuilds must NOT overwrite the sample
                    // (last-write would make Tints Catch ≠ /recolor Catch for the same slot).
                    void* prep = g_prepModel.load(std::memory_order_relaxed);
                    void* pm = LocalPlayerBodyModel();
                    const bool playerCatch = (pm && prep == pm)
                        || (!pm); // char-select / no world player: keep prior behavior
                    if (playerCatch)
                        CaptureCatchFromTextureCache(srcTexture, slot, name);
                }
                if (allow && hasHsl)
                {
                    const bool ok = TintTextureCacheForPaste(srcTexture, name, hsl, slot, nullptr);
                    didTint = ok;
                    tintName = name;
                    if (ok && !g_forceAllowPaste.load(std::memory_order_relaxed))
                        g_naturalTintPastes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (g_origPasteFromSkin)
            g_origPasteFromSkin(section, srcTexture, dstMips);
        if (didTint)
        {
            const bool restored = RestoreTextureCacheOrig(srcTexture);
            int mismatch = -1;
            if (restored && srcTexture)
            {
                std::lock_guard<std::mutex> lock(g_texMutex);
                auto it = g_texTint.find(srcTexture);
                if (it != g_texTint.end() && !it->second.orig.empty())
                {
                    if (it->second.paletted)
                    {
                        uint8_t* pal = SafeGetPal(srcTexture);
                        mismatch = (pal && std::memcmp(pal, it->second.orig.data(),
                            (std::min)(it->second.orig.size(), size_t{ 256 * 4 })) == 0) ? 0 : 1;
                    }
                    else
                    {
                        uint8_t* mip0 = SafeGetMip(srcTexture, 0);
                        mismatch = (mip0 && std::memcmp(mip0, it->second.orig.data(),
                            it->second.orig.size()) == 0) ? 0 : 1;
                    }
                }
            }
        }
    }

    void NotifyOcSlotChanged(int slot)
    {
        if (slot == 0 || slot == 2 || slot == 15 || slot == 16 || slot == 17)
            g_ocPixelTintLive.store(false, std::memory_order_relaxed);
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
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = h;
        }
        NotifyOcSlotChanged(slot);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            ArmPreviewCapture(800);
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveHslToDisk();
    }

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
        NotifyOcSlotChanged(slot);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            ArmPreviewCapture(800);
        if (IsBodyEquipSlot(slot))
            ForcePreviewOnlyRebuildForSlot(slot);
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
        NotifyOcSlotChanged(slot);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            ArmPreviewCapture(800);
        if (IsBodyEquipSlot(slot))
            ForcePreviewOnlyRebuildForSlot(slot);
    }

    // Selective draft: paperdoll/DressUp only until ApplyDraft commits to g_slotHsl.
    void SetSlotDraftSelective(int slot, const SelRule* rules, int ruleCount)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        if (!rules || ruleCount <= 0)
        {
            {
                std::lock_guard<std::mutex> lock(g_colorMutex);
                g_draftHsl[slot] = {};
            }
            NotifyOcSlotChanged(slot);
            if (g_previewUiActive.load(std::memory_order_relaxed))
                ArmPreviewCapture(800);
            if (IsBodyEquipSlot(slot))
                ForcePreviewOnlyRebuildForSlot(slot);
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
        NotifyOcSlotChanged(slot);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            ArmPreviewCapture(800);
        if (IsBodyEquipSlot(slot))
            ForcePreviewOnlyRebuildForSlot(slot);
    }

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
        NotifyOcSlotChanged(slot);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            ArmPreviewCapture(800);
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        else
            ForcePreviewOnlyRebuildForSlot(slot);
        SaveHslToDisk();
    }

    void ResetSlotTint(int slot)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = {};
            g_draftHsl[slot] = {};
        }
        NotifyOcSlotChanged(slot);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            ArmPreviewCapture(800);
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        else
            ForcePreviewOnlyRebuildForSlot(slot);
        SaveHslToDisk();
    }

    // nStops: 2, 3, or 5. fill: 0=auto from colors[0], 1=custom colors[0..nStops).
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
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = h;
        }
        NotifyOcSlotChanged(slot);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            ArmPreviewCapture(800);
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveHslToDisk();
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
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            h = g_slotHsl[slot];
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
                // Same source as last rule → only update destination / tol.
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
                // Drop oldest, append newest (max 4 chained replaces).
                for (int i = 0; i < kMaxSelRules - 1; ++i)
                    h.rules[i] = h.rules[i + 1];
                h.rules[kMaxSelRules - 1] = neu;
                h.ruleCount = kMaxSelRules;
                action = "shift_append";
            }
            SyncSlotMirrorsFromLastRule(h);
            g_slotHsl[slot] = h;
        }
        NotifyOcSlotChanged(slot);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            ArmPreviewCapture(800);
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveHslToDisk();
    }

    void ClearSlotHsl(int slot)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = {};
        }
        NotifyOcSlotChanged(slot);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            ArmPreviewCapture(800);
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveHslToDisk();
    }

    void ClearAllHsl()
    {
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            for (int i = 0; i < kMaxEquipSlots; ++i)
                g_slotHsl[i] = {};
        }
        g_ocPixelTintLive.store(false, std::memory_order_relaxed);
        RequestBodyRebuildForSlot(-1);
        SaveHslToDisk();
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
            const int mapped = SlotForComponentTexture(p);
            if (mapped != slot && !SlotMatchesLivePaste(slot, mapped))
                return;
            int score = ScoreTexPathForSlot(p, slot);
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
            std::lock_guard<std::mutex> lock(g_livePasteMu);
            for (int i = 0; i < g_livePasteN; ++i)
            {
                void* tex = g_livePastes[i].src;
                const char* name = TextureName(tex);
                std::lock_guard<std::mutex> tlock(g_texMutex);
                auto it = g_texTint.find(tex);
                if (it != g_texTint.end())
                    consider(tex, name, it->second);
            }
        }
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
        for (auto& kv : g_texNames)
        {
            if (!IsItemComponentTexture(kv.second.c_str()))
                continue;
            if (SlotForComponentTexture(kv.second.c_str()) != slot)
                continue;
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
            {
                std::lock_guard<std::mutex> lock(g_texMutex);
                for (auto& kv : g_texNames)
                {
                    const char* p = kv.second.c_str();
                    if (!IsItemComponentTexture(p))
                        continue;
                    const int mapped = SlotForComponentTexture(p);
                    if (!SlotMatchesLivePaste(slot, mapped))
                        continue;
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

    // WXL_RecolorSetSlotSelectiveDraft(slot, nRules [, sr,sg,sb, dr,dg,db, tol]...)
    // nRules=0 clears the selective draft for preview.
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

    int __cdecl LuaRecolorSetPreviewActive(void* state)
    {
        const bool active = state && wlua::ToNumber(state, 1) != 0.0;
        g_previewUiActive.store(active, std::memory_order_relaxed);
        if (!active)
        {
            g_previewCaptureUntil.store(0, std::memory_order_relaxed);
            g_previewModel.store(nullptr, std::memory_order_relaxed);
            g_previewComponent.store(nullptr, std::memory_order_relaxed);
        }
        else
            ArmPreviewCapture(1500);
        if (active)
            ArmUiCapture(1500);
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorArmPreviewCapture(void* state)
    {
        ArmPreviewCapture(800);
        ArmUiCapture(800);
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorArmUiCapture(void* state)
    {
        ArmUiCapture(2000);
        g_characterUiActive.store(true, std::memory_order_relaxed);
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
    }

    int __cdecl LuaRecolorSetCharacterUiActive(void* state)
    {
        const bool active = state && wlua::ToNumber(state, 1) != 0.0;
        g_characterUiActive.store(active, std::memory_order_relaxed);
        if (active)
            ArmUiCapture(2000);
        else
            ClearUiOcRoots();
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
            wlua::RegisterFunction("WXL_RecolorSetSlotSelectiveDraft", &LuaRecolorSetSlotSelectiveDraft);
            wlua::RegisterFunction("WXL_RecolorApplyDraft", &LuaRecolorApplyDraft);
            wlua::RegisterFunction("WXL_RecolorResetTint", &LuaRecolorResetTint);
            wlua::RegisterFunction("WXL_RecolorGetSlotGradient", &LuaRecolorGetSlotGradient);
            wlua::RegisterFunction("WXL_RecolorSetSlotSelective", &LuaRecolorSetSlotSelective);
            wlua::RegisterFunction("WXL_RecolorCatchSlotColors", &LuaRecolorCatchSlotColors);
            wlua::RegisterFunction("WXL_RecolorGetSlot", &LuaRecolorGetSlot);
            wlua::RegisterFunction("WXL_RecolorGetSlotTexPath", &LuaRecolorGetSlotTexPath);
            wlua::RegisterFunction("WXL_RecolorBakePreview", &LuaRecolorBakePreview);
            wlua::RegisterFunction("WXL_RecolorClearSlot", &LuaRecolorClearSlot);
            wlua::RegisterFunction("WXL_RecolorClearAll", &LuaRecolorClearAll);
            wlua::RegisterFunction("WXL_RecolorSetPreviewActive", &LuaRecolorSetPreviewActive);
            wlua::RegisterFunction("WXL_RecolorArmPreviewCapture", &LuaRecolorArmPreviewCapture);
            wlua::RegisterFunction("WXL_RecolorArmUiCapture", &LuaRecolorArmUiCapture);
            wlua::RegisterFunction("WXL_RecolorSetCharacterUiActive", &LuaRecolorSetCharacterUiActive);
            wlua::RegisterFunction("WXL_RecolorForceBodyRebuild", &LuaRecolorForceBodyRebuild);
            wlua::RegisterFunction("WXL_RecolorBeginBatch", &LuaRecolorBeginBatch);
            wlua::RegisterFunction("WXL_RecolorEndBatch", &LuaRecolorEndBatch);
            wlua::RegisterFunction("WXL_RecolorFlushTex", &LuaRecolorFlushTex);
            wlua::RegisterFunction("WXL_RecolorArmScreenSample", &LuaRecolorArmScreenSample);
            wlua::RegisterFunction("WXL_RecolorCancelScreenSample", &LuaRecolorCancelScreenSample);
            wlua::RegisterFunction("WXL_RecolorGetScreenSample", &LuaRecolorGetScreenSample);

            LoadHslFromDisk();

            on<&GearRecolor::OnBatchDraw>(ev::Event::OnM2BatchDraw);
            on<&GearRecolor::OnBlpLoad>(ev::Event::OnBlpLoad);
            on<&GearRecolor::OnTextureUpload>(ev::Event::OnTextureUpload);
            on<&GearRecolor::OnInput>(ev::Event::OnInput);
            on<&GearRecolor::OnEndScene>(ev::Event::OnEndScene);
            WLOG_INFO("gear-recolor: events bound (paste hooks via modules::Register)");
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

            // Phase 0: sample OC albedo for Catch (bound texture, pre tint redraw).
            // Phase 1: live mesh tint after native DIP.
            if (a.phase == 0)
            {
                bool allowed = ModelInAllowedTree(a.model);
                if (!allowed)
                    allowed = TryCaptureUiOcRoot(a.model) && ModelInAllowedTree(a.model);
                if (!allowed)
                    return;
                void* model = ResolveM2Model(a.model);
                const char* stem = model ? m2::PathStem(model) : nullptr;
                if (!PathLooksValid(stem))
                    return;
                int cands[4] = {};
                const int nc = CandidateSlotsForOcPath(stem, cands, 4);
                if (nc <= 0)
                    return;
                auto* dev = static_cast<IDirect3DDevice9*>(a.device);
                for (int i = 0; i < nc; ++i)
                    QueueOcCatchFromDevice(dev, cands[i], stem);
                return;
            }

            if (a.phase != 1 || !AnySlotActive())
                return;

            // Drop stale paperdoll OC roots when no UI is open — freed pointers can
            // be reused by world units and would wrongly pass ModelInAllowedTree.
            if (!g_characterUiActive.load(std::memory_order_relaxed)
                && !g_previewUiActive.load(std::memory_order_relaxed)
                && !IsUiCaptureArmed())
            {
                if (g_uiOcRootN > 0)
                    ClearUiOcRoots();
            }

            // ObjectComponents (helm/shoulder/weapon): only tint meshes under the
            // local player or the armed DressUp preview — never other PCs/NPCs.
            if (!ModelInAllowedTree(a.model))
            {
                // Paperdoll / DressUp: shallow UI roots registered on first OC draw.
                if (!TryCaptureUiOcRoot(a.model) || !ModelInAllowedTree(a.model))
                    return;
            }

            void* model = ResolveM2Model(a.model);
            const char* stem = model ? m2::PathStem(model) : nullptr;
            if (!PathLooksValid(stem))
                return;

            // Draft tints only affect DressUp / UI preview meshes — not the world player.
            bool previewDraw = false;
            if (g_previewUiActive.load(std::memory_order_relaxed))
            {
                void* player = LocalPlayerBodyModel();
                void* preview = g_previewModel.load(std::memory_order_relaxed);
                void* cur = a.model;
                for (int depth = 0; depth < 24 && cur; ++depth)
                {
                    if (player && cur == player)
                    {
                        previewDraw = false;
                        break;
                    }
                    if (preview && cur == preview)
                    {
                        previewDraw = true;
                        break;
                    }
                    bool uiHit = false;
                    {
                        std::lock_guard<std::mutex> lock(g_uiRootMu);
                        for (int i = 0; i < g_uiOcRootN; ++i)
                        {
                            if (cur == g_uiOcRoots[i])
                            {
                                uiHit = true;
                                break;
                            }
                        }
                    }
                    if (uiHit)
                    {
                        previewDraw = true;
                        break;
                    }
                    cur = wxl::game::unit::ModelParent(cur);
                }
            }
            g_hslPreferDraft.store(previewDraw, std::memory_order_relaxed);

            SlotHsl hsl{};
            int usedSlot = -1;
            const bool got = TryHslForPath(stem, hsl, &usedSlot);
            g_hslPreferDraft.store(false, std::memory_order_relaxed);
            if (!got)
                return;


            // Always live mesh for OC — upload bake is one-shot and skips live Hue.
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
            }
        }

        void OnTextureUpload(const ev::TextureUploadArgs& a)
        {
            // Capture OC albedo pixels from the live mip table (TextureCreate path —
            // SafeGetPal does not work on these handles). Fired AFTER native upload.
            if (!a.texture || !a.width || !a.height)
                return;
            // Preview OC draws must not clobber the Catch bank used by /recolor.
            if (g_previewUiActive.load(std::memory_order_relaxed)
                && g_hslPreferDraft.load(std::memory_order_relaxed))
                return;
            const char* name = TextureName(a.texture);
            if (!IsObjectComponentAlbedo(name))
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
        WLOG_INFO("gear-recolor: paste hooks skinLayout=%d itemPaste=%d renderPrep=%d",
            okSkinLayout ? 1 : 0, okItemPaste ? 1 : 0, okRenderPrep ? 1 : 0);
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
