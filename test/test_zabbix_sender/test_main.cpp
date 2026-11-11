#include <unity.h>
#include "UPSData.h"
#include "ZabbixSender.h"
#include <string>
#include <vector>

// The values tescom-agent sends for the same reading (nut-app crates/tescom-agent/src/items.rs)

static UPSData data;
static std::vector<Zabbix::Item> out;

void setUp(void) {
    data = UPSData();
    out.clear();
}
void tearDown(void) {}

static const char* value(const char* key) {
    for (const auto& i : out) {
        if (i.key == key) return i.value.c_str();
    }
    return nullptr;
}

static void reading() {
    data.set("battery.charge", "87");
    data.set("battery.voltage", "26.1");
    data.set("ups.load", "18");
    data.set("input.voltage", "0.0");
    data.set("input.frequency", "49.9");
    data.set("driver.name", "nutdrv_qx");
}

void test_on_battery_reading_becomes_items(void) {
    reading();
    Zabbix::items("shop-ups", Zabbix::Link::OK, "OB DISCHRG LB", &data, 1700000000, out);
    for (const auto& i : out) {
        TEST_ASSERT_EQUAL_STRING("shop-ups", i.host.c_str());
        TEST_ASSERT_EQUAL_UINT64(1700000000, i.clock);
    }
    TEST_ASSERT_EQUAL_STRING("ok", value("ups.link"));
    TEST_ASSERT_EQUAL_STRING("OB DISCHRG LB", value("ups.status"));
    TEST_ASSERT_EQUAL_STRING("battery", value("ups.power"));
    TEST_ASSERT_EQUAL_STRING("1", value("ups.on_battery"));
    TEST_ASSERT_EQUAL_STRING("1", value("ups.low_battery"));
    TEST_ASSERT_EQUAL_STRING("0", value("ups.replace_battery"));
    TEST_ASSERT_EQUAL_STRING("0", value("ups.overloaded"));
    TEST_ASSERT_EQUAL_STRING("87", value("ups.battery.charge"));
    TEST_ASSERT_EQUAL_STRING("26.1", value("ups.battery.voltage"));
    TEST_ASSERT_EQUAL_STRING("0", value("ups.input.voltage"));
    TEST_ASSERT_EQUAL_STRING("49.9", value("ups.input.frequency"));
    TEST_ASSERT_EQUAL_STRING("nutdrv_qx", value("ups.driver"));
    // Not reported by this UPS, so not sent
    TEST_ASSERT_NULL(value("ups.battery.runtime"));
    TEST_ASSERT_NULL(value("ups.output.voltage"));
    TEST_ASSERT_NULL(value("ups.model"));
}

void test_a_failed_link_only_reports_the_link(void) {
    reading();
    Zabbix::items("shop-ups", Zabbix::Link::STALE, "OL", &data, 0, out);
    TEST_ASSERT_EQUAL_UINT(1, out.size());
    TEST_ASSERT_EQUAL_STRING("stale", value("ups.link"));
    out.clear();
    Zabbix::items("shop-ups", Zabbix::Link::DRIVER_DOWN, "", nullptr, 0, out);
    TEST_ASSERT_EQUAL_STRING("driverDown", value("ups.link"));
}

void test_power_source_and_flags(void) {
    Zabbix::items("h", Zabbix::Link::OK, "OL CHRG RB OVER", nullptr, 0, out);
    TEST_ASSERT_EQUAL_STRING("mains", value("ups.power"));
    TEST_ASSERT_EQUAL_STRING("0", value("ups.on_battery"));
    TEST_ASSERT_EQUAL_STRING("1", value("ups.replace_battery"));
    TEST_ASSERT_EQUAL_STRING("1", value("ups.overloaded"));
    out.clear();
    Zabbix::items("h", Zabbix::Link::OK, "OL BYPASS", nullptr, 0, out);
    TEST_ASSERT_EQUAL_STRING("bypass", value("ups.power"));
    out.clear();
    Zabbix::items("h", Zabbix::Link::OK, "OFF OB", nullptr, 0, out);
    TEST_ASSERT_EQUAL_STRING("off", value("ups.power"));
    out.clear();
    // OBX is not OB
    Zabbix::items("h", Zabbix::Link::OK, "OBX", nullptr, 0, out);
    TEST_ASSERT_EQUAL_STRING("unknown", value("ups.power"));
}

void test_model_and_serial_prefer_device(void) {
    data.set("ups.model", "Leo LCD");
    data.set("device.serial", "TS123");
    data.set("ups.serial", "other");
    data.set("battery.charge", "not a number");
    Zabbix::items("h", Zabbix::Link::OK, "OL", &data, 0, out);
    TEST_ASSERT_EQUAL_STRING("Leo LCD", value("ups.model"));
    TEST_ASSERT_EQUAL_STRING("TS123", value("ups.serial"));
    TEST_ASSERT_NULL(value("ups.battery.charge"));
}

