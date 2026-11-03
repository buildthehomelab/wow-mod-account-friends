-- mod-account-friends: characters database. Account-wide friends that follow each other across
-- characters, in the style of Real ID: a player picks a friend tag (Name#1234), sends a request,
-- and once the other side accepts, each sees the other online whichever character they play.
-- Undo with data/sql/uninstall/mod_account_friends_uninstall_characters.sql.

-- One row per account that has picked a tag. `id` is what the client sees (its presence id), so
-- account ids never leave the server.
CREATE TABLE IF NOT EXISTS `account_friends_profile` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `account_id` INT UNSIGNED NOT NULL COMMENT 'acore_auth.account.id',
  `tag_name` VARCHAR(12) NOT NULL,
  `tag_code` SMALLINT UNSIGNED NOT NULL COMMENT '1000-9999, the part after #',
  `broadcast` VARCHAR(127) NOT NULL DEFAULT '',
  `broadcast_time` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time',
  `last_online` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time the account last left the game',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_account` (`account_id`),
  UNIQUE KEY `uk_tag` (`tag_name`, `tag_code`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='mod-account-friends: friend tags';

-- A friendship is two rows, one per side, so each side keeps its own note.
CREATE TABLE IF NOT EXISTS `account_friends_link` (
  `account_id` INT UNSIGNED NOT NULL,
  `friend_account_id` INT UNSIGNED NOT NULL,
  `note` VARCHAR(127) NOT NULL DEFAULT '',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`account_id`, `friend_account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-account-friends: accepted friendships';

-- Requests waiting for an answer. Ids are given by the worldserver.
CREATE TABLE IF NOT EXISTS `account_friends_request` (
  `id` INT UNSIGNED NOT NULL,
  `from_account_id` INT UNSIGNED NOT NULL,
  `to_account_id` INT UNSIGNED NOT NULL,
  `message` VARCHAR(127) NOT NULL DEFAULT '',
  `created_at` INT UNSIGNED NOT NULL COMMENT 'unix time',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_pair` (`from_account_id`, `to_account_id`),
  KEY `idx_to` (`to_account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-account-friends: pending requests';
