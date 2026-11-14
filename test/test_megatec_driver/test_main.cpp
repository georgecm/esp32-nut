#include <unity.h>
#include "DriverRegistry.h"
#include "IHidTransport.h"
#include "MegatecDriver.h"
#include "MegatecProtocol.h"
#include "UPSData.h"
#include "UpsProfiles.h"
#include <map>
#include <string>
#include <vector>

// The MEC0003 bridge as the drivers see it: each string index answers a Megatec reply,
// sent back as a string descriptor (bLength, 0x03, UTF-16LE)
class MockBridge : public IHidTransport {
public:
    struct Request {
        uint8_t index;
        uint16_t lang_id;
        uint16_t length;
    };
    std::map<uint8_t, std::string> replies;
    std::vector<Request> requests;
    bool corrupt = false;

    void lock() const override {}
    void unlock() const override {}
    HIDParser _parser;
    std::vector<HIDUsageDef> _usages;
    const HIDParser* getHIDParser() const override { return &_parser; }
    const std::vector<HIDUsageDef>& getUsages() const override { return _usages; }
    String getActiveBeeperPath() const override { return ""; }
    uint32_t getQuirks() const override { return 0; }
    bool isPollingPaused() const override { return false; }
    bool requestReport(uint8_t, uint8_t, uint16_t) override { return false; }
    bool requestStringDescriptor(uint8_t) override { return false; }

    bool requestCommandString(uint8_t index, uint16_t lang_id, uint16_t length,
                              uint8_t* out, size_t out_size, size_t& out_len) override {
        requests.push_back({index, lang_id, length});
        out_len = 0;
        auto it = replies.find(index);
        if (it == replies.end()) return false;
        size_t n = 2 + 2 * it->second.size();
        if (n > length) n = length & ~1u;
        if (n > out_size) return false;
        out[0] = (uint8_t)(corrupt ? n + 2 : n);
        out[1] = 0x03;
        for (size_t i = 0; 2 + 2 * i + 1 < n; i++) {
            out[2 + 2 * i] = (uint8_t)it->second[i];
            out[3 + 2 * i] = 0;
        }
        out_len = n;
        return true;
    }

    std::vector<uint8_t> indices() const {
        std::vector<uint8_t> v;
        for (const auto& r : requests) v.push_back(r.index);
        return v;
    }
};

// The Tescom LEO on mains (24 V pack), as fake-upsd's dump of 23 Oct 2026 has its values
static const char* LEO_Q1 = "(232.0 232.0 231.0 000 49.5 27.2 29.0 00001000\r";
static const char* LEO_F = "#230.0 004 024.0 50.0\r";
// The Leo SmartBox: 12 V battery floating at 13.8 V. Its F fills only the battery
// (read from the board on 2026-11-14); the input fields are blank
static const char* SMARTBOX_F = "#   .   .  12.00   . \r";

static UPSData data;
static MockBridge bridge;

void setUp(void) {
    data = UPSData();
    bridge = MockBridge();
}

void tearDown(void) {}

static void assertIndices(const std::vector<uint8_t>& expected) {
    std::vector<uint8_t> got = bridge.indices();
    TEST_ASSERT_EQUAL_UINT(expected.size(), got.size());
    for (size_t i = 0; i < expected.size(); i++) TEST_ASSERT_EQUAL_HEX8(expected[i], got[i]);
}

// --- MegatecProtocol ---

