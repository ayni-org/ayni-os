/* Bounded civil calendar values, Since 0.41. No process-global timezone state. */
#ifndef AYNI_CALENDAR_H
#define AYNI_CALENDAR_H
#include <stdint.h>

/* Maximum POSIX TZ bytes, without a terminating NUL. Since 0.41. */
#define AYNI_CALENDAR_TZ_MAX_BYTES 128u
/* Supported Gregorian local/UTC seconds: 1970-01-01 through 9999-12-31. Since 0.41. */
#define AYNI_CALENDAR_SECONDS_MAX INT64_C(253402300799)
/* Explicit absence of a scheduled civil/UTC occurrence. Since 0.41. */
#define AYNI_CALENDAR_TIME_NONE INT64_C(-1)
typedef uint32_t ayni_calendar_kind_t;
/* One occurrence at first_local_s. Since 0.41. */
#define AYNI_CALENDAR_ONCE ((ayni_calendar_kind_t)1)
/* Same local time every civil day, starting at first_local_s. Since 0.41. */
#define AYNI_CALENDAR_DAILY ((ayni_calendar_kind_t)2)
/* Same local weekday/time every week, starting at first_local_s. Since 0.41. */
#define AYNI_CALENDAR_WEEKLY ((ayni_calendar_kind_t)3)

/* Copied immutable rule. Local seconds encode Gregorian civil fields on the
 * Unix epoch grid, before applying timezone; they are not a UTC timestamp.
 * Changing timezone rules creates a new Goal. Since 0.41. */
typedef struct ayni_calendar_rule_v1 {
    uint32_t struct_size;      /* Exact sizeof this minor's layout. */
    ayni_calendar_kind_t kind; /* ONCE, DAILY or WEEKLY. */
    uint32_t rule_version;     /* Exactly 1: this civil recurrence interpretation. */
    uint32_t timezone_size;    /* 1..TZ_MAX_BYTES; no trailing NUL in the value. */
    int64_t first_local_s;     /* Inclusive first planned civil time; 0..SECONDS_MAX. */
    char timezone[AYNI_CALENDAR_TZ_MAX_BYTES]; /* POSIX TZ; explicit DST transition rules. */
} ayni_calendar_rule_v1;

typedef uint32_t ayni_utc_trust_t;
/* Absolute time is unavailable; calendar work waits. Since 0.41. */
#define AYNI_UTC_UNAVAILABLE ((ayni_utc_trust_t)0)
/* A wall-clock value exists but has no current synchronization evidence. Since 0.41. */
#define AYNI_UTC_UNTRUSTED ((ayni_utc_trust_t)1)
/* Host currently attests synchronization; civil triggers may advance. Since 0.41. */
#define AYNI_UTC_TRUSTED ((ayni_utc_trust_t)2)

/* One nonblocking Host reading. No lifetime beyond the synchronous call.
 * Unsupported ranges/trust values are treated as unavailable. Since 0.41. */
typedef struct ayni_utc_sample_v1 {
    uint32_t struct_size;   /* Host writes exact sizeof. */
    ayni_utc_trust_t trust; /* UNAVAILABLE, UNTRUSTED or TRUSTED. */
    int64_t utc_s;          /* Whole Unix seconds; meaningful only when time exists. */
} ayni_utc_sample_v1;

/* Copied Goal calendar diagnostics; all timestamps are planned instants.
 * No retained sample or media is exposed. Since 0.41. */
typedef struct ayni_calendar_info_v1 {
    uint32_t struct_size;       /* Caller supplies exact sizeof. */
    uint32_t waiting_for_time;  /* 1 when the current Host sample is not trusted. */
    uint64_t goal_id;           /* Retained stable Goal identity, never zero. */
    ayni_calendar_rule_v1 rule; /* Original copied civil rule. */
    int64_t last_local_s;       /* Last consumed civil key; -1 before the first. */
    int64_t last_utc_s;         /* Last consumed planned UTC; -1 before the first. */
    int64_t next_utc_s;         /* Next unconsumed planned instant; -1 when exhausted. */
    uint32_t taken;             /* All consumed/coalesced occurrences, including explicit skips. */
    uint32_t reserved;          /* Written zero. */
    uint64_t transition_id;     /* Row's last applied change, positive. */
    uint64_t commit_seq;        /* Row's last applied change; compare with durable_seq. */
} ayni_calendar_info_v1;
#endif
