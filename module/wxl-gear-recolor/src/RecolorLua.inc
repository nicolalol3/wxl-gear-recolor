// RecolorLua — WXL_Recolor* Lua bindings
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
        g_cachedSelfGuid.store(owner, std::memory_order_relaxed);
        // Server PushAllEquipped is source of truth — clear stale local slots.
        ClearAllHsl();
        wlua::PushBoolean(state, 1);
        return 1;
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
            // operator[] default-constructs array of zeros (empty slots).
            g_equipSnap[owner][static_cast<size_t>(slot)] = entry;
        }
        // Keep TextureComponents identity in sync with the equip snap.
        SetDisplaySlotFromItemEntry(owner, slot, entry);
        const OwnerGuid self = ActiveOwnerGuid();
        if (self && GuidSamePlayer(owner, self))
        {
            // Unequip OR item swap: drop stale paste refs for this slot.
            ClearCatchSampleForSlot(slot);
            if (entry == 0 && IsBodyEquipSlot(slot))
                RequestBodyRebuildForSlot(slot);
        }
        // Unequip / PUSH clear: drop any remote tint for this slot so paste cannot
        // keep painting empty feet after the item is gone.
        if (entry == 0)
        {
            if (!self || !GuidSamePlayer(owner, self))
                ClearRemoteSlotTint(owner, slot);
        }
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
        if (!TrySlotHsl(slot, h) || !h.active)
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

