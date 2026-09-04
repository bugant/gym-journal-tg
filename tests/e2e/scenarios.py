#!/usr/bin/env python3
"""End-to-end scenarios for the gym journal Telegram bot.

Each Scenario is a sequence of Steps. A Step:

  * sends one user message to the bot (through the mock Telegram server),
  * waits for the bot's single reply and checks it,
  * runs optional SQL assertions against the test database.

Reply checks are either exact (`expect`), a regex searched on the reply
(`expect_re`), or a list of required substrings (`contains`).

Every scenario starts from a clean database (tables truncated, sequences
reset), so session / message ids are deterministic (1, 2, ...) unless a
scenario notes otherwise.

A note on determinism: the bot answers every private message with exactly one
sendMessage, and the driver drives one message at a time, so reply order is
always deterministic.
"""

import time
from dataclasses import dataclass, field
from typing import List, Tuple

# Default simulated user / chat (user A). Some scenarios use a second user.
DEFAULT_CHAT = 1111
DEFAULT_FROM = 1111
USER_B_CHAT = 2222
USER_B_FROM = 2222

TODAY = time.strftime("%d/%m/%Y")
NOW_ISODATE = time.strftime("%Y-%m-%d")


@dataclass
class Step:
    send: str                       # message text delivered to the bot
    chat: int = DEFAULT_CHAT
    sender: int = DEFAULT_FROM
    username: str = "tester"
    expect: str = None              # exact reply text
    expect_re: str = None           # regex searched against reply text
    contains: List[str] = None      # all substrings must be in reply text
    db: List[Tuple[str, List[Tuple[str, ...]]]] = field(default_factory=list)


@dataclass
class Scenario:
    name: str
    steps: List[Step]


# ===========================================================================
# Boot & help
# ===========================================================================

boot_unknown_command = Scenario("boot: unknown command greets with /help hint", [
    Step("/hello",
         expect="Unknown command. Type /help for the list.",
         db=[("SELECT username FROM users WHERE user_id = %d" % DEFAULT_FROM,
              [("tester",)])]),
])

empty_text = Scenario("boot: empty text gets the intro", [
    Step("", expect="Hi! I track your gym sessions. Try /help."),
])

help_command = Scenario("boot: /help lists all commands", [
    Step("/help", contains=[
        "Gym Journal commands",
        "/new", "/add", "/done", "/list", "/show", "/stats",
        "/add bench 60x10x3",
    ]),
])

# ===========================================================================
# Sessions: /new
# ===========================================================================

new_session = Scenario("session: /new starts an open session today", [
    Step("/new",
         expect="Session #1 started (%s). "
                "Add sets with /add, finish with /done." % TODAY,
         db=[
             ("SELECT count(*) FROM sessions "
              "WHERE user_id = %d AND closed_at = 0" % DEFAULT_FROM, [("1",)]),
             ("SELECT (now()::date = to_timestamp(started_at)::date) "
              "FROM sessions", [("t",)]),
         ]),
])

new_rollover = Scenario("session: /new rolls over an open session", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/new", expect="Closed session #1. Session #2 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY,
         db=[
             ("SELECT count(*) FROM sessions", [("2",)]),
             ("SELECT (closed_at > 0) FROM sessions WHERE id = 1", [("t",)]),
             ("SELECT (closed_at = 0) FROM sessions WHERE id = 2", [("t",)]),
         ]),
])

new_with_date = Scenario("session: /new with an explicit date", [
    Step("/new 2026-07-10",
         expect="Session #1 started (10/07/2026). "
                "Add sets with /add, finish with /done.",
         db=[
             ("SELECT date(to_timestamp(started_at))::text FROM sessions",
              [("2026-07-10",)]),
         ]),
])

new_invalid_date = Scenario("session: /new rejects a malformed date", [
    Step("/new 2026-13-99",
         expect="Invalid date. Use YYYY-MM-DD, e.g. /new 2026-07-10"),
    Step("/new xyz",
         expect="Invalid date. Use YYYY-MM-DD, e.g. /new 2026-07-10"),
])

new_usage = Scenario("session: /new with too many args", [
    Step("/new 2026-01-01 2026-01-02",
         expect="Usage: /new [YYYY-MM-DD]"),
])

# ===========================================================================
# Sets: /add
# ===========================================================================

add_no_session = Scenario("add: /add without an open session", [
    Step("/add bench 60x10",
         expect="No active session. Start one with /new."),
])

