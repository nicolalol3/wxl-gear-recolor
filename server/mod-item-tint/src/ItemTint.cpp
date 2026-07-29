/*
 * mod-item-tint: persist + sync WXL gear tints (per item instance).
 *
 * Transmog-style sync (server-proactive, no client REQ):
 *   - OnPlayerAfterSetVisibleItemSlot → PUSH that slot to self + nearby
 *   - Login / MapChanged → PushAllEquipped + SyncNearby
 *   - OnPlayerUpdate (~500ms) → pairwise PushAll when a new player enters range
 *
 * Addon protocol (LANG_ADDON via SendDirectMessage):
 *   WXL_TINT SET\tslot\tmode\tdata   (UI write)
 *   WXL_TINT CLEAR\tslot
 *   WXL_TINT ACK\tok|err\t...
 *   WXL_TINT REQ   (client /reload — re-PUSH all equipped tints to self + resync nearby)
 *   WXL_TINT PUSH\t0xOwnerGuid\tslot\tmode\tdata\tentry
 *   WXL_TINT PUSH\t0xOwnerGuid\tslot\tclear
 */

#include "Chat.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "Tokenize.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* ADDON_PREFIX = "WXL_TINT";
    constexpr uint32 VISIBILITY_SYNC_MS = 500;
    // Transmog UI can leave bag slots empty for >400ms; 400 caused real CLEAR
    // broadcasts mid-preview and wiped nearby clients. Prefer longer settle.
    constexpr uint32 UNEQUIP_CLEAR_DEBOUNCE_MS = 1200;

    struct TintPayload
    {
        uint8 mode = 0;
        std::string data;
    };

    // item_guid (counter) -> tint
    std::unordered_map<uint32, TintPayload> g_tintsByItem;
    // owner counter -> item guids owned
    std::unordered_map<uint32, std::vector<uint32>> g_itemsByOwner;
    // observerLow -> set of ownerLow whose equipped tints were already pushed to observer
    std::unordered_map<uint32, std::unordered_set<uint32>> g_syncedOwnersForObserver;
    // per-player OnUpdate throttle
    std::unordered_map<uint32, uint32> g_updateAccumMs;
    // Debounced PUSH clear: key = (ownerLow << 8) | slot → due getMSTime()
    std::unordered_map<uint32, uint32> g_pendingUnequipClear;
    // Slots with a live broadcast tint: key = (ownerLow << 8) | slot. Transmog UI
    // pulses SetVisibleItemSlot(null) on all 19 slots — without this gate every
    // pulse broadcast a CLEAR for never-tinted slots (logs: 23x
    // visible_unequip_cleared → client clear_flush mask=251 body wipe).
    std::unordered_set<uint32> g_slotTintLive;
    // Last PUSH fingerprint so identical visible_item_slot re-pushes are skipped.
    std::unordered_map<uint32, std::string> g_slotTintLiveFp;

    void SyncNearbyTints(Player* player);

    std::string GuidHex(ObjectGuid const& guid)
    {
        std::ostringstream ss;
        ss << "0x" << std::hex << guid.GetRawValue();
        return ss.str();
    }

    void SendAddon(Player* player, std::string const& body)
    {
        if (!player || !player->GetSession())
            return;
        std::string const msg = std::string(ADDON_PREFIX) + "\t" + body;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, msg);
        player->SendDirectMessage(&data);
    }

    void RememberTint(uint32 itemGuid, uint32 owner, TintPayload const& tint)
    {
        g_tintsByItem[itemGuid] = tint;
        auto& list = g_itemsByOwner[owner];
        for (uint32 g : list)
            if (g == itemGuid)
                return;
        list.push_back(itemGuid);
    }

    void ForgetTint(uint32 itemGuid, uint32 owner)
    {
        g_tintsByItem.erase(itemGuid);
        auto it = g_itemsByOwner.find(owner);
        if (it == g_itemsByOwner.end())
            return;
        auto& list = it->second;
        list.erase(std::remove(list.begin(), list.end(), itemGuid), list.end());
        if (list.empty())
            g_itemsByOwner.erase(it);
    }

    void LoadOwnerTints(uint32 owner)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT item_guid, mode, data FROM custom_item_tint WHERE owner = {}", owner);
        if (!result)
            return;
        do
        {
            Field* fields = result->Fetch();
            uint32 itemGuid = fields[0].Get<uint32>();
            TintPayload tint;
            tint.mode = fields[1].Get<uint8>();
            tint.data = fields[2].Get<std::string>();
            RememberTint(itemGuid, owner, tint);
        } while (result->NextRow());
    }

    void PersistTint(uint32 itemGuid, uint32 owner, TintPayload const& tint)
    {
        std::string escaped = tint.data;
        CharacterDatabase.EscapeString(escaped);
        CharacterDatabase.Execute(
            "REPLACE INTO custom_item_tint (item_guid, owner, mode, data) VALUES ({}, {}, {}, '{}')",
            itemGuid, owner, uint32(tint.mode), escaped);
        RememberTint(itemGuid, owner, tint);
    }

    void DeleteTint(uint32 itemGuid, uint32 owner)
    {
        CharacterDatabase.Execute("DELETE FROM custom_item_tint WHERE item_guid = {}", itemGuid);
        ForgetTint(itemGuid, owner);
    }

    void MarkSynced(uint32 observerLow, uint32 ownerLow)
    {
        g_syncedOwnersForObserver[observerLow].insert(ownerLow);
    }

    bool IsSynced(uint32 observerLow, uint32 ownerLow)
    {
        auto it = g_syncedOwnersForObserver.find(observerLow);
        return it != g_syncedOwnersForObserver.end() && it->second.count(ownerLow) != 0;
    }

    void ClearSyncForPlayer(uint32 playerLow)
    {
        g_syncedOwnersForObserver.erase(playerLow);
        g_updateAccumMs.erase(playerLow);
        for (auto& kv : g_syncedOwnersForObserver)
            kv.second.erase(playerLow);
    }

    // #region agent log
    constexpr char const* DEBUG_LOG_PATH = "C:/Azerothcore/debug-a479da.log";

    void JsonEscapeAppend(std::ostringstream& ss, std::string const& in)
    {
        for (unsigned char c : in)
        {
            if (c == '"' || c == '\\')
                ss << '\\' << char(c);
            else if (c >= 32 && c < 127)
                ss << char(c);
            else
                ss << ' ';
        }
    }

    void AppendEquippedSlotsJson(Player* p, std::ostringstream& ss)
    {
        ss << '[';
        bool first = true;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            uint32 const itemGuid = item->GetGUID().GetCounter();
            auto it = g_tintsByItem.find(itemGuid);
            bool const hasTint = it != g_tintsByItem.end();
            if (!first)
                ss << ',';
            first = false;
            ss << "{\"slot\":" << uint32(slot)
               << ",\"itemGuid\":" << itemGuid
               << ",\"entry\":" << item->GetEntry()
               << ",\"hasTint\":" << (hasTint ? 1 : 0);
            if (hasTint)
            {
                ss << ",\"mode\":" << uint32(it->second.mode) << ",\"data\":\"";
                JsonEscapeAppend(ss, it->second.data);
                // In-memory map is loaded from DB on login and updated by PersistTint.
                ss << "\",\"saved\":1";
            }
            else
                ss << ",\"saved\":0";
            ss << '}';
        }
        ss << ']';
    }

    void DumpTintState(Player* observer, char const* reason)
    {
        if (!observer || !reason)
            return;
        std::ostringstream ss;
        long long const ts = static_cast<long long>(std::time(nullptr)) * 1000;
        ss << "{\"sessionId\":\"a479da\",\"side\":\"server\",\"hypothesisId\":\"DUMP\""
           << ",\"location\":\"ItemTint.cpp:DumpTintState\",\"message\":\"tint_dump\""
           << ",\"timestamp\":" << ts
           << ",\"data\":{\"reason\":\"";
        JsonEscapeAppend(ss, reason);
        ss << "\",\"observerLow\":" << observer->GetGUID().GetCounter()
           << ",\"observerName\":\"";
        JsonEscapeAppend(ss, observer->GetName());
        ss << "\",\"self\":";
        AppendEquippedSlotsJson(observer, ss);
        ss << ",\"nearby\":[";
        bool firstN = true;
        if (observer->IsInWorld())
        {
            if (Map* map = observer->FindMap())
            {
                uint32 const obsLow = observer->GetGUID().GetCounter();
                float const range = observer->GetVisibilityRange();
                Map::PlayerList const& players = map->GetPlayers();
                for (Map::PlayerList::const_iterator itr = players.begin();
                     itr != players.end(); ++itr)
                {
                    Player* other = itr->GetSource();
                    if (!other || other == observer || !other->IsInWorld())
                        continue;
                    if (!observer->IsWithinDistInMap(other, range))
                        continue;
                    if (!firstN)
                        ss << ',';
                    firstN = false;
                    uint32 const otherLow = other->GetGUID().GetCounter();
                    ss << "{\"ownerLow\":" << otherLow
                       << ",\"name\":\"";
                    JsonEscapeAppend(ss, other->GetName());
                    ss << "\",\"syncedToObs\":" << (IsSynced(obsLow, otherLow) ? 1 : 0)
                       << ",\"obsSyncedToThem\":" << (IsSynced(otherLow, obsLow) ? 1 : 0)
                       << ",\"slots\":";
                    AppendEquippedSlotsJson(other, ss);
                    ss << '}';
                }
            }
        }
        ss << "]}}\n";

        if (FILE* f = std::fopen(DEBUG_LOG_PATH, "a"))
        {
            std::fputs(ss.str().c_str(), f);
            std::fclose(f);
        }
    }
    // #endregion

    void BroadcastPush(Player* owner, uint8 slot, std::string const& pushBody)
    {
        // Never call GetMap() here — it asserts when m_currMap is null (login / equip-before-world).
        if (!owner || !owner->IsInWorld())
            return;
        Map* map = owner->FindMap();
        if (!map)
            return;

        std::string const full = std::string("PUSH\t") + GuidHex(owner->GetGUID()) + "\t"
            + std::to_string(uint32(slot)) + "\t" + pushBody;

        float const range = owner->GetVisibilityRange();
        uint32 const ownerLow = owner->GetGUID().GetCounter();
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* p = itr->GetSource();
            if (!p || !p->IsInWorld())
                continue;
            if (p != owner && !p->IsWithinDistInMap(owner, range))
                continue;
            SendAddon(p, full);
            MarkSynced(p->GetGUID().GetCounter(), ownerLow);
            // #region agent log
            DumpTintState(p, p == owner ? "push_to_self" : "push_to_observer");
            // #endregion
        }
    }

    void PushSlotTint(Player* owner, uint8 slot, Item* item)
    {
        if (!owner)
            return;
        // During login SetVisibleItemSlot fires before the player is on a map.
        // Still push to self via SendAddon; skip map broadcast until in-world.
        if (!owner->IsInWorld() || !owner->FindMap())
        {
            if (!item)
                return;
            uint32 const itemGuid = item->GetGUID().GetCounter();
            auto it = g_tintsByItem.find(itemGuid);
            if (it == g_tintsByItem.end())
                return;
            std::ostringstream ss;
            ss << "PUSH\t" << GuidHex(owner->GetGUID()) << '\t' << uint32(slot) << '\t'
               << uint32(it->second.mode) << '\t' << it->second.data
               << '\t' << item->GetEntry();
            SendAddon(owner, ss.str());
            // #region agent log
            DumpTintState(owner, "push_preworld_self");
            // #endregion
            return;
        }
        uint32 const liveKey = (owner->GetGUID().GetCounter() << 8) | uint32(slot);
        if (!item)
        {
            // CLEAR only if this slot ever broadcast a tint — otherwise it's UI
            // pulse noise for an always-empty slot.
            if (g_slotTintLive.erase(liveKey))
            {
                g_slotTintLiveFp.erase(liveKey);
                BroadcastPush(owner, slot, "clear");
                // #region agent log
                DumpTintState(owner, "clear_broadcast_empty");
                // #endregion
            }
            else
            {
                // #region agent log
                DumpTintState(owner, "clear_suppressed_empty");
                // #endregion
            }
            return;
        }
        uint32 const itemGuid = item->GetGUID().GetCounter();
        auto it = g_tintsByItem.find(itemGuid);
        if (it == g_tintsByItem.end())
        {
            // Equipped item with no saved tint — tell observers to drop tint for
            // this slot. (Bag item is authoritative; caller must not pass a
            // transient visible-only item here.)
            if (g_slotTintLive.erase(liveKey))
            {
                g_slotTintLiveFp.erase(liveKey);
                BroadcastPush(owner, slot, "clear");
                // #region agent log
                DumpTintState(owner, "clear_broadcast_untinted");
                // #endregion
            }
            return;
        }
        std::string const fp = std::to_string(itemGuid) + "\t"
            + std::to_string(uint32(it->second.mode)) + "\t" + it->second.data
            + "\t" + std::to_string(item->GetEntry());
        if (g_slotTintLive.count(liveKey))
        {
            auto fpIt = g_slotTintLiveFp.find(liveKey);
            if (fpIt != g_slotTintLiveFp.end() && fpIt->second == fp)
            {
                // #region agent log
                DumpTintState(owner, "push_skip_identical");
                // #endregion
                return;
            }
        }
        g_slotTintLive.insert(liveKey);
        g_slotTintLiveFp[liveKey] = fp;
        BroadcastPush(owner, slot,
            std::to_string(uint32(it->second.mode)) + "\t" + it->second.data
            + "\t" + std::to_string(item->GetEntry()));
    }

    void PushAllEquipped(Player* owner, Player* to)
    {
        if (!owner || !to)
            return;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = owner->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            auto it = g_tintsByItem.find(item->GetGUID().GetCounter());
            if (it == g_tintsByItem.end())
                continue;
            g_slotTintLive.insert((owner->GetGUID().GetCounter() << 8) | uint32(slot));
            std::ostringstream ss;
            ss << "PUSH\t" << GuidHex(owner->GetGUID()) << '\t' << uint32(slot) << '\t'
               << uint32(it->second.mode) << '\t' << it->second.data
               << '\t' << item->GetEntry();
            SendAddon(to, ss.str());
        }
        MarkSynced(to->GetGUID().GetCounter(), owner->GetGUID().GetCounter());
        // #region agent log
        DumpTintState(to, to == owner ? "push_all_self" : "push_all_to_observer");
        // #endregion
    }

    bool SanitizeTintData(std::string& data)
    {
        if (data.empty() || data.size() > 360)
            return false;
        for (char c : data)
        {
            if (c == '\'' || c == '"' || c == ';' || c == '\\' || c == '\n' || c == '\r')
                return false;
        }
        return true;
    }

    void HandleSet(Player* player, uint8 slot, uint8 mode, std::string data)
    {
        if (!player || slot >= EQUIPMENT_SLOT_END || mode > 2 || !SanitizeTintData(data))
        {
            SendAddon(player, "ACK\terr\tbad_args");
            return;
        }
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
        {
            SendAddon(player, "ACK\terr\tno_item");
            return;
        }
        TintPayload tint;
        tint.mode = mode;
        tint.data = std::move(data);
        PersistTint(item->GetGUID().GetCounter(), player->GetGUID().GetCounter(), tint);
        SendAddon(player, "ACK\tok\tset");
        PushSlotTint(player, slot, item);
        // #region agent log
        DumpTintState(player, "after_set");
        // #endregion
    }

    void HandleClear(Player* player, uint8 slot)
    {
        if (!player || slot >= EQUIPMENT_SLOT_END)
        {
            SendAddon(player, "ACK\terr\tbad_args");
            return;
        }
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (item)
            DeleteTint(item->GetGUID().GetCounter(), player->GetGUID().GetCounter());
        SendAddon(player, "ACK\tok\tclear");
        uint32 const liveKey = (player->GetGUID().GetCounter() << 8) | uint32(slot);
        g_slotTintLive.erase(liveKey);
        g_slotTintLiveFp.erase(liveKey);
        g_pendingUnequipClear.erase(liveKey);
        BroadcastPush(player, slot, "clear");
        // #region agent log
        DumpTintState(player, "after_clear");
        // #endregion
    }

    void HandleAddonBody(Player* player, std::string const& body)
    {
        std::vector<std::string_view> tokens = Acore::Tokenize(body, '\t', false);
        if (tokens.empty())
            return;

        if (tokens[0] == "SET" && tokens.size() >= 4)
        {
            uint32 slot = uint32(strtoul(std::string(tokens[1]).c_str(), nullptr, 10));
            uint32 mode = uint32(strtoul(std::string(tokens[2]).c_str(), nullptr, 10));
            HandleSet(player, uint8(slot), uint8(mode), std::string(tokens[3]));
            return;
        }
        if (tokens[0] == "CLEAR" && tokens.size() >= 2)
        {
            uint32 slot = uint32(strtoul(std::string(tokens[1]).c_str(), nullptr, 10));
            HandleClear(player, uint8(slot));
            return;
        }
        if (tokens[0] == "REQ")
        {
            LoadOwnerTints(player->GetGUID().GetCounter());
            PushAllEquipped(player, player);
            SyncNearbyTints(player);
            SendAddon(player, "ACK\tok\treq");
            // #region agent log
            DumpTintState(player, "reload_req");
            // #endregion
            return;
        }
        // SET / CLEAR / REQ only — server pushes proactively on equip/login.
    }

    void SyncNearbyTints(Player* player)
    {
        if (!player || !player->IsInWorld())
            return;
        Map* map = player->FindMap();
        if (!map)
            return;
        float const range = player->GetVisibilityRange();
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* other = itr->GetSource();
            if (!other || other == player || !other->IsInWorld())
                continue;
            if (!player->IsWithinDistInMap(other, range))
                continue;
            PushAllEquipped(other, player);
            PushAllEquipped(player, other);
        }
    }

    void SyncNewVisibilityPairs(Player* player)
    {
        if (!player || !player->IsInWorld())
            return;
        Map* map = player->FindMap();
        if (!map)
            return;

        uint32 const selfLow = player->GetGUID().GetCounter();
        float const range = player->GetVisibilityRange();
        bool didPush = false;
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* other = itr->GetSource();
            if (!other || other == player || !other->IsInWorld())
                continue;
            if (!player->IsWithinDistInMap(other, range))
                continue;

            uint32 const otherLow = other->GetGUID().GetCounter();
            if (!IsSynced(selfLow, otherLow))
            {
                PushAllEquipped(other, player);
                didPush = true;
            }
            if (!IsSynced(otherLow, selfLow))
            {
                PushAllEquipped(player, other);
                didPush = true;
            }
        }
        // #region agent log
        if (didPush)
            DumpTintState(player, "visibility_sync");
        // #endregion
    }
}

