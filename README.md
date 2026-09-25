# Gym Journal Telegram Bot

A minimal, single-user-per-chat Telegram bot for tracking gym sessions,
written in C on top of [botlib](https://github.com/antirez/botlib).

The bot is meant for **private chat only**: every DM reaches the callback,
and all data is scoped by your Telegram user id (`br->from`). It keeps no
in-memory state — everything lives in a PostgreSQL database.

---

## Build dependencies

Only system libraries, both ubiquitous:

- `libcurl`
- `libpq`

Plus a C11 compiler. On Debian/Ubuntu:

    sudo apt-get install build-essential libcurl4-openssl-dev libpq-dev

Before building the bot itself. The pure-layer test binary deliberately
links none of these (see "Tests" below).

## Building

    make

This produces the `gymjournal` binary. Build artifacts (`gymjournal`,
`test_gymjournal`, `*.o`) and the local database (`*.db`) are git-ignored.

## Token setup

1. Create a bot via [@BotFather](https://t.me/BotFather) and copy the API
   token it gives you.
2. Save it in any of these three ways (in order of precedence):
   - Pass it on the command line with `--apikey <token>`.
   - Export the `GYMJOURNAL_API_KEY` environment variable, e.g.
     `export GYMJOURNAL_API_KEY=123456:ABC...` (handy for systemd units,
     containers, and shells where you want to avoid writing secrets to disk).
   - Put the token on a single line in `apikey.txt` in the bot's working
     directory. **Never commit this file** — it is git-ignored.

An example token file is provided as `apikey.txt.example`.

## Running

    make && ./gymjournal

Optional command-line flags (passed through to botlib):

| Flag                    | Meaning                                                     |
|-------------------------|-------------------------------------------------------------|
| `--apikey <token>`      | Telegram bot API token (overrides `apikey.txt`).           |
| `--dburl <conninfo>`     | PostgreSQL libpq connection string (overrides `DATABASE_URL` and `PG*`). |
| `--debug`                | Enable debug logging. Repeat for more verbosity.           |
| `--verbose`              | Enable verbose message logging.                           |

Environment variables:

| Variable          | Meaning                                                           |
|-------------------|-------------------------------------------------------------------|
| `GYMJOURNAL_API_KEY` | Bot API token, injected between `--apikey` and `apikey.txt` (see "Token setup"). |
| `DATABASE_URL`       | PostgreSQL connection URI used when `--dburl` is not given (e.g. injected by the Aiven Runtime service integration). Falls back to the libpq `PG*` variables when unset. |
| `TELEGRAM_API_URL`   | Base URL of the Telegram Bot API (default `https://api.telegram.org`). Point this at a local mock server for end-to-end tests — see below. |

Then DM the bot from Telegram. The bot long-polls Telegram, so it needs
outbound HTTPS to `api.telegram.org`.

## Commands

All commands are dispatched case-insensitively; the leading `/` is optional
in matching but is typed in Telegram as `/help`, `/new`, etc.

| Command                              | Behavior                                              |
|--------------------------------------|-------------------------------------------------------|
| `/help`                              | List all commands with usage examples.               |
| `/new [YYYY-MM-DD]`                  | Start a session for today (or the given date). If a session is already open, it is closed first, then the new one starts; the reply reports both the closed (if any) and the new session id. |
| `/add <ex> <WxR[xN]> [more ...]`     | Add sets for one exercise. Replies with a collapsed one-line confirmation. |
| `/done`                              | Close the active session and print a summary.        |
| `/list`                              | Last ~10 sessions: id, date, #sets, status, total volume. |
| `/show [id]`                         | Show a session in full (default = active/last). Verifies ownership. |
| `/stats`                             | All-time totals: #sessions, #sets, total volume, heaviest single set, top exercise by volume. |
| unknown                              | Reply with a short hint pointing to `/help`.          |

### Input grammar for `/add`

One token per real sub-set, with a **repeat suffix** to keep uniform entry
terse without ambiguity. A token is sub-set-shaped iff it matches
`WxR`, `WxRxN`, or `xR` (case-insensitive on `x`).

| Shape     | Meaning                                              | Rows |
|-----------|------------------------------------------------------|------|
| `WxRxN`   | weight `W`, reps `R`, repeated `N` times             | N    |
| `WxR`     | one set at `W` kg for `R` reps                       | 1    |
| `xR`      | one bodyweight set (weight = 0) for `R` reps         | 1    |

Everything before the first sub-set-shaped token is the **exercise name**
(allowing multi-word names such as "Bench Press"). Decimal weights are
supported, e.g. `60.5x10`.

### Examples

    /add bench 60x10x3      # three sets of 60kg x 10 reps        -> 3 rows
    /add bench 60x10 55x8 50x6   # drop set, three distinct sets  -> 3 rows
    /add bench 60x10       # single set                          -> 1 row
    /add bench x10         # bodyweight single set (weight = 0)  -> 1 row
    /add Bench Press 60x10 # multi-word exercise name supported   -> 1 row

The reply for `/add` is a collapsed one-line confirmation such as:

    bench  60x10 + 55x8 + 50x6  [vol 1340]

Uniform groups collapse further:

    bench  3x10 @ 60kg  [vol 1800]

All-bodyweight groups show `[bw]` instead of a volume:

    pull ups  3x10  [bw]

## Display formats

### `/show`

```
Session #13  (10/07/2026, open)
  bench       60x10 + 55x8 + 50x6     vol 1340
  squat       5x5 @ 100kg              vol 2500
  pull ups    3x10                     bw
3 exercises, 9 sets, total vol 3840 kg*rep
```

### `/list`

```
Recent sessions:
#12  10/07  18 sets  closed  vol 4200 kg*rep
#13  10/07   3 sets  OPEN    vol  600 kg*rep
```

### Volume

`volume = sum(weight * reps)` across all real sub-set rows. Bodyweight sets
(weight = 0) contribute 0 to volume but still count toward set totals.

## Database schema

The bot prepends botlib's built-in `KeyValue` table (so the KV API still
works) and creates:

```sql
CREATE TABLE IF NOT EXISTS users (
    user_id   BIGINT PRIMARY KEY,
    username  TEXT,
    first_seen BIGINT
);

CREATE TABLE IF NOT EXISTS sessions (
    id         BIGSERIAL PRIMARY KEY,
    user_id    BIGINT,
    started_at BIGINT,
    closed_at  BIGINT             -- 0 = open
);

CREATE TABLE IF NOT EXISTS sets (
    id          BIGSERIAL PRIMARY KEY,
    session_id  BIGINT,
    user_id     BIGINT,
    exercise    TEXT NOT NULL,
    weight      REAL NOT NULL,    -- kg, 0 = bodyweight
    reps        BIGINT NOT NULL,
    group_id    BIGINT,           -- shared by rows from one /add
    created_at  BIGINT
);

CREATE INDEX IF NOT EXISTS idx_sets_session ON sets(session_id);
CREATE INDEX IF NOT EXISTS idx_sets_group   ON sets(group_id);
CREATE INDEX IF NOT EXISTS idx_sets_user_ex ON sets(user_id, exercise);
CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
```

Every query is scoped by `br->from` (your Telegram user ID), so multiple
users can share one bot instance without seeing each other's data. One
`sets` row is inserted per **real sub-set**; a `group_id` ties all rows
inserted by a single `/add` call together for collapsed display and
future edit/delete.

## Tests

The pure-logic layer (`gymjournal.c` / `gymjournal.h`) is Telegram-agnostic
and has a hand-rolled test runner that links **no curl, no botlib, no
PostgreSQL** — only `gymjournal.c`, `sds.c`, and `test_gymjournal.c`:

    make test

A successful run prints something like:

    ran 7 test groups, 0 assertion failure(s)

The runner is table-driven: each test is a `void` function registered in
the `tests[]` array near the bottom of `test_gymjournal.c`. To add a case,
write a `static void test_foo(void)` function that uses the `ASSERT` /
`ASSERT_STR` macros and append an entry to `tests[]`:

```c
static void test_foo(void) {
    ASSERT(some_pure_function(...) == 1);
}

/* ...later, in the table: */
{ "foo", test_foo },
```

Pass `-v` to the test binary for verbose per-group output:

    ./test_gymjournal -v

Live Telegram testing (the botlib long-poll loop, network, a real token,
real DMs, `handleRequest` dispatch, the schema on disk) is left to the
user — the pure layer's correctness does not depend on it, and
`test_gymjournal` deliberately links none of it.

## End-to-end tests

`make e2e` runs the **real bot binary** against a local mock Telegram Bot
API server and a scratch PostgreSQL cluster, asserting on the bot's replies
*and* on the rows it writes:

    make e2e

A successful run prints something like:

    ran 33 scenarios, 33 passed, 0 failed

### How it works

```
make e2e  ── run_e2e.py ─────────────────────────────────────────────
               │ spawns / orchestrates / asserts
               ├─ postgres  (scratch cluster: initdb + pg_ctl, high port)
               ├─ mock_tg   (local "Telegram", plain HTTP on 127.0.0.1)
               └─ ./gymjournal
                    TELEGRAM_API_URL=http://127.0.0.1:<mockport> \
                    --apikey 123456:TESTTOKEN --dburl host=...dbname=gjtest
```

- **`tests/e2e/mock_tg.py`** — a stdlib-only fake Telegram Bot API server.
  It answers `getMe`, `getUpdates`, `sendMessage` (and `editMessageText`,
  `sendPhoto`, `getFile` for completeness), keeps an outbox of user messages
  the driver enqueues and an inbox of everything the bot "sends".
- **`tests/e2e/pg_setup.py`** — provisions an isolated PostgreSQL cluster in
  a temp dir (requires `initdb`, `pg_ctl`, `createdb`, `psql` on PATH; no
  root, no touching your system cluster).
- **`tests/e2e/scenarios.py`** — the data-driven scenarios. Each step sends
  one user message, waits for the bot's single reply and checks it (exact,
  regex, or substring), then runs optional SQL assertions against the
  database. Every scenario starts from a freshly truncated database, so
  session/set ids are deterministic.
