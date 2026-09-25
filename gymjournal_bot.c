/* gymjournal_bot.c -- Telegram bot integration layer.
 *
 * Owns the PostgreSQL schema, the multi-user-scoped DB queries, the botlib
 * command handlers and the botlib callbacks (handleRequest, cron, main).
 * It builds in-memory SubsetRow[] / Group[] from the `sets` table and
 * delegates all formatting and math to the pure API in gymjournal.{h,c}.
 *
 * Depends on botlib (and transitively libcurl + libpq). The pure
 * layer and its tests link none of this.
 */

/* botlib.c enables _GNU_SOURCE for us; mirror it so strptime-style
 * helpers and struct tm helpers are visible here too. */
#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#include "botlib.h"
#include "gymjournal.h"

/* ============================================================================
 * Schema (plan section 3)
 * ========================================================================== */

/* TB_CREATE_KV_STORE is prepended automatically by botlib via dbInit() when
 * we pass it as the `createdb_query`; here we chain our own tables after the
 * KV store. The whole string is fed to PQexec(), which accepts multiple
 * statements separated by ';'. */
#define GYM_SCHEMA \
    TB_CREATE_KV_STORE \
    "CREATE TABLE IF NOT EXISTS users (" \
        "user_id   BIGINT PRIMARY KEY," \
        "username  TEXT," \
        "first_seen BIGINT" \
    ");" \
    "CREATE TABLE IF NOT EXISTS sessions (" \
        "id         BIGSERIAL PRIMARY KEY," \
        "user_id    BIGINT," \
        "started_at BIGINT," \
        "closed_at  BIGINT" \
    ");" \
    "CREATE TABLE IF NOT EXISTS sets (" \
        "id          BIGSERIAL PRIMARY KEY," \
        "session_id  BIGINT," \
        "user_id     BIGINT," \
        "exercise    TEXT NOT NULL," \
        "weight      REAL NOT NULL," \
        "reps        BIGINT NOT NULL," \
        "group_id    BIGINT," \
        "created_at  BIGINT" \
    ");" \
    "CREATE INDEX IF NOT EXISTS idx_sets_session ON sets(session_id);" \
    "CREATE INDEX IF NOT EXISTS idx_sets_group   ON sets(group_id);" \
    "CREATE INDEX IF NOT EXISTS idx_sets_user_ex ON sets(user_id, exercise);" \
    "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);"

/* ============================================================================
 * Helpers
 * ========================================================================== */

/* Format a unix timestamp as dd/mm (with_year == 0) or dd/mm/yyyy (== 1).
 * Always NUL-terminated. Uses localtime. */
static sds fmtDate(sds out, time_t t, int with_year) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (with_year)
        out = sdscatprintf(out, "%02d/%02d/%04d",
                           tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900);
    else
        out = sdscatprintf(out, "%02d/%02d", tmv.tm_mday, tmv.tm_mon + 1);
    return out;
}

/* Append a real number rendered without a trailing decimal point when it is
 * integral (so volumes/weights look like "1340" not "1340.000000"). */
static sds appendReal(sds s, double v) {
    if (v == floor(v)) return sdscatprintf(s, "%.0f", v);
    sds tmp = sdscatprintf(sdsempty(), "%.6g", v);
    s = sdscatsds(s, tmp);
    sdsfree(tmp);
    return s;
}

/* Resolve the active (open) session id for a user, or 0 if none is open. */
static int64_t getActiveSession(PGconn *db, int64_t user_id) {
    return sqlSelectInt(db,
        "SELECT id FROM sessions "
        "  WHERE user_id = ?i AND closed_at = 0 "
        "  ORDER BY started_at DESC LIMIT 1",
        user_id);
}

/* Most recent session id for a user (open or closed), or 0 if the user has
 * no sessions at all. Used as the default target for /show. */
static int64_t getLatestSession(PGconn *db, int64_t user_id) {
    return sqlSelectInt(db,
        "SELECT id FROM sessions "
        "  WHERE user_id = ?i "
        "  ORDER BY started_at DESC LIMIT 1",
        user_id);
}

