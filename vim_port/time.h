/*
 * time.h - Time functions stub for vim on seolsem
 */

#ifndef _TIME_H_SEOLSEM
#define _TIME_H_SEOLSEM

typedef long time_t;
typedef long clock_t;
typedef unsigned short size_t;

#define CLOCKS_PER_SEC 18  /* DOS timer ticks per second */

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

/* Stubs */
static time_t time(time_t *tloc) {
    time_t t = 0;
    if (tloc) *tloc = t;
    return t;
}

static struct tm *localtime(const time_t *timep) {
    static struct tm tm;
    (void)timep;
    tm.tm_sec = 0;
    tm.tm_min = 0;
    tm.tm_hour = 0;
    tm.tm_mday = 1;
    tm.tm_mon = 0;
    tm.tm_year = 126; /* 2026 - 1900 */
    tm.tm_wday = 0;
    tm.tm_yday = 0;
    tm.tm_isdst = 0;
    return &tm;
}

static struct tm *gmtime(const time_t *timep) {
    return localtime(timep);
}

static char *ctime(const time_t *timep) {
    static char buf[26] = "Mon Jan  1 00:00:00 2026\n";
    (void)timep;
    return buf;
}

static size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    (void)format;
    (void)tm;
    if (max > 0) s[0] = '\0';
    return 0;
}

static clock_t clock(void) {
    return 0;
}

static double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

#endif /* _TIME_H_SEOLSEM */
