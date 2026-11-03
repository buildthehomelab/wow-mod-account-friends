-- mod-account-friends: undo the module's characters database changes. Run it by hand on the
-- characters database after removing the module. Friend tags, account friendships and pending
-- requests are lost. Idempotent.

DROP TABLE IF EXISTS `account_friends_profile`;
DROP TABLE IF EXISTS `account_friends_link`;
DROP TABLE IF EXISTS `account_friends_request`;

DELETE FROM `updates` WHERE `name` LIKE 'mod\_account\_friends\_%';
