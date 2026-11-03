"""MySQL smoke test for mod-account-friends. Needs a throwaway MySQL 8 server; it DROPS and
recreates the database named in AF_TEST_DB_NAME (default af_smoke_test). Run from the repo root:

    docker run -d --rm --name af-sqltest -e MYSQL_ROOT_PASSWORD=test -p 127.0.0.1:33062:3306 mysql:8.4
    pip install PyMySQL==1.1.1
    python3 tests/sql_smoke.py

The module's statements are taken from src/AccountFriends.cpp itself (the string literals of its
Execute/Query/StringFormat/Append calls), filled the way fmt would ({} in order, {0} by index;
Python's str.format reads both the same way), so the test runs what the worldserver would send.
"""
import os
import re
import time

import pymysql

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

DB = dict(host=os.environ.get("AF_TEST_DB_HOST", "127.0.0.1"), port=int(os.environ.get("AF_TEST_DB_PORT", "33062")),
          user=os.environ.get("AF_TEST_DB_USER", "root"), password=os.environ.get("AF_TEST_DB_PASSWORD", "test"))
NAME = os.environ.get("AF_TEST_DB_NAME", "af_smoke_test")
CPP = open(REPO + "/src/AccountFriends.cpp").read()


def function_body(signature):
    """The body of the last function whose text starts with `signature` (skips declarations)."""
    start = CPP.rindex(signature)
    brace = CPP.index("{", start)
    depth, i = 0, brace
    while True:
        if CPP[i] == "{":
            depth += 1
        elif CPP[i] == "}":
            depth -= 1
            if depth == 0:
                return CPP[brace:i]
        i += 1


def statements(signature):
    """Every SQL format string in the function (not its chat messages), adjacent literals joined, in
    source order."""
    body = function_body(signature)
    out = []
    for m in re.finditer(r'(?:Execute|Query|StringFormat|Append)\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', body):
        text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))).replace('\\"', '"')
        if re.match(r"(SELECT|INSERT|UPDATE|DELETE|REPLACE|\{0\}\.)", text):  # skip chat text
            out.append(text)
    return out


def esc(text):
    return pymysql.converters.escape_string(text)


conn = pymysql.connect(autocommit=True, charset="utf8mb4", cursorclass=pymysql.cursors.DictCursor, **DB)
cur = conn.cursor()


def q(sql, args=None):
    cur.execute(sql, args)
    rows = cur.fetchall()
    warnings = [w["Message"] for w in (cur.execute("SHOW WARNINGS") and cur.fetchall() or [])]
    # INSERT/UPDATE IGNORE skipping a taken tag or pair is the point; anything else is a bug.
    assert not [w for w in warnings if "Duplicate entry" not in w and "doesn't exist" not in w], (sql, warnings)
    return rows


def run_file(path):
    sql = open(path).read()
    sql = "\n".join(line for line in sql.splitlines() if not line.strip().startswith("--"))
    for stmt in sql.split(";"):
        if stmt.strip():
            cur.execute(stmt)


print("MySQL", q("SELECT VERSION() v")[0]["v"], "sql_mode:", q("SELECT @@sql_mode m")[0]["m"])
q("DROP DATABASE IF EXISTS `%s`" % NAME)
q("CREATE DATABASE `%s`" % NAME)
q("USE `%s`" % NAME)
q("CREATE TABLE updates (name VARCHAR(200) PRIMARY KEY)")

INSTALL = REPO + "/data/sql/db-characters/mod_account_friends_characters.sql"
UNINSTALL = REPO + "/data/sql/uninstall/mod_account_friends_uninstall_characters.sql"
run_file(INSTALL)
run_file(INSTALL)
print("install applied twice: ok")

(COLUMNS,) = statements("std::string ProfileColumns")
PROFILE = COLUMNS.format("p")
(START_REQ,) = statements("void Startup")
LOAD_SELF, LOAD_FRIENDS, LOAD_REQUESTS = statements("void Load")
TAG_CODES, TAG_UPDATE, TAG_INSERT, TAG_READ = statements("void SetTag")
ADD_LOOKUP, ADD_INSERT = statements("void AddFriend")
ACCEPT_LINK, ACCEPT_DELETE = statements("void Accept(Online& entry, Request const request)\n")
(DECLINE,) = statements("void Decline")
(REMOVE,) = statements("void Remove")
(NOTE,) = statements("void SetNote")
(BROADCAST,) = statements("void SetBroadcast")
(LOGOUT,) = statements("void OnLogout")
print("statements from the C++: 18")


def values(row):
    return list(row.values())


now = int(time.time())

# Startup on empty tables.
assert values(q(START_REQ)[0]) == [0]

