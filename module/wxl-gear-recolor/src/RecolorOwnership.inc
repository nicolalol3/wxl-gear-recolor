// RecolorOwnership — persistence, display snaps, owner resolve, rebuild
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

    void DisplayInfoAddStem(DisplaySlotInfo& info, const char* texName)
    {
        if (info.stemCount >= kMaxDisplayStems)
            return;
        char stem[kStemCap] = {};
        CopyTexStem(texName, stem, sizeof(stem));
        if (!stem[0])
            return;
        for (uint8_t i = 0; i < info.stemCount; ++i)
        {
            if (_stricmp(info.stems[i], stem) == 0)
                return;
        }
        std::memcpy(info.stems[info.stemCount], stem, kStemCap);
        ++info.stemCount;
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
                DisplayInfoAddStem(out, tex);
            }
            DisplayInfoAddStem(out,
                *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffTex1));
            DisplayInfoAddStem(out,
                *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffTex2));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out.stemCount = 0;
        }
        return out.stemCount > 0 || out.displayId != 0;
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
                    return out.displayId != 0 || out.stemCount > 0;
                }
            }
            return false;
        }
        out = it->second[static_cast<size_t>(slot)];
        return out.displayId != 0 || out.stemCount > 0;
    }

    bool PathMatchesAnyStem(const char* path, const DisplaySlotInfo& info)
    {
        if (!path || !path[0] || info.stemCount == 0)
            return false;
        for (uint8_t i = 0; i < info.stemCount; ++i)
        {
            if (info.stems[i][0] && ContainsCI(path, info.stems[i]))
                return true;
        }
        return false;
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
                g_slotHsl[static_cast<size_t>(i)] = {};
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
        std::lock_guard<std::mutex> lock(g_remoteMu);
        auto it = g_remoteTints.find(owner);
        if (it != g_remoteTints.end())
        {
            for (int s = 0; s < kMaxEquipSlots; ++s)
            {
                if (it->second[static_cast<size_t>(s)].active)
                    mask |= SectionMaskForEquipSlot(s);
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

    void DirtyOwnerSections(OwnerGuid owner, uint32_t sectionMask)
    {
        if (!owner || sectionMask == 0)
            return;
        const OwnerGuid self = ActiveOwnerGuid();
        // Self: coalesced Force rebuild (works for local player).
        if (self && GuidSamePlayer(owner, self))
        {
            ScheduleOwnerSectionRebuild(owner, sectionMask);
            return;
        }
        // Remotes: per-slot Force (same paste path as self). Do NOT Schedule/coalesce
        // into mega-masks, and do NOT Reschedule remotes after every self rebuild —
        // those wiped composites (mask=255). Pure DirtyComponentSections never
        // produced tex_tint for remotes (logs: force_rem=0, only self force tints).
        ForceOwnerBodyRebuild(owner, sectionMask);
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
                }
                SaveHslToDisk();
            }
            else
            {
                {
                    std::lock_guard<std::mutex> lock(g_colorMutex);
                    g_slotHsl[slot] = *hslOrNull;
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

        // Observer must never store tint for an unequipped slot. Without a real
        // entry, paste would recolor empty feet / shared TextureCache paths and
        // look like "my boots on them".
        if (EquipSnapEntry(owner, slot) == 0)
        {
            ClearRemoteSlotTint(owner, slot);
            return false;
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
            // Expand per-slot like coalesce flush — never mega-mask Force.
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
        for (const auto& job : jobs)
        {
            // Logs (Zoccola): To all coalesced mask=255 → one Force wiped body
            // composites and raced remotes. Expand to per-slot Forces only.
            bool any = false;
            for (int s = 0; s < kMaxEquipSlots; ++s)
            {
                const uint32_t m = SectionMaskForEquipSlot(s);
                if (!m || (job.second & m) == 0)
                    continue;
                ForceOwnerBodyRebuild(job.first, m);
                any = true;
            }
            if (!any && job.second)
                ForceOwnerBodyRebuild(job.first, job.second);
        }
    }

    void FlushPendingRemoteApplies()
    {
        // Login PUSH often arrives before CharComponent exists (force hasComp:0).
        // pending_force never fired in logs — retry here once the unit is visible,
        // Force per active tint slot (never OR'd mega-mask).
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
            void* unitModel = nullptr;
            if (!FindOwnerCharComponent(owner, &unitModel))
                continue;
            const uint32_t pending = TakeRemotePendingMask(owner);
            if (!pending)
                continue;
            int forced = 0;
            for (int s = 0; s < kMaxEquipSlots; ++s)
            {
                SlotHsl h{};
                if (!TrySlotHslForOwner(owner, s, h) || !h.active)
                    continue;
                const uint32_t m = SectionMaskForEquipSlot(s);
                if (!m)
                    continue;
                ForceOwnerBodyRebuild(owner, m);
                ++forced;
            }
            if (forced == 0)
                OrRemotePendingMask(owner, pending);
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

