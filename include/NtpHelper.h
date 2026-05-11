#ifndef NTP_HELPER_H
#define NTP_HELPER_H

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include "DS3231RTC.h"

// On-demand NTP refresh of the DS3231 RTC.
//
// Design contract: DS3231 remains the system's source of truth (read once
// at boot, scheduling math runs off the RTC). NTP is a *writer only* —
// invoked explicitly by `/settime` (no-arg) to re-seed the RTC from a
// network time source. We do NOT poll NTP in the background and we do NOT
// override the RTC at boot; if WiFi is down or NTP is blocked, the RTC's
// own oscillator continues to be authoritative.
//
// Two-server fallback: SNTP tries the primary first, then secondary if the
// primary doesn't reply in time. Combined wall-clock budget is roughly
// `timeout_ms` total (SNTP itself retries internally; we just poll until
// `time()` advances or we hit the deadline).
namespace NtpHelper {

inline time_t syncFromPool(const char* primary,
                           const char* secondary,
                           unsigned long timeout_ms = 8000) {
    if (WiFi.status() != WL_CONNECTED) return 0;

    // UTC0 offset — matches the process TZ pinned in main.cpp:285. SNTP
    // starts in the background; first reply jumps the system clock from
    // ~1970 to the current epoch.
    configTime(0, 0, primary, secondary);

    // Sanity floor: any year >= 2024 means SNTP has replied. (Pre-sync
    // the system clock is whatever the RTC seeded it with at boot; it
    // could be near-current already, but we still want to confirm a fresh
    // NTP reply rather than trust stale state — so we wait for `time()`
    // to monotonically advance past the floor.)
    const time_t SANITY_FLOOR = 1704067200;  // 2024-01-01 UTC
    unsigned long deadline = millis() + timeout_ms;

    while ((long)(millis() - deadline) < 0) {
        time_t now = time(nullptr);
        if (now > SANITY_FLOOR) {
            // SNTP delivered. Mirror to the RTC so the new time survives
            // reboots without WiFi. setTime(time_t) uses localtime(), but
            // because process TZ is UTC0, localtime() == gmtime() and the
            // conversion is identity.
            DS3231RTC::setTime(now);
            return now;
        }
        delay(100);
    }
    return 0;
}

}  // namespace NtpHelper

#endif  // NTP_HELPER_H
