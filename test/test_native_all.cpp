#include <unity.h>

// Each module's tests live in its own file and exposes a register_<module>_tests() function.
extern void register_settings_tests();
extern void register_scheduler_tests();
// Phase 4+: extern more registrators here.

int main(int argc, char** argv) {
    UNITY_BEGIN();
    register_settings_tests();
    register_scheduler_tests();
    return UNITY_END();
}

void setUp() {}
void tearDown() {}
