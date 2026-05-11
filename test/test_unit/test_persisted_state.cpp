#include <unity.h>
#include "PersistedState.h"

static void test_persisted_state_round_trip() {
    PersistedState s{1714723200, 1715068800, true, 3};
    String json = PersistedState::toJson(s);
    PersistedState r;
    TEST_ASSERT_TRUE(PersistedState::fromJson(json.c_str(), r));
    TEST_ASSERT_EQUAL_INT64(1714723200, r.last_run_unix);
    TEST_ASSERT_EQUAL_INT64(1715068800, r.next_run_unix);
    TEST_ASSERT_TRUE(r.overflow_latched);
    TEST_ASSERT_EQUAL_INT(3, r.consecutive_skips_wet);
}

static void test_persisted_state_defaults() {
    PersistedState d = PersistedState::defaults();
    TEST_ASSERT_EQUAL_INT64(0, d.last_run_unix);
    TEST_ASSERT_EQUAL_INT64(0, d.next_run_unix);
    TEST_ASSERT_FALSE(d.overflow_latched);
    TEST_ASSERT_EQUAL_INT(0, d.consecutive_skips_wet);
}

static void test_persisted_state_rejects_garbage() {
    PersistedState r;
    TEST_ASSERT_FALSE(PersistedState::fromJson("garbage", r));
    TEST_ASSERT_FALSE(PersistedState::fromJson("{}", r));
}

void register_persisted_state_tests() {
    RUN_TEST(test_persisted_state_round_trip);
    RUN_TEST(test_persisted_state_defaults);
    RUN_TEST(test_persisted_state_rejects_garbage);
}