class ItemTintPlayerScript : public PlayerScript
{
public:
    ItemTintPlayerScript()
        : PlayerScript("ItemTintPlayerScript",
            { PLAYERHOOK_ON_LOGIN,
              PLAYERHOOK_ON_LOGOUT,
              PLAYERHOOK_ON_MAP_CHANGED,
              PLAYERHOOK_ON_UPDATE,
              PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE,
              PLAYERHOOK_ON_AFTER_SET_VISIBLE_ITEM_SLOT })
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;
        LoadOwnerTints(player->GetGUID().GetCounter());
        PushAllEquipped(player, player);
        SyncNearbyTints(player);
        // #region agent log
        DumpTintState(player, "login");
        // #endregion
    }

    void OnPlayerMapChanged(Player* player) override
    {
        if (!player)
            return;
        ClearSyncForPlayer(player->GetGUID().GetCounter());
        PushAllEquipped(player, player);
        SyncNearbyTints(player);
        // #region agent log
        DumpTintState(player, "map_changed");
        // #endregion
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;
        // #region agent log
        DumpTintState(player, "logout");
        // #endregion
        ClearSyncForPlayer(player->GetGUID().GetCounter());
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!player || !player->IsInWorld())
            return;
        uint32 const low = player->GetGUID().GetCounter();
        uint32 const now = getMSTime();
        // Fire debounced unequip CLEARs only if the bag slot is still empty.
        for (auto it = g_pendingUnequipClear.begin(); it != g_pendingUnequipClear.end(); )
        {
            if ((it->first >> 8) != low)
            {
                ++it;
                continue;
            }
            if (now < it->second)
            {
                ++it;
                continue;
            }
            uint8 const slot = uint8(it->first & 0xFF);
            it = g_pendingUnequipClear.erase(it);
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                continue;
            // PushSlotTint logs clear_broadcast_* / clear_suppressed_* itself.
            PushSlotTint(player, slot, nullptr);
        }
        uint32& accum = g_updateAccumMs[low];
        accum += diff;
        if (accum < VISIBILITY_SYNC_MS)
            return;
        accum = 0;
        SyncNewVisibilityPairs(player);
    }

    void OnPlayerAfterSetVisibleItemSlot(Player* player, uint8 slot, Item* item) override
    {
        if (!player)
            return;
        // Tint follows the real bag item. UI/transmog often pulses visible=null
        // (logs: dozens of visible_item_slot_unequip while other slots still tinted).
        // Debounce PUSH clear so observers are not wiped on flicker.
        Item* bagItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        uint32 const key = (player->GetGUID().GetCounter() << 8) | uint32(slot);
        if (bagItem)
        {
            g_pendingUnequipClear.erase(key);
            PushSlotTint(player, slot, bagItem);
            // #region agent log
            DumpTintState(player, "visible_item_slot");
            // #endregion
        }
        else
        {
            g_pendingUnequipClear[key] = getMSTime() + UNEQUIP_CLEAR_DEBOUNCE_MS;
            // #region agent log
            DumpTintState(player, "visible_unequip_debounce");
            // #endregion
        }
        (void)item;
    }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& /*type*/,
        uint32& lang, std::string& msg) override
    {
        if (!player || lang != LANG_ADDON)
            return;
        size_t tab = msg.find('\t');
        if (tab == std::string::npos)
            return;
        if (msg.substr(0, tab) != ADDON_PREFIX)
            return;
        HandleAddonBody(player, msg.substr(tab + 1));
        // Swallow so it never appears as a real whisper.
        msg.clear();
    }
};

class ItemTint_Command : public CommandScript
{
public:
    ItemTint_Command() : CommandScript("ItemTint_Command") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable tintTable =
        {
            { "dump", HandleTintDumpCommand, SEC_PLAYER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "tint", tintTable },
        };
        return commandTable;
    }

    static bool HandleTintDumpCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;
        // #region agent log
        DumpTintState(player, "cmd_tint_dump");
        // #endregion
        handler->SendSysMessage("ItemTint: dump written to debug-a479da.log");
        return true;
    }
};

void AddSC_ItemTint()
{
    new ItemTintPlayerScript();
    new ItemTint_Command();
}
