-- WXLRecolor: per-slot RGB colorize via ColorPicker.
-- Slash: /recolor

local ADDON_NAME = ...
WXLRecolorDB = WXLRecolorDB or {}

local EQUIP_SLOTS = {
    { name = "Head",      slot = 0  },
    { name = "Shoulder",  slot = 2  },
    { name = "Back",      slot = 14 },
    { name = "Chest",     slot = 4  },
    { name = "Shirt",     slot = 3  },
    { name = "Tabard",    slot = 18 },
    { name = "Wrist",     slot = 8  },
    { name = "Hands",     slot = 9  },
    { name = "Waist",     slot = 5  },
    { name = "Legs",      slot = 6  },
    { name = "Feet",      slot = 7  },
    { name = "Main Hand", slot = 15 },
    { name = "Off-hand",  slot = 16 },
    { name = "Ranged",    slot = 17 },
}

local selectedSlot = nil
local slotButtons = {}
local refreshSlots
local refreshColorPanel
local queuePreviewRefresh
local applySolidFromUi
local refreshSolidPane
local setHoleColor
local syncSlotToServer

local function ensureDB()
    if type(WXLRecolorDB) ~= "table" then
        WXLRecolorDB = {}
    end
    if type(WXLRecolorDB.slots) ~= "table" then
        WXLRecolorDB.slots = {}
    end
end

local function clamp01(v)
    v = tonumber(v) or 0
    if v < 0 then return 0 end
    if v > 1 then return 1 end
    return v
end

-- Server EQUIPMENT_SLOT 0..18 == WoW GetInventoryItemID invSlot 1..19.
-- Never tint / sync empty slots: that creates phantoms and corrupts body textures.
local function slotHasItem(slot)
    slot = tonumber(slot)
    if not slot or type(GetInventoryItemID) ~= "function" then
        return false
    end
    local id = GetInventoryItemID("player", slot + 1)
    return id ~= nil and id ~= 0
end

local function clearLocalSlot(slot)
    slot = tonumber(slot)
    if not slot then
        return
    end
    ensureDB()
    WXLRecolorDB.slots[tostring(slot)] = nil
    if type(WXL_RecolorClearSlot) == "function" then
        WXL_RecolorClearSlot(slot)
    end
end

-- Shared clipboard for tiny C/P next to color swatches (Solid + Selective).
local colorClip = { set = false, r = 0.5, g = 0.5, b = 0.5 }

local function tinyCpBtn(parent, label)
    local b = CreateFrame("Button", nil, parent)
    b:SetSize(14, 12)
    local bg = b:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints()
    bg:SetTexture("Interface\\Buttons\\WHITE8X8")
    bg:SetVertexColor(0.18, 0.18, 0.18, 0.95)
    b.bg = bg
    local fs = b:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    fs:SetPoint("CENTER", 0, 0)
    fs:SetText(label)
    b:SetFontString(fs)
    b:SetScript("OnEnter", function(self)
        self.bg:SetVertexColor(0.35, 0.35, 0.35, 1)
    end)
    b:SetScript("OnLeave", function(self)
        self.bg:SetVertexColor(0.18, 0.18, 0.18, 0.95)
    end)
    return b
end

-- getRgb: () -> r,g,b or nil if empty; setRgb(r,g,b) applies paste.
local function attachCopyPaste(anchor, getRgb, setRgb)
    local wrap = CreateFrame("Frame", nil, anchor:GetParent() or anchor)
    wrap:SetSize(14, 28)
    wrap:SetPoint("LEFT", anchor, "RIGHT", 2, 0)
    local cBtn = tinyCpBtn(wrap, "C")
    cBtn:SetPoint("TOP", 0, 0)
    cBtn:SetScript("OnClick", function()
        local r, g, b = getRgb()
        if r == nil then
            return
        end
        colorClip.set = true
        colorClip.r, colorClip.g, colorClip.b = clamp01(r), clamp01(g), clamp01(b)
    end)
    local pBtn = tinyCpBtn(wrap, "P")
    pBtn:SetPoint("BOTTOM", 0, 0)
    pBtn:SetScript("OnClick", function()
        if not colorClip.set then
            return
        end
        setRgb(colorClip.r, colorClip.g, colorClip.b)
    end)
    return wrap
end

local MAX_SEL_RULES = 3
local MAX_SEL_PAIRS = 3

local function near01(a, b)
    return math.abs((tonumber(a) or 0) - (tonumber(b) or 0)) < 0.045
end

local function cloneRule(r)
    return {
        sr = clamp01(r.sr or 0.8),
        sg = clamp01(r.sg or 0.2),
        sb = clamp01(r.sb or 0.2),
        r = clamp01(r.r or 0.2),
        g = clamp01(r.g or 0.4),
        b = clamp01(r.b or 0.9),
        tol = clamp01(r.tol or 0.35),
    }
end

