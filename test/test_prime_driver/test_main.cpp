#include <unity.h>
#include "Ch340Init.h"
#include "DriverRegistry.h"
#include "IHidTransport.h"
#include "ISerialPort.h"
#include "ModbusRtu.h"
#include "PrimeDriver.h"
#include "PrimeProtocol.h"
#include "UPSData.h"
#include "UpsProfiles.h"
#include <deque>
#include <map>
#include <vector>

// A Tescom Prime behind its CH340, as the serial driver sees it: each request written is
// answered from the register tables below, unless a test drops, corrupts or delays it.
class MockPrime : public ISerialPort {
public:
    std::map<uint16_t, uint16_t> input;   // function 04
    std::map<uint16_t, uint16_t> holding; // function 03
    std::map<uint16_t, uint8_t> bits;     // function 02
    std::vector<std::vector<uint8_t>> writes;
    std::deque<uint8_t> rx;
    // Requests left unanswered / answered with a bad CRC / with another slave, from the next one
    int drop = 0;
    int corrupt = 0;
    int wrong_slave = 0;
    // Exception code for every request to this register (function 03/04 start)
    std::map<uint16_t, uint8_t> exceptions;
    uint32_t slept_ms = 0;
    uint32_t timeouts = 0;
    int flushes = 0;
    int answered = 0;
    int unanswered = 0;
    // Bytes handed out per read() at most (0: all), as a slow USB IN would
    size_t chunk = 0;

    bool write(const uint8_t* data, size_t len) override {
        writes.emplace_back(data, data + len);
        if (len != 8 || !ModbusRtu::checkCrc(data, len)) return true;
        if (drop > 0) { drop--; return true; }
        uint8_t fn = data[1];
        uint16_t start = (uint16_t)(data[2] << 8 | data[3]);
        uint16_t count = (uint16_t)(data[4] << 8 | data[5]);
        std::vector<uint8_t> reply = { data[0], fn };
        auto ex = exceptions.find(start);
        if (ex != exceptions.end() && fn != 0x02) {
            reply[1] = fn | 0x80;
            reply.push_back(ex->second);
        } else if (fn == 0x02) {
            uint8_t bytes = (uint8_t)((count + 7) / 8);
            reply.push_back(bytes);
            for (uint8_t b = 0; b < bytes; b++) {
                uint8_t v = 0;
                for (int i = 0; i < 8; i++) {
                    auto it = bits.find((uint16_t)(start + b * 8 + i));
                    if (it != bits.end() && it->second) v |= (uint8_t)(1 << i);
                }
                reply.push_back(v);
            }
        } else {
            auto& table = fn == 0x04 ? input : holding;
            reply.push_back((uint8_t)(2 * count));
            for (uint16_t i = 0; i < count; i++) {
                auto it = table.find((uint16_t)(start + i));
                uint16_t v = it != table.end() ? it->second : 0;
                reply.push_back((uint8_t)(v >> 8));
                reply.push_back((uint8_t)(v & 0xFF));
            }
        }
        uint16_t crc = ModbusRtu::crc16(reply.data(), reply.size());
        reply.push_back((uint8_t)(crc & 0xFF));
        reply.push_back((uint8_t)(crc >> 8));
        if (corrupt > 0) { corrupt--; reply[3] ^= 0x55; }
        if (wrong_slave > 0) { wrong_slave--; reply[0] = 7; }
        rx.insert(rx.end(), reply.begin(), reply.end());
        return true;
    }

    size_t read(uint8_t* buf, size_t len, uint32_t timeout_ms) override {
        size_t n = 0;
        while (n < len && !rx.empty() && (chunk == 0 || n < chunk)) {
            buf[n++] = rx.front();
            rx.pop_front();
        }
        // Chunks keep coming within the timeout
        while (chunk && n < len && !rx.empty()) {
            buf[n++] = rx.front();
            rx.pop_front();
        }
        if (n < len) timeouts++;
        return n;
    }

    void flushInput() override {
        flushes++;
        rx.clear();
    }
    void sleepMs(uint32_t ms) override { slept_ms += ms; }
    void noteExchange(bool ok) override { (ok ? answered : unanswered)++; }

    // Function and start of each request, for the order of the poll
    std::vector<std::pair<uint8_t, uint16_t>> requests() const {
        std::vector<std::pair<uint8_t, uint16_t>> v;
        for (const auto& w : writes) v.push_back({ w[1], (uint16_t)(w[2] << 8 | w[3]) });
        return v;
    }
};