/* Send a short reply to the chat the request came from. Convenience wrapper
 * around the botlib send API so command handlers read naturally. A failed
 * send (e.g. Telegram rejecting the message) is logged, never silently
 * dropped. */
static void reply(BotRequest *br, sds text) {
    if (!botSendMessage(br->target, text, br->msg_id)) {
        fprintf(stderr, "ERROR: failed to send reply to chat %lld "
                        "(msg %lld): %.*s\n",
                (long long)br->target, (long long)br->msg_id,
                (int)sdslen(text), text);
    }
}

/* Find the index in argv[start..argc) of the first sub-set-shaped token.
 * Returns -1 if none is found. */
static int firstSubsetIdx(BotRequest *br, int start) {
    for (int i = start; i < br->argc; i++) {
        if (isSubsetToken(br->argv[i])) return i;
    }
    return -1;
}

/* Fetch the rows of one group from the DB into a freshly malloc'd
 * SubsetRow array and return it via *rows_out / *n_out. Returns 1 on
 * success (even for an empty group, with n_out == 0), 0 on allocation
 * failure. Caller frees *rows_out. */
static int loadGroupRows(PGconn *db, int64_t group_id,
                         SubsetRow **rows_out, int *n_out) {
    *rows_out = NULL;
    *n_out = 0;

    sqlRow row;
    sqlSelect(db, &row,
        "SELECT weight, reps FROM sets "
        "  WHERE group_id = ?i "
        "  ORDER BY id ASC",
        group_id);

    SubsetRow *buf = NULL;
    int cap = 0, n = 0;
    while (sqlNextRow(&row)) {
        if (n == cap) {
            int newcap = cap ? cap * 2 : 8;
            SubsetRow *t = realloc(buf, newcap * sizeof(SubsetRow));
            if (t == NULL) { sqlEnd(&row); free(buf); return 0; }
            buf = t;
            cap = newcap;
        }
        buf[n].weight = row.col[0].type == SQL_FLOAT
                            ? row.col[0].d
                            : (double)row.col[0].i;
        buf[n].reps = (int)row.col[1].i;
        n++;
    }
    sqlEnd(&row);

    *rows_out = buf;
    *n_out = n;
    return 1;
}

/* ============================================================================
 * Command handlers
 * ========================================================================== */

/* Forward declaration: cmdDone() reuses the default-target path of cmdShow()
 * to print the closing summary. */
static void cmdShow(PGconn *db, BotRequest *br);

static void cmdHelp(PGconn *db, BotRequest *br) {
    UNUSED(db);
    sds s =
        "Gym Journal commands (private chat only):\n"
        "\n"
        "/help                     show this message\n"
        "/new [YYYY-MM-DD]         start a new session for today\n"
        "/add <ex> <WxR[xN]> ...   add sets for one exercise, e.g.:\n"
        "    /add bench 60x10x3\n"
        "    /add bench 60x10 55x8 50x6\n"
        "    /add bench x10        (x10 = bodyweight)\n"
        "    /add Bench Press 60x5\n"
        "/done                     close the active session + summary\n"
        "/list                     last ~10 sessions\n"
        "/show [id]                show a session (default = active/last)\n"
        "/stats                    all-time totals\n";
    reply(br, s);
}

