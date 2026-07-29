// RecolorTexture — CharPaste hooks, TLS overlay, stem-only textureitem tint
    int __fastcall hkRenderPrep(void* component, void* /*edx*/, int a2)
    {
        bool allowed = false;
        void* model = ComponentModel(component);
        void* playerModel = LocalPlayerBodyModel();
        const bool isNpc = ComponentIsNpc(component);
        const char* reason = "deny";
        const OwnerGuid self = ActiveOwnerGuid();

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
                        allowed = true;
                        reason = "remote";
                        g_prepOwnerGuid.store(remoteOwner, std::memory_order_relaxed);
                        RememberRemoteCc(remoteOwner, component, model);
                        if (pending)
                        {
                            const uint32_t mask = TakeRemotePendingMask(remoteOwner);
                            if (mask)
                            {
                                SetComponentSectionDirtyReplaceSeh(component, mask);
                                g_forceAllowPaste.store(true, std::memory_order_relaxed);
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

        g_prepModel.store(model, std::memory_order_relaxed);
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

        const int rc = CallOrigRenderPrepSeh(component, a2);
        g_forceAllowPaste.store(false, std::memory_order_relaxed);
        g_assemblingAllowed.store(false, std::memory_order_relaxed);
        g_prepModel.store(nullptr, std::memory_order_relaxed);
        g_prepOwnerGuid.store(0, std::memory_order_relaxed);
        g_prepReasonCode.store(0, std::memory_order_relaxed);

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
        OwnerGuid owner = g_prepOwnerGuid.load(std::memory_order_relaxed);
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

        // Textureitem paste tint: ONLY ItemDisplayInfo stems for this owner.
        // Never map by geoset section / folder heuristics (face/chest bleed).
        int stemHits[kMaxEquipSlots];
        int hitN = 0;
        for (int slot = 0; slot < kMaxEquipSlots; ++slot)
        {
            DisplaySlotInfo info{};
            if (!GetDisplaySlotInfo(owner, slot, info) || info.stemCount == 0)
                continue;
            if (!PathMatchesAnyStem(name, info))
                continue;
            SlotHsl hsl{};
            if (!TrySlotHslForOwner(owner, slot, hsl))
                continue;
            stemHits[hitN++] = slot;
        }
        if (hitN == 0)
            return false;

        int chosen = stemHits[0];
        if (hitN > 1)
        {
            // Prefer the slot that actually has an equipped item entry.
            for (int i = 0; i < hitN; ++i)
            {
                if (EquipSnapEntry(owner, stemHits[i]) != 0)
                {
                    chosen = stemHits[i];
                    break;
                }
            }
        }
        if (!TrySlotHslForOwner(owner, chosen, out))
            return false;
        if (outSlot)
            *outSlot = chosen;
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
                if (g_slotHsl[i].active)
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
        // 0x4F08A0: items / face / hair overlays.
        std::lock_guard<std::recursive_mutex> gate(g_pasteGateMu);
        const bool allow = IsPasteTintAllowed();
        if (srcTexture)
        {
            const char* name = TextureName(srcTexture);
            if (IsItemComponentTexture(name))
            {
                SlotHsl hsl{};
                int slot = -1;
                const bool hasHsl = TryHslForComponentTexture(name, hsl, &slot);

                const OwnerGuid prepOwner = g_prepOwnerGuid.load(std::memory_order_relaxed);
                const OwnerGuid self = ActiveOwnerGuid();
                const uint32_t equipEntry = (slot >= 0) ? EquipSnapEntry(prepOwner, slot) : 0;
                const int equipKnown = EquipSnapKnown(prepOwner) ? 1 : 0;
                const bool isSelf = self != 0 && GuidSamePlayer(prepOwner, self);

                // Body tint requires stem match (hasHsl) AND known equip with item present.
                // equipKnown==0 → no NoteEquip sync yet; skip tint (prevents boot_ll ghost).
                const bool emptySlot = slot >= 0 && equipKnown && equipEntry == 0;
                const bool remoteNoItem = !isSelf && slot >= 0 && equipEntry == 0;
                const bool doTint = allow && hasHsl && slot >= 0 && prepOwner != 0
                    && equipKnown && !emptySlot && !remoteNoItem;
                bool pathOk = false;

                if (doTint)
                {
                    pathOk = CapturePathOrigForOverlay(srcTexture, name);
                    const bool ok = pathOk && ArmPasteTintOverlay(srcTexture, name, hsl);
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
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = h;
        }
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveHslToDisk();
    }

    void ResetSlotTint(int slot)
    {
        if (slot < 0 || slot >= kMaxEquipSlots)
            return;
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = {};
        }
        if (IsBodyEquipSlot(slot))
            RequestBodyRebuildForSlot(slot);
        SaveHslToDisk();
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
        {
            std::lock_guard<std::mutex> lock(g_colorMutex);
            g_slotHsl[slot] = h;
        }
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
        // Do NOT RequestBodyRebuildForSlot(-1) — full 0x3FF rebuild on login
        // (SetSelfGuid) re-pastes the entire body and bugs composites. Per-slot
        // ApplyLocalPayload / SetSlot rebuilds only the sections that need it.
        SaveHslToDisk();
    }

    // Solid OC PS — single color (byte-stable lighting path).