class MockHost : public IHidTransport {
public:
    bool paused = false;
    HIDParser _parser;
    std::vector<HIDUsageDef> _usages;
    void lock() const override {}
    void unlock() const override {}
    const HIDParser* getHIDParser() const override { return &_parser; }
    const std::vector<HIDUsageDef>& getUsages() const override { return _usages; }
    String getActiveBeeperPath() const override { return ""; }
    uint32_t getQuirks() const override { return 0; }
    bool isPollingPaused() const override { return paused; }
    bool requestReport(uint8_t, uint8_t, uint16_t) override { return false; }
    bool requestStringDescriptor(uint8_t) override { return false; }
};

static MockPrime ups;
static MockHost host;
static UPSData data;
static uint16_t regs[Prime::INPUT_REGS];
static uint8_t bit[Prime::STATUS_BITS];

// The Prime 1102 XL of nut-app docs/PRIME-MODBUS.md, 2026-11-03: online on mains, 78 %
// charged and charging, the ~20 W monitor as the only load
static void primeOnline() {
    ups.input = {
        { 0x05, 2314 }, { 0x06, 500 }, { 0x07, 2300 }, { 0x08, 500 }, { 0x0A, 514 }, { 0x0B, 1 },
        { 0x0C, 200 }, { 0x0E, 23 }, { 0x10, 10 }, { 0x24, 24 }, { 0x2E, 78 }, { 0x33, 2300 },
        { 0x34, 2314 }, { 0x36, 514 }, { 0x37, 3800 }, { 0x3E, 4 }, { 0x3F, 1 }, { 0x47, 2 }, { 0x48, 10 },
    };
    ups.bits = { { 121, 1 } };
    // "EA900_1-3K_G4", high byte first; beeper sounding
    ups.holding = {
        { 0x0A, 230 }, { 0x13, 0 },
        { 0xA1, 0x4541 }, { 0xA2, 0x3930 }, { 0xA3, 0x305F }, { 0xA4, 0x312D },
        { 0xA5, 0x334B }, { 0xA6, 0x5F47 }, { 0xA7, 0x3400 },
    };
}

// Same unit on battery with the Mac on its 65 W charger added: 53 %, 0x0E 82 W, 0x0C 249 VA,
// 0x10 12 %, 0x0B 61 minutes; mains bits 27, 28, 33, 35 set
static void primeOnBattery() {
    primeOnline();
    ups.input[0x05] = 5;
    ups.input[0x06] = 400;
    ups.input[0x08] = 499;
    ups.input[0x0A] = 506;
    ups.input[0x0B] = 61;
    ups.input[0x0C] = 249;
    ups.input[0x0E] = 82;
    ups.input[0x10] = 12;
    ups.input[0x2E] = 53;
    ups.input[0x34] = 5;
    ups.input[0x3E] = 1;
    ups.input[0x47] = 0;
    ups.input[0x48] = 9;
    ups.bits = { { 122, 1 }, { 27, 1 }, { 28, 1 }, { 33, 1 }, { 35, 1 } };
}

// The tables as the decoder gets them after a full read
static void loadTables() {
    for (uint16_t i = 0; i < Prime::INPUT_REGS; i++) {
        auto it = ups.input.find(i);
        regs[i] = it != ups.input.end() ? it->second : 0;
    }
    for (uint16_t i = 0; i < Prime::STATUS_BITS; i++) {
        auto it = ups.bits.find(i);
        bit[i] = it != ups.bits.end() ? it->second : 0;
    }
}

// Runs the driver's poll steps at time now until it stops asking (one request per call)
static void runPoll(PrimeDriver& driver, uint32_t now, int max_calls = 20) {
    for (int i = 0; i < max_calls; i++) {
        size_t before = ups.writes.size();
        driver.loop(&host, data, now);
        if (ups.writes.size() == before && i > 1) break;
    }
}

void setUp(void) {
    ups = MockPrime();
    host = MockHost();
    data = UPSData();
    memset(regs, 0, sizeof(regs));
    memset(bit, 0, sizeof(bit));
}

void tearDown(void) {}

static std::string status() { return UPSData::computeUPSStatusString(data).c_str(); }

// --- ModbusRtu ---