static void cmdNew(PGconn *db, BotRequest *br) {
    if (br->argc > 2) {
        reply(br, "Usage: /new [YYYY-MM-DD]");
        return;
    }

    time_t now = time(NULL);
    time_t started = now;
    if (br->argc == 2) {
        int y, m, d;
        char extra = '\0';
        /* Exactly YYYY-MM-DD, nothing else. */
        if (sscanf(br->argv[1], "%d-%d-%d%c", &y, &m, &d, &extra) != 3 ||
            y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) {
            reply(br, "Invalid date. Use YYYY-MM-DD, e.g. /new 2026-07-10");
            return;
        }
        struct tm tmv = {0};
        tmv.tm_year = y - 1900;
        tmv.tm_mon = m - 1;
        tmv.tm_mday = d;
        tmv.tm_hour = 12;   /* noon, to nudge DST edges into the right day */
        tmv.tm_isdst = -1;
        started = mktime(&tmv);
        if (started == (time_t)-1) {
            reply(br, "Invalid date.");
            return;
        }
    }

    /* If a session is already open, silently close it first so /new just
     * rolls over the previous session. We never refuse: the reply reports
     * both the closed (if any) and the new session id. `closed_id` is 0
     * when there was no open session to close. */
    int64_t closed_id = getActiveSession(db, br->from);
    if (closed_id != 0) {
        if (!sqlQuery(db, "UPDATE sessions SET closed_at = ?i WHERE id = ?i",
                      (int64_t)time(NULL), closed_id)) {
            reply(br, "Could not close the previous session.");
            return;
        }
    }

    int64_t id = sqlInsert(db,
        "INSERT INTO sessions (user_id, started_at, closed_at) "
        "VALUES (?i, ?i, ?i)",
        br->from, (int64_t)started, (int64_t)0);
    if (id == 0) {
        reply(br, "Could not create session.");
        return;
    }

    sds s = sdsempty();
    if (closed_id != 0) {
        s = sdscatprintf(s, "Closed session #%lld. ",
                         (long long)closed_id);
    }
    s = sdscat(s, "Session #");
    s = sdscatprintf(s, "%lld", (long long)id);
    s = sdscat(s, " started (");
    s = fmtDate(s, started, 1);
    s = sdscat(s, "). Add sets with /add, finish with /done.");
    reply(br, s);
    sdsfree(s);
}

static void cmdAdd(PGconn *db, BotRequest *br) {
    if (br->argc < 3) {
        reply(br, "Usage: /add <exercise> <WxR[xN]> [more ...]\n"
                  "Example: /add bench 60x10x3 55x8 50x6");
        return;
    }

    int64_t sid = getActiveSession(db, br->from);
    if (sid == 0) {
        reply(br, "No active session. Start one with /new.");
        return;
    }

    /* Locate the first sub-set token; everything before it is the name. */
    int first = firstSubsetIdx(br, 1);
    if (first < 0) {
        reply(br, "No sets given. Example: /add bench 60x10x3");
        return;
    }
    if (first == 1) {
        reply(br, "Missing exercise name. Example: /add bench 60x10");
        return;
    }

    char name[256];
    if (!extractName((const char *const *)br->argv, 1, first,
                     name, sizeof(name)) || name[0] == '\0') {
        reply(br, "Could not read the exercise name.");
        return;
    }

    /* Expand all sub-set tokens into individual SubsetRows (in order). */
    SubsetRow *rows = NULL;
    int ncap = 0, nrows = 0;
    for (int i = first; i < br->argc; i++) {
        double w = 0;
        int reps = 0, count = 0;
        if (!parseSubset(br->argv[i], &w, &reps, &count)) {
            sds s = sdscatprintf(sdsempty(),
                                 "Token '%s' is not a valid set. "
                                 "Use WxR or WxRxN (e.g. 60x10x3).",
                                 br->argv[i]);
            reply(br, s);
            sdsfree(s);
            free(rows);
            return;
        }
        for (int k = 0; k < count; k++) {
            if (nrows == ncap) {
                int newcap = ncap ? ncap * 2 : 8;
                SubsetRow *t = realloc(rows, newcap * sizeof(SubsetRow));
                if (t == NULL) { free(rows); reply(br, "Out of memory."); return; }
                rows = t;
                ncap = newcap;
            }
            rows[nrows].weight = w;
            rows[nrows].reps = reps;
            nrows++;
        }
    }
    if (nrows == 0) {
        reply(br, "No sets given.");
        free(rows);
        return;
    }

    /* Insert one row per real sub-set, sharing a group_id (the id of the
     * first inserted row of this /add). Wrap in a transaction so a partial
     * add never lands on disk. */
    sqlQuery(db, "BEGIN");
    int64_t *ids = malloc((size_t)nrows * sizeof(int64_t));
    if (ids == NULL) {
        sqlQuery(db, "ROLLBACK");
        free(rows);
        reply(br, "Out of memory.");
        return;
    }
    int ok = 1;
    for (int i = 0; i < nrows && ok; i++) {
        int64_t rid = sqlInsert(db,
            "INSERT INTO sets (session_id, user_id, exercise, weight, reps, "
            "                  group_id, created_at) "
            "VALUES (?i, ?i, ?s, ?d, ?i, ?i, ?i)",
            sid, br->from, name, rows[i].weight, (int64_t)rows[i].reps,
            (int64_t)0, (int64_t)time(NULL));
        if (rid == 0) { ok = 0; break; }
        ids[i] = rid;
    }
    if (!ok) {
        sqlQuery(db, "ROLLBACK");
        free(ids);
        free(rows);
        reply(br, "Could not insert sets.");
        return;
    }

    int64_t group_id = ids[0];
    for (int i = 0; i < nrows; i++) {
        if (!sqlQuery(db, "UPDATE sets SET group_id = ?i WHERE id = ?i",
                      group_id, ids[i])) {
            ok = 0;
            break;
        }
    }
    if (!ok) {
        sqlQuery(db, "ROLLBACK");
        free(ids);
        free(rows);
        reply(br, "Could not finalize group.");
        return;
    }
    sqlQuery(db, "COMMIT");
    free(ids);

    /* Reply with the collapsed one-line confirmation, as produced by the
     * pure layer, prefixed with the exercise name. */
    sds body = collapseGroup(rows, nrows);
    free(rows);

    sds s = sdsnew(name);
    s = sdscat(s, "  ");
    s = sdscatsds(s, body);
    sdsfree(body);
    reply(br, s);
    sdsfree(s);
}

