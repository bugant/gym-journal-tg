# Gym Journal Telegram Bot — Implementation Plan

A minimal, single-user-per-chat Telegram bot for tracking gym sessions, written
in C on top of [botlib](https://github.com/antirez/botlib).

---

## 1. Repo layout

A fresh git repo (user-provided path). Vendored botlib files are copied
verbatim; authored files are `gymjournal.h`, `gymjournal.c` (pure logic),
`gymjournal_bot.c` (bot entry + command handlers + DB code),
`test_gymjournal.c` (hand-rolled test runner), `Makefile`, `README.md`, and
`apikey.txt.example`.

```
gym-journal-bot/
├── Makefile
├── README.md
├── apikey.txt.example
├── gymjournal.h          # pure-logic API + structs (no botlib/sqlite3 deps)
├── gymjournal.c          # sub-set parser, name extraction, collapse fmt, math
├── gymjournal_bot.c      # main, handleRequest, cron, DB access, cmd handlers
├── test_gymjournal.c     # hand-rolled assert+runner; links no curl/botlib
├── botlib.h  botlib.c
├── sds.h    sds.c   sdsalloc.h
├── cJSON.h  cJSON.c
├── sqlite_wrap.h  sqlite_wrap.c
├── json_wrap.c
└── xmalloc.h
```

### Layering

- **`gymjournal.c` / `.h`** — Telegram-agnostic. Depends only on `sds`
  (for output buffers). No `botlib.h`, no `sqlite3`. Owns the bug-prone,
  fully-deterministic rules: sub-set token parsing, exercise-name extraction,
  collapsed-line formatting, volume/summary math. Hospitals its `SubsetRow`,
  `Group`, `SessionSummary` structs.
- **`gymjournal_bot.c`** — botlib integration layer. Owns the SQL schema,
  DB queries (`getActiveSession`, multi-user scoping by `br->from`), the
  command handlers, and the botlib callbacks (`handleRequest`, `cron`,
  `main`). Builds `SubsetRow[]` / `Group[]` from `sets`-table query
  results and delegates formatting + math to the pure API in `gymjournal.c`.
- **`test_gymjournal.c`** — `#include "gymjournal.h"` only. Hand-rolled
  `ASSERT` / `ASSERT_STR` + table-driven case list + own `main`. Links
  nothing Telegram-related.

## 2. Dependencies

Only system libraries, both ubiquitous:

- `libcurl`
- `libsqlite3`

Link: `-lpthread -lcurl -lsqlite3` (mirrors botlib's Makefile; ARM adds
`-latomic`).

No vendored third-party code beyond antirez/botlib's own files.

## 3. Data model

`startBot()` receives a "create DB" SQL string. We prepend
`TB_CREATE_KV_STORE` (so the built-in KV API still works) and append our own
tables. One `sets` row per **real sub-set**; a `group_id` ties all rows
inserted by a single `/add` call together for collapsed display and future
edit/delete.

```sql
-- from botlib (prepended automatically):
--   KeyValue(expire INT, key TEXT, value BLOB)

CREATE TABLE IF NOT EXISTS users (
    user_id   INTEGER PRIMARY KEY,
    username  TEXT,
    first_seen INTEGER
);

CREATE TABLE IF NOT EXISTS sessions (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER,
    started_at INTEGER,
    closed_at  INTEGER            -- 0 = open
);

CREATE TABLE IF NOT EXISTS sets (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id  INTEGER,
    user_id     INTEGER,
    exercise    TEXT NOT NULL,
    weight      REAL NOT NULL,    -- kg, 0 = bodyweight
    reps        INTEGER NOT NULL,
    group_id    INTEGER,          -- shared by rows from one /add
    created_at  INTEGER
);

CREATE INDEX IF NOT EXISTS idx_sets_session ON sets(session_id);
CREATE INDEX IF NOT EXISTS idx_sets_group   ON sets(group_id);
CREATE INDEX IF NOT EXISTS idx_sets_user_ex ON sets(user_id, exercise);
CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
```

### Multi-user

Every query is scoped by `br->from` (Telegram user ID). No in-memory state is
kept. The active session is resolved on demand:

```sql
SELECT id FROM sessions
  WHERE user_id = ?i AND closed_at = 0
  ORDER BY started_at DESC LIMIT 1;
```

## 4. Input grammar for `/add`

Flexible sub-sets, one token per real sub-set, with a **repeat suffix** to keep
uniform entry terse without ambiguity:

```
/add bench 60x10x3        # three sub-sets, each 60kg x 10 reps  -> 3 rows
/add bench 60x10 55x8 50x6# drop set, three distinct sub-sets   -> 3 rows
/add bench 60x10         # single sub-set                        -> 1 row
/add bench x10           # bodyweight single set (weight = 0)   -> 1 row
/add Bench Press 60x10   # multi-word exercise names supported
```

### Token rules (split on `x`/`X`)

A token is sub-set-shaped iff it matches
`^(\d*\.?\d+)?[xX](\d+)([xX](\d+))?$`:

| Shape      | Meaning                         |
|------------|---------------------------------|
| `WxRxN`    | weight W, reps R, repeated N times (N rows) |
| `WxR`      | one row (W, R)                  |
| `xR`       | one row, weight 0 (bodyweight)   |

Everything before the first sub-set-shaped token is the **exercise name**
(allows "bench press", "shoulder press", etc.).

### Parsing helper

`parseSubset(token, &weight, &reps, &count)` returns 1 on success and is
declared in `gymjournal.h` (pure layer). The bot layer calls it; the test
binary calls it directly.

## 5. Commands

Private chat only — every DM reaches the callback, so `triggers = NULL`.
Dispatch by `strcasecmp(br->argv[0] + 1, ...)` (skip the leading `/`).

| Command                              | Behavior |
|--------------------------------------|----------|
| `/help`                              | List all commands + usage examples. |
| `/new [YYYY-MM-DD]`                  | Start a session for `br->from`; if a session is already open, close it first, then start the new one. The reply reports both the closed (if any) and the new session id. Default date = now. |
| `/add <ex> <Sx...> [<Sx...>...]`     | Parse sub-set tokens, insert rows tied to the active session under one `group_id`. Replies with a collapsed one-line confirmation, e.g. `bench  60x10 + 55x8 + 50x6  (3 sets, vol 1340 kg*rep)`. |
| `/done`                              | Close the active session; print summary: #exercises (distinct groups), #sets (rows), total volume = sum(weight*reps). |
| `/list`                              | Last ~10 sessions: id, date (dd/mm), #sets, status (open/closed), total volume. |
| `/show [id]`                         | Default = active/last session; if `id` given, verify ownership (`br->from`). Print each group as **one collapsed line** (see below). |
| `/stats`                             | All-time totals: #sessions, #sets, total volume, heaviest single set (weight & exercise), top exercise by total volume. |
| unknown                              | Reply with a short hint pointing to `/help`. |

## 6. Display format

### Collapsed line per group (used by `/add` confirmation, `/show`, `/done`)

Formatting rules live in `collapseGroup(rows, n)` in `gymjournal.c` and are
covered by exact-string assertions in `test_gymjournal.c`:

- Multiple distinct sub-sets (literal `+`-joined form):
  `bench  60x10 + 55x8 + 50x6  [vol 1340]`
- Uniform group (**all** rows identical after repeat-suffix expansion):
  `bench  3x10 @ 60kg  [vol 1800]`
- Mixed near-uniform (e.g. same reps, different weights) → falls back to
  the literal `+`-joined form; `@` form is only for fully-identical rows.
- Bodyweight group (every row weight 0):
  `pull ups  3x10  [bw]`
- Mixed bodyweight + weighted rows in one group → numeric `[vol N]`
  (no `[bw]`); `[bw]` is only shown when **all** rows are bodyweight.

### `/list` row

```
#12  10/07  18 sets  closed  vol 4200 kg*rep
#13  10/07   3 sets  OPEN    vol  600 kg*rep
```

### `/show` full

```
Session #13  (10/07/2026, open)
  bench       60x10 + 55x8 + 50x6        vol 1340
  squat       5x5 @ 100kg                 vol 2500
  pull ups    3x10                        bw
3 exercises, 9 sets, total vol 3840 kg*rep
```

### Volume

`volume = sum(weight * reps)` across all real sub-set rows. Bodyweight sets
(weight 0) contribute 0 to volume but still count toward set totals.

## 7. Code structure

### Pure layer — `gymjournal.h` / `gymjournal.c`

```c
/* gymjournal.h — no botlib, no sqlite3, only sds for output */

typedef struct {
    double weight;   /* kg, 0 = bodyweight */
    int    reps;
} SubsetRow;

typedef struct {
    int                n;
    const SubsetRow   *rows;
} Group;

typedef struct {
    int    exercises;   /* distinct groups */
    int    sets;        /* total rows */
    double volume;      /* sum(weight*reps); bw rows contribute 0 */
} SessionSummary;

/* Sub-set token parsing (§4) */
int isSubsetToken(const char *s);
int parseSubset(const char *tok, double *weight, int *reps, int *count);
                       /* 1 on success; count>=1, expands WxRxN */

/* Exercise-name extraction: space-join tokens before first sub-set token */
int extractName(const char *const *argv, int start, int end,
                char *out, size_t outsz);

/* Collapsed formatting for one group (§6) */
sds collapseGroup(const SubsetRow *rows, int n);
                       /* "bench  60x10 + 55x8 + 50x6  [vol 1340]"        */
                       /* "bench  3x10 @ 60kg  [vol 1800]" (uniform)      */
                       /* "pull ups  3x10  [bw]"          (bodyweight)     */

/* Volume math */
double groupVolume(const SubsetRow *rows, int n);
SessionSummary summarizeSession(const Group *groups, int ng);
```

```c
/* gymjournal.c */
#include "gymjournal.h"
#include "sds.h"
/* isSubsetToken, parseSubset, extractName, collapseGroup, groupVolume,
 * summarizeSession implementations. No botlib, no sqlite3. */
```

### Bot layer — `gymjournal_bot.c`

```c
#include "botlib.h"
#include "gymjournal.h"

/* --- DB helpers (this layer only) ------------------------------------ */
static int64_t getActiveSession(sqlite3 *db, int64_t user_id);
static sds     collapseGroupDb(sqlite3 *db, int64_t group_id);
                       /* fetches rows for a group, builds SubsetRow[],
                        * delegates to gymjournal.c::collapseGroup */

/* --- command handlers ------------------------------------------------ */
static void cmdHelp (sqlite3 *db, BotRequest *br);
static void cmdNew  (sqlite3 *db, BotRequest *br);
static void cmdAdd  (sqlite3 *db, BotRequest *br);
static void cmdDone (sqlite3 *db, BotRequest *br);
static void cmdList (sqlite3 *db, BotRequest *br);
static void cmdShow (sqlite3 *db, BotRequest *br);
static void cmdStats(sqlite3 *db, BotRequest *br);

/* --- botlib callbacks ------------------------------------------------ */
void handleRequest(sqlite3 *db, BotRequest *br);
void cron(sqlite3 *db);

int main(int argc, char **argv);
```

### `handleRequest` flow

1. Upsert user row: `INSERT OR IGNORE INTO users VALUES(?i,?s,?i)` then update
   `username` if changed (track `br->from_username`).
2. Skip if `br->argc == 0`.
3. Strip leading `/` from `br->argv[0]`; dispatch via `strcasecmp`.

### `cmdAdd` flow

1. Resolve active session; reply error if none.
2. Walk `br->argv[1..]` to find the first sub-set token; collect the name =
   space-joined tokens before it via `extractName`.
3. Parse each remaining token via `parseSubset`, expanding repeat suffix into
   individual (weight, reps) `SubsetRow`s (in-memory array).
4. `BEGIN`/`COMMIT` a transaction; insert one `sets` row per `SubsetRow`
   with a shared `group_id` (first inserted row id reused for the rest).
5. Reply with the collapsed line + total volume for this group, produced by
   `collapseGroup(rows,n)` and `groupVolume(rows,n)` from the pure layer.

## 8. Build & run

### `Makefile`

Mirrors botlib's, swapping `mybot.c` -> the `gymjournal.c` pure layer, the
`gymjournal_bot.c` bot layer, and target name `gymjournal`. A separate
`test_gymjournal` target builds the pure layer + tests with **no curl, no
botlib, no sqlite** linked:

```make
FINAL_LIBS = -lpthread -lcurl -lsqlite3
# + -latomic on ARM
TEST_BIN   = test_gymjournal

gymjournal: botlib.c cJSON.c sds.c sqlite_wrap.c json_wrap.c \
            gymjournal.c gymjournal_bot.c
	$(CC) -g -O2 -Wall -W -std=c11 \
	    botlib.c cJSON.c sds.c json_wrap.c sqlite_wrap.c \
	    gymjournal.c gymjournal_bot.c \
	    -o gymjournal $(FINAL_LIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): gymjournal.c test_gymjournal.c sds.c
	$(CC) -g -O2 -Wall -W -std=c11 \
	    gymjournal.c sds.c test_gymjournal.c \
	    -o $(TEST_BIN) -lpthread
```

### Running

1. Create the bot via [@BotFather](https://t.me/BotFather), obtain the API
   token.
2. Save the token in `apikey.txt` (in the bot's working directory) **or** pass
   `--apikey <token>`.
3. `make && ./gymjournal`
4. DM the bot. Optional: `--dbfile path.db` to choose the database file
   (default `./gymjournal.db`), `--debug`/`--verbose` for logging.

### `README.md`

Will cover: build deps, token setup, command reference (with examples per
section 5), the note that the bot is meant for private chat, and the schema.

## 9. Verification

- `make` compiles with zero warnings under `-Wall -W`.
- `make test` builds `test_gymjournal` (links no curl, no botlib, no sqlite)
  and exits 0. It automates the previously-manual "subset parser sanity"
  check and also covers the rest of the pure layer:
  - `parseSubset` / `isSubsetToken`: `60x10x3`·`60x10`·`x10`·`60X10`
    (case-insensitive)·`60.5x10` (decimal); negatives `60`·`x`·`10x`·`abc`
    ·`60x10x`·`60x10x3x2` → return 0; absurd-input count clamping.
  - `extractName`: single-word / multi-word (`Bench Press`) / no sub-set
    token / name-stops-before-first-sub-set cases.
  - `collapseGroup`: exact-string asserts for all three §6 variants
    (distinct `+` form, uniform `@` form, bodyweight `[bw]`), the
    near-uniform-but-not-identical fallback rule, mixed bw+weighted rule.
  - `groupVolume` / `summarizeSession`: bodyweight=0 contribution, distinct
    group counting, `double`-accumulator stability on large sums.
  - End-to-ish expansion vector: `bench 60x10x3 55x8 50x6` → 5 `SubsetRow`s
    → literal `+` form; `squat 100x5x5` → uniform `@` form; `pull ups x10 x10 x10`
    → bodyweight `[bw]`.
- Live Telegram testing (the botlib long-poll loop, network, real token,
  `handleRequest` dispatch, DB schema on disk) remains deferred to the user
  — the pure layer's correctness does not depend on it, and `test_gymjournal`
  deliberately links none of it.

## 10. Implementation order

Steps are ordered so that the pure layer is built and verified before any
bot code is written, keeping each step independently checkable.

1. **Vendor botlib files** — copy `botlib.{h,c}`, `sds.{h,c}`, `sdsalloc.h`,
   `cJSON.{h,c}`, `sqlite_wrap.{h,c}`, `json_wrap.c`, `xmalloc.h` verbatim
   into the repo root. Do not modify them.
2. **Author `gymjournal.h`** — define `SubsetRow`, `Group`,
   `SessionSummary` structs and declare `isSubsetToken`, `parseSubset`,
   `extractName`, `collapseGroup`, `groupVolume`, `summarizeSession`. No
   `botlib` / `sqlite3` includes.
3. **Author `gymjournal.c`** — implement the pure API in this order:
   a. `isSubsetToken` (regex-free, manual `strchr`/`strtod`).
   b. `parseSubset` (weight/reps/count extraction + repeat expansion).
   c. `extractName` (space-join until first sub-set token).
   d. `groupVolume` and `summarizeSession` (math, `double` accumulator).
   e. `collapseGroup` (the three §6 variants + mixed-rule fallbacks).
4. **Author `Makefile`** — write both the `gymjournal` bot target and the
   `test_gymjournal` / `test` target exactly as in §8. Confirm the test
   target links no curl, no botlib, no sqlite.
5. **Author `test_gymjournal.c`** — hand-rolled `ASSERT` / `ASSERT_STR`
   macros, table-driven case function array, own `main`. Cover the full
   test-case inventory listed in §9.
6. **Verify the pure layer in isolation** — run `make test_gymjournal && ./test_gymjournal`
   and confirm it exits 0 with all assertions passing. Fix failures here before
   touching bot code.
7. **Author `gymjournal_bot.c`** — in this order:
   a. The SQL schema (§3) passed to `startBot()`.
   b. `getActiveSession` + multi-user scoping helpers (`br->from`).
   c. The botlib callbacks `handleRequest` and `cron`.
   d. Command handlers `cmdHelp`, `cmdNew`, `cmdAdd`, `cmdDone`, `cmdList`,
      `cmdShow`, `cmdStats`. `cmdAdd` must build the in-memory `SubsetRow[]`
      via `parseSubset` and call `collapseGroup` / `groupVolume` for its reply;
      `cmdShow`, `cmdDone` must build `Group[]` from DB results and call
      `summarizeSession`.
   e. `main` (argument parsing, `--apikey` / `--dbfile` / `--debug` passthrough
      to `startBot`).
8. **Verify the bot compiles but with a real DB-backed dry run** — `make`
   must build with zero warnings under `-Wall -W`. Run `./gymjournal --help`
   (or equivalent) to confirm it starts, allocated-parses the schema on a
   throwaway `:memory:` or temp DB file, and exits cleanly. No Telegram
   traffic exercised here.
9. **Author or update `README.md`** — build deps, token setup, command
   reference with the §5 examples, the private-chat note, the schema, and a
   "Tests" section documenting `make test` and how to add a case.
10. **Final regression** — `make && make test && test $? -eq 0`. Live Telegram
    testing (real token, real DMs) is left to the user as per §9.

## 11. Out of scope for v1

- Edit/delete of entries (`sets.id` and `group_id` already make this easy to
  add: `/del <group_id>`).
- RPE, duration, free-text notes.
- Group-chat triggers (only private DM for now).
- Export (CSV) and charts.
- Timezone handling (uses Unix timestamps everywhere; display assumes server
  `localtime`).