void test_crc16_known_frames(void) {
    // The command frames of PRIME-MODBUS.md, CRC low byte first
    const uint8_t quick_test[] = { 0x01, 0x06, 0xF0, 0x09, 0x00, 0x02 };
    TEST_ASSERT_EQUAL_HEX16(0x09EB, ModbusRtu::crc16(quick_test, 6));
    const uint8_t inverter_off[] = { 0x01, 0x06, 0xF0, 0x01, 0x00, 0x02 };
    TEST_ASSERT_EQUAL_HEX16(0xCB6A, ModbusRtu::crc16(inverter_off, 6));
    const uint8_t test_1min[] = { 0x01, 0x06, 0xF0, 0x09, 0x01, 0x05, 0xAB, 0x5B };
    TEST_ASSERT_TRUE(ModbusRtu::checkCrc(test_1min, sizeof(test_1min)));
    const uint8_t bad[] = { 0x01, 0x06, 0xF0, 0x09, 0x01, 0x05, 0xAB, 0x5C };
    TEST_ASSERT_FALSE(ModbusRtu::checkCrc(bad, sizeof(bad)));
    // Textbook frame: read holding register 0, slave 1
    const uint8_t read1[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x01 };
    TEST_ASSERT_EQUAL_HEX16(0x0A84, ModbusRtu::crc16(read1, 6));
}

void test_build_read_frames(void) {
    uint8_t f[8];
    TEST_ASSERT_EQUAL_UINT(8, ModbusRtu::buildRead(1, 0x04, 0x0010, 16, f));
    const uint8_t want[] = { 0x01, 0x04, 0x00, 0x10, 0x00, 0x10 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, 6);
    TEST_ASSERT_TRUE(ModbusRtu::checkCrc(f, 8));
    TEST_ASSERT_EQUAL_UINT(8, ModbusRtu::buildRead(1, 0x03, 0x00A1, 7, f));
    TEST_ASSERT_EQUAL_HEX8(0xA1, f[3]);
    TEST_ASSERT_EQUAL_HEX8(7, f[5]);
    TEST_ASSERT_EQUAL_UINT(8, ModbusRtu::buildRead(1, 0x02, 64, 64, f));
    // What read_regs / read_bits refuse, and the writes this port does not send
    TEST_ASSERT_EQUAL_UINT(0, ModbusRtu::buildRead(1, 0x04, 0, 33, f));
    TEST_ASSERT_EQUAL_UINT(0, ModbusRtu::buildRead(1, 0x02, 0, 65, f));
    TEST_ASSERT_EQUAL_UINT(0, ModbusRtu::buildRead(1, 0x04, 0, 0, f));
    TEST_ASSERT_EQUAL_UINT(0, ModbusRtu::buildRead(1, 0x06, 0x13, 1, f));
    TEST_ASSERT_EQUAL_UINT(37, ModbusRtu::replyLength(0x04, 16));
    TEST_ASSERT_EQUAL_UINT(13, ModbusRtu::replyLength(0x02, 64));
}

void test_decode_reply_frames(void) {
    uint8_t reply[] = { 0x01, 0x04, 0x04, 0x09, 0x0A, 0x01, 0xF4, 0, 0 };
    uint16_t crc = ModbusRtu::crc16(reply, 7);
    reply[7] = crc & 0xFF;
    reply[8] = crc >> 8;
    uint16_t out[2];
    TEST_ASSERT_TRUE(ModbusRtu::decodeRegisters(reply, sizeof(reply), 2, out));
    TEST_ASSERT_EQUAL_UINT16(2314, out[0]);
    TEST_ASSERT_EQUAL_UINT16(500, out[1]);
    // Byte count other than 2 x count: read_regs() fails it
    TEST_ASSERT_FALSE(ModbusRtu::decodeRegisters(reply, sizeof(reply), 1, out));

    // Bits 120-127 of the second block: online (bit 121) is bit 1 of byte 8 of bits 64-127
    uint8_t b[] = { 0x01, 0x02, 0x01, 0x02, 0, 0 };
    uint8_t bits[8];
    TEST_ASSERT_TRUE(ModbusRtu::decodeBits(b, sizeof(b), 8, bits));
    TEST_ASSERT_EQUAL_UINT8(0, bits[0]);
    TEST_ASSERT_EQUAL_UINT8(1, bits[1]);
    TEST_ASSERT_EQUAL_UINT8(0, bits[2]);
}