static void cmdDone(PGconn *db, BotRequest *br) {
    int64_t sid = getActiveSession(db, br->from);
    if (sid == 0) {
        reply(br, "No active session to close.");
        return;
    }

    if (!sqlQuery(db, "UPDATE sessions SET closed_at = ?i WHERE id = ?i",
                  (int64_t)time(NULL), sid)) {
        reply(br, "Could not close the session.");
        return;
    }

    /* Reuse the show-the-session code path for the closing summary. */
    cmdShow(db, br);   /* falls through to the default (now closed) session */
}

static void cmdList(PGconn *db, BotRequest *br) {
    const int LIMIT = 10;
    sqlRow row;
    sqlSelect(db, &row,
        "SELECT s.id, s.started_at, s.closed_at, "
        "       (SELECT COUNT(*) FROM sets WHERE session_id = s.id), "
        "       (SELECT COALESCE(SUM(weight*reps),0) FROM sets "
        "        WHERE session_id = s.id) "
        "  FROM sessions s "
        "  WHERE s.user_id = ?i "
        "  ORDER BY s.started_at DESC "
        "  LIMIT ?i",
        br->from, (int64_t)LIMIT);

    sds s = sdsnew("Recent sessions:\n");
    int any = 0;
    while (sqlNextRow(&row)) {
        any = 1;
        int64_t id = row.col[0].i;
        time_t started = (time_t)row.col[1].i;
        int64_t closed = row.col[2].i;
        int64_t nsets = row.col[3].i;
        double vol = row.col[4].type == SQL_FLOAT
                         ? row.col[4].d : (double)row.col[4].i;

        s = sdscatprintf(s, "#%lld  ", (long long)id);
        s = fmtDate(s, started, 0);
        s = sdscatprintf(s, "  %2lld sets  %-6s  vol ",
                        (long long)nsets,
                        closed ? "closed" : "OPEN");
        s = appendReal(s, vol);
        s = sdscat(s, " kg*rep\n");
    }
    sqlEnd(&row);

    if (!any) s = sdscat(s, "(none yet — start one with /new)");
    reply(br, s);
    sdsfree(s);
}

/* Print the full detail of a session: header + one collapsed line per
 * group + summary footer. `sid` is the session id, already verified as
 * owned by br->from. Return the possibly reallocated reply buffer. */
