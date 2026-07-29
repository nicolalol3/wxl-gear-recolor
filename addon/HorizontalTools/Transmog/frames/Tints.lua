local addon, ns = ...

local mainFrame = ns.mainFrame
if not mainFrame or not mainFrame.tabs or not mainFrame.tabs.tints then
    return
end

local tintsTab = mainFrame.tabs.tints

local SLOT_ORDER = {
    "Head", "Shoulder", "Back", "Chest", "Shirt", "Tabard",
    "Wrist", "Hands", "Waist", "Legs", "Feet",
    "Main Hand", "Off-hand", "Ranged",
}

local function deepCopyEntry(e)
    if type(e) ~= "table" then
        return nil
    end
    local out = {}
    for k, v in pairs(e) do
        if type(v) == "table" then
            out[k] = {}
            for k2, v2 in pairs(v) do
                if type(v2) == "table" then
                    out[k][k2] = {}
                    for k3, v3 in pairs(v2) do
                        out[k][k2][k3] = v3
                    end
                else
                    out[k][k2] = v2
                end
            end
        else
            out[k] = v
        end
    end
    return out
end

local SLOT_TO_RECOLOR = {
    ["Head"] = 0,
    ["Shoulder"] = 2,
    ["Back"] = 14,
    ["Chest"] = 4,
    ["Shirt"] = 3,
    ["Tabard"] = 18,
    ["Wrist"] = 8,
    ["Hands"] = 9,
    ["Waist"] = 5,
    ["Legs"] = 6,
    ["Feet"] = 7,
    ["Main Hand"] = 15,
    ["Off-hand"] = 16,
    ["Ranged"] = 17,
}

local function clamp01(v)
    v = tonumber(v) or 0
    if v < 0 then return 0 end
    if v > 1 then return 1 end
    return v
end

local function ensureDB()
    if type(WXLRecolorDB) ~= "table" then
        WXLRecolorDB = {}
    end
    if type(WXLRecolorDB.slots) ~= "table" then
        WXLRecolorDB.slots = {}
    end
end

local ADDON_MSG_PREFIX = "WXL_TINT"

local function slotHasItem(slot)
    slot = tonumber(slot)
    if not slot or type(GetInventoryItemID) ~= "function" then
        return false
    end
    local id = GetInventoryItemID("player", slot + 1)
    return id ~= nil and id ~= 0
end

local function syncSlotToServer(slot, allowClear)
    slot = tonumber(slot)
    if not slot or type(SendAddonMessage) ~= "function" then
        return
    end
    local function sendTint(body)
        SendAddonMessage(ADDON_MSG_PREFIX, body, "WHISPER", UnitName("player"))
    end
    if not slotHasItem(slot) then
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
    if allowClear ~= false then
        sendTint(string.format("CLEAR\t%d", slot))
    end
end

local function rgbToHsv(r, g, b)
    r, g, b = clamp01(r), clamp01(g), clamp01(b)
    local mx = math.max(r, g, b)
    local mn = math.min(r, g, b)
    local d = mx - mn
    local h = 0
    if d > 1e-6 then
        if mx == r then
            h = ((g - b) / d) % 6
        elseif mx == g then
            h = (b - r) / d + 2
        else
            h = (r - g) / d + 4
        end
        h = h / 6
        if h < 0 then
            h = h + 1
        end
    end
    local s = (mx > 1e-6) and (d / mx) or 0
    return h, s, mx
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

local function getSlotEntry(recolorSlot)
    ensureDB()
    local e = WXLRecolorDB.slots[tostring(recolorSlot)]
    if type(e) ~= "table" then
        return nil
    end
    return e
end

local MAX_SEL_PAIRS = 3
local EMPTY_RGB = { 0.18, 0.18, 0.18 }
local GOLD = { 1, 0.82, 0 }
local GRAY = { 0.55, 0.55, 0.55 }
local DIM = { 0.32, 0.32, 0.32 }

local PAIR_SRC_LABELS = { "A", "B", "C" }
local PAIR_DST_LABELS = { "A1", "B1", "C1" }

local function showWidgetTooltip(owner, anchor, lines)
    GameTooltip:SetOwner(owner, anchor or "ANCHOR_RIGHT")
    GameTooltip:ClearLines()
    for _, line in ipairs(lines) do
        GameTooltip:AddLine(line[1], line[2] or 1, line[3] or 1, line[4] or 1, line[5])
    end
    GameTooltip:Show()
end

local function bindTooltip(widget, anchor, linesOrText)
    widget:HookScript("OnEnter", function(self)
        if type(linesOrText) == "string" then
            showWidgetTooltip(self, anchor, { { linesOrText, 1, 1, 1, true } })
        else
            showWidgetTooltip(self, anchor, linesOrText)
        end
    end)
    widget:HookScript("OnLeave", function()
        GameTooltip:Hide()
    end)
end

local tintMode = "one" -- "one" | "multi"
local monochromeOn = false
local swatchSelected = false
local currentRgb = { r = 0.4, g = 0.4, b = 0.4 }
local tintsActive = false
local pickerUpdating = false
local hueUpdating = false
local activeMultiPair = nil -- 1..3

-- Live state per recolor-slot (survives slot switches; used by Copy from).
-- false = cleared this session; table = { ui=, one=, multi= }
local sessionStates = {}
local pendingTintEntry = nil
local activeSessionSlot = nil

local function emptyMultiPair()
    return {
        srcSet = false,
        dstSet = false,
        dstTouched = false, -- false = keep cloning from catch (A→A1) until user edits
        sr = 0.5, sg = 0.5, sb = 0.5,
        r = 0.5, g = 0.5, b = 0.5,
        tol = 0.35,
    }
end

local multiPairs = { emptyMultiPair(), emptyMultiPair(), emptyMultiPair() }

local modeRow = CreateFrame("Frame", nil, tintsTab)
modeRow:SetSize(420, 24)
modeRow:SetPoint("TOPLEFT", 12, -10)

local oneBtn = CreateFrame("Button", nil, modeRow, "UIPanelButtonTemplate")
oneBtn:SetSize(100, 22)
oneBtn:SetPoint("LEFT", 0, 0)
oneBtn:SetText("One Color")

local multiBtn = CreateFrame("Button", nil, modeRow, "UIPanelButtonTemplate")
multiBtn:SetSize(100, 22)
multiBtn:SetPoint("LEFT", oneBtn, "RIGHT", 8, 0)
multiBtn:SetText("Multicolor")

local advanced = CreateFrame("CheckButton", "$parentAdvancedMode", tintsTab, "UICheckButtonTemplate")
advanced:SetPoint("LEFT", multiBtn, "RIGHT", 12, 0)
advanced:Disable()
advanced:SetAlpha(0.45)
_G[advanced:GetName().."Text"]:SetText("Advanced")
_G[advanced:GetName().."Text"]:SetFontObject("GameFontDisableSmall")