void test_master_reads_registers(void) {
    primeOnline();
    ModbusRtu::Master m(ups);
    uint16_t out[16];
    TEST_ASSERT_EQUAL(ModbusRtu::Result::OK, m.readRegisters(0x04, 0x00, 16, out));
    TEST_ASSERT_EQUAL_UINT16(2314, out[0x05]);
    TEST_ASSERT_EQUAL_UINT16(514, out[0x0A]);
    TEST_ASSERT_EQUAL_UINT(1, ups.writes.size());
    TEST_ASSERT_EQUAL_UINT8(1, m.lastTries());
    // 50 ms before each request, input flushed first
    TEST_ASSERT_EQUAL_UINT32(50, ups.slept_ms);
    TEST_ASSERT_EQUAL_INT(1, ups.flushes);
}

void test_master_reply_in_pieces(void) {
    // A 37-byte reply comes as a 32-byte and a 5-byte USB packet: read as 3 + 34
    primeOnline();
    ups.chunk = 32;
    ModbusRtu::Master m(ups);
    uint16_t out[16];
    TEST_ASSERT_EQUAL(ModbusRtu::Result::OK, m.readRegisters(0x04, 0x20, 16, out));
    TEST_ASSERT_EQUAL_UINT16(78, out[0x2E - 0x20]);
}

void test_master_retries_after_timeout(void) {
    // "About one request in ten gets no reply, so a reader has to retry"
    primeOnline();
    ups.drop = 1;
    ModbusRtu::Master m(ups);
    uint16_t v = 0xFFFF;
    TEST_ASSERT_EQUAL(ModbusRtu::Result::OK, m.readRegisters(0x03, 0x13, 1, &v));
    TEST_ASSERT_EQUAL_UINT16(0, v);
    TEST_ASSERT_EQUAL_UINT(2, ups.writes.size());
    TEST_ASSERT_EQUAL_UINT8(2, m.lastTries());
    TEST_ASSERT_EQUAL_UINT32(1, ups.timeouts);
    // Gap before both requests, plus the pause after the missing header
    TEST_ASSERT_EQUAL_UINT32(150, ups.slept_ms);
}

void test_master_gives_up_after_three_tries(void) {
    primeOnline();
    ups.drop = 3;
    ModbusRtu::Master m(ups);
    uint16_t v;
    TEST_ASSERT_EQUAL(ModbusRtu::Result::NO_REPLY, m.readRegisters(0x03, 0x13, 1, &v));
    TEST_ASSERT_EQUAL_UINT(3, ups.writes.size());
    TEST_ASSERT_EQUAL_UINT8(3, m.lastTries());
}

void test_master_corrupt_frame_is_retried(void) {
    primeOnline();
    ups.corrupt = 1;
    ModbusRtu::Master m(ups);
    uint16_t out[16];
    TEST_ASSERT_EQUAL(ModbusRtu::Result::OK, m.readRegisters(0x04, 0x00, 16, out));
    TEST_ASSERT_EQUAL_UINT16(2314, out[0x05]);
    TEST_ASSERT_EQUAL_UINT(2, ups.writes.size());
    // Every try corrupt: no reply, never a wrong value
    ups.writes.clear();
    ups.corrupt = 3;
    memset(out, 0, sizeof(out));
    TEST_ASSERT_EQUAL(ModbusRtu::Result::NO_REPLY, m.readRegisters(0x04, 0x00, 16, out));
    TEST_ASSERT_EQUAL_UINT16(0, out[0x05]);
}

void test_master_wrong_slave_is_retried(void) {
    primeOnline();
    ups.wrong_slave = 1;
    ModbusRtu::Master m(ups);
    uint16_t v;
    TEST_ASSERT_EQUAL(ModbusRtu::Result::OK, m.readRegisters(0x03, 0x13, 1, &v));
    TEST_ASSERT_EQUAL_UINT(2, ups.writes.size());
}

void test_master_exception_not_retried(void) {
    // "Reads of bits 128-511 are refused"; the EA660 command block is refused too
    primeOnline();
    ups.exceptions[0x8000] = 2;
    ModbusRtu::Master m(ups);
    uint16_t v;
    TEST_ASSERT_EQUAL(ModbusRtu::Result::EXCEPTION, m.readRegisters(0x03, 0x8000, 1, &v));
    TEST_ASSERT_EQUAL_UINT8(2, m.lastException());
    TEST_ASSERT_EQUAL_UINT(1, ups.writes.size());
}

// --- PrimeProtocol: the readings of PRIME-MODBUS.md into NUT variables ---