static sds printSessionDetail(PGconn *db, sds s, int64_t sid, int64_t user_id) {
    /* So we can pad exercise names into a tidy column, look up the longest
     * name in this session first (cheap: few rows). */
    int namepad = 6;
    {
        sqlRow r;
        sqlSelect(db, &r,
            "SELECT MIN(exercise) FROM sets "
            "  WHERE session_id = ?i AND user_id = ?i "
            "  GROUP BY group_id ORDER BY MIN(id) ASC",
            sid, user_id);
        while (sqlNextRow(&r)) {
            size_t len = r.col[0].s ? strlen(r.col[0].s) : 0;
            if ((int)len > namepad) namepad = (int)len;
        }
        sqlEnd(&r);
    }

    /* Iterate over distinct groups in insertion order. For each group we
     * load its rows, format them with collapseGroup(), and add the volume
     * via groupVolume(); the Group[] array is built in parallel so we can
     * call summarizeSession() for the footer. */
        sqlRow row;
        sqlSelect(db, &row,
        "SELECT group_id, MIN(exercise) FROM sets "
        "  WHERE session_id = ?i AND user_id = ?i "
        "  GROUP BY group_id ORDER BY MIN(id) ASC",
        sid, user_id);

    Group *groups = NULL;
    SubsetRow **growns = NULL;
    int *gns = NULL;
    int ng = 0, gcap = 0;

    while (sqlNextRow(&row)) {
        int64_t group_id = row.col[0].i;
        const char *exercise = row.col[1].s;

        SubsetRow *rs = NULL;
        int rn = 0;
        if (!loadGroupRows(db, group_id, &rs, &rn)) {
            sqlEnd(&row);
            for (int i = 0; i < ng; i++) free(growns[i]);
            free(growns); free(gns); free(groups);
            sdsclear(s);
            s = sdscat(s, "Could not load session.");
            return s;
        }

        if (ng == gcap) {
            int newcap = gcap ? gcap * 2 : 8;
            Group *tg = realloc(groups, newcap * sizeof(Group));
            SubsetRow **tr = realloc(growns, newcap * sizeof(SubsetRow *));
            int *tn = realloc(gns, newcap * sizeof(int));
            if (!tg || !tr || !tn) {
                free(rs);
                if (tg) groups = tg;
                if (tr) growns = tr;
                if (tn) gns = tn;
                sqlEnd(&row);
                for (int i = 0; i < ng; i++) free(growns[i]);
                free(growns); free(gns); free(groups);
                sdsclear(s);
                s = sdscat(s, "Out of memory.");
                return s;
            }
            groups = tg; growns = tr; gns = tn; gcap = newcap;
        }
        groups[ng].n = rn;
        groups[ng].rows = rs;
        growns[ng] = rs;
        gns[ng] = rn;
        ng++;

        /* Header line per group. */
        s = sdscat(s, "  ");
        s = sdscat(s, exercise);
        int pad = namepad - (int)strlen(exercise);
        for (int i = 0; i < pad; i++) s = sdscat(s, " ");
        s = sdscat(s, "  ");
        sds body = collapseGroup(rs, rn);
        s = sdscatsds(s, body);
        sdsfree(body);
        s = sdscat(s, "\n");
    }
    sqlEnd(&row);

    SessionSummary sum = summarizeSession(groups, ng);

    s = sdscatprintf(s, "%d exercise%s, %d sets, total vol ",
                    sum.exercises, sum.exercises == 1 ? "" : "s",
                    sum.sets);
    s = appendReal(s, sum.volume);
    s = sdscat(s, " kg*rep");

    for (int i = 0; i < ng; i++) free(growns[i]);
    free(growns); free(gns); free(groups);
    return s;
}

