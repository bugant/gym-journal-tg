/* gymjournal.h -- pure-logic layer for the gym journal Telegram bot.
 *
 * No botlib, no sqlite3. Only depends on sds (for collapsed-line output
 * buffers) and the standard C library. Owns sub-set token parsing, exercise
 * name extraction, collapsed-line formatting and volume/summary math.
 */
#ifndef GYMJOURNAL_H
#define GYMJOURNAL_H

#include <stddef.h>
#include "sds.h"

/* A single real sub-set: one row of the `sets` table after repeat-suffix
 * expansion. weight is in kg; 0 means bodyweight. */
typedef struct {
    double weight;
    int    reps;
} SubsetRow;

/* A group of SubsetRow's that share an exercise name (and a group_id). */
typedef struct {
    int               n;
    const SubsetRow  *rows;
} Group;

/* Aggregate totals for one session. */
typedef struct {
    int    exercises;   /* distinct groups */
    int    sets;         /* total rows */
    double volume;       /* sum(weight*reps); bw rows contribute 0 */
} SessionSummary;

/* --- Sub-set token parsing (see plan section 4) ----------------------- */

/* Return 1 if `s` matches a sub-set-shaped token (WxR, WxRxN, xR), else 0.
 * Regex-free: manual character walk. Case-insensitive on the 'x' separator. */
int isSubsetToken(const char *s);

/* Parse a sub-set token into its components. On success returns 1 and stores
 * weight (kg, 0 for bodyweight), reps (>=1) and count (>=1, the Nx repeat
 * suffix; defaults to 1 when absent). On failure returns 0 and leaves the
 * outputs untouched. Absurd-count inputs are clamped to a safe maximum. */
int parseSubset(const char *tok, double *weight, int *reps, int *count);

/* --- Exercise-name extraction ---------------------------------------- */

/* Space-join argv[start..end) (exclusive end) into `out`, returning 1 on
 * success (at least one word joined) or 0 if the range is empty. `out` is
 * always NUL-terminated; outsz is the buffer capacity including the NUL. */
int extractName(const char *const *argv, int start, int end,
                char *out, size_t outsz);

/* --- Collapsed formatting for one group (see plan section 6) --------- */

/* Build the collapsed one-line representation of the rows of a single group.
 * Caller owns the returned sds and must sdsfree() it. Examples:
 *   "60x10 + 55x8 + 50x6  [vol 1340]"   distinct sub-sets
 *   "3x10 @ 60kg  [vol 1800]"          fully-uniform group
 *   "3x10  [bw]"                       bodyweight group
 * Note: the exercise name is *not* prepended here; the bot layer prefixes it.
 */
sds collapseGroup(const SubsetRow *rows, int n);

/* --- Volume math ----------------------------------------------------- */

/* Sum of weight*reps across rows; bodyweight (weight==0) rows contribute 0. */
double groupVolume(const SubsetRow *rows, int n);

/* Aggregate `ng` groups into a SessionSummary (distinct-group count via ng,
 * sets = total rows across all groups, volume = sum of group volumes). */
SessionSummary summarizeSession(const Group *groups, int ng);

#endif /* GYMJOURNAL_H */