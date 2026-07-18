// wxl-gear-recolor: per-slot RGB colorize via addon ColorPicker.
// Copyright (C) 2026. GPLv3 (see WarcraftXL LICENSE).
//
// Lua API:
//   WXL_RecolorSetSlot(slot, r, g, b)   rgb 0..1
//   WXL_RecolorGetSlot(slot) -> r, g, b, active
//   WXL_RecolorClearSlot / WXL_RecolorClearAll
// State file: slot r g b  (legacy HSL rows with values >1 are ignored)

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

    // Fields named hue/sat/light for legacy call sites; values are RGB 0..1.
    struct SlotHsl
    {
        bool active = false;
        float hue = 1.f;   // R
        float sat = 1.f;   // G
        float light = 1.f; // B
    };


    std::mutex g_colorMutex;
    SlotHsl g_slotHsl[kMaxEquipSlots];

    constexpr char kStateFile[] = "WarcraftXL_gear-recolor.state";

    float Clamp01(float v)
    {
        if (v < 0.f)
            return 0.f;
        if (v > 1.f)
            return 1.f;
        return v;
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
    void ApplyHslPixel(float& r, float& g, float& b, const SlotHsl& c)
    {
        if (!c.active)
            return;
        const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
        r = Clamp01(lum * c.hue);
        g = Clamp01(lum * c.sat);
        b = Clamp01(lum * c.light);
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
            fprintf(f, "%d %.6f %.6f %.6f\n", i, h.hue, h.sat, h.light);
        }
        fclose(f);
    }

    void LoadHslFromDisk()
    {
        FILE* f = nullptr;
        if (fopen_s(&f, kStateFile, "r") != 0 || !f)
            return;
        int n = 0;
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            int slot = 0;
            float r = 1.f, g = 1.f, b = 1.f;
            if (sscanf_s(line, "%d %f %f %f", &slot, &r, &g, &b) < 4)
                continue;
            if (slot < 0 || slot >= kMaxEquipSlots)
                continue;
            // Ignore legacy HSL state (hue often 0..360).
            if (r > 1.01f || g > 1.01f || b > 1.01f || b < -0.01f)
                continue;
            SlotHsl h{};
            h.active = true;
            h.hue = Clamp01(r);
            h.sat = Clamp01(g);
            h.light = Clamp01(b);
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = h;
            ++n;
        }
        fclose(f);
        if (n > 0)
            WLOG_INFO("gear-recolor: loaded {} slot RGB(s) from {}", n, kStateFile);
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
        if (!g_slotHsl[slot].active)
            return false;
        out = g_slotHsl[slot];
        return true;
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
    std::atomic<uint32_t> g_previewCaptureUntil{ 0 };
    std::atomic<bool> g_forceAllowPaste{ false };
    std::atomic<bool> g_assemblingAllowed{ false };
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
        addRoot(LocalPlayerBodyModel());
        addRoot(g_localPlayerModel.load(std::memory_order_relaxed));
        addRoot(ComponentModel(g_localPlayerComponent.load(std::memory_order_relaxed)));
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
        if (g_forceAllowPaste.load(std::memory_order_relaxed))
            return true;
        return g_assemblingAllowed.load(std::memory_order_relaxed);
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
        ClearUiOcRoots();
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
        __try
        {
            g_origRenderPrep(component, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ClearPlayerScope();
        }
        g_forceAllowPaste.store(false, std::memory_order_relaxed);
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
            else if (!locked && !playerModel && model)
            {
                // Login/load only — never keep optimism after a locked world session.
                allowed = true;
                reason = "login_optimism";
                g_localPlayerComponent.store(component, std::memory_order_relaxed);
                g_localPlayerModel.store(model, std::memory_order_relaxed);
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
            static void* s_charselectDirtyModel = nullptr;
            if (model != s_charselectDirtyModel)
            {
                s_charselectDirtyModel = model;
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


        g_assemblingAllowed.store(allowed, std::memory_order_relaxed);
        const int rc = g_origRenderPrep
            ? g_origRenderPrep(component, a2)
            : 0;
        g_assemblingAllowed.store(false, std::memory_order_relaxed);
        return rc;
    }

    // --- Paste hooks: tint TextureComponents via TexTintCache orig backup ---

    std::mutex g_texMutex;
    std::unordered_map<void*, std::string> g_texNames;

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
            if (g_slotHsl[i].active)
                return true;
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

    int SafeGetInfo(void* tex, void* info, int flag) noexcept
    {
        auto fn = reinterpret_cast<wxl::offsets::engine::gx::TextureCacheGetInfoFn>(
            wxl::offsets::engine::gx::kTextureCacheGetInfo);
        __try { return fn(tex, info, flag); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    void HslBgraBuffer(uint8_t* pixels, size_t count, const SlotHsl& hsl, bool skipLowAlpha)
    {
        if (!pixels || !count || !hsl.active || IsIdentityHsl(hsl))
            return;
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
        HslBgraBuffer(pixels, static_cast<size_t>(width) * static_cast<size_t>(height), hsl, true);
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
            // Remapping indices (requant) destroys authored metal dither.
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
            int changed = 0;
            {
                std::lock_guard<std::mutex> lock(g_texMutex);
                TexTintCache& st = g_texTint[tex];
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
                // Do NOT rewrite mip indices — that swapped/corrupted layer looks.
                std::memcpy(pal, st.orig.data(), 256 * 4);
                HslBgraBuffer(pal, 256, hsl, false);
                for (int i = 0; i < 256; ++i)
                {
                    if (std::memcmp(pal + i * 4, st.orig.data() + i * 4, 3) != 0)
                        ++changed;
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
        HslBgraBuffer(mip0, static_cast<size_t>(w) * static_cast<size_t>(h), hsl, true);
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

            RestoreTextureCacheOrig(copy[i].src);
        }

        g_forceAllowPaste.store(false, std::memory_order_relaxed);

    }

    void __cdecl hkPasteSkinLayout(int section, void* srcTexture, void* dstMips)
    {
        // 0x4F07D0: base skin laid into every body section — NEVER tint here.
        // Only track layers while assembling the local player / DressUp preview.
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
        const bool allow = IsPasteTintAllowed();
        const bool force = g_forceAllowPaste.load(std::memory_order_relaxed);
        bool didTint = false;
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
                    RememberLivePaste(section, srcTexture, dstMips, slot);
                if (allow && hasHsl)
                {
                    const char* mode = "none";
                    const bool ok = TintTextureCacheForPaste(srcTexture, name, hsl, slot, &mode);
                    didTint = ok;
                }
                else
                {
                }
            }
        }
        if (g_origPasteFromSkin)
            g_origPasteFromSkin(section, srcTexture, dstMips);
        if (didTint)
            RestoreTextureCacheOrig(srcTexture);
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
        h.hue = Clamp01(r);
        h.sat = Clamp01(g);
        h.light = Clamp01(b);
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = h;
        }
        NotifyOcSlotChanged(slot);
        if (g_previewUiActive.load(std::memory_order_relaxed))
            ArmPreviewCapture(800);
        // Body: per-slot dirty + RenderPrep (good live path). Do NOT ReplayLivePastes —
        // that painted the shared staging buffer for every layer and cross-influenced slots.
        // OC slots: mesh PS only — never force a full body rebuild.
        if (IsBodyEquipSlot(slot))
            ForceAllowedBodyRebuildForSlot(slot);
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
            ForceAllowedBodyRebuildForSlot(slot);
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
        ForceAllowedBodyRebuildForSlot(-1);
        SaveHslToDisk();
    }

    // OC mesh: same luminance * RGB colorize as body CPU path.
    // c0.rgb = picked color, c0.a unused.
    constexpr char kHslPsHlsl[] = R"(
sampler2D s0 : register(s0);
float4 c0 : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 t = tex2D(s0, uv);
    float lum = dot(t.rgb, float3(0.299, 0.587, 0.114));
    float3 outRgb = saturate(lum * c0.rgb);
    return float4(outRgb, t.a);
}
)";

    IDirect3DPixelShader9* EnsureHslPs(void* deviceRaw)
    {
        static IDirect3DPixelShader9* ps = nullptr;
        static bool tried = false;
        if (tried)
            return ps;
        tried = true;
        ps = static_cast<IDirect3DPixelShader9*>(
            gx::CompilePixelShader(gx::Device9(deviceRaw), kHslPsHlsl, "ps_2_0"));
        g_hslPsState.store(ps ? 1 : 0, std::memory_order_relaxed);
        if (ps)
            WLOG_INFO("gear-recolor: YIQ hue PS ready (all slots)");
        else
            WLOG_WARN("gear-recolor: YIQ PS compile failed; FF fallback");
        return ps;
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
        return 4;
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

    int __cdecl LuaRecolorForceBodyRebuild(void* state)
    {
        ForceAllowedBodyRebuild();
        if (state)
        {
            wlua::PushBoolean(state, 1);
            return 1;
        }
        return 0;
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

            LoadHslFromDisk();

            on<&GearRecolor::OnBatchDraw>(ev::Event::OnM2BatchDraw);
            on<&GearRecolor::OnBlpLoad>(ev::Event::OnBlpLoad);
            on<&GearRecolor::OnTextureUpload>(ev::Event::OnTextureUpload);
            WLOG_INFO("gear-recolor: events bound (paste hooks via modules::Register)");
        }

        void OnBatchDraw(const ev::M2BatchDrawArgs& a)
        {
            if (a.phase != 1 || !a.model || !a.device || !AnySlotActive())
                return;

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

            SlotHsl hsl{};
            int usedSlot = -1;
            if (!TryHslForPath(stem, hsl, &usedSlot))
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
            }
        }

        void OnTextureUpload(const ev::TextureUploadArgs& a)
        {
            // OC live Hue uses OnBatchDraw mesh PS (Photoshop-style adjust).
            // Do not bake HSL into GPU upload — that froze tint and hid Hue changes.
            (void)a;
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

            IDirect3DPixelShader9* hslPs = EnsureHslPs(a.device);
            const bool usePs = (hslPs != nullptr);

            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

            if (usePs)
            {
                const float c0[4] = { hsl.hue, hsl.sat, hsl.light, 1.f };
                dev->SetPixelShader(hslPs);
                dev->SetPixelShaderConstantF(0, c0, 1);
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
            }

            gx::Device9(a.device).DrawIndexedPrimitive(
                a.primType, a.baseVertex, a.minIndex, a.numVerts, a.startIndex, a.primCount);

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