# Two accounts pick tags. The read-back tells a lost race from a win.
assert q(TAG_CODES.format(esc("Pawster"))) == ()
q(TAG_INSERT.format(1, esc("Pawster"), 1234))
q(TAG_INSERT.format(2, esc("Anna"), 5678))
q(TAG_INSERT.format(3, esc("pawster"), 1234))   # same tag, any case: refused
assert q(TAG_READ.format(PROFILE, 3)) == ()
q(TAG_INSERT.format(3, esc("Zed"), 1111))
me = q(TAG_READ.format(PROFILE, 1))[0]
assert me["tag_name"] == "Pawster" and me["tag_code"] == 1234, me
assert [r["tag_code"] for r in q(TAG_CODES.format(esc("PAWSTER")))] == [1234]
q(TAG_UPDATE.format(esc("Anna"), 5678, 3))       # taken: Zed keeps his tag
assert q(TAG_READ.format(PROFILE, 3))[0]["tag_name"] == "Zed"
print("tags (taken, case-insensitive, rename refused): ok")

# Pawster asks Anna; the lookup carries the counters the module checks.
row = q(ADD_LOOKUP.format(PROFILE, 1, 1, esc("anna"), 5678))[0]
assert values(row)[:4] == [2, row["id"], "Anna", 5678] and values(row)[7:] == [0, 0, 0], row
q(ADD_INSERT.format(1, 1, 2, esc("it's me, \"Paw\" é \U0001f43c"), now))
q(ADD_INSERT.format(1, 1, 2, esc("again"), now))   # IGNORE: same pair
row = q(ADD_LOOKUP.format(PROFILE, 1, 1, esc("Anna"), 5678))[0]
assert values(row)[7:] == [1, 1, 0], row
assert q(ADD_LOOKUP.format(PROFILE, 1, 1, esc("Nobody"), 1000)) == ()

reqs = q(LOAD_REQUESTS.format(PROFILE, 2))
assert len(reqs) == 1 and values(reqs[0])[7:] == [1, "it's me, \"Paw\" é \U0001f43c", now], reqs
print("requests (lookup counters, duplicate ignored, escaping, utf8mb4): ok")

# Anna accepts: both rows of the friendship, the request gone.
cur.execute("START TRANSACTION")
q(ACCEPT_LINK.format(2, 1, 1, 2))
q(ACCEPT_DELETE.format(1, 2))
cur.execute("COMMIT")
assert q(LOAD_REQUESTS.format(PROFILE, 2)) == ()
mine = q(LOAD_FRIENDS.format(PROFILE, 1))
theirs = q(LOAD_FRIENDS.format(PROFILE, 2))
assert [r["tag_name"] for r in mine] == ["Anna"] and [r["tag_name"] for r in theirs] == ["Pawster"]
assert values(q(ADD_LOOKUP.format(PROFILE, 1, 1, esc("Anna"), 5678))[0])[9] == 1
print("accept (two link rows, request deleted): ok")

# Notes are per side; broadcasts and last online on the profile.
q(NOTE.format(esc("my panda"), 1, 2))
assert q(LOAD_FRIENDS.format(PROFILE, 1))[0]["note"] == "my panda"
assert q(LOAD_FRIENDS.format(PROFILE, 2))[0]["note"] == ""
q(BROADCAST.format(esc("raid at 8"), now, 1))
q(LOGOUT.format(now + 5, 1))
row = q(LOAD_SELF.format(PROFILE, 1))[0]
assert (row["broadcast"], row["broadcast_time"], row["last_online"]) == ("raid at 8", now, now + 5), row
print("notes, broadcast, last online: ok")

# Zed's request declined; a request back and forth is cleared by accept.
q(ADD_INSERT.format(2, 3, 1, "", now))
q(DECLINE.format(2, 1))
assert q(LOAD_REQUESTS.format(PROFILE, 1)) == ()
q(ADD_INSERT.format(3, 3, 1, "", now))
q(ADD_INSERT.format(4, 1, 3, "", now))
q(ACCEPT_LINK.format(1, 3, 3, 1))
q(ACCEPT_DELETE.format(3, 1))
assert q("SELECT COUNT(*) n FROM account_friends_request")[0]["n"] == 0
q(REMOVE.format(1, 3))
assert [r["tag_name"] for r in q(LOAD_FRIENDS.format(PROFILE, 1))] == ["Anna"]
assert values(q(START_REQ)[0]) == [0]
print("decline, crossed requests, remove: ok")

run_file(UNINSTALL)
left = q("SHOW TABLES LIKE 'account_friends%'")
assert not left, left
run_file(INSTALL)
print("uninstall, then install again: ok")
q("DROP DATABASE `%s`" % NAME)
print("PASS: schema and every module statement on MySQL")