local monoPane = CreateFrame("Frame", nil, tintsTab)
monoPane:SetPoint("TOPLEFT", 12, -40)
monoPane:SetPoint("BOTTOMRIGHT", -12, 8)

local multiPane = CreateFrame("Frame", nil, tintsTab)
multiPane:SetPoint("TOPLEFT", 12, -40)
multiPane:SetPoint("BOTTOMRIGHT", -12, 8)
multiPane:Hide()

---------------------------------------------------------------------------
-- One Color swatch
---------------------------------------------------------------------------
local swatchBtn = CreateFrame("Button", "$parentTintSwatch", monoPane, "ItemButtonTemplate")
swatchBtn:SetSize(36, 36)
swatchBtn:SetPoint("TOPLEFT", 0, 0)
swatchBtn:RegisterForClicks("LeftButtonUp")
swatchBtn:EnableMouse(true)
local swatchIcon = _G[swatchBtn:GetName() .. "IconTexture"]
if swatchIcon then
    swatchIcon:SetTexture("Interface\\Buttons\\WHITE8X8")
    swatchIcon:SetTexCoord(0, 1, 0, 1)
    swatchIcon:Show()
end

local swatchGloss = swatchBtn:CreateTexture(nil, "OVERLAY", nil, 1)
swatchGloss:SetTexture("Interface\\Buttons\\WHITE8X8")
if swatchIcon then
    swatchGloss:SetAllPoints(swatchIcon)
else
    swatchGloss:SetPoint("TOPLEFT", 4, -4)
    swatchGloss:SetPoint("BOTTOMRIGHT", -4, 4)
end
if swatchGloss.SetGradientAlpha then
    swatchGloss:SetGradientAlpha("VERTICAL", 1, 1, 1, 0.22, 0, 0, 0, 0.18)
else
    swatchGloss:SetVertexColor(1, 1, 1, 0.06)
end

swatchBtn.selectedBorder = swatchBtn:CreateTexture(nil, "OVERLAY", nil, 2)
swatchBtn.selectedBorder:SetTexture("Interface\\Buttons\\CheckButtonHilight")
swatchBtn.selectedBorder:SetBlendMode("ADD")
swatchBtn.selectedBorder:SetVertexColor(1, 0.85, 0.15, 0.9)
swatchBtn.selectedBorder:SetAllPoints()
swatchBtn.selectedBorder:Hide()

swatchBtn.outerRing = swatchBtn:CreateTexture(nil, "OVERLAY", nil, 3)
swatchBtn.outerRing:SetTexture("Interface\\Buttons\\UI-Quickslot2")
swatchBtn.outerRing:SetVertexColor(1, 0.82, 0)
swatchBtn.outerRing:SetPoint("TOPLEFT", -4, 4)
swatchBtn.outerRing:SetPoint("BOTTOMRIGHT", 4, -4)
swatchBtn.outerRing:Hide()

local monoCheck = CreateFrame("CheckButton", "$parentMonoCheck", monoPane, "UICheckButtonTemplate")
monoCheck:SetPoint("LEFT", swatchBtn, "RIGHT", 12, 0)
_G[monoCheck:GetName().."Text"]:SetText("Monochrome")
monoCheck:SetChecked(false)

bindTooltip(oneBtn, "ANCHOR_RIGHT",
    "The item will be recolored to match ONE color.")

bindTooltip(multiBtn, "ANCHOR_RIGHT", {
    {
        "Pick 3 colors ( A B C ) from the item. Then pick 3 new colors "
            .. "( A1 B1 C1 ). Old colors will match the new colors ( eg. A->A1 ).",
        1, 1, 1, true,
    },
    {
        "The Transmog Panel will attempt to pick A B C automatically by "
            .. "reading the texture. Accuracy is not guaranteed.",
        1, 1, 1, true,
    },
})

bindTooltip(monoCheck, "ANCHOR_RIGHT", {
    {
        "OFF: The original colors of the item will match the chosen one, "
            .. "but they'll follow their original light/dark tonality.",
        1, 1, 1, true,
    },
    {
        "ON: The original colors match the chosen one, their original "
            .. "light/dark tonality is ignored.",
        1, 1, 1, true,
    },
})

local TOL_SLIDER_TOOLTIP = {
    {
        "This is opacity. Higher value means a higher quantity of colors "
            .. "close to the selected one will match the new color.",
        1, 1, 1, true,
    },
    {
        "Too high or too low values can lead to pixellation. Recommended: 0.35.",
        1, 1, 1, true,
    },
}

---------------------------------------------------------------------------
-- Shared color picker (reparented between mono / multi)
---------------------------------------------------------------------------
local picker = CreateFrame("Frame", nil, monoPane)
picker:SetPoint("TOPLEFT", swatchBtn, "BOTTOMLEFT", 0, -10)
picker:Hide()

local WHEEL_GAP = 16
local SLIDER_GAP = 28
local WHEEL_SIZE = 128
local SLIDER_H = 104
local WHEEL_BORDER = math.floor(WHEEL_SIZE * (192 / 140) + 0.5)
local VALUE_W = 14
local VALUE_H = SLIDER_H
local VALUE_PAD = 6
local VALUE_BLOCK_W = VALUE_W + VALUE_PAD * 2
local VALUE_BLOCK_H = SLIDER_H + VALUE_PAD * 2
local BORDER_INSET = (WHEEL_BORDER - WHEEL_SIZE) / 2

local wheelHost = CreateFrame("Frame", nil, picker)
wheelHost:SetSize(WHEEL_SIZE, WHEEL_SIZE)
wheelHost:SetPoint("TOPLEFT", BORDER_INSET, -BORDER_INSET)
wheelHost:SetFrameLevel(picker:GetFrameLevel() + 2)

local colorSelect = CreateFrame("ColorSelect", "$parentColorSelect", picker)
colorSelect:SetPoint("TOPLEFT", wheelHost, "TOPLEFT", 0, 0)
colorSelect:SetSize(WHEEL_SIZE + WHEEL_GAP + VALUE_BLOCK_W, WHEEL_SIZE)
colorSelect:SetFrameLevel(wheelHost:GetFrameLevel())

local wheel = colorSelect:CreateTexture(nil, "BACKGROUND")
wheel:SetSize(WHEEL_SIZE, WHEEL_SIZE)
wheel:SetPoint("TOPLEFT", wheelHost, "TOPLEFT", 0, 0)
wheel:SetTexture("Interface\\Glues\\CharacterCreate\\ColorPickerColorWheel")
colorSelect:SetColorWheelTexture(wheel)

local RING_OX = -7.4
local RING_OY = -21.0
local RING_SIZE = 176

local wheelChrome = CreateFrame("Frame", nil, picker)
wheelChrome:SetSize(RING_SIZE, RING_SIZE)
wheelChrome:SetPoint("CENTER", wheelHost, "CENTER", RING_OX, RING_OY)
wheelChrome:SetFrameLevel(colorSelect:GetFrameLevel() + 6)
wheelChrome:EnableMouse(false)

