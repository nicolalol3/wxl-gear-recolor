// RecolorEvents — GearRecolor EventScript + InstallPasteHooks
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
            wlua::RegisterFunction("WXL_RecolorSetSlotGradient", &LuaRecolorSetSlotGradient);
            wlua::RegisterFunction("WXL_RecolorResetTint", &LuaRecolorResetTint);
            wlua::RegisterFunction("WXL_RecolorGetSlotGradient", &LuaRecolorGetSlotGradient);
            wlua::RegisterFunction("WXL_RecolorSetSlotSelective", &LuaRecolorSetSlotSelective);
            wlua::RegisterFunction("WXL_RecolorCatchSlotColors", &LuaRecolorCatchSlotColors);
            wlua::RegisterFunction("WXL_RecolorGetSlot", &LuaRecolorGetSlot);
            wlua::RegisterFunction("WXL_RecolorGetSlotTexPath", &LuaRecolorGetSlotTexPath);
            wlua::RegisterFunction("WXL_RecolorBakePreview", &LuaRecolorBakePreview);
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
            wlua::RegisterFunction("WXL_RecolorArmScreenSample", &LuaRecolorArmScreenSample);
            wlua::RegisterFunction("WXL_RecolorCancelScreenSample", &LuaRecolorCancelScreenSample);
            wlua::RegisterFunction("WXL_RecolorGetScreenSample", &LuaRecolorGetScreenSample);

            // Server PUSH after SetSelfGuid is source of truth (no disk load at init).

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