void test_decode_string_descriptor(void) {
    const uint8_t desc[] = { 8, 0x03, 'Q', 0, '1', 0, 0x20, 0x26 };
    char out[16];
    TEST_ASSERT_EQUAL_INT(3, Megatec::decodeStringDescriptor(desc, sizeof(desc), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Q1?", out);
    // bLength must be what came: "size mismatch" in hunnox_command
    TEST_ASSERT_EQUAL_INT(-1, Megatec::decodeStringDescriptor(desc, 6, out, sizeof(out)));
}

void test_q1_on_mains(void) {
    TEST_ASSERT_TRUE(Megatec::parseQ1(LEO_Q1, data));
    TEST_ASSERT_EQUAL_STRING("232.0", data.get("input.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("231.0", data.get("output.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("0", data.get("ups.load").c_str());
    TEST_ASSERT_EQUAL_STRING("49.5", data.get("input.frequency").c_str());
    TEST_ASSERT_EQUAL_STRING("27.20", data.get("battery.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("29.0", data.get("ups.temperature").c_str());
    TEST_ASSERT_EQUAL_STRING("offline / line interactive", data.get("ups.type").c_str());
    TEST_ASSERT_EQUAL_STRING("disabled", data.get("ups.beeper.status").c_str());
    TEST_ASSERT_EQUAL_STRING("OL", UPSData::computeUPSStatusString(data).c_str());
}

void test_q1_on_battery_low(void) {
    TEST_ASSERT_TRUE(Megatec::parseQ1("(000.0 000.0 230.0 035 50.0 21.4 30.0 11001001\r", data));
    TEST_ASSERT_EQUAL_STRING("OB LB", UPSData::computeUPSStatusString(data).c_str());
    TEST_ASSERT_EQUAL_STRING("enabled", data.get("ups.beeper.status").c_str());
    // Mains back: the flags clear
    TEST_ASSERT_TRUE(Megatec::parseQ1(LEO_Q1, data));
    TEST_ASSERT_EQUAL_STRING("OL", UPSData::computeUPSStatusString(data).c_str());
}

void test_q1_boost_trim_and_alarm(void) {
    TEST_ASSERT_TRUE(Megatec::parseQ1("(195.0 195.0 225.0 010 50.0 27.0 29.0 00101000\r", data));
    TEST_ASSERT_EQUAL_STRING("OL BOOST", UPSData::computeUPSStatusString(data).c_str());
    TEST_ASSERT_TRUE(Megatec::parseQ1("(255.0 255.0 230.0 010 50.0 27.0 29.0 00101000\r", data));
    TEST_ASSERT_EQUAL_STRING("OL TRIM", UPSData::computeUPSStatusString(data).c_str());
    TEST_ASSERT_TRUE(Megatec::parseQ1("(230.0 230.0 230.0 010 50.0 27.0 29.0 00011100\r", data));
    TEST_ASSERT_EQUAL_STRING("ALARM CAL OL", UPSData::computeUPSStatusString(data).c_str());
    TEST_ASSERT_TRUE(Megatec::parseQ1(LEO_Q1, data));
    TEST_ASSERT_FALSE(data.hasKey("ups.alarm"));
}

void test_q1_rejects_other_replies(void) {
    TEST_ASSERT_FALSE(Megatec::parseQ1("", data));
    TEST_ASSERT_FALSE(Megatec::parseQ1("#230.0 004 024.0 50.0\r", data));
    TEST_ASSERT_FALSE(Megatec::parseQ1("(232.0 232.0 231.0 000 49.5 27.2 29.0\r", data));
    TEST_ASSERT_FALSE(Megatec::parseQ1("(232.0 232.0 231.0 000 49.5 27.2 29.0 0000x000\r", data));
    TEST_ASSERT_EQUAL_UINT(0, data.getAll().size());
}

void test_q1_battery_packs(void) {
    // The LiON RT reports 1/48 of its pack (battery_voltage_reports_one_pack)
    TEST_ASSERT_TRUE(Megatec::parseQ1("(232.1 232.1 230.0 000 50.0 0.56 32.0 00000001\r", data, 48));
    TEST_ASSERT_EQUAL_STRING("26.88", data.get("battery.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("online", data.get("ups.type").c_str());
}

void test_f_and_charge_leo(void) {
    TEST_ASSERT_TRUE(Megatec::parseF(LEO_F, data));
    TEST_ASSERT_EQUAL_STRING("230", data.get("input.voltage.nominal").c_str());
    TEST_ASSERT_EQUAL_STRING("4.0", data.get("input.current.nominal").c_str());
    TEST_ASSERT_EQUAL_STRING("24.0", data.get("battery.voltage.nominal").c_str());
    TEST_ASSERT_EQUAL_STRING("50", data.get("input.frequency.nominal").c_str());
    TEST_ASSERT_TRUE(Megatec::parseQ1(LEO_Q1, data));
    Megatec::estimateCharge(data);
    // What nutdrv_qx guessed on the Pi for the LEO
    TEST_ASSERT_EQUAL_STRING("20.80", data.get("battery.voltage.low").c_str());
    TEST_ASSERT_EQUAL_STRING("26.00", data.get("battery.voltage.high").c_str());
    TEST_ASSERT_EQUAL_STRING("100", data.get("battery.charge").c_str());
}

void test_charge_smartbox_on_battery(void) {
    TEST_ASSERT_TRUE(Megatec::parseF(SMARTBOX_F, data));
    TEST_ASSERT_TRUE(Megatec::parseQ1("(000.0 000.0 230.0 020 50.0 11.7 30.0 10001000\r", data));
    Megatec::estimateCharge(data);
    TEST_ASSERT_EQUAL_STRING("10.40", data.get("battery.voltage.low").c_str());
    TEST_ASSERT_EQUAL_STRING("13.00", data.get("battery.voltage.high").c_str());
    TEST_ASSERT_EQUAL_STRING("50", data.get("battery.charge").c_str());
}

void test_f_smartbox_blank_fields(void) {
    TEST_ASSERT_TRUE(Megatec::parseF(SMARTBOX_F, data));
    TEST_ASSERT_EQUAL_STRING("12.0", data.get("battery.voltage.nominal").c_str());
    TEST_ASSERT_FALSE(data.hasKey("input.voltage.nominal"));
    TEST_ASSERT_FALSE(data.hasKey("input.current.nominal"));
    TEST_ASSERT_FALSE(data.hasKey("input.frequency.nominal"));
}

void test_f_blank_from_a_waking_smartbox(void) {
    // The first replies after it is plugged in, before the battery field fills
    TEST_ASSERT_FALSE(Megatec::parseF("#   .   .    .     . \r", data));
    TEST_ASSERT_FALSE(data.hasKey("battery.voltage.nominal"));
}

void test_f_zeros_from_a_waking_bridge(void) {
    TEST_ASSERT_FALSE(Megatec::parseF("#000.0 000 000.0 00.0\r", data));
    TEST_ASSERT_FALSE(data.hasKey("battery.voltage.nominal"));
    Megatec::estimateCharge(data);
    TEST_ASSERT_FALSE(data.hasKey("battery.charge"));
}

void test_no_ack(void) {
    TEST_ASSERT_TRUE(Megatec::isNoAck("UPS No Ack\r"));
    TEST_ASSERT_TRUE(Megatec::isNoAck("ups no ack"));
    TEST_ASSERT_FALSE(Megatec::isNoAck("UPS No Ack!"));
}

// --- MegatecDriver ---

void test_hunnox_lock_step_and_poll(void) {
    MegatecDriver driver;
    driver.setup();
    bridge.replies[0x00] = "\x09\x04";
    bridge.replies[0x01] = "";
    bridge.replies[0x03] = LEO_Q1;
    bridge.replies[0x0d] = LEO_F;

    driver.loop(&bridge, data, 10000);
    // F: step 0 (0x00 twice, 0x01), then F; Q1: step 1 asks 0x0d first, then Q1
    assertIndices({ 0x00, 0x00, 0x01, 0x0d, 0x0d, 0x03 });
    for (const auto& r : bridge.requests) TEST_ASSERT_EQUAL_HEX16(0x0409, r.lang_id);
    TEST_ASSERT_EQUAL_UINT16(102, bridge.requests[3].length);
    TEST_ASSERT_EQUAL_UINT16(102, bridge.requests[5].length);
    TEST_ASSERT_TRUE(driver.initialPollDone());
    TEST_ASSERT_EQUAL_STRING("OL", UPSData::computeUPSStatusString(data).c_str());
    TEST_ASSERT_EQUAL_STRING("100", data.get("battery.charge").c_str());
    TEST_ASSERT_EQUAL_STRING("hunnox", data.get("driver.parameter.subdriver").c_str());

    // Within the poll interval nothing is asked
    bridge.requests.clear();
    driver.loop(&bridge, data, 11000);
    TEST_ASSERT_EQUAL_UINT(0, bridge.requests.size());

    // Next poll: F is not asked again; step 2 skips 0x03 for Q1 itself
    driver.loop(&bridge, data, 12000);
    assertIndices({ 0x03 });
    // Then step 3 (no 0x0c with novendor), then step 1 again
    bridge.requests.clear();
    driver.loop(&bridge, data, 14000);
    assertIndices({ 0x03 });
    bridge.requests.clear();
    driver.loop(&bridge, data, 16000);
    assertIndices({ 0x0d, 0x03 });
}

void test_ratings_asked_until_real(void) {
    MegatecDriver driver;
    driver.setup();
    bridge.replies[0x03] = LEO_Q1;
    bridge.replies[0x0d] = "#000.0 000 000.0 00.0\r";
    driver.loop(&bridge, data, 10000);
    TEST_ASSERT_TRUE(driver.initialPollDone());
    TEST_ASSERT_FALSE(data.hasKey("battery.charge"));

    bridge.replies[0x0d] = LEO_F;
    bridge.requests.clear();
    driver.loop(&bridge, data, 12000);
    TEST_ASSERT_EQUAL_STRING("24.0", data.get("battery.voltage.nominal").c_str());
    TEST_ASSERT_EQUAL_STRING("100", data.get("battery.charge").c_str());
}

void test_corrupt_or_missing_reply_leaves_data(void) {
    MegatecDriver driver;
    driver.setup();
    bridge.replies[0x03] = LEO_Q1;
    bridge.corrupt = true;
    driver.loop(&bridge, data, 10000);
    TEST_ASSERT_FALSE(driver.initialPollDone());
    TEST_ASSERT_FALSE(data.hasKey("input.voltage"));

    bridge.corrupt = false;
    bridge.replies[0x03] = "UPS No Ack\r";
    driver.loop(&bridge, data, 12000);
    TEST_ASSERT_FALSE(driver.initialPollDone());
}

void test_fabula_asks_without_lock_step(void) {
    MegatecDriver driver(MegatecDriver::Transport::FABULA);
    driver.setup();
    bridge.replies[0x03] = LEO_Q1;
    bridge.replies[0x0d] = LEO_F;
    driver.loop(&bridge, data, 10000);
    assertIndices({ 0x0d, 0x03 });
    TEST_ASSERT_EQUAL_STRING("fabula", data.get("driver.parameter.subdriver").c_str());
}

void test_reads_only(void) {
    MegatecDriver driver;
    TEST_ASSERT_FALSE(driver.shutdownCommandsSupported());
    TEST_ASSERT_FALSE(driver.beeperControllable());
}

// --- DriverRegistry ---

void test_registry_picks_the_transport(void) {
    TEST_ASSERT_TRUE(DriverRegistry::isQx(0x0001, 0x0000));
    TEST_ASSERT_FALSE(DriverRegistry::isQx(0x0463, 0xFFFF));

    IUPSDriver* d = DriverRegistry::create(0x0001, 0x0000, "", "");
    TEST_ASSERT_EQUAL_STRING("MegatecDriver (hunnox)", d->getDriverName());
    delete d;
    d = DriverRegistry::create(0x0001, 0x0000, "", "MEC0003");
    TEST_ASSERT_EQUAL_STRING("MegatecDriver (hunnox)", d->getDriverName());
    delete d;
    d = DriverRegistry::create(0x0001, 0x0000, "MEC", "MEC0003");
    TEST_ASSERT_EQUAL_STRING("MegatecDriver (fabula)", d->getDriverName());
    delete d;
}

// --- UpsProfiles (the tables generated from nut-app's profiles/*.conf) ---

void test_profiles_exported(void) {
    const UpsProfiles::Profile* leo = UpsProfiles::find("leo");
    TEST_ASSERT_NOT_NULL(leo);
    TEST_ASSERT_EQUAL_STRING("Tescom Leo LCD", leo->model);
    TEST_ASSERT_EQUAL_STRING("Leo LCD", leo->shortModel());
    TEST_ASSERT_TRUE(leo->has_usb_id);
    TEST_ASSERT_EQUAL_HEX16(0x0001, leo->vid);
    TEST_ASSERT_EQUAL_HEX16(0x0000, leo->pid);
    TEST_ASSERT_EQUAL_STRING("hunnox", leo->subdriver);
    TEST_ASSERT_EQUAL_STRING("", leo->option("novendor"));
    TEST_ASSERT_NULL(leo->option("port"));
    const UpsProfiles::Profile* prime = UpsProfiles::find("prime");
    TEST_ASSERT_NOT_NULL(prime);
    TEST_ASSERT_EQUAL_HEX16(0x1a86, prime->vid);
    TEST_ASSERT_EQUAL_HEX16(0x7523, prime->pid);
    TEST_ASSERT_TRUE(UpsProfiles::knowsUsbId(0x0001, 0x0000));
    TEST_ASSERT_FALSE(UpsProfiles::knowsUsbId(0x0463, 0xffff));
}

static void pollOnce(MegatecDriver& driver, const char* f) {
    driver.setup();
    bridge.replies[0x03] = LEO_Q1;
    bridge.replies[0x0d] = f;
    driver.loop(&bridge, data, 10000);
}

void test_driver_names_the_leo(void) {
    MegatecDriver driver;
    pollOnce(driver, LEO_F);
    TEST_ASSERT_NOT_NULL(driver.profile());
    TEST_ASSERT_EQUAL_STRING("leo", driver.profile()->key);
    TEST_ASSERT_EQUAL_STRING("Tescom", data.get("device.mfr").c_str());
    TEST_ASSERT_EQUAL_STRING("Leo LCD", data.get("ups.model").c_str());
    TEST_ASSERT_EQUAL_STRING("Leo LCD", data.get("device.model").c_str());
    // The LEO profile sets no battery limits: NUT's guess stays
    TEST_ASSERT_EQUAL_STRING("20.80", data.get("battery.voltage.low").c_str());
}

void test_driver_names_the_smartbox(void) {
    MegatecDriver driver;
    pollOnce(driver, SMARTBOX_F);
    TEST_ASSERT_NOT_NULL(driver.profile());
    TEST_ASSERT_EQUAL_STRING("smartbox", driver.profile()->key);
    TEST_ASSERT_EQUAL_STRING("Leo SmartBox", data.get("ups.model").c_str());
}

void test_driver_without_a_fitting_profile(void) {
    MegatecDriver driver;
    pollOnce(driver, "#230.0 004 036.0 50.0\r");
    TEST_ASSERT_NULL(driver.profile());
    TEST_ASSERT_FALSE(data.hasKey("ups.model"));
    TEST_ASSERT_TRUE(driver.initialPollDone());
    // Another USB ID never gets a Tescom profile, even with the LEO's ratings
    MegatecDriver other(MegatecDriver::Transport::HUNNOX, 0x0665, 0x5161);
    data = UPSData();
    pollOnce(other, LEO_F);
    TEST_ASSERT_NULL(other.profile());
}

void test_profile_settings_of_the_lion(void) {
    // What the LiON RT's driver reports; nutdrv_qx_mec (krauler) is not ported yet
    data.set("driver.name", "nutdrv_qx_mec");
    data.set("battery.voltage.nominal", "24.0");
    data.set("battery.voltage.low", "20.00");
    const UpsProfiles::Profile* lion = UpsProfiles::identify(0x0001, 0x0000, data);
    TEST_ASSERT_NOT_NULL(lion);
    TEST_ASSERT_EQUAL_STRING("lion", lion->key);
    TEST_ASSERT_EQUAL_INT(48, UpsProfiles::batteryPacks(*lion));
    TEST_ASSERT_EQUAL_INT(1, UpsProfiles::batteryPacks(*UpsProfiles::find("leo")));

    UpsProfiles::apply(*lion, data);
    TEST_ASSERT_EQUAL_STRING("25.6", data.get("battery.voltage.nominal").c_str()); // override.
    TEST_ASSERT_EQUAL_STRING("230", data.get("input.voltage.nominal").c_str());
    TEST_ASSERT_EQUAL_STRING("20.00", data.get("battery.voltage.low").c_str()); // default. kept
    TEST_ASSERT_EQUAL_STRING("26.8", data.get("battery.voltage.high").c_str()); // default. set

    double low = 0, high = 0;
    TEST_ASSERT_TRUE(UpsProfiles::number(*lion, "battery.voltage.low", low));
    TEST_ASSERT_TRUE(UpsProfiles::number(*lion, "battery.voltage.high", high));
    data.set("battery.voltage", "25.40");
    Megatec::estimateCharge(data, low, high);
    TEST_ASSERT_EQUAL_STRING("50", data.get("battery.charge").c_str());
}

void test_match_numbers_and_text(void) {
    const UpsProfiles::Profile* leo = UpsProfiles::find("leo");
    data.set("driver.name", "nutdrv_qx");
    data.set("driver.parameter.subdriver", "hunnox");
    data.set("battery.voltage.nominal", "24.00");
    TEST_ASSERT_TRUE(UpsProfiles::matches(*leo, data));
    data.set("battery.voltage.nominal", "24.5");
    TEST_ASSERT_FALSE(UpsProfiles::matches(*leo, data));
    data.set("battery.voltage.nominal", "24");
    data.set("driver.parameter.subdriver", "fabula");
    TEST_ASSERT_FALSE(UpsProfiles::matches(*leo, data));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_decode_string_descriptor);
    RUN_TEST(test_q1_on_mains);
    RUN_TEST(test_q1_on_battery_low);
    RUN_TEST(test_q1_boost_trim_and_alarm);
    RUN_TEST(test_q1_rejects_other_replies);
    RUN_TEST(test_q1_battery_packs);
    RUN_TEST(test_f_and_charge_leo);
    RUN_TEST(test_charge_smartbox_on_battery);
    RUN_TEST(test_f_smartbox_blank_fields);
    RUN_TEST(test_f_blank_from_a_waking_smartbox);
    RUN_TEST(test_f_zeros_from_a_waking_bridge);
    RUN_TEST(test_no_ack);
    RUN_TEST(test_hunnox_lock_step_and_poll);
    RUN_TEST(test_ratings_asked_until_real);
    RUN_TEST(test_corrupt_or_missing_reply_leaves_data);
    RUN_TEST(test_fabula_asks_without_lock_step);
    RUN_TEST(test_reads_only);
    RUN_TEST(test_registry_picks_the_transport);
    RUN_TEST(test_profiles_exported);
    RUN_TEST(test_driver_names_the_leo);
    RUN_TEST(test_driver_names_the_smartbox);
    RUN_TEST(test_driver_without_a_fitting_profile);
    RUN_TEST(test_profile_settings_of_the_lion);
    RUN_TEST(test_match_numbers_and_text);
    return UNITY_END();
}