add_usage = Scenario("add: /add without enough args", [
    Step("/add", contains=["Usage: /add <exercise> <WxR[xN]>"]),
    Step("/add bench", contains=["Usage: /add <exercise> <WxR[xN]>"]),
])

add_missing_exercise = Scenario("add: missing exercise name", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add 60x10 55x8",
         expect="Missing exercise name. Example: /add bench 60x10"),
])

add_no_sets = Scenario("add: no sub-set tokens at all", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench front",
         expect="No sets given. Example: /add bench 60x10x3"),
])

add_single_set = Scenario("add: one set", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 60x10",
         expect="bench  60x10  [vol 600]",
         db=[
             ("SELECT count(*) FROM sets", [("1",)]),
             ("SELECT weight, reps, exercise FROM sets ORDER BY id",
              [("60", "10", "bench")]),
             ("SELECT count(DISTINCT group_id) FROM sets", [("1",)]),
             ("SELECT sum(weight * reps) FROM sets", [("600",)]),
         ]),
])

add_repeat_group = Scenario("add: WxRxN expands and collapses to a uniform line", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 60x10x3",
         expect="bench  3x10 @ 60kg  [vol 1800]",
         db=[
             ("SELECT count(*) FROM sets", [("3",)]),
             ("SELECT count(DISTINCT group_id) FROM sets", [("1",)]),
             ("SELECT sum(weight * reps) FROM sets", [("1800",)]),
         ]),
])

add_drop_set = Scenario("add: distinct sets stay literal", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 60x10 55x8 50x6",
         expect="bench  60x10 + 55x8 + 50x6  [vol 1340]",
         db=[
             ("SELECT weight FROM sets ORDER BY id", [("60",), ("55",), ("50",)]),
             ("SELECT reps FROM sets ORDER BY id", [("10",), ("8",), ("6",)]),
         ]),
])

add_mixed_repeat = Scenario("add: repeat + distinct in one command", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 60x10x3 55x8 50x6",
         expect="bench  60x10 + 60x10 + 60x10 + 55x8 + 50x6  [vol 2540]",
         db=[
             ("SELECT count(*) FROM sets", [("5",)]),
             ("SELECT count(DISTINCT group_id) FROM sets", [("1",)]),
         ]),
])

add_bodyweight = Scenario("add: bodyweight set (xR)", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add pull ups x10",
         expect="pull ups  x10  [bw]",
         db=[
             ("SELECT weight, reps FROM sets ORDER BY id", [("0", "10")]),
             ("SELECT sum(weight * reps) FROM sets", [("0",)]),
         ]),
])

add_multiword_name = Scenario("add: multi-word exercise name", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add Bench Press 60x5",
         expect="Bench Press  60x5  [vol 300]",
         db=[("SELECT exercise FROM sets", [("Bench Press",)])]),
])

add_bad_token_rollback = Scenario("add: bad token aborts the whole /add (rollback)", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 60x10 zzz",
         expect="Token 'zzz' is not a valid set. "
                "Use WxR or WxRxN (e.g. 60x10x3).",
         db=[
             ("SELECT count(*) FROM sets", [("0",)]),
             ("SELECT count(*) FROM sessions WHERE closed_at = 0", [("1",)]),
         ]),
])

# ===========================================================================
# Closing & summaries: /done, /list, /show, /stats
# ===========================================================================

done_no_session = Scenario("done: /done without an open session", [
    Step("/done", expect="No active session to close."),
])

done_summary = Scenario("done: /done closes the session and prints the summary", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 100x5 60x10x2",
         expect="bench  100x5 + 60x10 + 60x10  [vol 1700]"),
    Step("/done",
         contains=["Session #1  (%s, closed)" % TODAY,
                   "bench", "1 exercise, 3 sets, total vol 1700 kg*rep"],
         db=[("SELECT (closed_at > 0) FROM sessions WHERE id = 1", [("t",)])]),
])

list_sessions = Scenario("list: /list shows the open session", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 60x5", expect="bench  60x5  [vol 300]"),
    Step("/list", contains=[
        "Recent sessions:",
        "#1  ", "1 sets", "OPEN", "vol 300 kg*rep",
    ]),
])

list_after_done = Scenario("list: /list shows the closed session", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 60x5", expect="bench  60x5  [vol 300]"),
    Step("/done", contains=["Session #1  (%s, closed)" % TODAY]),
    Step("/list", contains=["#1  ", "1 sets", "closed", "vol 300 kg*rep"]),
])

