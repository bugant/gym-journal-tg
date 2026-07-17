/* test_gymjournal.c -- hand-rolled test runner for the pure layer.
 *
 * Links ONLY to gymjournal.c + sds.c (+ pthread for sds). No curl, no
 * botlib, no sqlite. Covers the test inventory from plan section 9.
 *
 * Each test function returns the number of failures it produced; the runner
 * sums these and exits non-zero if any assertion failed. Assertions are
 * logged to stderr with the failing expression and a short context.
 */
#include "gymjournal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* --- xmalloc shim ----------------------------------------------------- *
 * sds.c (vendored verbatim) calls xmalloc/xrealloc/xfree, which botlib.c
 * normally defines. The pure test layer deliberately links no botlib, so
 * we supply trivial abort-on-failure wrappers here instead. */
void *xmalloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (p == NULL) { fprintf(stderr, "xmalloc: out of memory\n"); abort(); }
    return p;
}
void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size ? size : 1);
    if (p == NULL) { fprintf(stderr, "xrealloc: out of memory\n"); abort(); }
    return p;
}
void xfree(void *ptr) {
    free(ptr);
}

static int g_failures = 0;

/* --- Core assert macros ----------------------------------------------- */

#define ASSERT(cond)                                                        \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d  ASSERT(%s)\n",                     \
                    __FILE__, __LINE__, #cond);                             \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define ASSERT_STR(actual, expected)                                        \
    do {                                                                    \
        const char *a_ = (actual);                                          \
        const char *e_ = (expected);                                        \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                            \
            fprintf(stderr,                                                 \
                    "FAIL %s:%d  ASSERT_STR\n   expected: \"%s\"\n"         \
                    "   actual:   \"%s\"\n",                                \
                    __FILE__, __LINE__, e_, a_ ? a_ : "<NULL>");            \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define ASSERT_DBL(actual, expected, eps)                                   \
    do {                                                                    \
        double a_ = (actual);                                               \
        double e_ = (expected);                                            \
        if (fabs(a_ - e_) > (eps)) {                                       \
            fprintf(stderr,                                                 \
                    "FAIL %s:%d  ASSERT_DBL  expected %.12g"               \
                    "  actual %.12g  (eps %.4g)\n",                         \
                    __FILE__, __LINE__, e_, a_, (double)(eps));             \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/* --- Helper: expand a token list into SubsetRow[] --------------------- */

/* Walk a NULL-terminated array of tokens, expanding any sub-set tokens via
 * parseSubset into the caller-provided SubsetRow buffer. Non-sub-set tokens
 * are skipped (i.e. exercise-name words). Returns the number of rows written
 * or -1 if the buffer is too small / a token failed to parse. */
static int expandTokens(const char *const *toks, SubsetRow *out, int cap) {
    int n = 0;
    for (int i = 0; toks[i] != NULL; i++) {
        if (!isSubsetToken(toks[i])) continue;
        double w; int r, c;
        if (!parseSubset(toks[i], &w, &r, &c)) return -1;
        for (int k = 0; k < c; k++) {
            if (n >= cap) return -1;
            out[n].weight = w;
            out[n].reps   = r;
            n++;
        }
    }
    return n;
}

/* Pretty sds printer for debugging on failure: returns the same sds owned by
 * the caller (no extra allocation) but we provide it as a shim so tests can
 * log. Not used in macros. */
static void dumpSds(const sds s) {
    fprintf(stderr, "   [sds len=%zu] \"%s\"\n", sdslen(s), s);
}

/* ===================================================================== */
/* Test groups                                                           */
/* ===================================================================== */

/* --- isSubsetToken positives & negatives ------------------------------ */
static void test_isSubsetToken(void) {
    /* positives */
    ASSERT(isSubsetToken("60x10x3")  == 1);
    ASSERT(isSubsetToken("60x10")    == 1);
    ASSERT(isSubsetToken("x10")      == 1);
    ASSERT(isSubsetToken("60X10")    == 1);   /* case-insensitive separator */
    ASSERT(isSubsetToken("60.5x10") == 1);    /* decimal weight */
    ASSERT(isSubsetToken(".5x10")    == 1);   /* leading-dot weight */
    ASSERT(isSubsetToken("60.x10")   == 0);   /* trailing dot is not valid */

    /* negatives */
    ASSERT(isSubsetToken("60")         == 0);
    ASSERT(isSubsetToken("x")          == 0);
    ASSERT(isSubsetToken("10x")        == 0);
    ASSERT(isSubsetToken("abc")        == 0);
    ASSERT(isSubsetToken("60x10x")    == 0);
    ASSERT(isSubsetToken("60x10x3x2") == 0);
    ASSERT(isSubsetToken("")           == 0);
    ASSERT(isSubsetToken("60x10x0")   == 1);   /* shape-valid: 0 count? no,
                                                  count segment present with
                                                  digit '0'; shape is legal */
    ASSERT(isSubsetToken("60x0")       == 1);  /* shape-valid reps=0 */
    ASSERT(isSubsetToken("6 0x10")    == 0);   /* space inside */
    ASSERT(isSubsetToken("x10x")        == 0);
    ASSERT(isSubsetToken("x10x3x2")    == 0);
}

/* --- parseSubset ------------------------------------------------------ */
static void test_parseSubset(void) {
    double w; int r, c;

    ASSERT(parseSubset("60x10x3", &w, &r, &c) == 1);
    ASSERT_DBL(w, 60.0, 1e-9); ASSERT(r == 10); ASSERT(c == 3);

    ASSERT(parseSubset("60x10", &w, &r, &c) == 1);
    ASSERT_DBL(w, 60.0, 1e-9); ASSERT(r == 10); ASSERT(c == 1);

    ASSERT(parseSubset("x10", &w, &r, &c) == 1);
    ASSERT_DBL(w, 0.0, 1e-9);  ASSERT(r == 10); ASSERT(c == 1);

    ASSERT(parseSubset("60X10", &w, &r, &c) == 1);
    ASSERT_DBL(w, 60.0, 1e-9); ASSERT(r == 10); ASSERT(c == 1);

    ASSERT(parseSubset("60.5x10", &w, &r, &c) == 1);
    ASSERT_DBL(w, 60.5, 1e-9); ASSERT(r == 10); ASSERT(c == 1);

    ASSERT(parseSubset(".5x10", &w, &r, &c) == 1);
    ASSERT_DBL(w, 0.5, 1e-9);  ASSERT(r == 10); ASSERT(c == 1);

    /* Negative shapes must return 0. */
    ASSERT(parseSubset("60", &w, &r, &c)         == 0);
    ASSERT(parseSubset("x", &w, &r, &c)          == 0);
    ASSERT(parseSubset("10x", &w, &r, &c)        == 0);
    ASSERT(parseSubset("abc", &w, &r, &c)        == 0);
    ASSERT(parseSubset("60x10x", &w, &r, &c)     == 0);
    ASSERT(parseSubset("60x10x3x2", &w, &r, &c)  == 0);

    /* Absurd-input count clamping. */
    ASSERT(parseSubset("60x10x99999999", &w, &r, &c) == 1);
    ASSERT(c >= 1 && c <= 1000);
    ASSERT(c == 1000);

    /* count segment with explicit 1 */
    ASSERT(parseSubset("60x10x1", &w, &r, &c) == 1);
    ASSERT_DBL(w, 60.0, 1e-9); ASSERT(r == 10); ASSERT(c == 1);
}

/* --- extractName ------------------------------------------------------ */
static void test_extractName(void) {
    char buf[256];

    /* single-word name, range covers exactly one token */
    { const char *const a[] = {"bench","60x10",NULL};
      ASSERT(extractName(a, 0, 1, buf, sizeof(buf)) == 1);
      ASSERT_STR(buf, "bench");
    }

    /* multi-word name "Bench Press" */
    { const char *const a[] = {"Bench","Press","60x10",NULL};
      ASSERT(extractName(a, 0, 2, buf, sizeof(buf)) == 1);
      ASSERT_STR(buf, "Bench Press");
    }

    /* no sub-set token in the range: caller passes the whole range, we just
     * join everything. */
    { const char *const a[] = {"Bench","Press",NULL};
      ASSERT(extractName(a, 0, 2, buf, sizeof(buf)) == 1);
      ASSERT_STR(buf, "Bench Press");
    }

    /* name stops before the first sub-set token (a later subset token must
     * not be folded into the name). Simulate cmdAdd: argv = /add bench squat
     * 60x10 55x8 -> first subset at index 3 -> name = [1,3) = "bench squat" */
    { const char *const a[] = {"bench","squat","60x10","55x8",NULL};
      ASSERT(extractName(a, 0, 2, buf, sizeof(buf)) == 1);
      ASSERT_STR(buf, "bench squat");
    }

    /* empty range returns 0 and writes empty buffer */
    { const char *const a[] = {"bench",NULL};
      ASSERT(extractName(a, 0, 0, buf, sizeof(buf)) == 0);
      ASSERT_STR(buf, "");
    }

    /* buffer truncation is safe (no overflow) */
    { const char *const a[] = {"verylongword","stilllong",NULL};
      char small[5];
      ASSERT(extractName(a, 0, 2, small, sizeof(small)) == 1);
      ASSERT(small[sizeof(small)-1] == '\0');
    }
}

/* --- groupVolume ----------------------------------------------------- */
static void test_groupVolume(void) {
    { SubsetRow rows[] = {{60.0,10},{55.0,8},{50.0,6}};
      /* 600 + 440 + 300 = 1340 */
      ASSERT_DBL(groupVolume(rows, 3), 1340.0, 1e-9);
    }
    /* bodyweight rows contribute 0 */
    { SubsetRow rows[] = {{0.0,10},{0.0,10},{0.0,10}};
      ASSERT_DBL(groupVolume(rows, 3), 0.0, 1e-9);
    }
    /* mixed bw + weighted */
    { SubsetRow rows[] = {{0.0,10},{60.0,5}};
      ASSERT_DBL(groupVolume(rows, 2), 300.0, 1e-9);
    }
    /* uniform weighted */
    { SubsetRow rows[] = {{60.0,10},{60.0,10},{60.0,10}};
      ASSERT_DBL(groupVolume(rows, 3), 1800.0, 1e-9);
    }
    /* decimal volume */
    { SubsetRow rows[] = {{60.5,10}};
      ASSERT_DBL(groupVolume(rows, 1), 605.0, 1e-9);
    }
    { SubsetRow rows[] = {{60.5,11}};
      ASSERT_DBL(groupVolume(rows, 1), 665.5, 1e-9);
    }
    /* NULL/empty -> 0 */
    ASSERT_DBL(groupVolume(NULL, 0), 0.0, 1e-9);

    /* large-sum stability: 100000 rows of (1e6 kg x 1e6 reps) -> 1e17 total.
     * Doubles have 52 bits of mantissa (~4.5e15 precise), so this exercises
     * accumulation but must stay finite/deterministic. */
    {
        SubsetRow big[100000];
        for (int i = 0; i < 100000; i++) { big[i].weight = 1e6; big[i].reps = 1000000; }
        double v = groupVolume(big, 100000);
        ASSERT(!isnan(v) && !isinf(v));
        ASSERT_DBL(v, 1e17, 1e15);   /* within 1% tolerance given rounding */
    }
}

/* --- summarizeSession ------------------------------------------------- */
static void test_summarizeSession(void) {
    /* 3 groups: distinct group count tracks ng (with non-empty groups) */
    { SubsetRow g1[] = {{60.0,10},{55.0,8},{50.0,6}};
      SubsetRow g2[] = {{100.0,5},{100.0,5},{100.0,5},{100.0,5},{100.0,5}};
      SubsetRow g3[] = {{0.0,10},{0.0,10},{0.0,10}};
      Group groups[] = {
        {3, g1}, {5, g2}, {3, g3}
      };
      SessionSummary s = summarizeSession(groups, 3);
      ASSERT(s.exercises == 3);
      ASSERT(s.sets == 3 + 5 + 3);
      /* vol = 1340 (bench) + 2500 (squat) + 0 (pull ups) = 3840. */
      ASSERT_DBL(s.volume, 3840.0, 1e-9);
    }

    /* skip empty groups: an empty group does not count as an exercise */
    { SubsetRow g1[] = {{60.0,10}};
      SubsetRow g2[] = {{0.0,10},{0.0,10}};
      Group groups[] = { {1, g1}, {0, NULL}, {2, g2} };
      SessionSummary s = summarizeSession(groups, 3);
      ASSERT(s.exercises == 2);
      ASSERT(s.sets == 3);
      ASSERT_DBL(s.volume, 600.0, 1e-9);
    }

    /* NULL/empty -> zeros */
    { SessionSummary s = summarizeSession(NULL, 0);
      ASSERT(s.exercises == 0); ASSERT(s.sets == 0);
      ASSERT_DBL(s.volume, 0.0, 1e-9);
    }
}

/* --- collapseGroup exact-string asserts ------------------------------- */
static void test_collapseGroup(void) {
    sds out;

    /* distinct `+` form */
    { SubsetRow rows[] = {{60.0,10},{55.0,8},{50.0,6}};
      out = collapseGroup(rows, 3);
      /* 600 + 440 + 300 = 1340 */
      ASSERT_STR(out, "60x10 + 55x8 + 50x6  [vol 1340]");
      sdsfree(out);
    }

    /* uniform `@` form (all rows identical, n>=2) */
    { SubsetRow rows[] = {{60.0,10},{60.0,10},{60.0,10}};
      out = collapseGroup(rows, 3);
      ASSERT_STR(out, "3x10 @ 60kg  [vol 1800]");
      sdsfree(out);
    }

    /* uniform bodyweight `[bw]` form */
    { SubsetRow rows[] = {{0.0,10},{0.0,10},{0.0,10}};
      out = collapseGroup(rows, 3);
      ASSERT_STR(out, "3x10  [bw]");
      sdsfree(out);
    }

    /* near-uniform-but-not-identical (same reps, different weights) must
     * fall back to the literal `+`-joined form. */
    { SubsetRow rows[] = {{60.0,10},{55.0,10}};
      out = collapseGroup(rows, 2);
      ASSERT_STR(out, "60x10 + 55x10  [vol 1150]");
      sdsfree(out);
    }

    /* mixed bodyweight + weighted in one group -> numeric [vol N], no [bw],
     * literal `+` form. */
    { SubsetRow rows[] = {{0.0,10},{60.0,5}};
      out = collapseGroup(rows, 2);
      ASSERT_STR(out, "x10 + 60x5  [vol 300]");
      sdsfree(out);
    }

    /* n=1 weighted -> literal form (uniform @ form requires n>=2) */
    { SubsetRow rows[] = {{60.0,10}};
      out = collapseGroup(rows, 1);
      ASSERT_STR(out, "60x10  [vol 600]");
      sdsfree(out);
    }

    /* n=1 bodyweight -> literal "xR" + [bw] */
    { SubsetRow rows[] = {{0.0,10}};
      out = collapseGroup(rows, 1);
      ASSERT_STR(out, "x10  [bw]");
      sdsfree(out);
    }

    /* decimal weight uniform form */
    { SubsetRow rows[] = {{60.5,10},{60.5,10},{60.5,10}};
      out = collapseGroup(rows, 3);
      ASSERT_STR(out, "3x10 @ 60.5kg  [vol 1815]");
      sdsfree(out);
    }

    /* decimal weight n=1, integer volume */
    { SubsetRow rows[] = {{60.5,10}};
      out = collapseGroup(rows, 1);
      ASSERT_STR(out, "60.5x10  [vol 605]");
      sdsfree(out);
    }

    /* decimal weight n=1, fractional volume */
    { SubsetRow rows[] = {{60.5,11}};
      out = collapseGroup(rows, 1);
      ASSERT_STR(out, "60.5x11  [vol 665.5]");
      sdsfree(out);
    }

    /* NULL/0 -> empty sds (still freeable) */
    { out = collapseGroup(NULL, 0);
      ASSERT(sdslen(out) == 0);
      sdsfree(out);
    }
}

/* --- end-to-ish expansion vectors (parser + collapse) ----------------- */
static void test_expansion(void) {
    SubsetRow rows[64];
    int n;
    sds out;

    /* "bench 60x10x3 55x8 50x6" -> 5 SubsetRows -> literal `+` form */
    { const char *const toks[] = {"bench","60x10x3","55x8","50x6",NULL};
      n = expandTokens(toks, rows, 64);
      ASSERT(n == 5);
      ASSERT_DBL(rows[0].weight, 60.0, 1e-9); ASSERT(rows[0].reps == 10);
      ASSERT_DBL(rows[1].weight, 60.0, 1e-9); ASSERT(rows[1].reps == 10);
      ASSERT_DBL(rows[2].weight, 60.0, 1e-9); ASSERT(rows[2].reps == 10);
      ASSERT_DBL(rows[3].weight, 55.0, 1e-9); ASSERT(rows[3].reps == 8);
      ASSERT_DBL(rows[4].weight, 50.0, 1e-9); ASSERT(rows[4].reps == 6);
      out = collapseGroup(rows, 5);
      /* vol = 3*600 + 440 + 300 = 2540 */
      ASSERT_STR(out, "60x10 + 60x10 + 60x10 + 55x8 + 50x6  [vol 2540]");
      sdsfree(out);
    }

    /* "squat 100x5x5" -> uniform `@` form */
    { const char *const toks[] = {"squat","100x5x5",NULL};
      n = expandTokens(toks, rows, 64);
      ASSERT(n == 5);
      out = collapseGroup(rows, 5);
      ASSERT_STR(out, "5x5 @ 100kg  [vol 2500]");
      sdsfree(out);
    }

    /* "pull ups x10 x10 x10" -> bodyweight `[bw]` */
    { const char *const toks[] = {"pull","ups","x10","x10","x10",NULL};
      n = expandTokens(toks, rows, 64);
      ASSERT(n == 3);
      out = collapseGroup(rows, 3);
      ASSERT_STR(out, "3x10  [bw]");
      sdsfree(out);
    }
}

/* ===================================================================== */
/* Runner                                                                */
/* ===================================================================== */

typedef void (*test_fn)(void);

static const struct {
    const char *name;
    test_fn     fn;
} tests[] = {
    { "isSubsetToken",    test_isSubsetToken    },
    { "parseSubset",      test_parseSubset      },
    { "extractName",      test_extractName      },
    { "groupVolume",      test_groupVolume      },
    { "summarizeSession", test_summarizeSession },
    { "collapseGroup",    test_collapseGroup    },
    { "expansion",        test_expansion        },
    { NULL, NULL }
};

int main(int argc, char **argv) {
    (void)dumpSds;             /* keep helper referenced */
    int verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    int total = 0;
    for (int i = 0; tests[i].name != NULL; i++) {
        int before = g_failures;
        tests[i].fn();
        int failed = g_failures - before;
        total++;
        if (verbose || failed) {
            printf("%-20s %s\n", tests[i].name,
                   failed ? "FAIL" : "ok");
        }
    }
    printf("ran %d test groups, %d assertion failure(s)\n",
           total, g_failures);
    return g_failures == 0 ? 0 : 1;
}