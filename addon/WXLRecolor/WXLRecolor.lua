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
    -- Solid (default) / legacy RGB
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

local function setSlotSolid(slot, r, g, b)
    ensureDB()
    WXLRecolorDB.slots[tostring(slot)] = {
        mode = 0,
        r = clamp01(r),
        g = clamp01(g),
        b = clamp01(b),
    }
    if type(WXL_RecolorBeginBatch) == "function" then
        WXL_RecolorBeginBatch()
        pushSlot(slot)
        WXL_RecolorEndBatch()
    else
        pushSlot(slot)
    end
    queuePreviewRefresh()
end

-- Append a selective rule (forceAppend=true never updates last — used by Add swap).
local function setSlotSelective(slot, sr, sg, sb, r, g, b, tol, forceAppend)
    ensureDB()
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
    if type(WXL_RecolorBeginBatch) == "function" then
        WXL_RecolorBeginBatch()
        pushSlot(slot)
        WXL_RecolorEndBatch()
    else
        pushSlot(slot)
    end
    queuePreviewRefresh()
end

-- Back-compat alias used by older call sites
local function setSlotColor(slot, r, g, b)
    setSlotSolid(slot, r, g, b)
end

local function clearSlot(slot)
    ensureDB()
    WXLRecolorDB.slots[tostring(slot)] = nil
    pushSlot(slot)
    queuePreviewRefresh()
end

local function clearAll()
    ensureDB()
    WXLRecolorDB.slots = {}
    if type(WXL_RecolorClearAll) == "function" then
        WXL_RecolorClearAll()
    end
    queuePreviewRefresh()
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
frame:SetSize(640, 680)
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
colorPanel:SetSize(340, 270)
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

local bigSwatch = solidPane:CreateTexture(nil, "ARTWORK")
bigSwatch:SetSize(36, 36)
bigSwatch:SetPoint("TOPLEFT", 4, -4)
bigSwatch:SetTexture("Interface\\Buttons\\WHITE8X8")
bigSwatch:SetVertexColor(0.4, 0.4, 0.4)

local pickBtn = CreateFrame("Button", nil, solidPane, "UIPanelButtonTemplate")
pickBtn:SetSize(110, 24)
pickBtn:SetPoint("LEFT", bigSwatch, "RIGHT", 12, 0)
pickBtn:SetText("Pick color")
pickBtn:Disable()

local copyToAllBtn = CreateFrame("Button", nil, solidPane, "UIPanelButtonTemplate")
copyToAllBtn:SetSize(70, 24)
copyToAllBtn:SetPoint("LEFT", pickBtn, "RIGHT", 8, 0)
copyToAllBtn:SetText("To all")
copyToAllBtn:Disable()

local solidHint = solidPane:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
solidHint:SetPoint("TOPLEFT", bigSwatch, "BOTTOMLEFT", 0, -8)
solidHint:SetText("Full recolor (monochrome tint)")

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

local function setHoleColor(pair, which, r, g, b)
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
    return hole
end

for i = 1, MAX_SEL_PAIRS do
    local colX = 40 + (i - 1) * 100
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

local applySelBtn = CreateFrame("Button", nil, selPane, "UIPanelButtonTemplate")
applySelBtn:SetSize(70, 22)
applySelBtn:SetPoint("LEFT", sampleHoleBtn, "RIGHT", 12, 0)
applySelBtn:SetText("Apply")
applySelBtn:Disable()

local copySelBtn = CreateFrame("Button", nil, selPane, "UIPanelButtonTemplate")
copySelBtn:SetSize(60, 22)
copySelBtn:SetPoint("LEFT", applySelBtn, "RIGHT", 6, 0)
copySelBtn:SetText("To all")
copySelBtn:Disable()

local selHint = selPane:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
selHint:SetPoint("LEFT", copySelBtn, "RIGHT", 8, 0)
selHint:SetText("RClick=clear | despeckle on")

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

local function openColorPickerRgb(initial, onChanged, onCancel)
    local prev = { r = initial.r, g = initial.g, b = initial.b }
    ensureColorPickerMovable()
    ColorPickerFrame.func = function()
        local r, g, b = ColorPickerFrame:GetColorRGB()
        onChanged(r, g, b)
    end
    ColorPickerFrame.cancelFunc = function()
        onCancel(prev.r, prev.g, prev.b)
    end
    ColorPickerFrame:SetColorRGB(initial.r, initial.g, initial.b)
    ColorPickerFrame.opacityFunc = nil
    ColorPickerFrame.hasOpacity = false
    ColorPickerFrame:Show()
end

local function openColorPicker(slot)
    local c = getSlot(slot) or { r = 0.8, g = 0.2, b = 0.2 }
    openColorPickerRgb(c, function(r, g, b)
        setSlotSolid(slot, r, g, b)
        setEditMode(0)
        refreshSlots()
        refreshColorPanel()
    end, function(r, g, b)
        setSlotSolid(slot, r, g, b)
        refreshSlots()
        refreshColorPanel()
    end)
end

pickBtn:SetScript("OnClick", function()
    if selectedSlot ~= nil then
        openColorPicker(selectedSlot)
    end
end)

copyToAllBtn:SetScript("OnClick", function()
    if selectedSlot == nil then
        return
    end
    local c = getSlot(selectedSlot)
    if not c or (c.mode or 0) ~= 0 then
        return
    end
    for _, entry in ipairs(EQUIP_SLOTS) do
        if entry.slot ~= selectedSlot then
            setSlotSolid(entry.slot, c.r, c.g, c.b)
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
    for _, entry in ipairs(EQUIP_SLOTS) do
        if entry.slot ~= selectedSlot then
            replaceSlotSelectiveRules(entry.slot, rules)
        end
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
        copyToAllBtn:Disable()
        pickHoleBtn:Disable()
        sampleHoleBtn:Disable()
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
    pickHoleBtn:Enable()
    sampleHoleBtn:Enable()
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

    if c and (c.mode or 0) == 0 then
        bigSwatch:SetVertexColor(c.r, c.g, c.b)
        copyToAllBtn:Enable()
    else
        bigSwatch:SetVertexColor(0.45, 0.45, 0.45)
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
        if type(WXL_RecolorSetSlotSelective) == "function" then
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
boot:RegisterEvent("PLAYER_LOGIN")
boot:RegisterEvent("VARIABLES_LOADED")
boot:RegisterEvent("PLAYER_ENTERING_WORLD")
boot:RegisterEvent("PLAYER_LEAVING_WORLD")

local function scheduleWorldBodyRefresh()
    ensureDB()
    -- Sync SavedVariables → C++ only. Do NOT ForceBodyRebuild: that post-assembly
    -- 0x3FF rebuild is what made in-world look worse than char-select Enter World.
    pushAll(false)
end

boot:SetScript("OnEvent", function(_, event, arg1)
    if event == "ADDON_LOADED" and arg1 == ADDON_NAME then
        ensureDB()
        -- Defer until world/API ready — avoid early partial pastes.
    elseif event == "PLAYER_LEAVING_WORLD" then
        -- Drop TextureCache tint backups before glue reuses pointers.
        if type(WXL_RecolorFlushTex) == "function" then
            WXL_RecolorFlushTex()
        end
    elseif event == "VARIABLES_LOADED" or event == "PLAYER_LOGIN" then
        scheduleWorldBodyRefresh()
    elseif event == "PLAYER_ENTERING_WORLD" then
        scheduleWorldBodyRefresh()
    end
end)