local wheelRing = wheelChrome:CreateTexture(nil, "ARTWORK")
wheelRing:SetTexture("Interface\\Minimap\\UI-Minimap-Border")
wheelRing:SetTexCoord(0.25, 1.0, 0.125, 0.875)
wheelRing:SetAllPoints(wheelChrome)

local thumbLayer = CreateFrame("Frame", nil, picker)
thumbLayer:SetAllPoints(wheelHost)
thumbLayer:SetFrameLevel(colorSelect:GetFrameLevel() + 16)
thumbLayer:EnableMouse(false)

local wheelThumb = thumbLayer:CreateTexture(nil, "OVERLAY")
wheelThumb:SetTexture("Interface\\Buttons\\UI-ColorPicker-Buttons")
wheelThumb:SetSize(10, 10)
wheelThumb:SetTexCoord(0, 0.15625, 0, 0.625)
colorSelect:SetColorWheelThumbTexture(wheelThumb)

local valueWrap = CreateFrame("Frame", nil, picker)
valueWrap:SetSize(VALUE_BLOCK_W, VALUE_BLOCK_H)
valueWrap:SetPoint("TOPLEFT", wheelHost, "TOPRIGHT", WHEEL_GAP, 0)
valueWrap:SetFrameLevel(picker:GetFrameLevel() + 1)
if valueWrap.SetBackdrop then
    valueWrap:SetBackdrop({
        bgFile = "Interface\\Buttons\\WHITE8X8",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true,
        tileSize = 8,
        edgeSize = 12,
        insets = { left = VALUE_PAD, right = VALUE_PAD, top = VALUE_PAD, bottom = VALUE_PAD },
    })
    valueWrap:SetBackdropColor(0, 0, 0, 1)
    valueWrap:SetBackdropBorderColor(0.78, 0.78, 0.78, 1)
end

local valueTex = colorSelect:CreateTexture(nil, "ARTWORK")
valueTex:SetSize(VALUE_W, VALUE_H)
valueTex:SetPoint("CENTER", valueWrap, "CENTER", 0, 0)
colorSelect:SetColorValueTexture(valueTex)

local valueThumb = colorSelect:CreateTexture(nil, "OVERLAY")
valueThumb:SetTexture("Interface\\Buttons\\UI-ColorPicker-Buttons")
valueThumb:SetSize(44, 14)
valueThumb:SetTexCoord(0.25, 1.0, 0, 0.875)
colorSelect:SetColorValueThumbTexture(valueThumb)

picker:SetSize(RING_SIZE + WHEEL_GAP + VALUE_BLOCK_W + SLIDER_GAP + 24, RING_SIZE)

local hueSlider = CreateFrame("Slider", "$parentHueSlider", picker, "OptionsSliderTemplate")
hueSlider:SetOrientation("VERTICAL")
hueSlider:SetWidth(16)
hueSlider:SetHeight(SLIDER_H)
hueSlider:SetPoint("LEFT", valueWrap, "RIGHT", SLIDER_GAP, 0)
hueSlider:SetPoint("TOP", valueWrap, "TOP", 0, 0)
hueSlider:SetFrameLevel(colorSelect:GetFrameLevel() + 5)
hueSlider:EnableMouse(true)
hueSlider:SetMinMaxValues(0, 360)
hueSlider:SetValueStep(1)
hueSlider:SetValue(0)
if hueSlider.SetObeyStepOnDrag then
    hueSlider:SetObeyStepOnDrag(true)
end
_G[hueSlider:GetName().."Low"]:SetText("")
_G[hueSlider:GetName().."High"]:SetText("")
_G[hueSlider:GetName().."Text"]:SetText("")

local lightLabel = picker:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
lightLabel:SetPoint("BOTTOM", valueWrap, "TOP", 0, 2)
lightLabel:SetText("Light")

local hueLabel = picker:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
hueLabel:SetPoint("BOTTOM", hueSlider, "TOP", 0, 2)
hueLabel:SetText("Hue")

---------------------------------------------------------------------------
-- Multicolor: Tol / A B C / arrows / A1 B1 C1  (same as /recolor selective)
---------------------------------------------------------------------------
local multiCols = {}

local function refreshMultiHoleVisual(hole)
    local p = multiPairs[hole.pair]
    local set = (hole.which == "src") and p.srcSet or p.dstSet
    local icon = hole.icon
    if icon then
        if set then
            if hole.which == "src" then
                icon:SetVertexColor(p.sr, p.sg, p.sb)
            else
                icon:SetVertexColor(p.r, p.g, p.b)
            end
        else
            icon:SetVertexColor(EMPTY_RGB[1], EMPTY_RGB[2], EMPTY_RGB[3])
        end
    end
    local active = (activeMultiPair == hole.pair)
    if hole.selectedBorder then
        hole.selectedBorder:Hide()
    end
    if hole.outerRing then
        hole.outerRing:Hide()
    end
    if active and hole.which == "dst" then
        if hole.selectedBorder then
            hole.selectedBorder:SetVertexColor(1, 0.85, 0.15, 0.9)
            hole.selectedBorder:Show()
        end
        if hole.outerRing then
            hole.outerRing:SetVertexColor(GOLD[1], GOLD[2], GOLD[3])
            hole.outerRing:Show()
        end
    elseif active and hole.which == "src" then
        if hole.selectedBorder then
            hole.selectedBorder:SetVertexColor(0.75, 0.75, 0.75, 0.7)
            hole.selectedBorder:Show()
        end
        if hole.outerRing then
            hole.outerRing:SetVertexColor(GRAY[1], GRAY[2], GRAY[3])
            hole.outerRing:Show()
        end
    end
end

local function refreshAllMultiHoles()
    for i = 1, MAX_SEL_PAIRS do
        local col = multiCols[i]
        if col then
            refreshMultiHoleVisual(col.srcHole)
            refreshMultiHoleVisual(col.dstHole)
            if col.tolSlider then
                local tol = multiPairs[i].tol or 0.35
                col.tolSlider:SetValue(tol)
                local txt = _G[col.tolSlider:GetName() .. "Text"]
                if txt then
                    txt:SetText(string.format("%.2f", tol))
                end
            end
        end
    end
end