show_detail = Scenario("show: /show prints full session detail", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 60x10x3", expect="bench  3x10 @ 60kg  [vol 1800]"),
    Step("/add squat 100x5 100x5", expect="squat  2x5 @ 100kg  [vol 1000]"),
    Step("/show", contains=[
        "Session #1  (%s, open)" % TODAY,
        "bench", "3x10 @ 60kg",
        "squat", "2x5 @ 100kg",
        "2 exercises, 5 sets, total vol 2800 kg*rep",
    ]),
])

show_by_id = Scenario("show: /show <id> targets a specific session", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 60x10", expect="bench  60x10  [vol 600]"),
    Step("/show 1", contains=[
        "Session #1  (%s, open)" % TODAY,
        "bench", "1 exercise, 1 sets, total vol 600 kg*rep",
    ]),
])

show_no_session = Scenario("show: /show with no sessions at all", [
    Step("/show", expect="You have no sessions yet."),
])

show_nonexistent = Scenario("show: /show of a session id that does not exist", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/show 99", expect="No such session."),
])

show_foreign = Scenario("show: /show refuses another user's session", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/show 1",
         chat=USER_B_CHAT, sender=USER_B_FROM, username="other",
         expect="That session is not yours.",
         db=[
             ("SELECT count(*) FROM users", [("2",)]),
             ("SELECT username FROM users WHERE user_id = %d"
              % USER_B_FROM, [("other",)]),
         ]),
])

show_usage = Scenario("show: /show rejects bad arguments", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/show 1 2", expect="Usage: /show [id]"),
    Step("/show abc", expect="Usage: /show [id]"),
])

stats_empty = Scenario("stats: /stats on a fresh user", [
    Step("/stats", contains=[
        "sessions: 0", "sets:     0",
        "heaviest: (none)", "top lift: (none)",
    ]),
])

stats_totals = Scenario("stats: /stats sums the all-time totals", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 100x5 60x10x2",
         expect="bench  100x5 + 60x10 + 60x10  [vol 1700]"),
    Step("/add squat 80x5", expect="squat  80x5  [vol 400]"),
    Step("/done", contains=["Session #1  (%s, closed)" % TODAY]),
    Step("/stats", contains=[
        "sessions: 1", "sets:     4", "volume:   2100 kg*rep",
        "heaviest: 100 kg on bench",
        "top lift: bench (vol 1700 kg*rep)",
    ]),
])

# ===========================================================================
# Data integrity
# ===========================================================================

db_group_linkage = Scenario("db: sets are linked to the right session and user", [
    Step("/new", expect="Session #1 started (%s). "
                        "Add sets with /add, finish with /done." % TODAY),
    Step("/add bench 60x10x2 55x5",
         expect="bench  60x10 + 60x10 + 55x5  [vol 1475]",
         db=[
             ("SELECT session_id FROM sets GROUP BY session_id", [("1",)]),
             ("SELECT user_id FROM sets GROUP BY user_id", [("1111",)]),
             ("SELECT count(*) FROM sets s "
              "JOIN sessions s2 ON s.session_id = s2.id "
              "WHERE s2.user_id = %d" % DEFAULT_FROM, [("3",)]),
         ]),
])

db_username_update = Scenario("db: username changes are persisted", [
    Step("/new", username="alice",
         expect="Session #1 started (%s). "
                "Add sets with /add, finish with /done." % TODAY,
         db=[("SELECT username FROM users WHERE user_id = %d"
              % DEFAULT_FROM, [("alice",)])]),
    Step("/list", username="alice_new",
         contains=["Recent sessions:"],
         db=[("SELECT username FROM users WHERE user_id = %d"
              % DEFAULT_FROM, [("alice_new",)])]),
])

SCENARIOS = [
    boot_unknown_command,
    empty_text,
    help_command,
    new_session,
    new_rollover,
    new_with_date,
    new_invalid_date,
    new_usage,
    add_no_session,
    add_usage,
    add_missing_exercise,
    add_no_sets,
    add_single_set,
    add_repeat_group,
    add_drop_set,
    add_mixed_repeat,
    add_bodyweight,
    add_multiword_name,
    add_bad_token_rollback,
    done_no_session,
    done_summary,
    list_sessions,
    list_after_done,
    show_detail,
    show_by_id,
    show_no_session,
    show_nonexistent,
    show_foreign,
    show_usage,
    stats_empty,
    stats_totals,
    db_group_linkage,
    db_username_update,
]