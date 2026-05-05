#ifndef MOISTURE_SENSOR_H
#define MOISTURE_SENSOR_H

#ifndef NATIVE_TEST
#include <Arduino.h>
#include "config.h"
#endif

namespace Moisture {

inline int average(const int* samples, int n) {
    if (n <= 0) return 0;
    long sum = 0;
    for (int i = 0; i < n; ++i) sum += samples[i];
    return (int)(sum / n);
}

inline bool isWet(int raw, int threshold) {
    return raw <= threshold;
}

inline int pctFromCalibration(int raw, int cal_wet, int cal_dry) {
    if (cal_wet <= 0 || cal_dry <= 0) return -1;
    if (cal_wet >= cal_dry) return -1;  // inverted/garbage calibration
    if (raw <= cal_wet) return 100;
    if (raw >= cal_dry) return 0;
    int range = cal_dry - cal_wet;
    int from_dry = cal_dry - raw;
    return (from_dry * 100) / range;
}

#ifndef NATIVE_TEST
inline int readAveragedRaw() {
    int samples[SOIL_AVG_SAMPLES];
    for (int i = 0; i < SOIL_AVG_SAMPLES; ++i) {
        samples[i] = analogRead(SOIL_SENSOR_AOUT_PIN);
        delayMicroseconds(200);
    }
    return average(samples, SOIL_AVG_SAMPLES);
}
#endif

} // namespace Moisture

#endif // MOISTURE_SENSOR_H
