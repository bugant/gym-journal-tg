/* gymjournal.c -- pure-logic implementation for the gym journal bot.
 *
 * No botlib, no sqlite3. Only sds (for output buffers) and the standard
 * C library. See gymjournal.h for the API contract.
 */
#include "gymjournal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Maximum repeat count we will accept from an Nx suffix. Inputs above this
 * are clamped rather than rejected, so a runaway parse can't OOM the bot. */
#define MAX_SUBSET_COUNT 1000

/* ----------------------------------------------------------------------
 * Sub-set token parsing
 * ---------------------------------------------------------------------- */

/* Return 1 iff `s` matches ^(\d*\.?\d+)?[xX](\d+)([xX](\d+))?$  (regex-free).
 * Walks the string once. */
int isSubsetToken(const char *s) {
    if (s == NULL || *s == '\0') return 0;

    const char *p = s;
    int digits_before_dot = 0;
    int digits_after_dot  = 0;
    int saw_dot           = 0;

    /* Optional weight part: (\d*\.?\d+) — zero or more digits, optional '.',
     * then one or more digits. If present, it must contain at least one digit
     * (so "." alone is not accepted). The weight part may be entirely absent
     * (bodyweight form: "xR"). */
    while (*p != '\0' && *p != 'x' && *p != 'X') {
        if (*p == '.') {
            if (saw_dot) return 0;        /* two dots */
            saw_dot = 1;
        } else if (isdigit((unsigned char)*p)) {
            if (saw_dot) digits_after_dot++;
            else         digits_before_dot++;
        } else {
            return 0;                     /* non-numeric junk in weight */
        }
        p++;
    }
    if (saw_dot && digits_after_dot == 0) return 0;       /* trailing '.' */
    /* If anything was consumed as weight, it must have digits somewhere. */
    if (p != s && (digits_before_dot + digits_after_dot) == 0) return 0;

    /* Mandatory x/X separator. */
    if (*p != 'x' && *p != 'X') return 0;
    p++;

    /* Mandatory reps: one or more digits. */
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;

    /* Optional count segment: [xX](\d+) */
    if (*p != '\0') {
        if (*p != 'x' && *p != 'X') return 0;
        p++;
        if (!isdigit((unsigned char)*p)) return 0;       /* e.g. "60x10x" */
        while (isdigit((unsigned char)*p)) p++;
        if (*p != '\0') return 0;                        /* e.g. "60x10x3x2" */
    }
    return 1;
}

/* Parse a sub-set token into weight/reps/count. On a malformed token
 * returns 0 and leaves outputs untouched. On success returns 1 with:
 *   *weight = parsed kg (0 for bodyweight form),
 *   *reps   >= 1,
 *   *count  >= 1 (clamped to MAX_SUBSET_COUNT — absurd但仍视为有限).
 */
int parseSubset(const char *tok, double *weight, int *reps, int *count) {
    if (!isSubsetToken(tok)) return 0;
    if (weight == NULL || reps == NULL || count == NULL) return 0;

    const char *p = tok;
    const char *weight_start = NULL;
    const char *weight_end   = NULL;
    int   reps_val   = 0;
    int   count_val  = 1;

    /* Locate the weight span (up to the first x/X). */
    weight_start = p;
    while (*p != '\0' && *p != 'x' && *p != 'X') p++;
    weight_end = p;

    /* Skip the separator. (guaranteed by isSubsetToken) */
    p++;

    /* Parse reps. */
    while (isdigit((unsigned char)*p)) {
        reps_val = reps_val * 10 + (*p - '0');
        p++;
    }
    if (reps_val < 1) return 0;   /* defensive; isSubsetToken guarantees >=1 */

    /* Optional count segment. */
    if (*p != '\0') {
        p++;   /* skip x/X */
        count_val = 0;
        while (isdigit((unsigned char)*p)) {
            count_val = count_val * 10 + (*p - '0');
            p++;
        }
    }

    /* Weight. Empty span = bodyweight = 0. */
    if (weight_end == weight_start) {
        *weight = 0.0;
    } else {
        /* strtod handles integer + decimal forms. */
        char buf[64];
        size_t span = (size_t)(weight_end - weight_start);
        if (span >= sizeof(buf)) span = sizeof(buf) - 1;
        memcpy(buf, weight_start, span);
        buf[span] = '\0';
        char *endp = NULL;
        double w = strtod(buf, &endp);
        if (endp == buf || *endp != '\0') return 0;
        if (w < 0.0) return 0;                  /* no negative weights */
        *weight = w;
    }

    if (count_val < 1) count_val = 1;
    if (count_val > MAX_SUBSET_COUNT) count_val = MAX_SUBSET_COUNT;

    *reps  = reps_val;
    *count = count_val;
    return 1;
}

/* ----------------------------------------------------------------------
 * Exercise-name extraction
 * ---------------------------------------------------------------------- */

