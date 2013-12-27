#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <json/json.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/types.h>
#include <math.h>
#include <limits.h>
#include <uuid/uuid.h>
#include <assert.h>

#include "util.h"
#include "log.h"
#include "tstat.h"


void tstat_push (tstat_t *ts, double x)
{
    if (ts->min == 0 || x < ts->min)
        ts->min = x;
    if (ts->max == 0 || x > ts->max)
        ts->max = x;
/* running variance
 * ref Knuth TAOCP vol 2, 3rd edition, page 232
 * and http://www.johndcook.com/standard_deviation.html
 */
    if (++ts->n == 1) {
        ts->M = ts->newM = x;
        ts->S = 0;
    } else {
        ts->newM = ts->M + (x - ts->M)/ts->n;
        ts->newS = ts->S + (x - ts->M)*(x - ts->newM);

        ts->M = ts->newM;
        ts->S = ts->newS;
    }
}
double tstat_mean (tstat_t *ts)
{
    return (ts->n > 0) ? ts->newM : 0;
}
double tstat_min (tstat_t *ts)
{
    return ts->min;
}
double tstat_max (tstat_t *ts)
{
    return ts->max;
}
double tstat_variance (tstat_t *ts)
{
    return (ts->n > 1) ? ts->newS/(ts->n - 1) : 0;
}
double tstat_stddev (tstat_t *ts)
{
    return sqrt (tstat_variance (ts));
}
int tstat_count (tstat_t *ts)
{
    return ts->n;
}

void util_json_object_add_tstat (json_object *o, const char *name,
                                 tstat_t *ts, double scale)
{
    json_object *to = util_json_object_new_object ();

    util_json_object_add_int (to, "count", tstat_count (ts));
    util_json_object_add_double (to, "min", tstat_min (ts)*scale);
    util_json_object_add_double (to, "mean", tstat_mean (ts)*scale);
    util_json_object_add_double (to, "stddev", tstat_stddev (ts)*scale);
    util_json_object_add_double (to, "max", tstat_max (ts)*scale);

    json_object_object_add (o, name, to);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
