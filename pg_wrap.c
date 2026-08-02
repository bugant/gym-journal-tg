#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stddef.h>
#include <libpq-fe.h>

#include "pg_wrap.h"
#include "sds.h"
#include "xmalloc.h"

static pthread_mutex_t DbMutex;
static pthread_once_t DbMutexOnce = PTHREAD_ONCE_INIT;

static void initMutex(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&DbMutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void pgDbLock(void) {
    pthread_once(&DbMutexOnce, initMutex);
    pthread_mutex_lock(&DbMutex);
}

void pgDbUnlock(void) {
    pthread_mutex_unlock(&DbMutex);
}

static int makeQuery(const char *sql, va_list ap, sds *out, char spec[SQL_MAX_SPEC],
                     int *num, const char **values, int *lengths, int *formats,
                     Oid *types, char buffers[SQL_MAX_SPEC][64]) {
    sds query = sdsempty();
    const char *p = sql;
    int n = 0;
    while (*p) {
        if (*p == '?') {
            if (p[1] != 's' && p[1] != 'b' && p[1] != 'i' && p[1] != 'd') goto error;
            if (n == SQL_MAX_SPEC) goto error;
            spec[n] = p[1];
            if (p[1] == 'b') {
                values[n] = va_arg(ap, const char *);
                lengths[n] = (int)va_arg(ap, size_t);
                formats[n] = 1;
                types[n] = 17; /* bytea */
            } else if (p[1] == 's') {
                values[n] = va_arg(ap, const char *);
                lengths[n] = 0;
                formats[n] = 0;
                types[n] = 25; /* text */
            } else if (p[1] == 'i') {
                int64_t v = va_arg(ap, int64_t);
                snprintf(buffers[n], sizeof(buffers[n]), "%lld", (long long)v);
                values[n] = buffers[n]; lengths[n] = 0; formats[n] = 0; types[n] = 20;
            } else {
                double v = va_arg(ap, double);
                snprintf(buffers[n], sizeof(buffers[n]), "%.17g", v);
                values[n] = buffers[n]; lengths[n] = 0; formats[n] = 0; types[n] = 701;
            }
            query = sdscatprintf(query, "$%d", ++n);
            p += 2;
        } else {
            query = sdscatlen(query, p, 1);
            p++;
        }
    }
    *out = query; *num = n;
    return 1;
error:
    sdsfree(query);
    return 0;
}

static int fillRow(sqlRow *row, PGresult *res, int current) {
    row->res = res;
    row->current = current;
    row->cols = PQnfields(res);
    row->col = xmalloc((size_t)row->cols * sizeof(*row->col));
    row->owned = calloc((size_t)row->cols, sizeof(*row->owned));
    if (!row->owned) abort();
    for (int j = 0; j < row->cols; j++) {
        sqlCol *c = &row->col[j];
        if (PQgetisnull(res, current, j)) {
            c->type = SQL_NULL; c->s = NULL; c->i = 0; c->d = 0;
        } else if (PQftype(res, j) == 701 || PQftype(res, j) == 700) {
            c->type = SQL_FLOAT; c->d = strtod(PQgetvalue(res, current, j), NULL);
        } else if (PQftype(res, j) == 17) {
            size_t len = 0;
            unsigned char *v = PQunescapeBytea((unsigned char *)PQgetvalue(res, current, j), &len);
            c->type = SQL_BLOB; c->s = (const char *)v; c->i = (int64_t)len; row->owned[j] = v;
        } else if (PQftype(res, j) == 20 || PQftype(res, j) == 21 || PQftype(res, j) == 23) {
            c->type = SQL_INTEGER; c->i = strtoll(PQgetvalue(res, current, j), NULL, 10);
        } else {
            c->type = SQL_TEXT; c->s = PQgetvalue(res, current, j); c->i = PQgetlength(res, current, j);
        }
    }
    return 1;
}

int sqlGenericQuery(PGconn *dbhandle, sqlRow *row, const char *sql, va_list ap) {
    char spec[SQL_MAX_SPEC], buffers[SQL_MAX_SPEC][64];
    const char *values[SQL_MAX_SPEC]; int lengths[SQL_MAX_SPEC], formats[SQL_MAX_SPEC];
    Oid types[SQL_MAX_SPEC]; int n;
    sds query = NULL;
    if (row) { memset(row, 0, sizeof(*row)); }
    va_list copy; va_copy(copy, ap);
    int ok = makeQuery(sql, copy, &query, spec, &n, values, lengths, formats, types, buffers);
    va_end(copy);
    if (!ok) return SQL_ERROR;
    pgDbLock();
    PGresult *res = PQexecParams(dbhandle, query, n, types, values, lengths, formats, 0);
    pgDbUnlock();
    sdsfree(query);
    if (!res) return SQL_ERROR;
    ExecStatusType status = PQresultStatus(res);
    if (status == PGRES_TUPLES_OK && PQntuples(res) > 0 && row) {
        row->res = res;
        row->current = 0;
        return SQL_ROW;
    }
    if (status == PGRES_COMMAND_OK) { PQclear(res); return SQL_DONE; }
    fprintf(stderr, "PostgreSQL query error: %s\n", PQresultErrorMessage(res));
    PQclear(res);
    return SQL_ERROR;
}

void sqlEnd(sqlRow *row) {
    if (!row) return;
    if (row->owned) {
        for (int j = 0; j < row->cols; j++)
            if (row->owned[j]) PQfreemem(row->owned[j]);
        free(row->owned);
    }
    xfree(row->col);
    if (row->res) PQclear(row->res);
    memset(row, 0, sizeof(*row));
}

int sqlNextRow(sqlRow *row) {
    if (!row || !row->res) return 0;
    if (row->current >= PQntuples(row->res)) { sqlEnd(row); return 0; }
    if (row->col) {
        for (int j = 0; j < row->cols; j++)
            if (row->owned[j]) PQfreemem(row->owned[j]);
        free(row->owned); xfree(row->col); row->col = NULL; row->owned = NULL;
    }
    fillRow(row, row->res, row->current++);
    return 1;
}

int sqlInsert(PGconn *db, const char *sql, ...) {
    sds statement = sdscatprintf(sdsempty(), "%s RETURNING id", sql);
    va_list ap; va_start(ap, sql);
    sqlRow row; int rc = sqlGenericQuery(db, &row, statement, ap);
    va_end(ap); sdsfree(statement);
    if (rc != SQL_ROW || !row.col || row.col[0].type != SQL_INTEGER) { sqlEnd(&row); return 0; }
    int64_t id = row.col[0].i; sqlEnd(&row); return id;
}

int sqlQuery(PGconn *db, const char *sql, ...) {
    va_list ap; va_start(ap, sql); int rc = sqlGenericQuery(db, NULL, sql, ap); va_end(ap); return rc == SQL_DONE;
}
int sqlSelect(PGconn *db, sqlRow *row, const char *sql, ...) {
    va_list ap; va_start(ap, sql); int rc = sqlGenericQuery(db, row, sql, ap); va_end(ap); return rc;
}
int sqlSelectOneRow(PGconn *db, sqlRow *row, const char *sql, ...) {
    va_list ap; va_start(ap, sql); int rc = sqlGenericQuery(db, row, sql, ap); va_end(ap);
    if (rc == SQL_ROW) sqlNextRow(row);
    return rc;
}
int64_t sqlSelectInt(PGconn *db, const char *sql, ...) {
    sqlRow row; va_list ap; va_start(ap, sql); int rc = sqlGenericQuery(db, &row, sql, ap); va_end(ap);
    int64_t result = 0; if (rc == SQL_ROW && row.col) result = row.col[0].i; sqlEnd(&row); return result;
}

int kvSetLen(PGconn *db, const char *key, const char *value, size_t vlen, int64_t expire) {
    if (expire) expire += time(NULL);
    return sqlQuery(db, "INSERT INTO KeyValue(expire,key,value) VALUES(?i,?s,?b) "
                    "ON CONFLICT (key) DO UPDATE SET expire=EXCLUDED.expire,value=EXCLUDED.value",
                    expire, key, value, vlen);
}
int kvSet(PGconn *db, const char *key, const char *value, int64_t expire) { return kvSetLen(db, key, value, strlen(value), expire); }
sds kvGet(PGconn *db, const char *key) {
    sqlRow row; sds value = NULL;
    if (sqlSelect(db, &row, "SELECT expire,value FROM KeyValue WHERE key=?s", key) == SQL_ROW && sqlNextRow(&row)) {
        if (row.col[0].i && row.col[0].i < time(NULL)) sqlQuery(db, "DELETE FROM KeyValue WHERE key=?s", key);
        else value = sdsnewlen(row.col[1].s, row.col[1].i);
        sqlEnd(&row);
    }
    return value;
}
void kvDel(PGconn *db, const char *key) { sqlQuery(db, "DELETE FROM KeyValue WHERE key=?s", key); }
