#ifndef PG_WRAP_H
#define PG_WRAP_H

#include <libpq-fe.h>
#include <stdint.h>
#include <stdarg.h>
#include "sds.h"

#define SQL_MAX_SPEC 32
#define SQL_ROW 1
#define SQL_DONE 2
#define SQL_ERROR 0
#define SQL_INTEGER 1
#define SQL_FLOAT 2
#define SQL_TEXT 3
#define SQL_BLOB 4
#define SQL_NULL 5

typedef struct sqlCol {
    int type;
    int64_t i;
    const char *s;
    double d;
} sqlCol;

typedef struct sqlRow {
    PGresult *res;
    int current;
    int cols;
    sqlCol *col;
    void **owned;
} sqlRow;

void pgDbLock(void);
void pgDbUnlock(void);
int sqlGenericQuery(PGconn *dbhandle, sqlRow *row, const char *sql, va_list ap);
void sqlEnd(sqlRow *row);
int sqlNextRow(sqlRow *row);
int sqlInsert(PGconn *dbhandle, const char *sql, ...);
int sqlQuery(PGconn *dbhandle, const char *sql, ...);
int sqlSelect(PGconn *dbhandle, sqlRow *row, const char *sql, ...);
int sqlSelectOneRow(PGconn *dbhandle, sqlRow *row, const char *sql, ...);
int64_t sqlSelectInt(PGconn *dbhandle, const char *sql, ...);
int kvSetLen(PGconn *dbhandle, const char *key, const char *value, size_t vlen, int64_t expire);
int kvSet(PGconn *dbhandle, const char *key, const char *value, int64_t expire);
sds kvGet(PGconn *dbhandle, const char *key);
void kvDel(PGconn *dbhandle, const char *key);

#endif
