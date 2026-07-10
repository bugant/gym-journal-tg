# Gym Journal Telegram Bot — Implementation Plan

A minimal, single-user-per-chat Telegram bot for tracking gym sessions, written
in C on top of [botlib](https://github.com/antirez/botlib).

---

## 1. Repo layout

A fresh git repo (user-provided path). Vendored botlib files are copied
verbatim; only `gymjournal.c`, `Makefile`, `README.md`, and
`apikey.txt.example` are authored.

```
gym-journal-bot/
├── Makefile
├── README.md
├── apikey.txt.example
├── gymjournal.c          # our bot logic (the only authored source file)
├── botlib.h  botlib.c
├── sds.h    sds.c   sdsalloc.h
├── cJSON.h  cJSON.c
├── sqlite_wrap.h  sqlite_wrap.c
├── json_wrap.c
└── xmalloc.h
```

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

`parseSubset(token, &weight, &reps, &count)` returns 1 on success.

## 5. Commands

Private chat only — every DM reaches the callback, so `triggers = NULL`.
Dispatch by `strcasecmp(br->argv[0] + 1, ...)` (skip the leading `/`).

| Command                              | Behavior |
|--------------------------------------|----------|
| `/help`                              | List all commands + usage examples. |
| `/new [YYYY-MM-DD]`                  | Start a session for `br->from`; error if one is already open. Default date = now. |
| `/add <ex> <Sx...> [<Sx...>...]`     | Parse sub-set tokens, insert rows tied to the active session under one `group_id`. Replies with a collapsed one-line confirmation, e.g. `bench  60x10 + 55x8 + 50x6  (3 sets, vol 1380 kg*rep)`. |
| `/done`                              | Close the active session; print summary: #exercises (distinct groups), #sets (rows), total volume = sum(weight*reps). |
| `/list`                              | Last ~10 sessions: id, date (dd/mm), #sets, status (open/closed), total volume. |
| `/show [id]`                         | Default = active/last session; if `id` given, verify ownership (`br->from`). Print each group as **one collapsed line** (see below). |
| `/stats`                             | All-time totals: #sessions, #sets, total volume, heaviest single set (weight & exercise), top exercise by total volume. |
| unknown                              | Reply with a short hint pointing to `/help`. |

## 6. Display format

### Collapsed line per group (used by `/add` confirmation, `/show`, `/done`)

- Multiple distinct sub-sets:
  `bench  60x10 + 55x8 + 50x6  [vol 1380]`
- Uniform group (all rows identical after repeat-suffix expansion):
  `bench  3x10 @ 60kg  [vol 1800]`
- Bodyweight (weight 0):
  `pull ups  3x10  [bw]`

### `/list` row

```
#12  10/07  18 sets  closed  vol 4200 kg*rep
#13  10/07   3 sets  OPEN    vol  600 kg*rep
```

### `/show` full

```
Session #13  (10/07/2026, open)
  bench       60x10 + 55x8 + 50x6        vol 1380
  squat       5x5 @ 100kg                 vol 2500
  pull ups    3x10                        bw
3 exercises, 9 sets, total vol 3880 kg*rep
```

### Volume

`volume = sum(weight * reps)` across all real sub-set rows. Bodyweight sets
(weight 0) contribute 0 to volume but still count toward set totals.

## 7. Code structure (`gymjournal.c`)

```c
#include "botlib.h"

/* --- helpers --------------------------------------------------------- */
static int64_t getActiveSession(sqlite3 *db, int64_t user_id);
static int      isSubsetToken(const char *s);
static int      parseSubset(const char *tok, double *w, int *r, int *count);
static sds      collapseGroup(sqlite3 *db, int64_t group_id);

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
   space-joined tokens before it.
3. Parse each remaining token via `parseSubset`, expanding repeat suffix into
   individual (weight, reps) pairs.
4. `BEGIN`/`COMMIT` a transaction; insert one `sets` row per pair with a
   shared `group_id` (first inserted row id reused for the rest).
5. Reply with the collapsed line + total volume for this group.

## 8. Build & run

### `Makefile`

Mirrors botlib's, swapping `mybot.c` -> `gymjournal.c` and target name
`gymjournal`:

```make
FINAL_LIBS = -lpthread -lcurl -lsqlite3
# + -latomic on ARM

gymjournal: botlib.c cJSON.c sds.c sqlite_wrap.c json_wrap.c gymjournal.c
	$(CC) -g -O2 -Wall -W -std=c11 \
	    botlib.c cJSON.c sds.c json_wrap.c sqlite_wrap.c gymjournal.c \
	    -o gymjournal $(FINAL_LIBS)
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
- Code-review sanity for the subset parser (regex-free, manual `strchr`/`strtod`).
- Live Telegram testing deferred to the user (requires a real token).

## 10. Out of scope for v1

- Edit/delete of entries (`sets.id` and `group_id` already make this easy to
  add: `/del <group_id>`).
- RPE, duration, free-text notes.
- Group-chat triggers (only private DM for now).
- Export (CSV) and charts.
- Timezone handling (uses Unix timestamps everywhere; display assumes server
  `localtime`).
