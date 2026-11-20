#include <unity.h>
#include "SetupButton.h"

// The setup button held 10 s opens the setup network (docs/tescom-stick.md, "Setup")

void setUp(void) {}
void tearDown(void) {}

// Up for a while, then held from `from` to `to`, sampled every 10 ms: the first time it fired
static uint32_t holdFrom(SetupButton& b, uint32_t from, uint32_t to) {
    for (uint32_t t = 0; t < from; t += 10) b.update(false, t);
    for (uint32_t t = from; t <= to; t += 10) {
        if (b.update(true, t)) return t;
    }
    return 0;
}

void test_fires_after_ten_seconds_once(void) {
    SetupButton b;
    uint32_t fired = holdFrom(b, 1000, 25000);
    TEST_ASSERT_EQUAL_UINT32(11000, fired);
    // Still held: no second time
    TEST_ASSERT_FALSE(b.update(true, 30000));
    TEST_ASSERT_FALSE(b.holding(30000));
}

void test_short_press_does_nothing(void) {
    SetupButton b;
    TEST_ASSERT_EQUAL_UINT32(0, holdFrom(b, 1000, 9000));
    for (uint32_t t = 9010; t < 9500; t += 10) TEST_ASSERT_FALSE(b.update(false, t));
    // The next press starts from zero
    for (uint32_t t = 9500; t < 19000; t += 10) TEST_ASSERT_FALSE(b.update(true, t));
    TEST_ASSERT_TRUE(b.update(true, 19500));
}

void test_held_at_boot_never_counts(void) {
    SetupButton b;
    for (uint32_t t = 0; t < 30000; t += 10) TEST_ASSERT_FALSE(b.update(true, t));
    // Let go, then a real hold works
    for (uint32_t t = 30000; t < 30200; t += 10) b.update(false, t);
    bool fired = false;
    for (uint32_t t = 30200; t <= 40300 && !fired; t += 10) fired = b.update(true, t);
    TEST_ASSERT_TRUE(fired);
}

void test_bounce_is_not_a_release(void) {
    SetupButton b;
    for (uint32_t t = 0; t < 1000; t += 10) b.update(false, t);
    for (uint32_t t = 1000; t < 6000; t += 10) b.update(true, t);
    // 20 ms of bounce
    b.update(false, 6000);
    b.update(false, 6010);
    bool fired = false;
    uint32_t at = 0;
    for (uint32_t t = 6020; t <= 12000 && !fired; t += 10) {
        fired = b.update(true, t);
        at = t;
    }
    TEST_ASSERT_TRUE(fired);
    TEST_ASSERT_UINT32_WITHIN(60, 11000, at);
}

void test_holding_shows_after_a_second(void) {
    SetupButton b;
    for (uint32_t t = 0; t < 1000; t += 10) b.update(false, t);
    for (uint32_t t = 1000; t < 1900; t += 10) b.update(true, t);
    TEST_ASSERT_FALSE(b.holding(1900));
    for (uint32_t t = 1900; t < 2200; t += 10) b.update(true, t);
    TEST_ASSERT_TRUE(b.holding(2200));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_fires_after_ten_seconds_once);
    RUN_TEST(test_short_press_does_nothing);
    RUN_TEST(test_held_at_boot_never_counts);
    RUN_TEST(test_bounce_is_not_a_release);
    RUN_TEST(test_holding_shows_after_a_second);
    return UNITY_END();
}
