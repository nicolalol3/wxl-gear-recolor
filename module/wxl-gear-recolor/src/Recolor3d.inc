// Recolor3d — OC pixel shaders, bake preview, Catch sampling
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
                && dinfo.stemCount > 0 && PathMatchesAnyStem(p, dinfo);
            // Preview bake: prefer ItemDisplayInfo stems; folder heuristic only
            // when stems are unknown (UI swatch, not in-world paste tint).
            const int mapped = SlotForComponentTexture(p);
            if (!stemOk)
            {
                if (dinfo.stemCount > 0)
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
            && dinfo.stemCount > 0;
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
                && dinfo.stemCount > 0;
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