static void cmdShow(PGconn *db, BotRequest *br) {
    int64_t sid = 0;

    if (br->argc == 1) {
        sid = getLatestSession(db, br->from);   /* open if any, else last */
    } else if (br->argc == 2) {
        /* /show <id> with explicit numeric id; verify ownership. */
        char extra = '\0';
        long long parsed = 0;
        if (sscanf(br->argv[1], "%lld%c", &parsed, &extra) != 1 || parsed < 1) {
            reply(br, "Usage: /show [id]");
            return;
        }
        int64_t owner = sqlSelectInt(db,
            "SELECT user_id FROM sessions WHERE id = ?i", (int64_t)parsed);
        if (owner == 0) {
            reply(br, "No such session.");
            return;
        }
        if (owner != br->from) {
            reply(br, "That session is not yours.");
            return;
        }
        sid = (int64_t)parsed;
    } else {
        reply(br, "Usage: /show [id]");
        return;
    }

    if (sid == 0) {
        reply(br, "You have no sessions yet.");
        return;
    }

    /* Header: "#id  (dd/mm/yyyy, open|closed)". */
    sqlRow r;
    sqlSelectOneRow(db, &r,
        "SELECT started_at, closed_at FROM sessions WHERE id = ?i",
        sid);
    if (r.col == NULL) {
        reply(br, "No such session.");
        return;
    }
    time_t started = (time_t)r.col[0].i;
    int64_t closed = r.col[1].i;
    sqlEnd(&r);

    sds s = sdscatprintf(sdsempty(), "Session #%lld  (", (long long)sid);
    s = fmtDate(s, started, 1);
    s = sdscat(s, closed ? ", closed)" : ", open)");
    s = sdscat(s, "\n");

    s = printSessionDetail(db, s, sid, br->from);
    reply(br, s);
    sdsfree(s);
}

static void cmdStats(PGconn *db, BotRequest *br) {
    int64_t nsessions = sqlSelectInt(db,
        "SELECT COUNT(*) FROM sessions WHERE user_id = ?i", br->from);
    int64_t nsets = sqlSelectInt(db,
        "SELECT COUNT(*) FROM sets WHERE user_id = ?i", br->from);
    double totalvol = 0.0;
    {
        sqlRow r;
        sqlSelectOneRow(db, &r,
            "SELECT COALESCE(SUM(weight*reps),0) FROM sets WHERE user_id = ?i",
            br->from);
        if (r.col && (r.col[0].type == SQL_FLOAT || r.col[0].type == SQL_INTEGER))
            totalvol = r.col[0].type == SQL_FLOAT ? r.col[0].d : (double)r.col[0].i;
        sqlEnd(&r);
    }

    sds s = sdsnew("Your all-time stats:\n");
    s = sdscatprintf(s, "  sessions: %lld\n", (long long)nsessions);
    s = sdscatprintf(s, "  sets:     %lld\n", (long long)nsets);
    s = sdscat(s, "  volume:   ");
    s = appendReal(s, totalvol);
    s = sdscat(s, " kg*rep\n");

    /* Heaviest single set. */
    {
        sqlRow r;
        sqlSelectOneRow(db, &r,
            "SELECT weight, exercise FROM sets "
            "  WHERE user_id = ?i AND weight > 0 "
            "  ORDER BY weight DESC LIMIT 1",
            br->from);
        if (r.col && r.col[0].type != SQL_NULL) {
            double w = r.col[0].type == SQL_FLOAT ? r.col[0].d
                                                     : (double)r.col[0].i;
            const char *ex = r.col[1].s ? r.col[1].s : "?";
            s = sdscat(s, "  heaviest: ");
            s = appendReal(s, w);
            s = sdscatprintf(s, " kg on %s\n", ex);
        } else {
            s = sdscat(s, "  heaviest: (none)\n");
        }
        sqlEnd(&r);
    }

    /* Top exercise by total volume. */
    {
        sqlRow r;
        sqlSelectOneRow(db, &r,
            "SELECT exercise, SUM(weight*reps) AS vol FROM sets "
            "  WHERE user_id = ?i "
            "  GROUP BY exercise ORDER BY vol DESC LIMIT 1",
            br->from);
        if (r.col && r.col[0].type != SQL_NULL) {
            const char *ex = r.col[0].s ? r.col[0].s : "?";
            double vol = r.col[1].type == SQL_FLOAT ? r.col[1].d
                                                       : (double)r.col[1].i;
            s = sdscat(s, "  top lift: ");
            s = sdscat(s, ex);
            s = sdscat(s, " (vol ");
            s = appendReal(s, vol);
            s = sdscat(s, " kg*rep)\n");
        } else {
            s = sdscat(s, "  top lift: (none)\n");
        }
        sqlEnd(&r);
    }

    reply(br, s);
    sdsfree(s);
}