local function makeMultiHole(parent, name, pair, which, size)
    local hole = CreateFrame("Button", name, parent, "ItemButtonTemplate")
    hole:SetSize(size, size)
    hole.pair = pair
    hole.which = which
    hole:RegisterForClicks("LeftButtonUp")
    hole:EnableMouse(true)

    local icon = _G[hole:GetName() .. "IconTexture"]
    if icon then
        icon:SetTexture("Interface\\Buttons\\WHITE8X8")
        icon:SetTexCoord(0, 1, 0, 1)
        icon:Show()
        icon:SetVertexColor(EMPTY_RGB[1], EMPTY_RGB[2], EMPTY_RGB[3])
    end
    hole.icon = icon
    hole.tex = icon -- alias used by older refresh paths

    local gloss = hole:CreateTexture(nil, "OVERLAY", nil, 1)
    gloss:SetTexture("Interface\\Buttons\\WHITE8X8")
    if icon then
        gloss:SetAllPoints(icon)
    else
        gloss:SetPoint("TOPLEFT", 4, -4)
        gloss:SetPoint("BOTTOMRIGHT", -4, 4)
    end
    if gloss.SetGradientAlpha then
        gloss:SetGradientAlpha("VERTICAL", 1, 1, 1, 0.22, 0, 0, 0, 0.18)
    else
        gloss:SetVertexColor(1, 1, 1, 0.06)
    end
    hole.gloss = gloss

    hole.selectedBorder = hole:CreateTexture(nil, "OVERLAY", nil, 2)
    hole.selectedBorder:SetTexture("Interface\\Buttons\\CheckButtonHilight")
    hole.selectedBorder:SetBlendMode("ADD")
    hole.selectedBorder:SetVertexColor(1, 0.85, 0.15, 0.9)
    hole.selectedBorder:SetAllPoints()
    hole.selectedBorder:Hide()

    hole.outerRing = hole:CreateTexture(nil, "OVERLAY", nil, 3)
    hole.outerRing:SetTexture("Interface\\Buttons\\UI-Quickslot2")
    hole.outerRing:SetVertexColor(GOLD[1], GOLD[2], GOLD[3])
    hole.outerRing:SetPoint("TOPLEFT", -4, 4)
    hole.outerRing:SetPoint("BOTTOMRIGHT", 4, -4)
    hole.outerRing:Hide()

    return hole
end

-- Spread A/B/C across available width; drop block ~10px for breathing room.
local HOLE = 36
local MULTI_TOP = -10
local MULTI_SPAN = 340
local COL_W = math.floor(MULTI_SPAN / MAX_SEL_PAIRS)
local multiAnchor = CreateFrame("Frame", nil, multiPane)
multiAnchor:SetSize(MULTI_SPAN, 160)
multiAnchor:SetPoint("TOPLEFT", 0, MULTI_TOP)

-- Talent-frame down arrow (UI-TalentArrows, "top" branch — same asset as talent links).
local MULTI_ARROW_TEX = "Interface\\TalentFrame\\UI-TalentArrows"
local MULTI_ARROW_SIZE = 32
-- top[1] from TalentFrameBase.lua; full 32x32 cell — 16x16 showed only the tip.
local MULTI_ARROW_DOWN_COORDS = { 0, 0.5, 0, 0.5 }

for i = 1, MAX_SEL_PAIRS do
    local colX = (i - 1) * COL_W
    local holeX = colX + math.floor((COL_W - HOLE) / 2)

    local tolName = "HT_TintsTol" .. i
    local tolSlider = CreateFrame("Slider", tolName, multiPane, "OptionsSliderTemplate")
    tolSlider:SetWidth(math.max(72, COL_W - 12))
    tolSlider:SetHeight(14)
    tolSlider:SetPoint("TOPLEFT", multiAnchor, "TOPLEFT", colX + 6, 0)
    tolSlider:SetMinMaxValues(0.05, 0.5)
    tolSlider:SetValueStep(0.01)
    tolSlider:SetValue(0.35)
    _G[tolName .. "Low"]:SetText("")
    _G[tolName .. "High"]:SetText("")
    _G[tolName .. "Text"]:SetText("0.35")
    tolSlider.pairIndex = i
    tolSlider:SetScript("OnValueChanged", function(self, value)
        multiPairs[self.pairIndex].tol = value
        _G[self:GetName() .. "Text"]:SetText(string.format("%.2f", value))
        if ns._tintsOnMultiEdited then
            ns._tintsOnMultiEdited()
        end
    end)
    tolSlider:EnableMouse(true)
    bindTooltip(tolSlider, "ANCHOR_RIGHT", TOL_SLIDER_TOOLTIP)
    local tolThumb = _G[tolName .. "Thumb"]
    if tolThumb and tolThumb.HookScript then
        bindTooltip(tolThumb, "ANCHOR_RIGHT", TOL_SLIDER_TOOLTIP)
    end

    local srcHole = makeMultiHole(multiPane, "HT_TintsMultiSrc" .. i, i, "src", HOLE)
    srcHole:SetPoint("TOPLEFT", multiAnchor, "TOPLEFT", holeX, -28)
    bindTooltip(srcHole, "ANCHOR_RIGHT", PAIR_SRC_LABELS[i])

    local arrow = multiPane:CreateTexture(nil, "OVERLAY")
    arrow:SetTexture(MULTI_ARROW_TEX)
    arrow:SetSize(MULTI_ARROW_SIZE, MULTI_ARROW_SIZE)
    arrow:SetTexCoord(
        MULTI_ARROW_DOWN_COORDS[1], MULTI_ARROW_DOWN_COORDS[2],
        MULTI_ARROW_DOWN_COORDS[3], MULTI_ARROW_DOWN_COORDS[4])
    arrow:SetPoint("TOPLEFT", srcHole, "BOTTOMLEFT", math.floor((HOLE - MULTI_ARROW_SIZE) / 2), -2)

    local dstHole = makeMultiHole(multiPane, "HT_TintsMultiDst" .. i, i, "dst", HOLE)
    dstHole:SetPoint("TOP", srcHole, "BOTTOM", 0, -36)
    bindTooltip(dstHole, "ANCHOR_RIGHT", PAIR_DST_LABELS[i])

    multiCols[i] = {
        srcHole = srcHole,
        dstHole = dstHole,
        tolSlider = tolSlider,
        arrow = arrow,
    }
end

local multiPickerAnchor = CreateFrame("Frame", nil, multiPane)
multiPickerAnchor:SetSize(1, 1)
multiPickerAnchor:SetPoint("TOPLEFT", multiCols[1].dstHole, "BOTTOMLEFT", -24, -14)

local function currentRecolorSlot()
    local name = ns.GetCurrentTransmogSlot and ns.GetCurrentTransmogSlot() or nil
    if not name then
        return nil, nil
    end
    return SLOT_TO_RECOLOR[name], name
end

local function refreshSwatchFill()
    if swatchIcon then
        swatchIcon:SetVertexColor(currentRgb.r, currentRgb.g, currentRgb.b)
    end
end

local function getSelectiveRules(e)
    if type(e) ~= "table" or type(e.rules) ~= "table" then
        return {}
    end
    return e.rules
end

local function lockPickerLayout()
    wheel:ClearAllPoints()
    wheel:SetSize(WHEEL_SIZE, WHEEL_SIZE)
    wheel:SetPoint("TOPLEFT", wheelHost, "TOPLEFT", 0, 0)
    valueTex:ClearAllPoints()
    valueTex:SetSize(VALUE_W, VALUE_H)
    valueTex:SetPoint("CENTER", valueWrap, "CENTER", 0, 0)
