-- Run once on acore_characters (HeidiSQL).
-- Persists per-item-instance gear tints for WXL gear-recolor (transmog-style visibility).

CREATE TABLE IF NOT EXISTS `custom_item_tint` (
  `item_guid` INT UNSIGNED NOT NULL COMMENT 'item_instance.guid',
  `owner`     INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `mode`      TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 solid, 1 selective, 2 gradient',
  `data`      TEXT NOT NULL COMMENT 'tint payload without slot prefix',
  PRIMARY KEY (`item_guid`),
  KEY `idx_owner` (`owner`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
