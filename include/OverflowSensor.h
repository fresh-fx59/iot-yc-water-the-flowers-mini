#ifndef OVERFLOW_SENSOR_H
#define OVERFLOW_SENSOR_H

#ifdef NATIVE_TEST
#include "TestConfig.h"
#else
#include "config.h"
#endif

class OverflowSensor {
public:
    // Returns true ONLY on the read that flipped the latch from false → true.
    bool pushReading(bool low_observed) {
        window_[head_] = low_observed;
        head_ = (head_ + 1) % OVERFLOW_DEBOUNCE_WINDOW;
        if (count_ < OVERFLOW_DEBOUNCE_WINDOW) ++count_;

        int lows = triggerStreak();
        if (!latched_ && lows >= OVERFLOW_DEBOUNCE_TRIP_THRESHOLD) {
            latched_ = true;
            return true;
        }
        return false;
    }

    int triggerStreak() const {
        int c = 0;
        for (int i = 0; i < count_; ++i) if (window_[i]) ++c;
        return c;
    }

    bool latched() const { return latched_; }
    void setLatched(bool v) { latched_ = v; }
    void reset() {
        latched_ = false;
        for (int i = 0; i < OVERFLOW_DEBOUNCE_WINDOW; ++i) window_[i] = false;
        head_ = 0;
        count_ = 0;
    }

private:
    bool window_[OVERFLOW_DEBOUNCE_WINDOW]{};
    int  head_ = 0;
    int  count_ = 0;
    bool latched_ = false;
};

#endif // OVERFLOW_SENSOR_H
