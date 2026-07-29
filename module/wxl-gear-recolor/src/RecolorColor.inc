// RecolorColor — SlotHsl, color math, selective pixel ops
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
    // Stems come from texture[8] (body overlays) plus ModelTexture[0/1].
    constexpr int kMaxDisplayStems = 10;
    constexpr size_t kStemCap = 64;
    struct DisplaySlotInfo
    {
        uint32_t displayId = 0;
        char stems[kMaxDisplayStems][kStemCap] = {};
        uint8_t stemCount = 0;
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

    uint32_t EquipSnapEntry(OwnerGuid owner, int slot)
    {
        if (!owner || slot < 0 || slot >= kMaxEquipSlots)
            return 0;
        std::lock_guard<std::mutex> lock(g_equipSnapMu);
        auto it = g_equipSnap.find(owner);
        if (it == g_equipSnap.end())
            return 0;
        return it->second[static_cast<size_t>(slot)];
    }

    bool EquipSnapKnown(OwnerGuid owner)
    {
        std::lock_guard<std::mutex> lock(g_equipSnapMu);
        return g_equipSnap.find(owner) != g_equipSnap.end();
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
                }
                else if (slot < kMaxEquipSlots)
                {
                    it->second[static_cast<size_t>(slot)] = {};
                    if (!OwnerHasAnyTintLocked(it->second))
                    {
                        g_remoteTints.erase(it);
                        g_remoteCcModels.erase(owner);
                        g_remoteCcComponents.erase(owner);
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