void test_numbers_as_the_agent_writes_them(void) {
    TEST_ASSERT_EQUAL_STRING("0", Zabbix::formatNumber(0.0).c_str());
    TEST_ASSERT_EQUAL_STRING("100", Zabbix::formatNumber(100.0).c_str());
    TEST_ASSERT_EQUAL_STRING("26.1", Zabbix::formatNumber(26.1).c_str());
    TEST_ASSERT_EQUAL_STRING("1.56", Zabbix::formatNumber(1.56).c_str());
    TEST_ASSERT_EQUAL_STRING("-3.5", Zabbix::formatNumber(-3.5).c_str());
    TEST_ASSERT_EQUAL_STRING("12912", Zabbix::formatNumber(12912).c_str());
    TEST_ASSERT_EQUAL_STRING("0.008", Zabbix::formatNumber(0.008).c_str());
}

void test_request_and_frame(void) {
    std::vector<Zabbix::Item> items = { { "a\"b", "ups.link", "ok", 1700000000 }, { "h", "ups.status", "OL", 0 } };
    std::string json = Zabbix::requestJson(items, 1700000005);
    TEST_ASSERT_EQUAL_STRING("{\"request\":\"sender data\",\"data\":["
                             "{\"host\":\"a\\\"b\",\"key\":\"ups.link\",\"value\":\"ok\",\"clock\":1700000000},"
                             "{\"host\":\"h\",\"key\":\"ups.status\",\"value\":\"OL\"}],\"clock\":1700000005}",
                             json.c_str());
    std::string f = Zabbix::frame("{}");
    TEST_ASSERT_EQUAL_UINT(15, f.size());
    TEST_ASSERT_EQUAL_MEMORY("ZBXD\x01\x02\0\0\0\0\0\0\0{}", f.data(), 15);
    // No clock when there is no time yet
    TEST_ASSERT_EQUAL_STRING("{\"request\":\"sender data\",\"data\":[]}", Zabbix::requestJson({}, 0).c_str());
}

void test_reply(void) {
    std::string r = Zabbix::frame(
        "{\"response\":\"success\",\"info\":\"processed: 17; failed: 1; total: 18; seconds spent: 0.000123\"}");
    TEST_ASSERT_EQUAL_UINT(r.size(), Zabbix::replyLength((const uint8_t*)r.data(), 13));
    Zabbix::Outcome o;
    TEST_ASSERT_TRUE(Zabbix::parseReply((const uint8_t*)r.data(), r.size(), o));
    TEST_ASSERT_TRUE(o.ok);
    TEST_ASSERT_EQUAL_UINT32(17, o.processed);
    TEST_ASSERT_EQUAL_UINT32(1, o.failed);
    TEST_ASSERT_EQUAL_UINT32(18, o.total);

    r = Zabbix::frame("{\"response\":\"failed\",\"info\":\"bad\"}");
    TEST_ASSERT_TRUE(Zabbix::parseReply((const uint8_t*)r.data(), r.size(), o));
    TEST_ASSERT_FALSE(o.ok);
    TEST_ASSERT_EQUAL_STRING("bad", o.info.c_str());

    const uint8_t http[] = "HTTP/1.1 400 Bad Request\r\n";
    TEST_ASSERT_EQUAL_UINT(0, Zabbix::replyLength(http, sizeof(http) - 1));
    TEST_ASSERT_FALSE(Zabbix::parseReply(http, sizeof(http) - 1, o));
    // Truncated
    r = Zabbix::frame("{\"response\":\"success\"}");
    TEST_ASSERT_FALSE(Zabbix::parseReply((const uint8_t*)r.data(), r.size() - 1, o));
}

void test_backlog_drops_the_oldest(void) {
    Zabbix::Backlog b(3);
    b.add({ { "h", "k", "1", 1 }, { "h", "k", "2", 2 } });
    b.add({ { "h", "k", "3", 3 }, { "h", "k", "4", 4 } });
    TEST_ASSERT_EQUAL_UINT(3, b.size());
    TEST_ASSERT_EQUAL_UINT(1, b.dropped());
    TEST_ASSERT_EQUAL_STRING("2", b.items().front().value.c_str());
    b.clear();
    TEST_ASSERT_EQUAL_UINT(0, b.size());
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_on_battery_reading_becomes_items);
    RUN_TEST(test_a_failed_link_only_reports_the_link);
    RUN_TEST(test_power_source_and_flags);
    RUN_TEST(test_model_and_serial_prefer_device);
    RUN_TEST(test_numbers_as_the_agent_writes_them);
    RUN_TEST(test_request_and_frame);
    RUN_TEST(test_reply);
    RUN_TEST(test_backlog_drops_the_oldest);
    return UNITY_END();
}
