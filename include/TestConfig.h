#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H

#ifdef NATIVE_TEST

#include <cstdint>

static const int      DEFAULT_INTERVAL_DAYS         = 4;
static const int      DEFAULT_SCHEDULE_HOUR         = 7;
static const int      DEFAULT_SCHEDULE_MINUTE       = 0;
static const uint32_t DEFAULT_MAX_RUNTIME_SEC       = 120;
static const int      DEFAULT_SOIL_THRESHOLD        = 1800;
static const int      DEFAULT_CALIBRATION_DRY       = 0;
static const int      DEFAULT_CALIBRATION_WET       = 0;

static const unsigned long SCHEDULE_GRACE_MS = 12UL * 3600UL * 1000UL;

static const int          SOIL_AVG_SAMPLES                 = 8;
static const int          OVERFLOW_DEBOUNCE_WINDOW         = 7;
static const int          OVERFLOW_DEBOUNCE_TRIP_THRESHOLD = 5;

static const int CONSECUTIVE_SKIPS_WET_ALERT_THRESHOLD = 2;

#endif
#endif
