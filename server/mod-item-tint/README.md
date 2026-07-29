# mod-item-tint (AzerothCore)

**AzerothCore-specific.** Persist and broadcast gear tints for WarcraftXL clients.

## Requirements

- AzerothCore 3.3.5a worldserver with **module support**
- This fork's transmog / visible-item hooks (module may need adaptation on stock AC)
- Clients with WarcraftXL + HorizontalTools (or compatible `WXL_TINT` handler)

## SQL

Run on **acore_characters** (once):

```
data/sql/db-characters/custom_item_tint.sql
```

Creates `custom_item_tint` (`item_guid`, `owner`, `mode`, `data`).

## Install

Copy this folder to `azerothcore-wotlk/modules/mod-item-tint/` and rebuild worldserver.

## Protocol

See header comment in `src/ItemTint.cpp`.