end

picker:SetScript("OnUpdate", function(self)
    if self:IsShown() then
        lockPickerLayout()
    end
end)

local function syncPickerFromCurrent()
    pickerUpdating = true
    local r, g, b = currentRgb.r, currentRgb.g, currentRgb.b
    local h, s, v = rgbToHsv(r, g, b)
    if colorSelect.SetColorHSV then
        colorSelect:SetColorHSV(h, s, v)
    else
        colorSelect:SetColorRGB(r, g, b)
    end
    hueUpdating = true
    hueSlider:SetValue(math.floor(h * 360 + 0.5))
    hueUpdating = false
    lockPickerLayout()
    refreshSwatchFill()
    pickerUpdating = false
end

local function rulesFromMultiPairs()
    local rules = {}
    for i = 1, MAX_SEL_PAIRS do
        local p = multiPairs[i]
        if p.srcSet and p.dstSet then
            rules[#rules + 1] = {
                sr = p.sr, sg = p.sg, sb = p.sb,
                r = p.r, g = p.g, b = p.b,
                tol = p.tol or 0.35,
            }
        end
    end
    return rules
end

local function snapshotMultiPairs()
    local out = {}
    for i = 1, MAX_SEL_PAIRS do
        local p = multiPairs[i]
        out[i] = {
            srcSet = p.srcSet and true or false,
            sr = p.sr, sg = p.sg, sb = p.sb,
            dstSet = p.dstSet and true or false,
            dstTouched = p.dstTouched and true or false,
            r = p.r, g = p.g, b = p.b,
            tol = p.tol or 0.35,
        }
    end
    return out
end

local function multiPackageHasSources(multi)
    if type(multi) ~= "table" then
        return false
    end
    for i = 1, MAX_SEL_PAIRS do
        local d = multi[i]
        if type(d) == "table" and d.srcSet then
            return true
        end
    end
    return false
end

-- Restore A/B/C + A1/B1/C1 (+ tol). Used by session restore and Copy from.
local function applyMultiPairs(pairs)
    for i = 1, MAX_SEL_PAIRS do
        local d = pairs and pairs[i]
        local p = multiPairs[i]
        if type(d) == "table" then
            if d.srcSet then
                p.srcSet = true
                p.sr = clamp01(d.sr or 0.5)
                p.sg = clamp01(d.sg or 0.5)
                p.sb = clamp01(d.sb or 0.5)
            else
                p.srcSet = false
                p.sr, p.sg, p.sb = 0.5, 0.5, 0.5
            end
            if d.dstSet then
                p.dstSet = true
                p.dstTouched = d.dstTouched ~= false
                p.r = clamp01(d.r or 0.5)
                p.g = clamp01(d.g or 0.5)
                p.b = clamp01(d.b or 0.5)
            else
                p.dstSet = false
                p.dstTouched = false
                p.r, p.g, p.b = 0.5, 0.5, 0.5
            end
            p.tol = tonumber(d.tol) or 0.35
        else
            multiPairs[i] = emptyMultiPair()
        end
    end
end

local function buildOneEntryFromUI()
    local r, g, b = currentRgb.r, currentRgb.g, currentRgb.b
    if monochromeOn then
        return { mode = 0, r = r, g = g, b = b }
    end
    return { mode = 2, stops = 3, fill = 0, r = r, g = g, b = b, colors = {} }
end

