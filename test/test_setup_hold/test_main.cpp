#include <unity.h>
#include "SetupHold.h"

// The setup network kept up after the Wi-Fi was saved (docs/tescom-stick.md, "Setup")

void setUp(void) {}
void tearDown(void) {}

void test_inactive_until_started(void) {
    SetupHold h;
    TEST_ASSERT_FALSE(h.active());
    TEST_ASSERT_FALSE(h.update(100000));
    TEST_ASSERT_EQUAL_UINT32(0, h.leftS(0));
}

void test_ends_after_max_without_answer(void) {
    SetupHold h(60000, 20000);
    h.start(1000);
    TEST_ASSERT_FALSE(h.update(60999));
    TEST_ASSERT_EQUAL_UINT32(1, h.leftS(60999));
    TEST_ASSERT_TRUE(h.update(61000));
    TEST_ASSERT_FALSE(h.active());
    TEST_ASSERT_FALSE(h.update(70000));  // only once
}

void test_ends_grace_after_the_answer(void) {
    SetupHold h(60000, 20000);
    h.start(0);
    h.answerSeen(10000);
    h.answerSeen(25000);  // only the first read counts
    TEST_ASSERT_EQUAL_UINT32(20, h.leftS(10000));
    TEST_ASSERT_FALSE(h.update(29999));
    TEST_ASSERT_TRUE(h.update(30000));
}

void test_late_answer_keeps_the_max(void) {
    SetupHold h(60000, 20000);
    h.start(0);
    h.answerSeen(50000);
    TEST_ASSERT_EQUAL_UINT32(10, h.leftS(50000));
    TEST_ASSERT_FALSE(h.update(59999));
    TEST_ASSERT_TRUE(h.update(60000));
}

void test_saving_again_starts_over(void) {
    SetupHold h(60000, 20000);
    h.start(0);
    h.answerSeen(5000);
    h.start(20000);  // the Wi-Fi saved again (wrong password)
    TEST_ASSERT_FALSE(h.update(30000));
    TEST_ASSERT_FALSE(h.update(79999));
    TEST_ASSERT_TRUE(h.update(80000));
}

void test_wraps_with_millis(void) {
    SetupHold h(60000, 20000);
    h.start(0xFFFFF000u);
    TEST_ASSERT_FALSE(h.update(1000));
    TEST_ASSERT_TRUE(h.update(0xFFFFF000u + 60000u));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_inactive_until_started);
    RUN_TEST(test_ends_after_max_without_answer);
    RUN_TEST(test_ends_grace_after_the_answer);
    RUN_TEST(test_late_answer_keeps_the_max);
    RUN_TEST(test_saving_again_starts_over);
    RUN_TEST(test_wraps_with_millis);
    return UNITY_END();
}