void test_decode_online(void) {
    primeOnline();
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OL CHRG", status().c_str());
    TEST_ASSERT_EQUAL_STRING("231.4", data.get("input.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("50.0", data.get("input.frequency").c_str());
    TEST_ASSERT_EQUAL_STRING("231.4", data.get("input.bypass.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("230.0", data.get("output.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("50.0", data.get("output.frequency").c_str());
    TEST_ASSERT_EQUAL_STRING("51.4", data.get("battery.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("78", data.get("battery.charge").c_str());
    TEST_ASSERT_EQUAL_STRING("10", data.get("ups.load").c_str());
    TEST_ASSERT_EQUAL_STRING("23", data.get("ups.realpower").c_str());
    TEST_ASSERT_EQUAL_STRING("200", data.get("ups.power").c_str());
    TEST_ASSERT_EQUAL_STRING("24", data.get("ups.temperature").c_str());
    // 0x0B means something else on mains: no run time
    TEST_ASSERT_FALSE(data.hasKey("battery.runtime"));
}

void test_decode_charged_online(void) {
    primeOnline();
    ups.input[0x2E] = 100;
    ups.input[0x3E] = 1;
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OL", status().c_str());
    // Charging register up but full: no CHRG either (r[0x2E] < 100)
    ups.input[0x3E] = 4;
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OL", status().c_str());
}

void test_decode_on_battery(void) {
    primeOnBattery();
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OB DISCHRG", status().c_str());
    TEST_ASSERT_EQUAL_STRING("0.5", data.get("input.voltage").c_str());
    // Below 50 V in: no input frequency (0x06 reads 400 without mains)
    TEST_ASSERT_EQUAL_STRING("0.0", data.get("input.frequency").c_str());
    TEST_ASSERT_EQUAL_STRING("49.9", data.get("output.frequency").c_str());
    TEST_ASSERT_EQUAL_STRING("50.6", data.get("battery.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("53", data.get("battery.charge").c_str());
    TEST_ASSERT_EQUAL_STRING("3660", data.get("battery.runtime").c_str());
    TEST_ASSERT_EQUAL_STRING("12", data.get("ups.load").c_str());
    TEST_ASSERT_EQUAL_STRING("82", data.get("ups.realpower").c_str());
    TEST_ASSERT_EQUAL_STRING("249", data.get("ups.power").c_str());

    // Mains back: OL again, the run time goes
    primeOnline();
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OL CHRG", status().c_str());
    TEST_ASSERT_FALSE(data.hasKey("battery.runtime"));
}

void test_decode_low_battery(void) {
    primeOnBattery();
    ups.input[0x2E] = 21;
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OB DISCHRG", status().c_str());
    ups.input[0x2E] = 20; // at or below lowbatt
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OB DISCHRG LB", status().c_str());
}

void test_decode_bypass_and_standby(void) {
    // Bypass, checked with the screen: 0x48 = 8, bit 123, battery full at 54.2 V
    primeOnline();
    ups.input[0x05] = 2310;
    ups.input[0x07] = 2310;
    ups.input[0x0A] = 542;
    ups.input[0x2E] = 100;
    ups.input[0x33] = 10;
    ups.input[0x34] = 2310;
    ups.input[0x48] = 8;
    ups.bits = { { 123, 1 } };
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OL BYPASS", status().c_str());
    TEST_ASSERT_EQUAL_STRING("231.0", data.get("output.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("100", data.get("battery.charge").c_str());

    // Standby without mains (bit 120): OFF alone
    ups.bits = { { 120, 1 } };
    ups.input[0x48] = 0;
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OFF", status().c_str());
}

void test_decode_mode_register_while_starting(void) {
    // No status bit set yet: the mode register 0x48 decides
    primeOnBattery();
    ups.bits.clear();
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OB DISCHRG", status().c_str());
    primeOnline();
    ups.bits.clear();
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OL CHRG", status().c_str());
    ups.input[0x48] = 8;
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OL BYPASS", status().c_str());
    ups.input[0x48] = 0;
    loadTables();
    Prime::decode(regs, bit, data);
    TEST_ASSERT_EQUAL_STRING("OFF", status().c_str());
}

void test_decode_model_and_beeper(void) {
    const uint16_t model[] = { 0x4541, 0x3930, 0x305F, 0x312D, 0x334B, 0x5F47, 0x3400 };
    char text[15];
    Prime::decodeModel(model, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("EA900_1-3K_G4", text);
    Prime::decodeBeeper(1, data);
    TEST_ASSERT_EQUAL_STRING("muted", data.get("ups.beeper.status").c_str());
    Prime::decodeBeeper(0, data);
    TEST_ASSERT_EQUAL_STRING("enabled", data.get("ups.beeper.status").c_str());
}

void test_static_info(void) {
    Prime::setStaticInfo(data);
    TEST_ASSERT_EQUAL_STRING("Tescom", data.get("device.mfr").c_str());
    TEST_ASSERT_EQUAL_STRING("Prime", data.get("device.model").c_str());
    TEST_ASSERT_EQUAL_STRING("ups", data.get("device.type").c_str());
    TEST_ASSERT_EQUAL_STRING("online", data.get("ups.type").c_str());
    TEST_ASSERT_EQUAL_STRING("48", data.get("battery.voltage.nominal").c_str());
    TEST_ASSERT_EQUAL_STRING("230", data.get("input.voltage.nominal").c_str());
    TEST_ASSERT_EQUAL_STRING("230", data.get("output.voltage.nominal").c_str());
    TEST_ASSERT_EQUAL_STRING("2000", data.get("ups.power.nominal").c_str());
    TEST_ASSERT_EQUAL_STRING("2000", data.get("ups.realpower.nominal").c_str());
    TEST_ASSERT_EQUAL_STRING("20", data.get("battery.charge.low").c_str());
    TEST_ASSERT_EQUAL_STRING("No test initiated", data.get("ups.test.result").c_str());
}

// --- PrimeDriver ---

void test_driver_full_poll(void) {
    primeOnline();
    PrimeDriver driver(&ups);
    driver.setup();
    TEST_ASSERT_FALSE(driver.initialPollDone());
    runPoll(driver, 1000);
    TEST_ASSERT_TRUE(driver.initialPollDone());

    // Model once, input 0x00-0x4F by 16, bits 0-127 by 64, then the beeper
    std::vector<std::pair<uint8_t, uint16_t>> want = {
        { 0x03, 0xA1 }, { 0x04, 0x00 }, { 0x04, 0x10 }, { 0x04, 0x20 }, { 0x04, 0x30 },
        { 0x04, 0x40 }, { 0x02, 0 }, { 0x02, 64 }, { 0x03, 0x13 },
    };
    auto got = ups.requests();
    TEST_ASSERT_EQUAL_UINT(want.size(), got.size());
    for (size_t i = 0; i < want.size(); i++) {
        TEST_ASSERT_EQUAL_HEX8(want[i].first, got[i].first);
        TEST_ASSERT_EQUAL_HEX16(want[i].second, got[i].second);
    }
    TEST_ASSERT_EQUAL_INT(0, ups.unanswered);

    TEST_ASSERT_EQUAL_STRING("tescom_prime", data.get("driver.name").c_str());
    TEST_ASSERT_EQUAL_STRING("0.04", data.get("driver.version.internal").c_str());
    TEST_ASSERT_EQUAL_STRING("2", data.get("driver.parameter.pollinterval").c_str());
    TEST_ASSERT_NOT_NULL(driver.profile());
    TEST_ASSERT_EQUAL_STRING("prime", driver.profile()->key);
    TEST_ASSERT_EQUAL_STRING("Tescom", data.get("ups.mfr").c_str());
    TEST_ASSERT_EQUAL_STRING("Prime", data.get("ups.model").c_str());
    TEST_ASSERT_EQUAL_STRING("Tescom", data.get("device.mfr").c_str());
    TEST_ASSERT_EQUAL_STRING("Prime", data.get("device.model").c_str());
    TEST_ASSERT_EQUAL_STRING("EA900_1-3K_G4", data.get("ups.firmware.aux").c_str());
    TEST_ASSERT_EQUAL_STRING("enabled", data.get("ups.beeper.status").c_str());
    TEST_ASSERT_EQUAL_STRING("OL CHRG", status().c_str());
    TEST_ASSERT_EQUAL_STRING("231.4", data.get("input.voltage").c_str());
    TEST_ASSERT_EQUAL_STRING("78", data.get("battery.charge").c_str());
}

void test_driver_polls_every_two_seconds(void) {
    primeOnline();
    PrimeDriver driver(&ups);
    driver.setup();
    runPoll(driver, 1000);
    size_t first = ups.writes.size();
    // Within pollinterval: nothing asked
    runPoll(driver, 2999);
    TEST_ASSERT_EQUAL_UINT(first, ups.writes.size());
    // Next poll: the model is not asked again
    ups.input[0x2E] = 79;
    runPoll(driver, 3000);
    TEST_ASSERT_EQUAL_UINT(first + 8, ups.writes.size());
    TEST_ASSERT_EQUAL_STRING("79", data.get("battery.charge").c_str());
}

void test_driver_no_reply_ends_the_poll(void) {
    primeOnline();
    PrimeDriver driver(&ups);
    driver.setup();
    // Model answered, then the second input block gets nothing on all three tries
    driver.loop(&host, data, 1000); // model
    driver.loop(&host, data, 1000); // 0x00
    ups.drop = 3;
    driver.loop(&host, data, 1000); // 0x10
    TEST_ASSERT_EQUAL_INT(1, ups.unanswered);
    TEST_ASSERT_FALSE(driver.initialPollDone());
    // Nothing committed from a half-read poll
    TEST_ASSERT_FALSE(data.hasKey("input.voltage"));
    TEST_ASSERT_FALSE(data.hasKey("driver.name"));
    size_t n = ups.writes.size();
    driver.loop(&host, data, 1500);
    TEST_ASSERT_EQUAL_UINT(n, ups.writes.size()); // waits for the next poll
    // UPS back: a full poll from the top
    runPoll(driver, 3000);
    TEST_ASSERT_TRUE(driver.initialPollDone());
    TEST_ASSERT_EQUAL_STRING("OL CHRG", status().c_str());
}

void test_driver_values_stay_on_a_failed_poll(void) {
    // A later failed poll leaves the last readings: the host's link monitor makes them stale
    primeOnline();
    PrimeDriver driver(&ups);
    driver.setup();
    runPoll(driver, 1000);
    primeOnBattery();
    ups.drop = 100;
    runPoll(driver, 3000);
    TEST_ASSERT_TRUE(ups.unanswered >= 1);
    TEST_ASSERT_EQUAL_STRING("OL CHRG", status().c_str());
}

void test_driver_corrupt_frame_recovers_in_the_request(void) {
    primeOnBattery();
    PrimeDriver driver(&ups);
    driver.setup();
    ups.corrupt = 2; // model reply and its first retry
    runPoll(driver, 1000);
    TEST_ASSERT_TRUE(driver.initialPollDone());
    TEST_ASSERT_EQUAL_INT(0, ups.unanswered);
    TEST_ASSERT_EQUAL_STRING("EA900_1-3K_G4", data.get("ups.firmware.aux").c_str());
    TEST_ASSERT_EQUAL_STRING("OB DISCHRG", status().c_str());
}

void test_driver_model_and_beeper_failures_are_not_fatal(void) {
    primeOnline();
    ups.exceptions[0xA1] = 2;
    ups.exceptions[0x13] = 2;
    PrimeDriver driver(&ups);
    driver.setup();
    runPoll(driver, 1000);
    TEST_ASSERT_TRUE(driver.initialPollDone());
    TEST_ASSERT_FALSE(data.hasKey("ups.firmware.aux"));
    TEST_ASSERT_FALSE(data.hasKey("ups.beeper.status"));
    TEST_ASSERT_EQUAL_STRING("OL CHRG", status().c_str());
    // Asked again on the next polls, MODEL_TRIES in all
    runPoll(driver, 3000);
    runPoll(driver, 5000);
    runPoll(driver, 7000);
    int model_reads = 0;
    for (const auto& r : ups.requests()) model_reads += r.second == 0xA1;
    TEST_ASSERT_EQUAL_INT(PrimeDriver::MODEL_TRIES, model_reads);
}

void test_driver_waits_while_polling_is_paused(void) {
    primeOnline();
    PrimeDriver driver(&ups);
    driver.setup();
    host.paused = true;
    runPoll(driver, 1000);
    TEST_ASSERT_EQUAL_UINT(0, ups.writes.size());
}

void test_driver_reads_only(void) {
    primeOnline();
    PrimeDriver driver(&ups);
    driver.setup();
    runPoll(driver, 1000);
    runPoll(driver, 3000);
    for (const auto& w : ups.writes) {
        TEST_ASSERT_TRUE(w[1] == 0x02 || w[1] == 0x03 || w[1] == 0x04);
    }
    TEST_ASSERT_FALSE(driver.beeperControllable());
    TEST_ASSERT_FALSE(driver.shutdownCommandsSupported());
    TEST_ASSERT_FALSE(driver.delayVariablesSupported());
}

void test_registry_serial_path(void) {
    TEST_ASSERT_TRUE(DriverRegistry::isSerial(0x1A86, 0x7523));
    TEST_ASSERT_FALSE(DriverRegistry::isSerial(0x0001, 0x0000));
    TEST_ASSERT_FALSE(DriverRegistry::isSerial(0x051D, 0x0002));
    IUPSDriver* d = DriverRegistry::createSerial(0x1A86, 0x7523, &ups);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_NOT_NULL(dynamic_cast<PrimeDriver*>(d));
    delete d;
    TEST_ASSERT_NULL(DriverRegistry::createSerial(0x0001, 0x0000, &ups));
    TEST_ASSERT_NULL(DriverRegistry::createSerial(0x1A86, 0x7523, nullptr));
    // The HID table is not changed by it
    d = DriverRegistry::create(0x1A86, 0x7523);
    TEST_ASSERT_NULL(dynamic_cast<PrimeDriver*>(d));
    delete d;
}

// --- CH340 set-up (Linux ch341.c) ---

void test_ch340_divisor(void) {
    // 9600: prescaler 2, divisor 78 (0xB2), with or without the limited prescaler
    TEST_ASSERT_EQUAL_HEX32(0xB202, Ch340::divisor(9600));
    TEST_ASSERT_EQUAL_HEX32(0xB202, Ch340::divisor(9600, true));
    // The classic table values of the old driver
    TEST_ASSERT_EQUAL_HEX32(0xCC03, Ch340::divisor(115200));
    TEST_ASSERT_EQUAL_HEX32(0xD902, Ch340::divisor(19200));
}

void test_ch340_init_sequence(void) {
    Ch340::Request r[4];
    // A CH340 reporting version 0x31: divisor with bit 7, line control 8N1, DTR on RTS off
    size_t n = Ch340::serialInit(0x31, 9600, Ch340::LCR_8N1, Ch340::BIT_DTR, r, 4);
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_HEX8(0xA1, r[0].bRequest);
    TEST_ASSERT_EQUAL_HEX16(0, r[0].wValue);
    TEST_ASSERT_EQUAL_HEX16(0, r[0].wIndex);
    TEST_ASSERT_EQUAL_HEX8(0x9A, r[1].bRequest);
    TEST_ASSERT_EQUAL_HEX16(0x1312, r[1].wValue);
    TEST_ASSERT_EQUAL_HEX16(0xB282, r[1].wIndex);
    TEST_ASSERT_EQUAL_HEX8(0x9A, r[2].bRequest);
    TEST_ASSERT_EQUAL_HEX16(0x2518, r[2].wValue);
    TEST_ASSERT_EQUAL_HEX16(0x00C3, r[2].wIndex);
    TEST_ASSERT_EQUAL_HEX8(0xA4, r[3].bRequest);
    TEST_ASSERT_EQUAL_HEX16(0xFFDF, r[3].wValue);
    // Version 0x27: no bit 7, no line control register
    n = Ch340::serialInit(0x27, 9600, Ch340::LCR_8N1, Ch340::BIT_DTR, r, 4);
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_HEX16(0xB202, r[1].wIndex);
    TEST_ASSERT_EQUAL_HEX8(0xA4, r[2].bRequest);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_crc16_known_frames);
    RUN_TEST(test_build_read_frames);
    RUN_TEST(test_decode_reply_frames);
    RUN_TEST(test_master_reads_registers);
    RUN_TEST(test_master_reply_in_pieces);
    RUN_TEST(test_master_retries_after_timeout);
    RUN_TEST(test_master_gives_up_after_three_tries);
    RUN_TEST(test_master_corrupt_frame_is_retried);
    RUN_TEST(test_master_wrong_slave_is_retried);
    RUN_TEST(test_master_exception_not_retried);
    RUN_TEST(test_decode_online);
    RUN_TEST(test_decode_charged_online);
    RUN_TEST(test_decode_on_battery);
    RUN_TEST(test_decode_low_battery);
    RUN_TEST(test_decode_bypass_and_standby);
    RUN_TEST(test_decode_mode_register_while_starting);
    RUN_TEST(test_decode_model_and_beeper);
    RUN_TEST(test_static_info);
    RUN_TEST(test_driver_full_poll);
    RUN_TEST(test_driver_polls_every_two_seconds);
    RUN_TEST(test_driver_no_reply_ends_the_poll);
    RUN_TEST(test_driver_values_stay_on_a_failed_poll);
    RUN_TEST(test_driver_corrupt_frame_recovers_in_the_request);
    RUN_TEST(test_driver_model_and_beeper_failures_are_not_fatal);
    RUN_TEST(test_driver_waits_while_polling_is_paused);
    RUN_TEST(test_driver_reads_only);
    RUN_TEST(test_registry_serial_path);
    RUN_TEST(test_ch340_divisor);
    RUN_TEST(test_ch340_init_sequence);
    return UNITY_END();
}