local function buildMultiEntryFromUI()
    local rules = rulesFromMultiPairs()
    if #rules == 0 then
        return nil
    end
    local last = rules[#rules]
    return {
        mode = 1,
        rules = rules,
        r = last.r, g = last.g, b = last.b,
        sr = last.sr, sg = last.sg, sb = last.sb,
        tol = last.tol,
    }
end

local function packSessionState()
    return {
        ui = tintMode,
        one = (tintMode == "one") and buildOneEntryFromUI() or nil,
        multi = snapshotMultiPairs(),
    }
end

local function saveSessionForActive()
    if not activeSessionSlot then
        return
    end
    sessionStates[activeSessionSlot] = packSessionState()
    if tintMode == "one" then
        pendingTintEntry = buildOneEntryFromUI()
    else
        pendingTintEntry = buildMultiEntryFromUI()
    end
end

local function dbEntryToSession(e)
    if type(e) ~= "table" then
        return nil
    end
    if (e.mode or 0) == 1 then
        local multi = {}
        local rules = getSelectiveRules(e)
        for i = 1, MAX_SEL_PAIRS do
            local r = rules[i]
            if type(r) == "table" then
                multi[i] = {
                    srcSet = true,
                    sr = clamp01(r.sr or 0.5),
                    sg = clamp01(r.sg or 0.5),
                    sb = clamp01(r.sb or 0.5),
                    dstSet = true,
                    dstTouched = true,
                    r = clamp01(r.r or 0.5),
                    g = clamp01(r.g or 0.5),
                    b = clamp01(r.b or 0.5),
                    tol = tonumber(r.tol) or 0.35,
                }
            else
                multi[i] = emptyMultiPair()
            end
        end
        return { ui = "multi", one = nil, multi = multi }
    end
    return {
        ui = "one",
        one = deepCopyEntry(e),
        multi = nil,
    }
end

local function getSessionPackage(recolorSlot)
    if recolorSlot == nil then
        return nil
    end
    local sess = sessionStates[recolorSlot]
    if sess == false then
        return false
    end
    if type(sess) == "table" and sess.ui then
        return deepCopyEntry(sess)
    end
    -- Legacy session was a raw entry
    if type(sess) == "table" and sess.mode ~= nil then
        return dbEntryToSession(sess)
    end
    return dbEntryToSession(getSlotEntry(recolorSlot))
end

local function pushSelectiveLikeRecolor(slot, rules)
    if not slot then
        return
    end
    -- Same engine path as /recolor replaceSlotSelectiveRules → pushSlot.
    local batched = type(WXL_RecolorBeginBatch) == "function"
        and type(WXL_RecolorEndBatch) == "function"
    if batched then
        WXL_RecolorBeginBatch()
    end
    if type(WXL_RecolorClearSlot) == "function" then
        WXL_RecolorClearSlot(slot)
    end
    if type(WXL_RecolorSetSlotSelective) == "function" and rules then
        for i, rule in ipairs(rules) do
            WXL_RecolorSetSlotSelective(slot,
                rule.sr or 0, rule.sg or 0, rule.sb or 0,
                rule.r or 0.5, rule.g or 0.5, rule.b or 0.5,
                rule.tol or 0.35, i > 1 and 1 or 0)
        end
    end
    if batched then
        WXL_RecolorEndBatch(1)
    end
    if type(WXL_RecolorArmPreviewCapture) == "function" then
        WXL_RecolorArmPreviewCapture()
    end
    if type(WXL_RecolorForceBodyRebuild) == "function" then
        WXL_RecolorForceBodyRebuild()
    end
    if ns.UpdateTransmogPreviewModel then
        ns.UpdateTransmogPreviewModel()
    end
    if ns.RefreshTransmogPreviewTint then
        ns.RefreshTransmogPreviewTint()
    end
end

local function pushDraftEntry(slot, e)
    if not slot then
        return
    end
    if not e then
        if type(WXL_RecolorSetSlotSelectiveDraft) == "function" then
            WXL_RecolorSetSlotSelectiveDraft(slot, 0)
        end
        if type(WXL_RecolorResetTint) == "function" then
            -- Only clear draft/committed when explicitly resetting; for nil pending
            -- during multi with incomplete pairs, clear selective draft only.
        end
        if type(WXL_RecolorSetSlotDraft) == "function" then
            -- leave solid draft alone unless full reset
        end
        if ns.UpdateTransmogPreviewModel then
            ns.UpdateTransmogPreviewModel()
        end
        if ns.RefreshTransmogPreviewTint then
            ns.RefreshTransmogPreviewTint()
        end
        return
    end
    if (e.mode or 0) == 1 then
        local rules = getSelectiveRules(e)
        if type(WXL_RecolorSetSlotSelectiveDraft) == "function" then
            if #rules == 0 then
                WXL_RecolorSetSlotSelectiveDraft(slot, 0)
            else
                local args = { slot, #rules }
                for _, rule in ipairs(rules) do
                    args[#args + 1] = rule.sr or 0
                    args[#args + 1] = rule.sg or 0
                    args[#args + 1] = rule.sb or 0
                    args[#args + 1] = rule.r or 0.5
                    args[#args + 1] = rule.g or 0.5
                    args[#args + 1] = rule.b or 0.5
                    args[#args + 1] = rule.tol or 0.35
                end
                WXL_RecolorSetSlotSelectiveDraft(unpack(args))
            end
        end
    elseif (e.mode or 0) == 2 then
        if type(WXL_RecolorSetSlotGradientDraft) == "function" then
            local nStops = e.stops or 3
            local fill = e.fill or 0
            if fill == 0 then
                WXL_RecolorSetSlotGradientDraft(slot, nStops, 0, e.r or 0.4, e.g or 0.4, e.b or 0.4)
            else
                local args = { slot, nStops, 1 }
                for i = 1, nStops do
                    local col = e.colors and e.colors[i]
                    args[#args + 1] = col and col.r or e.r
                    args[#args + 1] = col and col.g or e.g
                    args[#args + 1] = col and col.b or e.b
                end
                WXL_RecolorSetSlotGradientDraft(unpack(args))
            end
        end
    elseif type(WXL_RecolorSetSlotDraft) == "function" then
        WXL_RecolorSetSlotDraft(slot, e.r or 0.4, e.g or 0.4, e.b or 0.4)
    end
    -- Never full Dress while drafting: TryOn pastes with preferDraft=off and
    -- overwrites the live draft tint. C++ ForcePreviewOnlyRebuild + tint refresh.
    if ns.RefreshTransmogPreviewTint then
        ns.RefreshTransmogPreviewTint()
    end
end

local function pushDraft()
    local slot = select(1, currentRecolorSlot())
    if not slot then
        return
    end
    saveSessionForActive()
    pushDraftEntry(slot, pendingTintEntry)
end

-- Same engine as /recolor runCatchColors (player catch bank). Auto on Multicolor.
local function runCatchForCurrentSlot()
    local slot = select(1, currentRecolorSlot())
    if not slot or type(WXL_RecolorCatchSlotColors) ~= "function" then
        return false
    end
    -- Refresh local-player paste bank so Catch sees the same sample as /recolor.
    if type(WXL_RecolorForceBodyRebuild) == "function" then
        WXL_RecolorForceBodyRebuild()
    end
    local n, r1, g1, b1, r2, g2, b2, r3, g3, b3 = WXL_RecolorCatchSlotColors(slot)
    n = tonumber(n) or 0
    if n < 1 then
        refreshAllMultiHoles()
        return false
    end
    local cols = {
        { r = r1, g = g1, b = b1 },
        { r = r2, g = g2, b = b2 },
        { r = r3, g = g3, b = b3 },
    }
    -- Exact /recolor loop: only set src for i=1..n (leave other pairs alone).
    for i = 1, math.min(n, MAX_SEL_PAIRS) do
        local c = cols[i]
        if c and c.r ~= nil then
            local p = multiPairs[i]
            p.srcSet = true
            p.sr = clamp01(c.r)
            p.sg = clamp01(c.g)
            p.sb = clamp01(c.b)
            -- C/P A→A1 equivalent for untouched destinations only.
            if not p.dstTouched then
                p.dstSet = true
                p.r, p.g, p.b = p.sr, p.sg, p.sb
            end
        end
    end
    refreshAllMultiHoles()
    return true
end

local function anchorPickerForMode()
    picker:ClearAllPoints()
    if tintMode == "multi" then
        picker:SetParent(multiPane)
        picker:SetPoint("TOPLEFT", multiPickerAnchor, "TOPLEFT", 0, 0)
    else
        picker:SetParent(monoPane)
        picker:SetPoint("TOPLEFT", swatchBtn, "BOTTOMLEFT", 0, -10)
    end
end

local function setSwatchSelected(sel)
    swatchSelected = sel and true or false
    if swatchSelected and tintMode == "one" then
        swatchBtn.selectedBorder:Show()
        swatchBtn.outerRing:Show()
        anchorPickerForMode()
        picker:Show()
        syncPickerFromCurrent()
        lockPickerLayout()
    else
        swatchBtn.selectedBorder:Hide()
        swatchBtn.outerRing:Hide()
        if tintMode ~= "multi" or not activeMultiPair then
            picker:Hide()
        end
    end
end

local function setActiveMultiPair(pair)
    activeMultiPair = pair
    refreshAllMultiHoles()
    if pair then
        local p = multiPairs[pair]
        -- Picker always edits A1/B1/C1 (destination), same as Pick on dst in /recolor.
        if p.dstSet then
            currentRgb.r, currentRgb.g, currentRgb.b = p.r, p.g, p.b
        elseif p.srcSet then
            currentRgb.r, currentRgb.g, currentRgb.b = p.sr, p.sg, p.sb
        else
            currentRgb.r, currentRgb.g, currentRgb.b = 0.5, 0.5, 0.5
        end
        anchorPickerForMode()
        picker:Show()
        syncPickerFromCurrent()
        lockPickerLayout()
    else
        picker:Hide()
    end
end

for i = 1, MAX_SEL_PAIRS do
    local col = multiCols[i]
    col.srcHole:SetScript("OnClick", function()
        setActiveMultiPair(i)
    end)
    col.dstHole:SetScript("OnClick", function()
        setActiveMultiPair(i)
    end)
end

local function onColorChanged(r, g, b)
    currentRgb.r, currentRgb.g, currentRgb.b = clamp01(r), clamp01(g), clamp01(b)
    refreshSwatchFill()
    if tintMode == "multi" and activeMultiPair then
        local p = multiPairs[activeMultiPair]
        p.r, p.g, p.b = currentRgb.r, currentRgb.g, currentRgb.b
        p.dstSet = true
        p.dstTouched = true
        refreshAllMultiHoles()
    end
    pushDraft()
end

ns._tintsOnMultiEdited = function()
    if tintMode == "multi" then
        if activeMultiPair then
            multiPairs[activeMultiPair].dstTouched = true
        end
        pushDraft()
    end
end

colorSelect:SetScript("OnColorSelect", function(self, r, g, b)
    if pickerUpdating then
        return
    end
    lockPickerLayout()
    onColorChanged(r, g, b)
end)

hueSlider:SetScript("OnValueChanged", function(self, value)
    if hueUpdating or pickerUpdating or not picker:IsShown() then
        return
    end
    local r, g, b = colorSelect:GetColorRGB()
    local _, s, v = rgbToHsv(r, g, b)
    local h = (tonumber(value) or 0) / 360
    local nr, ng, nb = hsvToRgb(h, s, v)
    hueUpdating = true
    pickerUpdating = true
    colorSelect:SetColorRGB(nr, ng, nb)
    pickerUpdating = false
    hueUpdating = false
    lockPickerLayout()
    onColorChanged(nr, ng, nb)
end)

swatchBtn:SetScript("OnClick", function()
    if tintMode == "one" and not swatchSelected then
        setSwatchSelected(true)
    end
end)

monoCheck:SetScript("OnClick", function(self)
    monochromeOn = self:GetChecked() and true or false
    if tintMode == "one" then
        pushDraft()
    end
end)

---------------------------------------------------------------------------
-- Bottom actions
---------------------------------------------------------------------------
local actionRow = CreateFrame("Frame", "HorizontalToolsTintsActionRow", mainFrame)
actionRow:SetHeight(24)
actionRow:SetPoint("BOTTOMLEFT", 410, 8)
actionRow:SetPoint("BOTTOMRIGHT", -6, 8)
actionRow:SetFrameStrata(mainFrame:GetFrameStrata() or "MEDIUM")
actionRow:SetFrameLevel((mainFrame.stats and mainFrame.stats:GetFrameLevel() or mainFrame:GetFrameLevel()) + 20)
actionRow:Hide()

local function ensureActionRowVisible()
    if not tintsActive then
        return
    end
    if mainFrame.stats and mainFrame.stats:IsShown() then
        mainFrame.stats:Hide()
    end
    actionRow:SetFrameLevel((mainFrame.stats and mainFrame.stats:GetFrameLevel() or mainFrame:GetFrameLevel()) + 20)
    actionRow:Show()
end

if mainFrame.stats then
    mainFrame.stats:HookScript("OnShow", function(self)
        if tintsActive then
            self:Hide()
            ensureActionRowVisible()
        end
    end)
end

actionRow:HookScript("OnHide", function(self)
    if tintsActive then
        self._restorePending = true
    end
end)

actionRow:SetScript("OnUpdate", function(self)
    if self._restorePending and tintsActive then
        self._restorePending = false
        ensureActionRowVisible()
    end
end)

local applyBtn = CreateFrame("Button", nil, actionRow, "UIPanelButtonTemplate")
applyBtn:SetSize(80, 22)
applyBtn:SetPoint("LEFT", 0, 0)
applyBtn:SetText("Apply")

local copyBtn = CreateFrame("Button", nil, actionRow, "UIPanelButtonTemplate")
copyBtn:SetSize(95, 22)
copyBtn:SetPoint("LEFT", applyBtn, "RIGHT", 8, 0)
copyBtn:SetText("Copy from...")

local resetBtn = CreateFrame("Button", nil, actionRow, "UIPanelButtonTemplate")
resetBtn:SetSize(100, 22)
resetBtn:SetPoint("LEFT", copyBtn, "RIGHT", 8, 0)
resetBtn:SetText("Reset Tint")

local copyDropDown = CreateFrame("Frame", "HorizontalToolsTintsCopyDropDown", actionRow, "UIDropDownMenuTemplate")
copyDropDown:Hide()

local function toggleCopyMenu()
    ToggleDropDownMenu(1, nil, copyDropDown, copyBtn, 0, 0)
    local list = _G["DropDownList1"]
    if list and copyBtn then
        list:ClearAllPoints()
        list:SetPoint("BOTTOMLEFT", copyBtn, "TOPLEFT", 0, 0)
    end
end

copyBtn:SetScript("OnClick", toggleCopyMenu)

local function commitSlotEntry(slot, e)
    if not slot then
        return
    end
    ensureDB()
    if e then
        WXLRecolorDB.slots[tostring(slot)] = deepCopyEntry(e)
    else
        WXLRecolorDB.slots[tostring(slot)] = nil
    end
    if not e then
        if type(WXL_RecolorResetTint) == "function" then
            WXL_RecolorResetTint(slot)
        elseif type(WXL_RecolorClearSlot) == "function" then
            WXL_RecolorClearSlot(slot)
        end
        if type(WXL_RecolorArmPreviewCapture) == "function" then
            WXL_RecolorArmPreviewCapture()
        end
        if type(WXL_RecolorForceBodyRebuild) == "function" then
            WXL_RecolorForceBodyRebuild()
        end
        if ns.UpdateTransmogPreviewModel then
            ns.UpdateTransmogPreviewModel()
        end
        syncSlotToServer(slot)
        return
    end
    -- Ensure draft matches UI, then commit draft → committed tint (solid + selective).
    pushDraftEntry(slot, e)
    if type(WXL_RecolorApplyDraft) == "function" then
        WXL_RecolorApplyDraft(slot)
    elseif (e.mode or 0) == 1 then
        pushSelectiveLikeRecolor(slot, getSelectiveRules(e))
    elseif type(WXL_RecolorBeginBatch) == "function" then
        WXL_RecolorBeginBatch()
        pushDraftEntry(slot, e)
        WXL_RecolorEndBatch(1)
    end
    if type(WXL_RecolorArmPreviewCapture) == "function" then
        WXL_RecolorArmPreviewCapture()
    end
    if type(WXL_RecolorForceBodyRebuild) == "function" then
        WXL_RecolorForceBodyRebuild()
    end
    if ns.UpdateTransmogPreviewModel then
        ns.UpdateTransmogPreviewModel()
    end
    syncSlotToServer(slot)
end

local function applyOneEntryToUI(e)
    if e and (e.mode == 0 or e.mode == 2) then
        currentRgb.r = clamp01(e.r or 0.4)
        currentRgb.g = clamp01(e.g or 0.4)
        currentRgb.b = clamp01(e.b or 0.4)
        monochromeOn = (e.mode == 0)
        monoCheck:SetChecked(monochromeOn)
    else
        currentRgb.r, currentRgb.g, currentRgb.b = 0.4, 0.4, 0.4
        monochromeOn = false
        monoCheck:SetChecked(false)
    end
    refreshSwatchFill()
end

local refreshModeButtons

local function applySessionPackage(pkg, opts)
    opts = opts or {}
    if pkg == false or pkg == nil then
        tintMode = "one"
        applyOneEntryToUI(nil)
        for i = 1, MAX_SEL_PAIRS do
            multiPairs[i] = emptyMultiPair()
        end
        activeMultiPair = nil
        refreshModeButtons()
        return
    end
    tintMode = (pkg.ui == "multi") and "multi" or "one"
    if tintMode == "one" then
        applyOneEntryToUI(pkg.one)
        if type(pkg.multi) == "table" then
            applyMultiPairs(pkg.multi)
        end
    else
        if type(pkg.multi) == "table" then
            applyMultiPairs(pkg.multi)
        else
            for i = 1, MAX_SEL_PAIRS do
                multiPairs[i] = emptyMultiPair()
            end
        end
    end
    -- Catch auto unless we already have A/B/C (Copy from / saved tint).
    local skipCatch = opts.skipCatch
        or opts.keepSources
        or multiPackageHasSources(pkg.multi)
    refreshModeButtons({ skipCatch = skipCatch })
end

refreshModeButtons = function(opts)
    opts = opts or {}
    if tintMode == "one" then
        oneBtn:Disable()
        multiBtn:Enable()
        monoPane:Show()
        multiPane:Hide()
        activeMultiPair = nil
        setSwatchSelected(true)
        anchorPickerForMode()
    else
        oneBtn:Enable()
        multiBtn:Disable()
        monoPane:Hide()
        multiPane:Show()
        swatchSelected = false
        swatchBtn.selectedBorder:Hide()
        swatchBtn.outerRing:Hide()
        anchorPickerForMode()
        if not opts.skipCatch then
            runCatchForCurrentSlot()
        else
            refreshAllMultiHoles()
        end
        if not activeMultiPair then
            setActiveMultiPair(1)
        else
            setActiveMultiPair(activeMultiPair)
        end
    end
end

local function copyTintFromSlot(srcName)
    local dstSlot, dstName = currentRecolorSlot()
    if not dstSlot or not srcName or srcName == dstName then
        return
    end
    local srcSlot = SLOT_TO_RECOLOR[srcName]
    if not srcSlot then
        return
    end
    saveSessionForActive()
    local pkg = getSessionPackage(srcSlot)
    if pkg == false then
        applySessionPackage(false)
        pushDraftEntry(dstSlot, nil)
        saveSessionForActive()
        return
    end
    if not pkg then
        return
    end
    -- Full tint clone: A/B/C + A1/B1/C1 (+ tol / mode) from the source slot.
    applySessionPackage(pkg, { skipCatch = true, keepSources = true })
    saveSessionForActive()
    pushDraft()
end

UIDropDownMenu_Initialize(copyDropDown, function()
    local currentName = select(2, currentRecolorSlot())
    for _, slotName in ipairs(SLOT_ORDER) do
        if slotName ~= currentName and SLOT_TO_RECOLOR[slotName] then
            local info = UIDropDownMenu_CreateInfo()
            info.text = slotName
            info.notCheckable = true
            info.func = function()
                copyTintFromSlot(slotName)
            end
            UIDropDownMenu_AddButton(info)
        end
    end
end)

applyBtn:SetScript("OnClick", function()
    local slot = select(1, currentRecolorSlot())
    if not slot then
        return
    end
    saveSessionForActive()
    if tintMode == "multi" then
        -- Use the SAME A/B/C that drove the live preview — do not re-Catch on Apply
        -- (Catch can return different clusters and preview≠result).
        pendingTintEntry = buildMultiEntryFromUI()
        if not pendingTintEntry then
            return
        end
    else
        pendingTintEntry = buildOneEntryFromUI()
    end
    sessionStates[slot] = packSessionState()
    commitSlotEntry(slot, pendingTintEntry)
end)

resetBtn:SetScript("OnClick", function()
    local slot = select(1, currentRecolorSlot())
    if not slot then
        return
    end
    pendingTintEntry = nil
    sessionStates[slot] = false
    currentRgb.r, currentRgb.g, currentRgb.b = 0.4, 0.4, 0.4
    monochromeOn = false
    monoCheck:SetChecked(false)
    for i = 1, MAX_SEL_PAIRS do
        multiPairs[i] = emptyMultiPair()
    end
    activeMultiPair = nil
    tintMode = "one"
    refreshModeButtons()
    commitSlotEntry(slot, nil)
    if picker:IsShown() then
        syncPickerFromCurrent()
    end
end)

oneBtn:SetScript("OnClick", function()
    saveSessionForActive()
    tintMode = "one"
    refreshModeButtons()
    saveSessionForActive()
    pushDraft()
end)

multiBtn:SetScript("OnClick", function()
    saveSessionForActive()
    tintMode = "multi"
    -- Auto-Catch unless A/B/C already set (Copy from / prior catch).
    refreshModeButtons({ skipCatch = multiPackageHasSources(multiPairs) })
    saveSessionForActive()
    pushDraft()
end)

local function loadFromSlot(slotName)
    if activeSessionSlot then
        saveSessionForActive()
    end

    local recolorSlot = SLOT_TO_RECOLOR[slotName or ""]
    activeSessionSlot = recolorSlot
    if not recolorSlot then
        pendingTintEntry = nil
        applySessionPackage(nil)
        return
    end

    local pkg = getSessionPackage(recolorSlot)
    applySessionPackage(pkg, { reCatch = true })
    saveSessionForActive()
    pushDraft()
end

ns.SetTintsTabActive = function(active)
    tintsActive = active and true or false
    if active then
        ensureActionRowVisible()
    else
        actionRow:Hide()
        if mainFrame.stats then
            mainFrame.stats:Show()
        end
    end
    if tintsActive then
        local name = ns.GetCurrentTransmogSlot and ns.GetCurrentTransmogSlot()
        loadFromSlot(name)
        if type(WXL_RecolorSetPreviewActive) == "function" then
            WXL_RecolorSetPreviewActive(1)
        end
    else
        setSwatchSelected(false)
        activeMultiPair = nil
        picker:Hide()
        if type(WXL_RecolorSetPreviewActive) == "function" then
            WXL_RecolorSetPreviewActive(0)
        end
    end
end

ns.OnTransmogSlotChanged = function(slotName)
    if not tintsActive then
        return
    end
    loadFromSlot(slotName)
end

refreshModeButtons()
refreshSwatchFill()
refreshAllMultiHoles()
