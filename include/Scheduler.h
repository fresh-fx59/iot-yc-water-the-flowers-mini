#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <ctime>

namespace Scheduler {

enum class Decision { WAIT, FIRE, SKIP_RECOMPUTE };

inline time_t alignToHM(time_t day_anchor, int hour, int minute) {
    // day_anchor anywhere in a UTC day -> return that day at hour:minute UTC
    std::tm tm_utc;
    gmtime_r(&day_anchor, &tm_utc);
    tm_utc.tm_hour = hour;
    tm_utc.tm_min  = minute;
    tm_utc.tm_sec  = 0;
    return timegm(&tm_utc);
}

// Defensive sanity floor for last_run_unix. If the persisted last_run is
// before this (e.g. the device once booted with the RTC at epoch and wrote a
// 1970-based timestamp into /state.json) treat it as "no prior run yet" — the
// alternative is computing next_run = last_run + interval, which leaves
// next_run permanently in the past and shouldFireNow stuck in SKIP_RECOMPUTE.
// 1700000000 = 2023-11-14 22:13 UTC; predates this project so any real run
// will be after it.
static const time_t LAST_RUN_UNIX_SANITY_FLOOR = 1700000000;

inline time_t computeNextRun(time_t now_unix, time_t last_run_unix, int interval_days, int hour, int minute) {
    if (last_run_unix < LAST_RUN_UNIX_SANITY_FLOOR) {
        time_t candidate = alignToHM(now_unix, hour, minute);
        if (candidate <= now_unix) candidate += 86400;
        return candidate;
    }
    time_t base = alignToHM(last_run_unix, hour, minute);
    return base + (time_t)interval_days * 86400;
}

inline Decision shouldFireNow(time_t now_unix, time_t next_run_unix, unsigned long grace_ms) {
    if (now_unix < next_run_unix) return Decision::WAIT;
    // Compare in seconds to avoid 32-bit unsigned-long overflow when delta is huge
    // (e.g. RTC battery failed, system rebooted weeks/months later).
    time_t delta_s = now_unix - next_run_unix;
    unsigned long grace_s = grace_ms / 1000UL;
    if ((unsigned long)delta_s < grace_s) return Decision::FIRE;
    return Decision::SKIP_RECOMPUTE;
}

} // namespace Scheduler

#endif // SCHEDULER_H