- **`tests/e2e/run_e2e.py`** — the driver: boots postgres, boots the mock,
  spawns the bot with `TELEGRAM_API_URL` pointing at the mock, waits for the
  bot's `getMe` to come up, then replays the scenarios.

### Running subsets

    make e2e-verbose                        # per-step reply output
    make e2e-filter FILTER=add              # only scenarios matching "add"
    python3 tests/e2e/run_e2e.py --list-scenarios

### Adding a scenario

Add a `Scenario(name, steps)` to `scenarios.py` and append it to the
`SCENARIOS` list. Each `Step(send, expect=..., contains=..., db=[(sql, rows)])`
sends `send` to the bot and, after the reply arrives, checks the expected
text and runs the `db` SQL assertions (rows as lists of tab-separated
strings). The bot answers every private message with exactly one
`sendMessage`, and the driver drives one message at a time, so ordering is
deterministic.

### Requirements

- A system PostgreSQL server (`postgresql-server` on Fedora) with the client
  tools on `PATH`.
- `python3` (stdlib only).
- The bot built with `make`.

## Layering

- **`gymjournal.c` / `gymjournal.h`** — Telegram-agnostic. Depends only on
  `sds`. Owns sub-set token parsing, exercise-name extraction, collapsed-line
  formatting, and volume/summary math. Covered by `test_gymjournal.c`.
- **`gymjournal_bot.c`** — botlib integration layer. Owns the SQL schema,
  the multi-user-scoped DB queries, the command handlers, and the botlib
  callbacks (`handleRequest`, `cron`, `main`). Builds in-memory
  `SubsetRow[]` / `Group[]` from DB results and delegates formatting + math
  to the pure API.
- **`test_gymjournal.c`** — `#include "gymjournal.h"` only. Hand-rolled
  `ASSERT` / `ASSERT_STR` + table-driven case list + own `main`.
