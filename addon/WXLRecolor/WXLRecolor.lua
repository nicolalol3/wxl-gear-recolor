-- WXLRecolor: per-slot RGB colorize via ColorPicker.
-- Slash: /recolor

local ADDON_NAME = "WXLRecolor"
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

local function normalizeEntry(c)
    if type(c) ~= "table" then
        return nil
    end
    -- New RGB format
    if c.r ~= nil or c.g ~= nil or c.b ~= nil then
        return {
            r = clamp01(c.r or 1),
            g = clamp01(c.g or 1),
            b = clamp01(c.b or 1),
        }
    end
    -- Drop legacy HSL saved vars
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
    if type(WXL_RecolorSetSlot) == "function" then
        WXL_RecolorSetSlot(slot, c.r, c.g, c.b)
    end
end

local function pushAll()
    ensureDB()
    if type(WXL_RecolorClearAll) == "function" then
        WXL_RecolorClearAll()
    end
    for key in pairs(WXLRecolorDB.slots) do
        local slot = tonumber(key)
        if slot then
            pushSlot(slot)
        end
    end
end

local function setSlotColor(slot, r, g, b)
    ensureDB()
    WXLRecolorDB.slots[tostring(slot)] = {
        r = clamp01(r),
        g = clamp01(g),
        b = clamp01(b),
    }
    pushSlot(slot)
    queuePreviewRefresh()
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
frame:SetSize(640, 520)
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
        model:SetUnit("player")
        model:Dress()
        applyModelCamera()
    end
    self:Hide()
end)

queuePreviewRefresh = function()
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

-- Color panel
local colorPanel = CreateFrame("Frame", nil, frame)
colorPanel:SetSize(340, 110)
colorPanel:SetPoint("TOPLEFT", slotPanel, "BOTTOMLEFT", 0, -10)
makeBackdrop(colorPanel, 0.7)

local colorTitle = colorPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
colorTitle:SetPoint("TOPLEFT", 12, -10)
colorTitle:SetText("Color")

local slotLabel = colorPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
slotLabel:SetPoint("TOPLEFT", 12, -30)
slotLabel:SetText("Select a slot")

local bigSwatch = colorPanel:CreateTexture(nil, "ARTWORK")
bigSwatch:SetSize(36, 36)
bigSwatch:SetPoint("TOPLEFT", 12, -52)
bigSwatch:SetTexture("Interface\\Buttons\\WHITE8X8")
bigSwatch:SetVertexColor(0.4, 0.4, 0.4)

local pickBtn = CreateFrame("Button", nil, colorPanel, "UIPanelButtonTemplate")
pickBtn:SetSize(110, 24)
pickBtn:SetPoint("LEFT", bigSwatch, "RIGHT", 12, 0)
pickBtn:SetText("Pick color")
pickBtn:Disable()

local copyToAllBtn = CreateFrame("Button", nil, colorPanel, "UIPanelButtonTemplate")
copyToAllBtn:SetSize(70, 24)
copyToAllBtn:SetPoint("LEFT", pickBtn, "RIGHT", 8, 0)
copyToAllBtn:SetText("To all")
copyToAllBtn:Disable()

local function openColorPicker(slot)
    local c = getSlot(slot) or { r = 0.8, g = 0.2, b = 0.2 }
    local prev = { r = c.r, g = c.g, b = c.b }

    ColorPickerFrame.func = function()
        local r, g, b = ColorPickerFrame:GetColorRGB()
        setSlotColor(slot, r, g, b)
        refreshSlots()
        refreshColorPanel()
    end
    ColorPickerFrame.cancelFunc = function()
        setSlotColor(slot, prev.r, prev.g, prev.b)
        refreshSlots()
        refreshColorPanel()
    end
    ColorPickerFrame:SetColorRGB(c.r, c.g, c.b)
    ColorPickerFrame.opacityFunc = nil
    ColorPickerFrame.hasOpacity = false
    ColorPickerFrame:Show()
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
    if not c then
        return
    end
    for _, entry in ipairs(EQUIP_SLOTS) do
        if entry.slot ~= selectedSlot then
            setSlotColor(entry.slot, c.r, c.g, c.b)
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
    if c then
        bigSwatch:SetVertexColor(c.r, c.g, c.b)
        copyToAllBtn:Enable()
    else
        bigSwatch:SetVertexColor(0.45, 0.45, 0.45)
        copyToAllBtn:Disable()
    end
    pickBtn:Enable()
end

refreshSlots = function()
    for _, btn in ipairs(slotButtons) do
        local c = getSlot(btn.equipSlot)
        if c then
            btn.swatch:SetVertexColor(c.r, c.g, c.b)
            btn.label:SetTextColor(1, 1, 1)
            btn.mark:SetText("•")
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
        apiStatus:SetText("WarcraftXL color API ready")
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
        refreshSlots()
        refreshColorPanel()
        openColorPicker(info.slot)
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
    refreshSlots()
    refreshColorPanel()
end)

makeBtn("Reset Slot", -208, function()
    if selectedSlot == nil then
        return
    end
    clearSlot(selectedSlot)
    refreshSlots()
    refreshColorPanel()
end)

frame:SetScript("OnShow", function()
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
boot:SetScript("OnEvent", function(_, event, arg1)
    if event == "ADDON_LOADED" and arg1 == ADDON_NAME then
        ensureDB()
        pushAll()
    elseif event == "VARIABLES_LOADED" or event == "PLAYER_LOGIN" then
        ensureDB()
        pushAll()
        local t, elapsed = CreateFrame("Frame"), 0
        t:SetScript("OnUpdate", function(self, dt)
            elapsed = elapsed + dt
            if type(WXL_RecolorSetSlot) == "function" or elapsed > 5 then
                pushAll()
                refreshSlots()
                self:SetScript("OnUpdate", nil)
            end
        end)
    end
end)