int extractName(const char *const *argv, int start, int end,
                char *out, size_t outsz) {
    if (out == NULL || outsz == 0) return 0;
    if (argv == NULL || start < 0 || end < start) { out[0] = '\0'; return 0; }

    size_t cap  = outsz - 1;   /* reserve 1 byte for the NUL terminator */
    size_t used = 0;
    int    wrote_any = 0;
    int    truncated = 0;

    for (int i = start; i < end && !truncated; i++) {
        if (argv[i] == NULL) continue;
        const char *word = argv[i];
        size_t wlen = strlen(word);
        size_t sep  = wrote_any ? 1 : 0;
        if (used + sep >= cap) break;       /* no room for sep + a real char */
        if (sep) out[used++] = ' ';
        size_t room = cap - used;
        size_t copy = (wlen < room) ? wlen : room;
        memcpy(out + used, word, copy);
        used += copy;
        wrote_any = 1;
        if (copy < wlen) truncated = 1;     /* word didn't fully fit */
    }
    out[used] = '\0';
    return wrote_any ? 1 : 0;
}

/* ----------------------------------------------------------------------
 * Volume math
 * ---------------------------------------------------------------------- */

double groupVolume(const SubsetRow *rows, int n) {
    double vol = 0.0;
    if (rows == NULL || n <= 0) return 0.0;
    for (int i = 0; i < n; i++) {
        if (rows[i].weight > 0.0)
            vol += rows[i].weight * (double)rows[i].reps;
    }
    return vol;
}

SessionSummary summarizeSession(const Group *groups, int ng) {
    SessionSummary s;
    s.exercises = 0;
    s.sets      = 0;
    s.volume    = 0.0;
    if (groups == NULL || ng <= 0) return s;
    for (int i = 0; i < ng; i++) {
        if (groups[i].n <= 0) continue;
        s.exercises++;
        s.sets += groups[i].n;
        s.volume += groupVolume(groups[i].rows, groups[i].n);
    }
    return s;
}

/* ----------------------------------------------------------------------
 * Collapsed formatting
 * ---------------------------------------------------------------------- */

/* Append a weight to `s` in kg, stripping trailing zeros / decimal point so
 * integer weights render as "60" rather than "60.000000". */
static sds appendWeight(sds s, double w) {
    if (w == floor(w)) {
        return sdscatprintf(s, "%.0f", w);
    }
    /* Print with enough precision, then trim. Use %g with 6 sig figs. */
    sds tmp = sdscatprintf(sdsempty(), "%.6g", w);
    s = sdscatsds(s, tmp);
    sdsfree(tmp);
    return s;
}

/* Append a volume: integer-shaped volumes render without a decimal point. */
static sds appendVolume(sds s, double v) {
    if (v == floor(v)) {
        return sdscatprintf(s, "%.0f", v);
    }
    sds tmp = sdscatprintf(sdsempty(), "%.6g", v);
    s = sdscatsds(s, tmp);
    sdsfree(tmp);
    return s;
}

/* Format one row's weightxRreps (or "xR" for bodyweight) for the literal
 * `+`-joined form. */
static sds appendRowLiteral(sds s, const SubsetRow *r) {
    if (r->weight == 0.0) {
        return sdscatprintf(s, "x%d", r->reps);
    }
    s = appendWeight(s, r->weight);
    return sdscatprintf(s, "x%d", r->reps);
}

/* Is every row in the group identical (same weight AND same reps)? */
static int allUniform(const SubsetRow *rows, int n) {
    if (n < 1) return 0;
    for (int i = 1; i < n; i++) {
        if (rows[i].weight != rows[0].weight || rows[i].reps != rows[0].reps)
            return 0;
    }
    return 1;
}

/* Is every row bodyweight (weight == 0)? */
static int allBodyweight(const SubsetRow *rows, int n) {
    if (n < 1) return 0;
    for (int i = 0; i < n; i++)
        if (rows[i].weight != 0.0) return 0;
    return 1;
}

/* Build the collapsed one-line representation of a single group's rows.
 * The exercise name is intentionally *not* prepended; the caller prefixes it. */
sds collapseGroup(const SubsetRow *rows, int n) {
    sds out = sdsempty();
    if (rows == NULL || n <= 0) return out;

    int uniform = allUniform(rows, n);
    int bw      = allBodyweight(rows, n);

    /* --- Sub-sets layout (left of the bracket) --- */
    if (uniform && n >= 2) {
        /* <count>x<reps> [@ <weight>kg]   (weight omitted when bodyweight) */
        out = sdscatprintf(out, "%dx%d", n, rows[0].reps);
        if (!bw) {
            out = sdscat(out, " @ ");
            out = appendWeight(out, rows[0].weight);
            out = sdscat(out, "kg");
        }
    } else {
        /* Literal `+`-joined form, one entry per row. */
        for (int i = 0; i < n; i++) {
            if (i > 0) out = sdscat(out, " + ");
            out = appendRowLiteral(out, &rows[i]);
        }
    }

    /* --- Bracket (right) --- */
    out = sdscat(out, "  [");
    if (bw) {
        out = sdscat(out, "bw");
    } else {
        out = sdscat(out, "vol ");
        double vol = groupVolume(rows, n);
        out = appendVolume(out, vol);
    }
    out = sdscat(out, "]");
    return out;
}