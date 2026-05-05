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

inline time_t computeNextRun(time_t now_unix, time_t last_run_unix, int interval_days, int hour, int minute) {
    if (last_run_unix == 0) {
        time_t candidate = alignToHM(now_unix, hour, minute);
        if (candidate <= now_unix) candidate += 86400;
        return candidate;
    }
    time_t base = alignToHM(last_run_unix, hour, minute);
    return base + (time_t)interval_days * 86400;
}

inline Decision shouldFireNow(time_t now_unix, time_t next_run_unix, unsigned long grace_ms) {
    if (now_unix < next_run_unix) return Decision::WAIT;
    unsigned long delta_ms = (unsigned long)(now_unix - next_run_unix) * 1000UL;
    if (delta_ms < grace_ms) return Decision::FIRE;
    return Decision::SKIP_RECOMPUTE;
}

} // namespace Scheduler

#endif // SCHEDULER_H