/* ============================================================================
 * Botlib callbacks
 * ========================================================================== */

/* Per-request entry point. Upserts the user row, then dispatches the
 * command. The request object is freed by botlib after we return. */
void handleRequest(PGconn *db, BotRequest *br) {
    /* Upsert user row, tracking username changes. */
    sqlQuery(db, "INSERT INTO users (user_id, username, first_seen) "
                 "VALUES (?i, ?s, ?i) ON CONFLICT (user_id) DO NOTHING",
             br->from,
             br->from_username ? br->from_username : "",
             (int64_t)time(NULL));
    if (br->from_username)
        sqlQuery(db, "UPDATE users SET username = ?s WHERE user_id = ?i",
                 br->from_username, br->from);

    if (br->argc == 0 && br->type != TB_TYPE_PRIVATE) return;

    /* Only react to commands in private chat. */
    if (br->type != TB_TYPE_PRIVATE) {
        /* Group messages only reach us through triggers; the gym journal is
         * private-chat only, so politely ignore group traffic. */
        return;
    }

    if (br->argc == 0) {
        sds s = sdsnew("Hi! I track your gym sessions. Try /help.");
        reply(br, s);
        sdsfree(s);
        return;
    }

    const char *cmd = br->argv[0];
    if (cmd[0] == '/') cmd++;      /* strip leading slash */

    if      (!strcasecmp(cmd, "help"))  cmdHelp(db, br);
    else if (!strcasecmp(cmd, "new"))   cmdNew(db, br);
    else if (!strcasecmp(cmd, "add"))   cmdAdd(db, br);
    else if (!strcasecmp(cmd, "done"))  cmdDone(db, br);
    else if (!strcasecmp(cmd, "list"))  cmdList(db, br);
    else if (!strcasecmp(cmd, "show"))  cmdShow(db, br);
    else if (!strcasecmp(cmd, "stats")) cmdStats(db, br);
    else {
        sds s = sdsnew("Unknown command. Type /help for the list.");
        reply(br, s);
        sdsfree(s);
    }
}

/* Cron hook. Currently nothing to do periodically; kept for future use
 * (e.g. session timeouts). */
void cron(PGconn *db) {
    UNUSED(db);
}

/* ============================================================================
 * Entry point
 * ========================================================================== */

int main(int argc, char **argv) {
    /* All option parsing (--apikey, --dburl, --debug, --verbose) is handled
     * by botlib's startBot(); we just pass through. Triggers == NULL means
     * every private DM reaches the callback, which is what we want.
     *
     * botlib resolves the API key as: --apikey flag, else apikey.txt. We add
     * a third source in between, the GYMJOURNAL_API_KEY environment variable,
     * by injecting it as a synthetic --apikey argument when it is set and the
     * caller did not already pass --apikey explicitly. Final precedence:
     *   --apikey flag  >  GYMJOURNAL_API_KEY env  >  apikey.txt file
     */
    const char *envkey = getenv("GYMJOURNAL_API_KEY");
    int has_apikey_arg = 0;
    for (int j = 1; j < argc; j++) {
        if (!strcmp(argv[j], "--apikey")) { has_apikey_arg = 1; break; }
    }

    if (envkey && *envkey && !has_apikey_arg) {
        char **newargv = malloc(sizeof(char *) * (argc + 3));
        if (newargv == NULL) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        memcpy(newargv, argv, sizeof(char *) * argc);
        newargv[argc]     = (char *)"--apikey";
        newargv[argc + 1] = (char *)envkey;
        newargv[argc + 2] = NULL;
        int rc = startBot((char *)GYM_SCHEMA, argc + 2, newargv,
                          TB_FLAGS_NONE, handleRequest, cron, NULL);
        free(newargv); /* not reached: startBot loops forever or exits */
        return rc;
    }

    return startBot((char *)GYM_SCHEMA, argc, argv, TB_FLAGS_NONE,
                    handleRequest, cron, NULL);
}
