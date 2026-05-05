#include <unity.h>

// Each module's tests live in its own file and exposes a register_<module>_tests() function.
extern void register_settings_tests();
extern void register_scheduler_tests();
extern void register_moisture_tests();
extern void register_overflow_tests();
extern void register_persisted_state_tests();
extern void register_watering_controller_tests();
// Phase 7+: extern more registrators here.

int main(int argc, char** argv) {
    UNITY_BEGIN();
    register_settings_tests();
    register_scheduler_tests();
    register_moisture_tests();
    register_overflow_tests();
    register_persisted_state_tests();
    register_watering_controller_tests();
    return UNITY_END();
}

void setUp() {}
void tearDown() {}