local function getSelectiveRules(c)
    if type(c) ~= "table" or (tonumber(c.mode) or 0) ~= 1 then
        return {}
    end
    local out = {}
    if type(c.rules) == "table" and #c.rules > 0 then
        for i, r in ipairs(c.rules) do
            if type(r) == "table" then
                out[#out + 1] = cloneRule(r)
            end
            if #out >= MAX_SEL_RULES then
                break
            end
        end
        return out
    end
    -- Legacy single-rule entry
    if c.r ~= nil or c.g ~= nil or c.b ~= nil then
        out[1] = cloneRule(c)
    end
    return out
end

local function storeSelectiveRules(slot, rules)
    ensureDB()
    if not rules or #rules == 0 then
        WXLRecolorDB.slots[tostring(slot)] = nil
        return
    end
    local packed = {}
    for i, r in ipairs(rules) do
        packed[i] = cloneRule(r)
    end
    local last = packed[#packed]
    WXLRecolorDB.slots[tostring(slot)] = {
        mode = 1,
        rules = packed,
        sr = last.sr,
        sg = last.sg,
        sb = last.sb,
        r = last.r,
        g = last.g,
        b = last.b,
        tol = last.tol,
    }
end

local function normalizeEntry(c)
    if type(c) ~= "table" then
        return nil
    end
    local mode = tonumber(c.mode) or 0
    if mode == 1 then
        local rules = getSelectiveRules(c)
        if #rules == 0 then
            return nil
        end
        local last = rules[#rules]
        return {
            mode = 1,
            rules = rules,
            r = last.r,
            g = last.g,
            b = last.b,
            sr = last.sr,
            sg = last.sg,
            sb = last.sb,
            tol = last.tol,
        }
    end
    if mode == 2 then
        local nStops = tonumber(c.stops) or 3
        if nStops ~= 2 and nStops ~= 3 and nStops ~= 5 then
            nStops = 3
        end
        local fill = (tonumber(c.fill) or 0) ~= 0 and 1 or 0
        local colors = {}
        if type(c.colors) == "table" then
            for i = 1, nStops do
                local col = c.colors[i]
                if type(col) == "table" then
                    colors[i] = {
                        r = clamp01(col.r or 0.5),
                        g = clamp01(col.g or 0.5),
                        b = clamp01(col.b or 0.5),
                    }
                end
            end
        end
        local r = clamp01(c.r or 0.2)
        local g = clamp01(c.g or 0.4)
        local b = clamp01(c.b or 0.9)
        if fill == 1 then
            for i = 1, nStops do
                if not colors[i] then
                    colors[i] = { r = r, g = g, b = b }
                end
            end
            local mid = colors[math.floor(nStops / 2) + 1] or colors[1]
            r, g, b = mid.r, mid.g, mid.b
        end
        return {
            mode = 2,
            stops = nStops,
            fill = fill,
            colors = colors,
            r = r, g = g, b = b,
        }
    end
    -- Solid single (default) / legacy RGB
    if c.r ~= nil or c.g ~= nil or c.b ~= nil then
        return {
            mode = 0,
            r = clamp01(c.r or 1),
            g = clamp01(c.g or 1),
            b = clamp01(c.b or 1),
        }
    end
    return nil
end

local function getSlot(slot)
    ensureDB()
    local n = normalizeEntry(WXLRecolorDB.slots[tostring(slot)])
    if n then
        WXLRecolorDB.slots[tostring(slot)] = n
    else
        WXLRecolorDB.slots[tostring(slot)] = nil
    end
    return n
end

local function pushSlot(slot)
    ensureDB()
    if not slotHasItem(slot) then
        clearLocalSlot(slot)
        return
    end
    local c = getSlot(slot)
    if not c then
        if type(WXL_RecolorClearSlot) == "function" then
            WXL_RecolorClearSlot(slot)
        end
        return
    end
    if (c.mode or 0) == 1 then
        if type(WXL_RecolorSetSlotSelective) == "function" then
            if type(WXL_RecolorClearSlot) == "function" then
                WXL_RecolorClearSlot(slot)
            end
            local rules = getSelectiveRules(c)
            for i, rule in ipairs(rules) do
                WXL_RecolorSetSlotSelective(slot, rule.sr, rule.sg, rule.sb,
                    rule.r, rule.g, rule.b, rule.tol or 0.35, i > 1 and 1 or 0)
            end
        elseif type(WXL_RecolorSetSlot) == "function" then
            WXL_RecolorSetSlot(slot, c.r, c.g, c.b)
        end
    elseif (c.mode or 0) == 2 then
        if type(WXL_RecolorSetSlotGradient) == "function" then
            local nStops = c.stops or 3
            local fill = c.fill or 0
            if fill == 0 then
                WXL_RecolorSetSlotGradient(slot, nStops, 0, c.r, c.g, c.b)
            else
                local args = { slot, nStops, 1 }
                for i = 1, nStops do
                    local col = c.colors and c.colors[i]
                    args[#args + 1] = col and col.r or c.r
                    args[#args + 1] = col and col.g or c.g
                    args[#args + 1] = col and col.b or c.b
                end
                WXL_RecolorSetSlotGradient(unpack(args))
            end
        elseif type(WXL_RecolorSetSlot) == "function" then
            WXL_RecolorSetSlot(slot, c.r, c.g, c.b)
        end
    else
        if type(WXL_RecolorSetSlot) == "function" then
            WXL_RecolorSetSlot(slot, c.r, c.g, c.b)
        end
    end
end

local function pushAll(forceRebuild)
    ensureDB()
    -- forceRebuild=false on enter-world: sync C++ colors only, keep natural paste quality.
    if forceRebuild == nil then
        forceRebuild = true
    end
    local batched = type(WXL_RecolorBeginBatch) == "function"
        and type(WXL_RecolorEndBatch) == "function"
    if batched then
        WXL_RecolorBeginBatch()
    end
    if type(WXL_RecolorClearAll) == "function" then
        WXL_RecolorClearAll()
    end
    for key in pairs(WXLRecolorDB.slots) do
        local slot = tonumber(key)
        if slot then
            pushSlot(slot)
        end
    end
    if batched then
        WXL_RecolorEndBatch(forceRebuild and 1 or 0)
    elseif forceRebuild and type(WXL_RecolorForceBodyRebuild) == "function" then
        WXL_RecolorForceBodyRebuild()
    end
end

local function previewSlotLocal(slot)
    if type(WXL_RecolorBeginBatch) == "function" then
        WXL_RecolorBeginBatch()
        pushSlot(slot)
        WXL_RecolorEndBatch()
    else
        pushSlot(slot)
    end
    queuePreviewRefresh()
end

-- Local preview only — no server SET until explicit Apply / To all / Reset.
local function setSlotSolid(slot, r, g, b)
    ensureDB()
    if not slotHasItem(slot) then
        clearLocalSlot(slot)
        return
    end
    WXLRecolorDB.slots[tostring(slot)] = {
        mode = 0,
        r = clamp01(r),
        g = clamp01(g),
        b = clamp01(b),
    }
    previewSlotLocal(slot)
end

local function setSlotGradient(slot, nStops, fill, baseOrColors)
    ensureDB()
    if not slotHasItem(slot) then
        clearLocalSlot(slot)
        return
    end
    nStops = tonumber(nStops) or 3
    if nStops ~= 2 and nStops ~= 3 and nStops ~= 5 then
        nStops = 3
    end
    fill = (tonumber(fill) or 0) ~= 0 and 1 or 0
    local entry = {
        mode = 2,
        stops = nStops,
        fill = fill,
        colors = {},
        r = 0.2, g = 0.4, b = 0.9,
    }
    if fill == 0 then
        local c = baseOrColors or {}
        entry.r = clamp01(c.r or 0.2)
        entry.g = clamp01(c.g or 0.4)
        entry.b = clamp01(c.b or 0.9)
    else
        for i = 1, nStops do
            local c = (type(baseOrColors) == "table" and baseOrColors[i]) or {}
            entry.colors[i] = {
                r = clamp01(c.r or 0.5),
                g = clamp01(c.g or 0.5),
                b = clamp01(c.b or 0.5),
            }
        end
        local mid = entry.colors[math.floor(nStops / 2) + 1] or entry.colors[1]
        entry.r, entry.g, entry.b = mid.r, mid.g, mid.b
    end
    WXLRecolorDB.slots[tostring(slot)] = entry
    previewSlotLocal(slot)
end

-- Append a selective rule (forceAppend=true never updates last — used by Add swap).
local function setSlotSelective(slot, sr, sg, sb, r, g, b, tol, forceAppend)
    ensureDB()
    if not slotHasItem(slot) then
        clearLocalSlot(slot)
        return
    end
    local rules = getSelectiveRules(getSlot(slot))
    local neu = {
        sr = clamp01(sr),
        sg = clamp01(sg),
        sb = clamp01(sb),
        r = clamp01(r),
        g = clamp01(g),
        b = clamp01(b),
        tol = clamp01(tol or 0.35),
    }
    if #rules > 0 then
        local last = rules[#rules]
        if (not forceAppend) and near01(last.sr, neu.sr) and near01(last.sg, neu.sg)
            and near01(last.sb, neu.sb) then
            rules[#rules] = neu
        elseif #rules >= MAX_SEL_RULES then
            table.remove(rules, 1)
            rules[#rules + 1] = neu
        else
            rules[#rules + 1] = neu
        end
    else
        rules[1] = neu
    end
    storeSelectiveRules(slot, rules)
    previewSlotLocal(slot)
end

-- Back-compat alias used by older call sites
local function setSlotColor(slot, r, g, b)
    setSlotSolid(slot, r, g, b)
end

local function clearSlot(slot)
    ensureDB()
    WXLRecolorDB.slots[tostring(slot)] = nil
    pushSlot(slot)
    syncSlotToServer(slot)
    queuePreviewRefresh()
end

local function clearAll()
    ensureDB()
    WXLRecolorDB.slots = {}
    if type(WXL_RecolorClearAll) == "function" then
        WXL_RecolorClearAll()
    end
    for _, info in ipairs(EQUIP_SLOTS) do
        syncSlotToServer(info.slot)
    end
    queuePreviewRefresh()
end

-- Server sync (mod-item-tint): Transmog-style — server PUSH only, no client REQ.
local ADDON_MSG_PREFIX = "WXL_TINT"

local function sendTint(body)
    if type(SendAddonMessage) ~= "function" then
        return
    end
    SendAddonMessage(ADDON_MSG_PREFIX, body, "WHISPER", UnitName("player"))
end

-- /reload does not fire PLAYER_ENTERING_WORLD; cold login PUSH can race SetSelfGuid.
local cancelReloadRecovery = false

local function scheduleTintResync(delaySec)
    delaySec = tonumber(delaySec) or 0.5
    local follow = CreateFrame("Frame")
    local t = 0
    local phase = 0
    follow:SetScript("OnUpdate", function(self, elapsed)
        t = t + elapsed
        if phase == 0 then
            if t < delaySec then
                return
            end
            phase = 1
            t = 0
            WXLRecolor_RefreshEquipSnap()
            sendTint("REQ")
            return
        end
        if t < 1.0 then
            return
        end
        self:SetScript("OnUpdate", nil)
        WXLRecolor_RefreshEquipSnap()
        if type(WXL_RecolorForceBodyRebuild) == "function" then
            WXL_RecolorForceBodyRebuild()
        end
    end)
end

local function runReloadRecovery()
    local guid = UnitGUID and UnitGUID("player")
    if not guid then
        return
    end
    if type(WXL_RecolorSetSelfGuid) == "function" then
        WXL_RecolorSetSelfGuid(guid)
    end
    if type(WXL_RecolorOnUiReload) == "function" then
        WXL_RecolorOnUiReload()
    elseif type(WXL_RecolorForceBodyRebuild) == "function" then
        WXL_RecolorForceBodyRebuild()
    end
    WXLRecolor_RefreshEquipSnap()
    scheduleTintResync(0.25)
end

syncSlotToServer = function(slot, allowClear)
    slot = tonumber(slot)
    if not slot then
        return
    end
    -- Never SET an empty slot (server has no item → phantom local tint only).
    if not slotHasItem(slot) then
        clearLocalSlot(slot)
        if allowClear ~= false then
            sendTint(string.format("CLEAR\t%d", slot))
        end
        return
    end
    if type(WXL_RecolorGetSlotPayload) == "function" then
        local ok, mode, data = WXL_RecolorGetSlotPayload(slot)
        if ok and mode ~= nil and data then
            sendTint(string.format("SET\t%d\t%d\t%s", slot, mode, data))
            return
        end
    end
    -- Enter-world bulk sync must NOT CLEAR empty slots (login PUSH flood → crash).
    if allowClear ~= false then
        sendTint(string.format("CLEAR\t%d", slot))
    end
end

-- Auto multi-client dump (no manual commands required).
local function noteUnitEquip(unit)
    if type(WXL_RecolorNoteEquip) ~= "function" then
        return
    end
    if type(UnitGUID) ~= "function" or type(GetInventoryItemID) ~= "function" then
        return
    end
    local g = UnitGUID(unit)
    if not g then
        return
    end
    for slot = 0, 18 do
        local entry = GetInventoryItemID(unit, slot + 1) or 0
        WXL_RecolorNoteEquip(g, slot, entry)
    end
end

function WXLRecolor_RefreshEquipSnap()
    if type(WXL_RecolorBeginEquipSnap) == "function" then
        WXL_RecolorBeginEquipSnap()
        -- ONLY local inventory. Noting target/party via GetInventoryItemID writes
        -- entry=0 for every remote slot and falsely flags feet/etc as phantoms.
        noteUnitEquip("player")
    end
end

-- Kept for /tintdump slash compat — refreshes local equip snap only.
function WXLRecolor_DumpTintState(_reason)
    WXLRecolor_RefreshEquipSnap()
end

local function applyPush(ownerGuid, slot, modeOrClear, data)
    if not ownerGuid or not slot then
        return
    end
    local selfGuid = UnitGUID and UnitGUID("player")
    local function guidLow(g)
        if type(g) ~= "string" then
            return nil
        end
        local hex = g:match("0[xX](%x+)$") or g:match("(%x+)$")
        if not hex then
            return nil
        end
        return tonumber(hex, 16)
    end
    local isSelf = false
    if selfGuid and tostring(ownerGuid) == tostring(selfGuid) then
        isSelf = true
    else
        local a, b = guidLow(ownerGuid), guidLow(selfGuid)
        if a and b and a == b then
            isSelf = true
        end
    end

    local applyFn = WXL_RecolorApplyOwnerTint
    if type(applyFn) ~= "function" then
        if isSelf and type(WXL_RecolorApplyLocalPayload) == "function" then
            if modeOrClear == "clear" or modeOrClear == nil then
                WXL_RecolorApplyLocalPayload(slot, "clear")
            else
                WXL_RecolorApplyLocalPayload(slot, tonumber(modeOrClear), data)
            end
        elseif type(WXL_RecolorSetRemote) == "function" then
            if modeOrClear == "clear" or modeOrClear == nil then
                if type(WXL_RecolorClearRemote) == "function" then
                    WXL_RecolorClearRemote(ownerGuid, slot)
                end
            else
                WXL_RecolorSetRemote(ownerGuid, slot, tonumber(modeOrClear), data)
            end
        end
    elseif modeOrClear == "clear" or modeOrClear == nil then
        applyFn(ownerGuid, slot, "clear")
    else
        local mode = tonumber(modeOrClear)
        if mode == nil or not data then
            return
        end
        applyFn(ownerGuid, slot, mode, data)
    end

    if isSelf then
        ensureDB()
        if modeOrClear == "clear" or modeOrClear == nil then
            WXLRecolorDB.slots[tostring(slot)] = nil
        end
    end
end

local function handleTintAddon(msg)
    if type(msg) ~= "string" or msg == "" then
        return
    end
    if msg:sub(1, #ADDON_MSG_PREFIX + 1) == (ADDON_MSG_PREFIX .. "\t") then
        msg = msg:sub(#ADDON_MSG_PREFIX + 2)
    end
    local parts = {}
    for part in string.gmatch(msg, "[^\t]+") do
        parts[#parts + 1] = part
    end
    if parts[1] == "PUSH" and parts[2] and parts[3] then
        local owner = parts[2]
        local slot = tonumber(parts[3])
        if (parts[4] == "clear") then
            -- Do NOT NoteEquip(0) on CLEAR — transmog visible-slot flicker
            -- broadcasts CLEAR for all slots and zeroed equip/display snap on
            -- observers before the real SET, breaking paste matching (H4 logs).
            applyPush(owner, slot, "clear")
        elseif parts[4] and parts[5] then
            -- Note entry BEFORE apply so push_remote dump sees real entry (not phantom).
            local entry = tonumber(parts[6]) or 0
            if type(WXL_RecolorNoteEquip) == "function" then
                WXL_RecolorNoteEquip(owner, slot, entry)
            end
            -- Never apply remote/self tint for an empty slot (server should not
            -- SET these; guard anyway — observer phantom boots).
            if entry == 0 then
                applyPush(owner, slot, "clear")
            else
                applyPush(owner, slot, parts[4], parts[5])
            end
        end
    end
end

local tintComm = CreateFrame("Frame")
tintComm:RegisterEvent("CHAT_MSG_ADDON")
tintComm:RegisterEvent("CHAT_MSG_WHISPER")
tintComm:RegisterEvent("PLAYER_ENTERING_WORLD")
tintComm:RegisterEvent("PLAYER_LEAVING_WORLD")
tintComm:RegisterEvent("PLAYER_EQUIPMENT_CHANGED")
tintComm:SetScript("OnEvent", function(_, event, ...)
    if event == "CHAT_MSG_ADDON" then
        local prefix, message = ...
        if prefix == ADDON_MSG_PREFIX then
            handleTintAddon(message)
        elseif type(message) == "string" and (
            message:find("^PUSH\t")
            or message:sub(1, #ADDON_MSG_PREFIX + 1) == (ADDON_MSG_PREFIX .. "\t")
        ) then
            handleTintAddon(message)
        elseif type(prefix) == "string" and (
            prefix:find("^PUSH\t")
            or prefix:sub(1, #ADDON_MSG_PREFIX + 1) == (ADDON_MSG_PREFIX .. "\t")
        ) then
            handleTintAddon(prefix)
        end
    elseif event == "CHAT_MSG_WHISPER" then
        local message = ...
        if type(message) == "string" and message:sub(1, #ADDON_MSG_PREFIX + 1) == (ADDON_MSG_PREFIX .. "\t") then
            handleTintAddon(message)
        end
    elseif event == "PLAYER_EQUIPMENT_CHANGED" then
        local invSlot = ...
        -- Do NOT sync CLEAR→server here (tmog visible-slot flicker storms).
        -- Server OnPlayerAfterSetVisibleItemSlot owns unequip clears.
        -- DO drop local phantom tint when the bag slot is actually empty —
        -- otherwise g_slotHsl stays active (phantom:1) while observers race CLEAR.
        if invSlot then
            local slot = tonumber(invSlot) - 1
            if slot and slot >= 0 and slot <= 18 then
                local g = UnitGUID and UnitGUID("player")
                if not slotHasItem(slot) then
                    clearLocalSlot(slot)
                    if g and type(WXL_RecolorNoteEquip) == "function" then
                        WXL_RecolorNoteEquip(g, slot, 0)
                    end
                elseif g and type(WXL_RecolorNoteEquip) == "function"
                    and type(GetInventoryItemID) == "function" then
                    local entry = GetInventoryItemID("player", slot + 1) or 0
                    WXL_RecolorNoteEquip(g, slot, entry)
                end
            end
        end
        WXLRecolor_DumpTintState("equipment_changed_" .. tostring(invSlot or "?"))
    elseif event == "PLAYER_LEAVING_WORLD" then
        WXLRecolor_DumpTintState("leaving_world")
        if type(WXL_RecolorClearAllRemote) == "function" then
            WXL_RecolorClearAllRemote()
        end
    elseif event == "PLAYER_ENTERING_WORLD" then
        cancelReloadRecovery = true
        local guid = UnitGUID and UnitGUID("player")
        if guid and type(WXL_RecolorSetSelfGuid) == "function" then
            WXL_RecolorSetSelfGuid(guid)
        end
        -- Drop any leftover DB/C++ tints on unequipped slots (old "To all" phantoms).
        for s = 0, 18 do
            if not slotHasItem(s) then
                clearLocalSlot(s)
            end
        end
        -- Equip snap now; REQ after settle (login PUSH often arrives before addon ready).
        WXLRecolor_DumpTintState("entering_world_immediate")
        scheduleTintResync(0.5)
    end
end)

if type(ChatFrame_AddMessageEventFilter) == "function" then
    ChatFrame_AddMessageEventFilter("CHAT_MSG_WHISPER", function(_, _, msg)
        if type(msg) == "string" and msg:sub(1, #ADDON_MSG_PREFIX + 1) == (ADDON_MSG_PREFIX .. "\t") then
            return true
        end
    end)
end

if type(RegisterAddonMessagePrefix) == "function" then
    RegisterAddonMessagePrefix(ADDON_MSG_PREFIX)
end

-- Optional manual dump still available; not required.
SLASH_WXLTINTDUMP1 = "/tintdump"
SlashCmdList["WXLTINTDUMP"] = function(msg)
    local reason = (type(msg) == "string" and msg ~= "" and msg) or "slash_tintdump"
    WXLRecolor_DumpTintState(reason)
end

local function makeBackdrop(f, a)
    f:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true,
        tileSize = 16,
        edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    f:SetBackdropColor(0, 0, 0, a or 0.75)
end

local frame = CreateFrame("Frame", "WXLRecolorFrame", UIParent)
frame:SetSize(640, 750)
frame:SetPoint("CENTER")
frame:SetFrameStrata("DIALOG")
frame:SetToplevel(true)
frame:SetMovable(true)
frame:EnableMouse(true)
frame:Hide()
frame:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true,
    tileSize = 32,
    edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
tinsert(UISpecialFrames, "WXLRecolorFrame")

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
title:SetPoint("TOP", 0, -16)
title:SetText("Gear Recolor")

local drag = CreateFrame("Frame", nil, frame)
drag:SetPoint("TOPLEFT", 12, -10)
drag:SetPoint("TOPRIGHT", -40, -10)
drag:SetHeight(28)
drag:EnableMouse(true)
drag:SetScript("OnMouseDown", function() frame:StartMoving() end)
drag:SetScript("OnMouseUp", function() frame:StopMovingOrSizing() end)

local closeBtn = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
closeBtn:SetPoint("TOPRIGHT", -4, -4)

-- Model
local modelPanel = CreateFrame("Frame", nil, frame)
modelPanel:SetSize(240, 400)
modelPanel:SetPoint("TOPLEFT", 18, -44)
makeBackdrop(modelPanel, 0.85)

local modelHint = modelPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
modelHint:SetPoint("BOTTOM", 0, 6)
modelHint:SetText("Scroll=zoom  |  Alt+drag=move")

local model = CreateFrame("DressUpModel", "WXLRecolorModel", modelPanel)
model:SetPoint("TOPLEFT", 5, -5)
model:SetPoint("BOTTOMRIGHT", -5, 22)
model:SetUnit("player")
model.camScale = 1.0
model.posX = 0
model.posY = 0
model.posZ = 0
model:EnableMouse(true)
model:EnableMouseWheel(true)

local function applyModelCamera()
    model:SetPosition(model.posX, model.posY, model.posZ)
    model:SetModelScale(model.camScale)
end

model:SetScript("OnMouseWheel", function(self, delta)
    self.camScale = math.max(0.4, math.min(2.5, self.camScale + delta * 0.08))
    applyModelCamera()
end)

model:SetScript("OnMouseDown", function(self, button)
    self.dragButton = button
    self.lastX, self.lastY = GetCursorPosition()
end)
model:SetScript("OnMouseUp", function(self)
    self.dragButton = nil
end)
model:SetScript("OnUpdate", function(self)
    if not self.dragButton then
        return
    end
    local x, y = GetCursorPosition()
    local dx = (x - (self.lastX or x)) * 0.01
    local dy = (y - (self.lastY or y)) * 0.01
    self.lastX, self.lastY = x, y
    if self.dragButton == "LeftButton" and IsAltKeyDown() then
        self.posX = self.posX + dx
        self.posY = self.posY + dy
        applyModelCamera()
    elseif self.dragButton == "RightButton" then
        self:SetFacing((self:GetFacing() or 0) + dx)
    elseif self.dragButton == "LeftButton" then
        self:SetFacing((self:GetFacing() or 0) + dx)
    end
end)

local previewDirty = false
local previewAccum = 0
local previewDriver = CreateFrame("Frame", nil, frame)
previewDriver:Hide()
previewDriver:SetScript("OnUpdate", function(self, elapsed)
    if not previewDirty then
        self:Hide()
        return
    end
    previewAccum = previewAccum + elapsed
    if previewAccum < 0.12 then
        return
    end
    previewAccum = 0
    previewDirty = false
    if frame:IsShown() then
        if type(WXL_RecolorArmPreviewCapture) == "function" then
            WXL_RecolorArmPreviewCapture()
        end
        model:SetUnit("player")
        model:Dress()
        applyModelCamera()
        if type(WXL_RecolorForceBodyRebuild) == "function" then
            WXL_RecolorForceBodyRebuild()
        end
    end
    self:Hide()
end)

queuePreviewRefresh = function()
    if type(WXL_RecolorArmPreviewCapture) == "function" then
        WXL_RecolorArmPreviewCapture()
    end
    previewDirty = true
    previewAccum = 0
    previewDriver:Show()
end

-- Slot list
local slotPanel = CreateFrame("Frame", nil, frame)
slotPanel:SetSize(340, 280)
slotPanel:SetPoint("TOPLEFT", modelPanel, "TOPRIGHT", 12, 0)
makeBackdrop(slotPanel, 0.7)

local slotTitle = slotPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
slotTitle:SetPoint("TOPLEFT", 12, -10)
slotTitle:SetText("Equipment slots")

local listHost = CreateFrame("Frame", nil, slotPanel)
listHost:SetPoint("TOPLEFT", 10, -30)
listHost:SetPoint("BOTTOMRIGHT", -10, 10)

-- Color panel (Solid / Selective tabs)
local colorPanel = CreateFrame("Frame", nil, frame)
colorPanel:SetSize(340, 340)
colorPanel:SetPoint("TOPLEFT", slotPanel, "BOTTOMLEFT", 0, -10)
makeBackdrop(colorPanel, 0.7)

local colorTitle = colorPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
colorTitle:SetPoint("TOPLEFT", 12, -8)
colorTitle:SetText("Color")

local slotLabel = colorPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
slotLabel:SetPoint("TOPLEFT", 12, -26)
slotLabel:SetText("Select a slot")

local editMode = 0 -- 0 solid, 1 selective (UI tab; may differ until applied)
local tabSolid = CreateFrame("Button", nil, colorPanel, "UIPanelButtonTemplate")
tabSolid:SetSize(90, 20)
tabSolid:SetPoint("TOPRIGHT", -100, -8)
tabSolid:SetText("Solid")
local tabSelective = CreateFrame("Button", nil, colorPanel, "UIPanelButtonTemplate")
tabSelective:SetSize(90, 20)
tabSelective:SetPoint("LEFT", tabSolid, "RIGHT", 4, 0)
tabSelective:SetText("Selective")

local solidPane = CreateFrame("Frame", nil, colorPanel)
solidPane:SetPoint("TOPLEFT", 8, -46)
solidPane:SetPoint("BOTTOMRIGHT", -8, 8)

local selPane = CreateFrame("Frame", nil, colorPanel)
selPane:SetPoint("TOPLEFT", 8, -46)
selPane:SetPoint("BOTTOMRIGHT", -8, 8)
selPane:Hide()

-- Solid sub-mode state (UI)
local solidKind = 0 -- 0 single, 1 gradient
local solidStops = 3
local solidFill = 0 -- 0 auto, 1 custom
local solidBase = { r = 0.2, g = 0.4, b = 0.9 }
local solidColors = {}
local activeSolidStop = 1
local refreshSolidPane

local function ensureSolidColors(n)
    n = n or solidStops
    for i = 1, 5 do
        if not solidColors[i] then
            solidColors[i] = { r = 0.5, g = 0.5, b = 0.5 }
        end
    end
end
ensureSolidColors(5)

local kindSingleBtn = CreateFrame("Button", nil, solidPane, "UIPanelButtonTemplate")
kindSingleBtn:SetSize(70, 20)
kindSingleBtn:SetPoint("TOPLEFT", 4, -2)
kindSingleBtn:SetText("Single")
local kindGradBtn = CreateFrame("Button", nil, solidPane, "UIPanelButtonTemplate")
kindGradBtn:SetSize(80, 20)
kindGradBtn:SetPoint("LEFT", kindSingleBtn, "RIGHT", 4, 0)
kindGradBtn:SetText("Gradient")

local stopRow = CreateFrame("Frame", nil, solidPane)
stopRow:SetSize(300, 22)
stopRow:SetPoint("TOPLEFT", 4, -26)
local stopLabel = stopRow:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
stopLabel:SetPoint("LEFT", 0, 0)
stopLabel:SetText("Stops")
local stopBtns = {}
for _, n in ipairs({ 2, 3, 5 }) do
    local b = CreateFrame("Button", nil, stopRow, "UIPanelButtonTemplate")
    b:SetSize(28, 20)
    if #stopBtns == 0 then
        b:SetPoint("LEFT", stopLabel, "RIGHT", 8, 0)
    else
        b:SetPoint("LEFT", stopBtns[#stopBtns], "RIGHT", 3, 0)
    end
    b:SetText(tostring(n))
    b.nStops = n
    stopBtns[#stopBtns + 1] = b
end

local fillRow = CreateFrame("Frame", nil, solidPane)
fillRow:SetSize(300, 22)
fillRow:SetPoint("TOPLEFT", 4, -50)
local fillAutoBtn = CreateFrame("Button", nil, fillRow, "UIPanelButtonTemplate")
fillAutoBtn:SetSize(70, 20)
fillAutoBtn:SetPoint("LEFT", 0, 0)
fillAutoBtn:SetText("Auto")
local fillCustomBtn = CreateFrame("Button", nil, fillRow, "UIPanelButtonTemplate")
fillCustomBtn:SetSize(70, 20)
fillCustomBtn:SetPoint("LEFT", fillAutoBtn, "RIGHT", 4, 0)
fillCustomBtn:SetText("Custom")

-- Swatches on one row; Pick/To all on the next — never overlap Custom stops.
local swatchHost = CreateFrame("Frame", nil, solidPane)
swatchHost:SetSize(320, 32)
swatchHost:SetPoint("TOPLEFT", 4, -78)

local bigSwatchHost = CreateFrame("Frame", nil, swatchHost)
bigSwatchHost:SetSize(32, 32)
bigSwatchHost:SetPoint("LEFT", 0, 0)
local bigSwatch = bigSwatchHost:CreateTexture(nil, "ARTWORK")
bigSwatch:SetAllPoints()
bigSwatch:SetTexture("Interface\\Buttons\\WHITE8X8")
bigSwatch:SetVertexColor(0.4, 0.4, 0.4)
local bigSwatchCp = attachCopyPaste(bigSwatchHost, function()
    return solidBase.r, solidBase.g, solidBase.b
end, function(r, g, b)
    solidBase.r, solidBase.g, solidBase.b = r, g, b
    applySolidFromUi()
    refreshSolidPane()
    refreshSlots()
    refreshColorPanel()
end)

local gradStopHosts = {}
for i = 1, 5 do
    local host = CreateFrame("Button", nil, swatchHost)
    host:SetSize(28, 28)
    -- Leave room for C/P beside each stop (~14px + gap).
    host:SetPoint("LEFT", (i - 1) * 48, 0)
    local tex = host:CreateTexture(nil, "ARTWORK")
    tex:SetAllPoints()
    tex:SetTexture("Interface\\Buttons\\WHITE8X8")
    tex:SetVertexColor(0.4, 0.4, 0.4)
    local border = host:CreateTexture(nil, "BORDER")
    border:SetTexture("Interface\\Buttons\\WHITE8X8")
    border:SetPoint("TOPLEFT", -1, 1)
    border:SetPoint("BOTTOMRIGHT", 1, -1)
    border:SetVertexColor(0.35, 0.35, 0.35)
    host.tex = tex
    host.border = border
    host.idx = i
    host:Hide()
    host.cp = attachCopyPaste(host, function()
        ensureSolidColors(solidStops)
        local c = solidColors[host.idx]
        if not c then
            return nil
        end
        return c.r, c.g, c.b
    end, function(r, g, b)
        ensureSolidColors(solidStops)
        solidColors[host.idx] = { r = r, g = g, b = b }
        activeSolidStop = host.idx
        applySolidFromUi()
        refreshSolidPane()
        refreshSlots()
        refreshColorPanel()
    end)
    host.cp:Hide()
    gradStopHosts[i] = host
end

local actionRow = CreateFrame("Frame", nil, solidPane)
actionRow:SetSize(320, 24)
actionRow:SetPoint("TOPLEFT", swatchHost, "BOTTOMLEFT", 0, -6)

local pickBtn = CreateFrame("Button", nil, actionRow, "UIPanelButtonTemplate")
pickBtn:SetSize(92, 24)
pickBtn:SetPoint("LEFT", 0, 0)
pickBtn:SetText("Pick color")
pickBtn:Disable()

local applySolidBtn = CreateFrame("Button", nil, actionRow, "UIPanelButtonTemplate")
applySolidBtn:SetSize(58, 24)
applySolidBtn:SetPoint("LEFT", pickBtn, "RIGHT", 6, 0)
applySolidBtn:SetText("Apply")
applySolidBtn:Disable()

local copyToAllBtn = CreateFrame("Button", nil, actionRow, "UIPanelButtonTemplate")
copyToAllBtn:SetSize(58, 24)
copyToAllBtn:SetPoint("LEFT", applySolidBtn, "RIGHT", 6, 0)
copyToAllBtn:SetText("To all")
copyToAllBtn:Disable()

local solidHint = solidPane:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
solidHint:SetPoint("TOPLEFT", actionRow, "BOTTOMLEFT", 0, -6)
solidHint:SetWidth(310)
solidHint:SetJustifyH("LEFT")
solidHint:SetText("Preview only until Apply — then saved on server")

applySolidFromUi = function()
    if selectedSlot == nil then
        return
    end
    if solidKind == 0 then
        setSlotSolid(selectedSlot, solidBase.r, solidBase.g, solidBase.b)
    else
        if solidFill == 0 then
            setSlotGradient(selectedSlot, solidStops, 0, solidBase)
        else
            ensureSolidColors(solidStops)
            local cols = {}
            for i = 1, solidStops do
                cols[i] = {
                    r = solidColors[i].r,
                    g = solidColors[i].g,
                    b = solidColors[i].b,
                }
            end
            setSlotGradient(selectedSlot, solidStops, 1, cols)
        end
    end
end

local function loadSolidUiFromSlot(c)
    if c and (c.mode or 0) == 2 then
        solidKind = 1
        solidStops = c.stops or 3
        solidFill = c.fill or 0
        solidBase.r, solidBase.g, solidBase.b = c.r, c.g, c.b
        ensureSolidColors(solidStops)
        if type(c.colors) == "table" then
            for i = 1, solidStops do
                if c.colors[i] then
                    solidColors[i].r = c.colors[i].r
                    solidColors[i].g = c.colors[i].g
                    solidColors[i].b = c.colors[i].b
                end
            end
        end
    elseif c and (c.mode or 0) == 0 then
        solidKind = 0
        solidBase.r, solidBase.g, solidBase.b = c.r, c.g, c.b
    else
        solidKind = 0
        solidBase.r, solidBase.g, solidBase.b = 0.4, 0.4, 0.4
    end
end

refreshSolidPane = function()
    if solidKind == 0 then
        kindSingleBtn:Disable()
        kindGradBtn:Enable()
        stopRow:Hide()
        fillRow:Hide()
        bigSwatchHost:Show()
        bigSwatchCp:Show()
        bigSwatch:SetVertexColor(solidBase.r, solidBase.g, solidBase.b)
        for i = 1, 5 do
            gradStopHosts[i]:Hide()
            if gradStopHosts[i].cp then
                gradStopHosts[i].cp:Hide()
            end
        end
        pickBtn:SetText("Pick color")
        solidHint:SetText("Preview only until Apply — then saved on server")
    else
        kindSingleBtn:Enable()
        kindGradBtn:Disable()
        stopRow:Show()
        fillRow:Show()
        for _, b in ipairs(stopBtns) do
            if b.nStops == solidStops then
                b:Disable()
            else
                b:Enable()
            end
        end
        if solidFill == 0 then
            fillAutoBtn:Disable()
            fillCustomBtn:Enable()
            bigSwatchHost:Show()
            bigSwatchCp:Show()
            bigSwatch:SetVertexColor(solidBase.r, solidBase.g, solidBase.b)
            for i = 1, 5 do
                gradStopHosts[i]:Hide()
                if gradStopHosts[i].cp then
                    gradStopHosts[i].cp:Hide()
                end
            end
            pickBtn:SetText("Pick base")
            solidHint:SetText(string.format(
                "Gradient %d stops — auto shades of one color", solidStops))
        else
            fillAutoBtn:Enable()
            fillCustomBtn:Disable()
            bigSwatchHost:Hide()
            bigSwatchCp:Hide()
            ensureSolidColors(solidStops)
            for i = 1, 5 do
                local h = gradStopHosts[i]
                if i <= solidStops then
                    h:Show()
                    if h.cp then
                        h.cp:Show()
                    end
                    h.tex:SetVertexColor(solidColors[i].r, solidColors[i].g, solidColors[i].b)
                    if activeSolidStop == i then
                        h.border:SetVertexColor(1, 0.85, 0.2)
                    else
                        h.border:SetVertexColor(0.35, 0.35, 0.35)
                    end
                else
                    h:Hide()
                    if h.cp then
                        h.cp:Hide()
                    end
                end
            end
            pickBtn:SetText("Pick stop")
            solidHint:SetText(string.format(
                "Gradient %d stops — pick each color (stop %d)", solidStops, activeSolidStop))
        end
    end
end

kindSingleBtn:SetScript("OnClick", function()
    solidKind = 0
    refreshSolidPane()
    applySolidFromUi()
    refreshSlots()
    refreshColorPanel()
end)
kindGradBtn:SetScript("OnClick", function()
    solidKind = 1
    if solidStops ~= 2 and solidStops ~= 3 and solidStops ~= 5 then
        solidStops = 3
    end
    refreshSolidPane()
    applySolidFromUi()
    refreshSlots()
    refreshColorPanel()
end)
for _, b in ipairs(stopBtns) do
    b:SetScript("OnClick", function(self)
        solidStops = self.nStops
        if activeSolidStop > solidStops then
            activeSolidStop = solidStops
        end
        refreshSolidPane()
        applySolidFromUi()
        refreshSlots()
    end)
end
fillAutoBtn:SetScript("OnClick", function()
    solidFill = 0
    refreshSolidPane()
    applySolidFromUi()
    refreshSlots()
end)
fillCustomBtn:SetScript("OnClick", function()
    solidFill = 1
    ensureSolidColors(solidStops)
    -- Seed custom stops from current base if unset-looking
    for i = 1, solidStops do
        local t = (i - 1) / math.max(1, solidStops - 1)
        solidColors[i].r = clamp01(solidBase.r * (0.16 + t * 0.84))
        solidColors[i].g = clamp01(solidBase.g * (0.16 + t * 0.84))
        solidColors[i].b = clamp01(solidBase.b * (0.16 + t * 0.84))
    end
    activeSolidStop = 1
    refreshSolidPane()
    applySolidFromUi()
    refreshSlots()
end)
for i = 1, 5 do
    gradStopHosts[i]:SetScript("OnClick", function(self)
        activeSolidStop = self.idx
        refreshSolidPane()
    end)
end

-- Selective: 3 fixed pairs A→A1, B→B1, C→C1 with per-pair tolerance.
local PAIR_LABELS = { "A", "B", "C" }
local PAIR_DST_LABELS = { "A1", "B1", "C1" }
local EMPTY_RGB = { 0.18, 0.18, 0.18 }

local function emptySelPair()
    return {
        srcSet = false,
        dstSet = false,
        sr = 0.5, sg = 0.5, sb = 0.5,
        r = 0.5, g = 0.5, b = 0.5,
        tol = 0.35,
    }
end

local selPairs = { emptySelPair(), emptySelPair(), emptySelPair() }
local syncPairsFromSlot = true
local activeHole = nil -- { pair = 1..3, which = "src"|"dst" }

local function rulesFromSelPairs()
    local rules = {}
    for i = 1, MAX_SEL_PAIRS do
        local p = selPairs[i]
        if p.srcSet and p.dstSet then
            rules[#rules + 1] = cloneRule({
                sr = p.sr, sg = p.sg, sb = p.sb,
                r = p.r, g = p.g, b = p.b,
                tol = p.tol,
            })
        end
    end
    return rules
end

local function loadSelPairsFromRules(rules)
    for i = 1, MAX_SEL_PAIRS do
        selPairs[i] = emptySelPair()
        local r = rules and rules[i]
        if type(r) == "table" then
            local p = selPairs[i]
            p.srcSet = true
            p.dstSet = true
            p.sr, p.sg, p.sb = r.sr, r.sg, r.sb
            p.r, p.g, p.b = r.r, r.g, r.b
            p.tol = r.tol or 0.35
        end
    end
end

local function replaceSlotSelectiveRules(slot, rules)
    if not rules or #rules == 0 then
        clearSlot(slot)
        return
    end
    storeSelectiveRules(slot, rules)
    pushSlot(slot)
    queuePreviewRefresh()
end

local sampleHint = selPane:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
sampleHint:SetPoint("BOTTOMLEFT", 4, 2)
sampleHint:SetWidth(320)
sampleHint:SetJustifyH("LEFT")
sampleHint:SetText("")
sampleHint:Hide()

local samplePoll = CreateFrame("Frame")
samplePoll:Hide()

local fromLabel = selPane:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
fromLabel:SetPoint("TOPLEFT", 4, -2)
fromLabel:SetText("From")

local toLabel = selPane:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
toLabel:SetPoint("TOPLEFT", 4, -78)
toLabel:SetText("To")

local tolRowLabel = selPane:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
tolRowLabel:SetPoint("TOPLEFT", 4, -154)
tolRowLabel:SetText("Tol")

local pairCols = {}

local function refreshHoleVisual(hole)
    local p = selPairs[hole.pair]
    local set = (hole.which == "src") and p.srcSet or p.dstSet
    if set then
        if hole.which == "src" then
            hole.tex:SetVertexColor(p.sr, p.sg, p.sb)
        else
            hole.tex:SetVertexColor(p.r, p.g, p.b)
        end
    else
        hole.tex:SetVertexColor(EMPTY_RGB[1], EMPTY_RGB[2], EMPTY_RGB[3])
    end
    if activeHole and activeHole.pair == hole.pair and activeHole.which == hole.which then
        hole.border:SetVertexColor(1, 0.85, 0.2)
    else
        hole.border:SetVertexColor(0.35, 0.35, 0.35)
    end
end

local function refreshAllHoles()
    for i = 1, MAX_SEL_PAIRS do
        local col = pairCols[i]
        refreshHoleVisual(col.srcHole)
        refreshHoleVisual(col.dstHole)
        col.tolSlider:SetValue(selPairs[i].tol or 0.35)
    end
end

local function setActiveHole(pair, which)
    activeHole = { pair = pair, which = which }
    refreshAllHoles()
    sampleHint:SetText(string.format("Selected %s — Pick or Game",
        which == "src" and PAIR_LABELS[pair] or PAIR_DST_LABELS[pair]))
    sampleHint:Show()
end

setHoleColor = function(pair, which, r, g, b)
    local p = selPairs[pair]
    if which == "src" then
        p.sr, p.sg, p.sb = r, g, b
        p.srcSet = true
    else
        p.r, p.g, p.b = r, g, b
        p.dstSet = true
    end
    refreshAllHoles()
end

local function clearHole(pair, which)
    local p = selPairs[pair]
    if which == "src" then
        p.srcSet = false
        p.sr, p.sg, p.sb = 0.5, 0.5, 0.5
    else
        p.dstSet = false
        p.r, p.g, p.b = 0.5, 0.5, 0.5
    end
    refreshAllHoles()
end

local function makeHole(parent, pair, which, x, y, labelText)
    local hole = CreateFrame("Button", nil, parent)
    hole:SetSize(28, 28)
    hole:SetPoint("TOPLEFT", x, y)
    hole.pair = pair
    hole.which = which

    local border = hole:CreateTexture(nil, "BACKGROUND")
    border:SetAllPoints()
    border:SetTexture("Interface\\Buttons\\WHITE8X8")
    border:SetVertexColor(0.35, 0.35, 0.35)
    hole.border = border

    local tex = hole:CreateTexture(nil, "ARTWORK")
    tex:SetPoint("TOPLEFT", 2, -2)
    tex:SetPoint("BOTTOMRIGHT", -2, 2)
    tex:SetTexture("Interface\\Buttons\\WHITE8X8")
    tex:SetVertexColor(EMPTY_RGB[1], EMPTY_RGB[2], EMPTY_RGB[3])
    hole.tex = tex

    local lab = hole:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    lab:SetPoint("BOTTOM", hole, "TOP", 0, 1)
    lab:SetText(labelText)

    hole:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    hole:SetScript("OnClick", function(_, button)
        if button == "RightButton" then
            clearHole(pair, which)
        else
            setActiveHole(pair, which)
        end
    end)
    hole.cp = attachCopyPaste(hole, function()
        local p = selPairs[pair]
        if which == "src" then
            if not p.srcSet then
                return nil
            end
            return p.sr, p.sg, p.sb
        end
        if not p.dstSet then
            return nil
        end
        return p.r, p.g, p.b
    end, function(r, g, b)
        setHoleColor(pair, which, r, g, b)
    end)
    return hole
end

for i = 1, MAX_SEL_PAIRS do
    local colX = 40 + (i - 1) * 112
    local srcHole = makeHole(selPane, i, "src", colX, -18, PAIR_LABELS[i])
    local dstHole = makeHole(selPane, i, "dst", colX, -94, PAIR_DST_LABELS[i])

    local tolName = "WXLRecolorTol" .. i
    local tolSlider = CreateFrame("Slider", tolName, selPane, "OptionsSliderTemplate")
    tolSlider:SetWidth(88)
    tolSlider:SetHeight(14)
    tolSlider:SetPoint("TOPLEFT", colX - 8, -168)
    tolSlider:SetMinMaxValues(0.05, 0.5)
    tolSlider:SetValueStep(0.01)
    tolSlider:SetValue(0.35)
    _G[tolName .. "Low"]:SetText("")
    _G[tolName .. "High"]:SetText("")
    _G[tolName .. "Text"]:SetText("0.35")
    tolSlider.pairIndex = i
    tolSlider:SetScript("OnValueChanged", function(self, value)
        selPairs[self.pairIndex].tol = value
        _G[self:GetName() .. "Text"]:SetText(string.format("%.2f", value))
    end)

    pairCols[i] = { srcHole = srcHole, dstHole = dstHole, tolSlider = tolSlider }
end

local pickHoleBtn = CreateFrame("Button", nil, selPane, "UIPanelButtonTemplate")
pickHoleBtn:SetSize(52, 20)
pickHoleBtn:SetPoint("TOPLEFT", 4, -210)
pickHoleBtn:SetText("Pick")
pickHoleBtn:Disable()

local sampleHoleBtn = CreateFrame("Button", nil, selPane, "UIPanelButtonTemplate")
sampleHoleBtn:SetSize(58, 20)
sampleHoleBtn:SetPoint("LEFT", pickHoleBtn, "RIGHT", 4, 0)
sampleHoleBtn:SetText("Game")
sampleHoleBtn:Disable()

local catchHoleBtn = CreateFrame("Button", nil, selPane, "UIPanelButtonTemplate")
catchHoleBtn:SetSize(58, 20)
catchHoleBtn:SetPoint("LEFT", sampleHoleBtn, "RIGHT", 4, 0)
catchHoleBtn:SetText("Catch")
catchHoleBtn:Disable()

local applySelBtn = CreateFrame("Button", nil, selPane, "UIPanelButtonTemplate")
applySelBtn:SetSize(70, 22)
applySelBtn:SetPoint("LEFT", catchHoleBtn, "RIGHT", 10, 0)
applySelBtn:SetText("Apply")
applySelBtn:Disable()

local copySelBtn = CreateFrame("Button", nil, selPane, "UIPanelButtonTemplate")
copySelBtn:SetSize(60, 22)
copySelBtn:SetPoint("LEFT", applySelBtn, "RIGHT", 6, 0)
copySelBtn:SetText("To all")
copySelBtn:Disable()

local selHint = selPane:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
selHint:SetPoint("TOPLEFT", pickHoleBtn, "BOTTOMLEFT", 0, -4)
selHint:SetWidth(320)
selHint:SetJustifyH("LEFT")
selHint:SetText("RClick=clear | Apply saves on server | Catch=3 colors from tex")

samplePoll:SetScript("OnUpdate", function(self)
    if type(WXL_RecolorGetScreenSample) ~= "function" then
        self:Hide()
        return
    end
    local r, g, b, ready = WXL_RecolorGetScreenSample()
    if not ready or ready == 0 then
        return
    end
    self:Hide()
    sampleHint:Hide()
    if activeHole then
        setHoleColor(activeHole.pair, activeHole.which, r, g, b)
    end
    if WXLRecolorFrame and not WXLRecolorFrame:IsShown() then
        WXLRecolorFrame:Show()
    end
end)

local function armGameSample()
    if not activeHole then
        sampleHint:SetText("Click a hole (A/B/C or A1/B1/C1) first")
        sampleHint:Show()
        return
    end
    if type(WXL_RecolorArmScreenSample) ~= "function" then
        sampleHint:SetText("Restart client for Game sample")
        sampleHint:Show()
        return
    end
    sampleHint:SetText("Click gear in the world…")
    sampleHint:Show()
    WXL_RecolorArmScreenSample()
    samplePoll:Show()
    if frame:IsShown() then
        frame:Hide()
    end
end

local function runCatchColors()
    if selectedSlot == nil then
        sampleHint:SetText("Select a slot first")
        sampleHint:Show()
        return
    end
    if type(WXL_RecolorCatchSlotColors) ~= "function" then
        sampleHint:SetText("Restart client for Catch")
        sampleHint:Show()
        return
    end
    local n, r1, g1, b1, r2, g2, b2, r3, g3, b3 = WXL_RecolorCatchSlotColors(selectedSlot)
    n = tonumber(n) or 0
    if n < 1 then
        sampleHint:SetText("Catch: no colors yet — unequip/re-equip once (or open preview)")
        sampleHint:Show()
        return
    end
    local cols = {
        { r = r1, g = g1, b = b1 },
        { r = r2, g = g2, b = b2 },
        { r = r3, g = g3, b = b3 },
    }
    for i = 1, math.min(n, MAX_SEL_PAIRS) do
        local c = cols[i]
        if c and c.r ~= nil then
            setHoleColor(i, "src", clamp01(c.r), clamp01(c.g), clamp01(c.b))
        end
    end
    sampleHint:SetText(string.format("Catch: filled A.. with %d distant colors", n))
    sampleHint:Show()
end

local function setEditMode(mode)
    editMode = mode
    if mode == 1 then
        solidPane:Hide()
        selPane:Show()
        tabSolid:Enable()
        tabSelective:Disable()
    else
        selPane:Hide()
        solidPane:Show()
        tabSolid:Disable()
        tabSelective:Enable()
        refreshSolidPane()
    end
end

tabSolid:SetScript("OnClick", function()
    setEditMode(0)
    refreshColorPanel()
end)
tabSelective:SetScript("OnClick", function()
    setEditMode(1)
    refreshColorPanel()
end)
setEditMode(0)

local colorPickerMovable = false
local hueSliderReady = false
local hueSliderUpdating = false

local function rgbToHsv(r, g, b)
    r = clamp01(r)
    g = clamp01(g)
    b = clamp01(b)
    local maxc = math.max(r, g, b)
    local minc = math.min(r, g, b)
    local d = maxc - minc
    local h = 0
    if d > 1e-6 then
        if maxc == r then
            h = ((g - b) / d) % 6
        elseif maxc == g then
            h = (b - r) / d + 2
        else
            h = (r - g) / d + 4
        end
        h = h / 6
        if h < 0 then
            h = h + 1
        end
    end
    local s = (maxc < 1e-6) and 0 or (d / maxc)
    return h, s, maxc
end

local function hsvToRgb(h, s, v)
    h = h % 1
    if h < 0 then
        h = h + 1
    end
    s = clamp01(s)
    v = clamp01(v)
    if s < 1e-6 then
        return v, v, v
    end
    local i = math.floor(h * 6)
    local f = h * 6 - i
    local p = v * (1 - s)
    local q = v * (1 - f * s)
    local t = v * (1 - (1 - f) * s)
    i = i % 6
    if i == 0 then return v, t, p end
    if i == 1 then return q, v, p end
    if i == 2 then return p, v, t end
    if i == 3 then return p, q, v end
    if i == 4 then return t, p, v end
    return v, p, q
end

local function ensureColorPickerMovable()
    if colorPickerMovable or not ColorPickerFrame then
        return
    end
    colorPickerMovable = true
    ColorPickerFrame:SetMovable(true)
    ColorPickerFrame:SetClampedToScreen(true)

    local drag = CreateFrame("Frame", nil, ColorPickerFrame)
    drag:SetPoint("TOPLEFT", 6, -4)
    drag:SetPoint("TOPRIGHT", -6, -4)
    drag:SetHeight(24)
    drag:EnableMouse(true)
    drag:SetFrameLevel(ColorPickerFrame:GetFrameLevel() + 5)
    drag:SetScript("OnMouseDown", function()
        ColorPickerFrame:StartMoving()
    end)
    drag:SetScript("OnMouseUp", function()
        ColorPickerFrame:StopMovingOrSizing()
    end)
end

local function ensureHueSlider()
    if hueSliderReady or not ColorPickerFrame then
        return
    end
    -- Tints may have already installed the bordered hue rail.
    if ColorPickerFrame.wxlHueSlider then
        hueSliderReady = true
        return
    end
    hueSliderReady = true

    local label = ColorPickerFrame:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    label:SetPoint("TOPRIGHT", -18, -36)
    label:SetText("Hue")

    local rail = CreateFrame("Frame", "WXLRecolorHueRail", ColorPickerFrame)
    rail:SetSize(28, 128)
    rail:SetPoint("TOP", label, "BOTTOM", 0, -4)
    rail:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true,
        tileSize = 8,
        edgeSize = 12,
        insets = { left = 2, right = 2, top = 2, bottom = 2 },
    })
    rail:SetBackdropColor(0.05, 0.05, 0.05, 0.95)
    rail:SetBackdropBorderColor(0.7, 0.7, 0.7, 1)

    local slider = CreateFrame("Slider", "WXLRecolorHueSlider", rail, "OptionsSliderTemplate")
    slider:SetWidth(16)
    slider:SetHeight(112)
    slider:SetOrientation("VERTICAL")
    slider:SetPoint("CENTER", 0, 0)
    slider:SetMinMaxValues(0, 360)
    slider:SetValueStep(1)
    slider:SetValue(0)
    if slider.SetObeyStepOnDrag then
        slider:SetObeyStepOnDrag(true)
    end
    _G[slider:GetName() .. "Low"]:SetText("")
    _G[slider:GetName() .. "High"]:SetText("")
    _G[slider:GetName() .. "Text"]:SetText("")
    ColorPickerFrame.wxlHueSlider = slider
    ColorPickerFrame.wxlHueLabel = label
    ColorPickerFrame.wxlHueRail = rail

    slider:SetScript("OnValueChanged", function(self, value)
        if hueSliderUpdating or not ColorPickerFrame:IsShown() then
            return
        end
        local r, g, b = ColorPickerFrame:GetColorRGB()
        local _, s, v = rgbToHsv(r, g, b)
        local h = (tonumber(value) or 0) / 360
        local nr, ng, nb = hsvToRgb(h, s, v)
        hueSliderUpdating = true
        ColorPickerFrame:SetColorRGB(nr, ng, nb)
        hueSliderUpdating = false
        if ColorPickerFrame.func then
            ColorPickerFrame.func()
        end
    end)
end

local function syncHueSliderFromRgb(r, g, b)
    if not ColorPickerFrame or not ColorPickerFrame.wxlHueSlider then
        return
    end
    local h = rgbToHsv(r, g, b)
    hueSliderUpdating = true
    ColorPickerFrame.wxlHueSlider:SetValue(math.floor(h * 360 + 0.5))
    hueSliderUpdating = false
end

local function openColorPickerRgb(initial, onChanged, onCancel)
    local prev = { r = initial.r, g = initial.g, b = initial.b }
    ensureColorPickerMovable()
    ensureHueSlider()
    ColorPickerFrame.func = function()
        local r, g, b = ColorPickerFrame:GetColorRGB()
        if not hueSliderUpdating then
            syncHueSliderFromRgb(r, g, b)
        end
        onChanged(r, g, b)
    end
    ColorPickerFrame.cancelFunc = function()
        onCancel(prev.r, prev.g, prev.b)
    end
    ColorPickerFrame:SetColorRGB(initial.r, initial.g, initial.b)
    ColorPickerFrame.opacityFunc = nil
    ColorPickerFrame.hasOpacity = false
    syncHueSliderFromRgb(initial.r, initial.g, initial.b)
    -- Make room for the hue strip on the right
    if not ColorPickerFrame.wxlHueWidened then
        ColorPickerFrame.wxlHueWidened = true
        ColorPickerFrame:SetWidth((ColorPickerFrame:GetWidth() or 365) + 36)
    end
    ColorPickerFrame:Show()
end

local function openColorPicker(slot)
    loadSolidUiFromSlot(getSlot(slot))
    local init = { r = solidBase.r, g = solidBase.g, b = solidBase.b }
    if solidKind == 1 and solidFill == 1 then
        ensureSolidColors(solidStops)
        local c = solidColors[activeSolidStop]
        init = { r = c.r, g = c.g, b = c.b }
    end
    openColorPickerRgb(init, function(r, g, b)
        if solidKind == 1 and solidFill == 1 then
            solidColors[activeSolidStop] = { r = r, g = g, b = b }
        else
            solidBase.r, solidBase.g, solidBase.b = r, g, b
        end
        applySolidFromUi()
        setEditMode(0)
        refreshSolidPane()
        refreshSlots()
        refreshColorPanel()
    end, function(r, g, b)
        if solidKind == 1 and solidFill == 1 then
            solidColors[activeSolidStop] = { r = r, g = g, b = b }
        else
            solidBase.r, solidBase.g, solidBase.b = r, g, b
        end
        applySolidFromUi()
        refreshSolidPane()
        refreshSlots()
        refreshColorPanel()
    end)
end

pickBtn:SetScript("OnClick", function()
    if selectedSlot ~= nil then
        openColorPicker(selectedSlot)
    end
end)

applySolidBtn:SetScript("OnClick", function()
    if selectedSlot == nil then
        return
    end
    applySolidFromUi()
    syncSlotToServer(selectedSlot)
    refreshSlots()
    refreshColorPanel()
end)

copyToAllBtn:SetScript("OnClick", function()
    if selectedSlot == nil then
        return
    end
    applySolidFromUi()
    local c = getSlot(selectedSlot)
    if not c then
        return
    end
    local mode = c.mode or 0
    if mode ~= 0 and mode ~= 2 then
        return
    end
    if type(WXL_RecolorBeginBatch) == "function" then
        WXL_RecolorBeginBatch()
    end
    for _, entry in ipairs(EQUIP_SLOTS) do
        if entry.slot ~= selectedSlot and slotHasItem(entry.slot) then
            if mode == 2 then
                if (c.fill or 0) == 0 then
                    setSlotGradient(entry.slot, c.stops or 3, 0, { r = c.r, g = c.g, b = c.b })
                else
                    setSlotGradient(entry.slot, c.stops or 3, 1, c.colors)
                end
            else
                setSlotSolid(entry.slot, c.r, c.g, c.b)
            end
        end
    end
    if type(WXL_RecolorEndBatch) == "function" then
        WXL_RecolorEndBatch()
    end
    syncSlotToServer(selectedSlot)
    for _, entry in ipairs(EQUIP_SLOTS) do
        if entry.slot ~= selectedSlot and slotHasItem(entry.slot) then
            syncSlotToServer(entry.slot)
        end
    end
    refreshSlots()
    refreshColorPanel()
end)

pickHoleBtn:SetScript("OnClick", function()
    if not activeHole then
        sampleHint:SetText("Click a hole first")
        sampleHint:Show()
        return
    end
    local p = selPairs[activeHole.pair]
    local init
    if activeHole.which == "src" then
        init = { r = p.sr, g = p.sg, b = p.sb }
    else
        init = { r = p.r, g = p.g, b = p.b }
    end
    local pair, which = activeHole.pair, activeHole.which
    openColorPickerRgb(init, function(r, g, b)
        setHoleColor(pair, which, r, g, b)
    end, function(r, g, b)
        setHoleColor(pair, which, r, g, b)
    end)
end)

sampleHoleBtn:SetScript("OnClick", function()
    armGameSample()
end)

catchHoleBtn:SetScript("OnClick", function()
    runCatchColors()
end)

applySelBtn:SetScript("OnClick", function()
    if selectedSlot == nil then
        return
    end
    local rules = rulesFromSelPairs()
    if #rules == 0 then
        sampleHint:SetText("Fill at least one pair (e.g. A + A1)")
        sampleHint:Show()
        return
    end
    replaceSlotSelectiveRules(selectedSlot, rules)
    syncSlotToServer(selectedSlot)
    syncPairsFromSlot = true
    setEditMode(1)
    refreshSlots()
    refreshColorPanel()
end)

copySelBtn:SetScript("OnClick", function()
    if selectedSlot == nil then
        return
    end
    local rules = rulesFromSelPairs()
    if #rules == 0 then
        local c = getSlot(selectedSlot)
        if c and (c.mode or 0) == 1 then
            rules = getSelectiveRules(c)
        end
    end
    if not rules or #rules == 0 then
        return
    end
    replaceSlotSelectiveRules(selectedSlot, rules)
    syncSlotToServer(selectedSlot)
    if type(WXL_RecolorBeginBatch) == "function" then
        WXL_RecolorBeginBatch()
    end
    for _, entry in ipairs(EQUIP_SLOTS) do
        if entry.slot ~= selectedSlot and slotHasItem(entry.slot) then
            replaceSlotSelectiveRules(entry.slot, rules)
            syncSlotToServer(entry.slot)
        end
    end
    if type(WXL_RecolorEndBatch) == "function" then
        WXL_RecolorEndBatch()
    end
    refreshSlots()
    refreshColorPanel()
end)

local apiStatus = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
apiStatus:SetPoint("BOTTOMLEFT", 22, 16)
apiStatus:SetWidth(280)
apiStatus:SetJustifyH("LEFT")

refreshColorPanel = function()
    if selectedSlot == nil then
        slotLabel:SetText("Select a slot")
        bigSwatch:SetVertexColor(0.4, 0.4, 0.4)
        pickBtn:Disable()
        applySolidBtn:Disable()
        copyToAllBtn:Disable()
        pickHoleBtn:Disable()
        sampleHoleBtn:Disable()
        catchHoleBtn:Disable()
        applySelBtn:Disable()
        copySelBtn:Disable()
        activeHole = nil
        loadSelPairsFromRules(nil)
        refreshAllHoles()
        return
    end
    local name = "?"
    for _, info in ipairs(EQUIP_SLOTS) do
        if info.slot == selectedSlot then
            name = info.name
            break
        end
    end
    local c = getSlot(selectedSlot)
    slotLabel:SetText(name)

    pickBtn:Enable()
    applySolidBtn:Enable()
    pickHoleBtn:Enable()
    sampleHoleBtn:Enable()
    catchHoleBtn:Enable()
    applySelBtn:Enable()

    if syncPairsFromSlot then
        if c and (c.mode or 0) == 1 then
            loadSelPairsFromRules(getSelectiveRules(c))
        else
            loadSelPairsFromRules(nil)
        end
        syncPairsFromSlot = false
    end
    refreshAllHoles()

    loadSolidUiFromSlot(c)
    refreshSolidPane()
    if editMode == 0 then
        copyToAllBtn:Enable()
    else
        copyToAllBtn:Disable()
    end

    if c and (c.mode or 0) == 1 then
        copySelBtn:Enable()
    else
        copySelBtn:Disable()
    end
end

refreshSlots = function()
    for _, btn in ipairs(slotButtons) do
        local c = getSlot(btn.equipSlot)
        if c then
            btn.swatch:SetVertexColor(c.r, c.g, c.b)
            btn.label:SetTextColor(1, 1, 1)
            if (c.mode or 0) == 1 then
                btn.mark:SetText("S")
            elseif (c.mode or 0) == 2 then
                btn.mark:SetText("G")
            else
                btn.mark:SetText("•")
            end
        else
            btn.swatch:SetVertexColor(0.4, 0.4, 0.4)
            btn.label:SetTextColor(0.7, 0.7, 0.7)
            btn.mark:SetText("")
        end
        if selectedSlot == btn.equipSlot then
            btn:LockHighlight()
        else
            btn:UnlockHighlight()
        end
    end
    if type(WXL_RecolorSetSlot) == "function" then
        if type(WXL_RecolorSetSlotGradient) == "function" then
            apiStatus:SetText("WarcraftXL color API ready (solid+gradient+selective)")
        elseif type(WXL_RecolorSetSlotSelective) == "function" then
            apiStatus:SetText("WarcraftXL color API ready (solid+selective)")
        else
            apiStatus:SetText("WXL API ready — restart for Selective")
        end
        apiStatus:SetTextColor(0.4, 0.9, 0.4)
    else
        apiStatus:SetText("WXL API missing — restart client")
        apiStatus:SetTextColor(0.95, 0.75, 0.3)
    end
end

local COLS, ROW_H, COL_W = 2, 22, 160
for i, info in ipairs(EQUIP_SLOTS) do
    local col = (i - 1) % COLS
    local row = math.floor((i - 1) / COLS)
    local btn = CreateFrame("Button", nil, listHost)
    btn:SetSize(COL_W, ROW_H)
    btn:SetPoint("TOPLEFT", col * COL_W, -row * ROW_H)
    btn:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight", "ADD")
    btn.equipSlot = info.slot

    local swatch = btn:CreateTexture(nil, "ARTWORK")
    swatch:SetSize(10, 10)
    swatch:SetPoint("LEFT", 4, 0)
    swatch:SetTexture("Interface\\Buttons\\WHITE8X8")
    btn.swatch = swatch

    local label = btn:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    label:SetPoint("LEFT", swatch, "RIGHT", 6, 0)
    label:SetPoint("RIGHT", -14, 0)
    label:SetJustifyH("LEFT")
    label:SetText(info.name)
    btn.label = label

    local mark = btn:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    mark:SetPoint("RIGHT", -2, 0)
    mark:SetText("")
    btn.mark = mark

    btn:SetScript("OnClick", function()
        selectedSlot = info.slot
        syncPairsFromSlot = true
        activeHole = nil
        local c = getSlot(info.slot)
        if c and (c.mode or 0) == 1 then
            setEditMode(1)
        else
            setEditMode(0)
        end
        refreshSlots()
        refreshColorPanel()
    end)
    slotButtons[#slotButtons + 1] = btn
end

local function makeBtn(text, x, fn)
    local b = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    b:SetSize(90, 22)
    b:SetPoint("BOTTOMRIGHT", x, 14)
    b:SetText(text)
    b:SetScript("OnClick", fn)
end

makeBtn("Close", -16, function()
    frame:Hide()
end)

makeBtn("Clear All", -112, function()
    clearAll()
    selectedSlot = nil
    loadSelPairsFromRules(nil)
    refreshSlots()
    refreshColorPanel()
end)

makeBtn("Reset Slot", -208, function()
    if selectedSlot == nil then
        return
    end
    clearSlot(selectedSlot)
    syncPairsFromSlot = true
    loadSelPairsFromRules(nil)
    refreshSlots()
    refreshColorPanel()
end)

frame:SetScript("OnShow", function()
    if type(WXL_RecolorSetPreviewActive) == "function" then
        WXL_RecolorSetPreviewActive(1)
    end
    -- Preview only — do NOT sync/CLEAR to server (that wiped nearby via PUSH).
    model:SetUnit("player")
    model:Dress()
    applyModelCamera()
    ensureDB()
    for key, raw in pairs(WXLRecolorDB.slots) do
        local n = normalizeEntry(raw)
        WXLRecolorDB.slots[key] = n
    end
    refreshSlots()
    refreshColorPanel()
    queuePreviewRefresh()
end)

frame:SetScript("OnHide", function()
    if type(WXL_RecolorSetPreviewActive) == "function" then
        WXL_RecolorSetPreviewActive(0)
    end
end)

-- Default game paperdoll (CharacterModelFrame): arm UI OC root capture.
local charHook = CreateFrame("Frame")
charHook:RegisterEvent("PLAYER_LOGIN")
charHook:SetScript("OnEvent", function(self)
    self:UnregisterEvent("PLAYER_LOGIN")
    if not CharacterFrame then
        return
    end
    CharacterFrame:HookScript("OnShow", function()
        if type(WXL_RecolorSetCharacterUiActive) == "function" then
            WXL_RecolorSetCharacterUiActive(1)
        elseif type(WXL_RecolorArmUiCapture) == "function" then
            WXL_RecolorArmUiCapture()
        end
    end)
    CharacterFrame:HookScript("OnHide", function()
        if type(WXL_RecolorSetCharacterUiActive) == "function" then
            WXL_RecolorSetCharacterUiActive(0)
        end
    end)
end)

SLASH_WXLRECOLOR1 = "/recolor"
SlashCmdList["WXLRECOLOR"] = function()
    if frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
    end
end

local boot = CreateFrame("Frame")
boot:RegisterEvent("ADDON_LOADED")
boot:RegisterEvent("PLAYER_LEAVING_WORLD")
-- Do NOT pushAll on LOGIN / VARIABLES_LOADED / ENTERING_WORLD.
-- That re-applied account-wide WXLRecolorDB over server PUSH and made both
-- dual-client characters share the same slot tints. Server PUSH →
-- ApplyLocalPayload / SetRemote is the only world source of truth.

boot:SetScript("OnEvent", function(_, event, arg1)
    if event == "ADDON_LOADED" and arg1 == ADDON_NAME then
        ensureDB()
    elseif event == "PLAYER_LEAVING_WORLD" then
        if type(WXL_RecolorFlushTex) == "function" then
            WXL_RecolorFlushTex()
        end
    end
end)

-- After /reload: in-world but no PLAYER_ENTERING_WORLD — recover tints from server.
if UnitGUID and UnitGUID("player") then
    local reloadProbe = CreateFrame("Frame")
    local probeT = 0
    reloadProbe:SetScript("OnUpdate", function(self, elapsed)
        probeT = probeT + elapsed
        if probeT < 1.0 then
            return
        end
        self:SetScript("OnUpdate", nil)
        if not cancelReloadRecovery then
            runReloadRecovery()
        end
    end)
end
