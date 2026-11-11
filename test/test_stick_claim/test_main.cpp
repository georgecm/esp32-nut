#include <unity.h>
#include <ArduinoJson.h>
#include "StickClaim.h"
#include <string>

using namespace StickClaim;

static const char* START_OK =
    "{\"userCode\":\"XFV7-S224\",\"verifyUrl\":\"https://bridge.connect.tescom.gr/?code=XFV7-S224\","
    "\"pollToken\":\"tok\",\"interval\":5,\"expiresIn\":900}";
static const char* POLL_OK =
    "{\"status\":\"approved\",\"deviceId\":\"dev-1\",\"name\":\"Leo LCD (stick)\",\"organization\":\"Shop SA\","
    "\"agentToken\":\"jwt\",\"zabbix\":{\"server\":\"zabbix.metagr.id\",\"port\":10051,\"host\":\"tc-3c91565e\"},"
    "\"tls\":{\"ca\":\"CA\",\"serverCa\":\"SCA\",\"cert\":\"CERT\"}}";

void setUp(void) {}
void tearDown(void) {}

void test_start_body(void) {
    StartRequest r;
    r.mac = "14:c1:9f:2c:49:c0";
    r.model = "Leo LCD";
    r.fw_version = "1.13.0";
    r.csr_pem = "-----BEGIN CERTIFICATE REQUEST-----\nX\n-----END CERTIFICATE REQUEST-----\n";
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, startBody(r)));
    TEST_ASSERT_EQUAL_STRING("tescom", doc["tenant"]);
    TEST_ASSERT_EQUAL_STRING("14:c1:9f:2c:49:c0", doc["mac"]);
    TEST_ASSERT_EQUAL_STRING("Leo LCD", doc["model"]);
    TEST_ASSERT_FALSE(doc["serial"].is<const char*>()); // left out when unknown
    TEST_ASSERT_EQUAL_STRING(r.csr_pem.c_str(), doc["csr"]);
    TEST_ASSERT_EQUAL_STRING("{\"pollToken\":\"a\\\"b\"}", pollBody("a\"b").c_str());
}

void test_starts_at_once_then_polls_on_interval(void) {
    Flow f;
    TEST_ASSERT_TRUE(f.due(0));
    TEST_ASSERT_TRUE(f.onStart(201, START_OK, 1000));
    TEST_ASSERT_EQUAL(State::PENDING, f.state());
    TEST_ASSERT_EQUAL_STRING("XFV7-S224", f.userCode().c_str());
    TEST_ASSERT_EQUAL_STRING("tok", f.pollToken().c_str());
    TEST_ASSERT_EQUAL_UINT32(900, f.expiresInS(1000));
    TEST_ASSERT_FALSE(f.due(5999));
    TEST_ASSERT_TRUE(f.due(6000));
    TEST_ASSERT_FALSE(f.onPoll(202, "{\"status\":\"pending\",\"expiresIn\":800}", 6000));
    TEST_ASSERT_EQUAL(State::PENDING, f.state());
    TEST_ASSERT_EQUAL_UINT32(800, f.expiresInS(6000));
    TEST_ASSERT_FALSE(f.due(10999));
    TEST_ASSERT_TRUE(f.due(11000));
}

void test_approved_keeps_the_credentials(void) {
    Flow f;
    f.onStart(201, START_OK, 0);
    TEST_ASSERT_TRUE(f.onPoll(200, POLL_OK, 5000));
    TEST_ASSERT_EQUAL(State::CLAIMED, f.state());
    const Credentials& c = f.credentials();
    TEST_ASSERT_EQUAL_STRING("dev-1", c.device_id.c_str());
    TEST_ASSERT_EQUAL_STRING("jwt", c.agent_token.c_str());
    TEST_ASSERT_EQUAL_STRING("zabbix.metagr.id", c.zabbix_server.c_str());
    TEST_ASSERT_EQUAL_UINT16(10051, c.zabbix_port);
    TEST_ASSERT_EQUAL_STRING("tc-3c91565e", c.zabbix_host.c_str());
    TEST_ASSERT_EQUAL_STRING("SCA", c.server_ca_pem.c_str());
    TEST_ASSERT_EQUAL_STRING("CERT", c.cert_pem.c_str());
    TEST_ASSERT_EQUAL_STRING("", f.userCode().c_str());
    // A claimed stick never calls start or poll again
    TEST_ASSERT_FALSE(f.due(1000000));
}

void test_incomplete_approval_is_not_kept(void) {
    Flow f;
    f.onStart(201, START_OK, 0);
    TEST_ASSERT_FALSE(f.onPoll(200, "{\"status\":\"approved\",\"deviceId\":\"d\"}", 5000));
    TEST_ASSERT_EQUAL(State::PENDING, f.state());
    TEST_ASSERT_EQUAL_STRING("the cloud's answer is missing credentials", f.lastError().c_str());
}

void test_expired_code_starts_again(void) {
    Flow f;
    f.onStart(201, START_OK, 0);
    TEST_ASSERT_FALSE(f.onPoll(410, "{\"error\":\"the code expired; start again\"}", 5000));
    TEST_ASSERT_EQUAL(State::UNCLAIMED, f.state());
    TEST_ASSERT_EQUAL_STRING("the code expired; start again", f.lastError().c_str());
    TEST_ASSERT_TRUE(f.due(5000));

    f.onStart(201, START_OK, 10000);
    f.expireIfNeeded(10000 + 899000);
    TEST_ASSERT_EQUAL(State::PENDING, f.state());
    f.expireIfNeeded(10000 + 900000);
    TEST_ASSERT_EQUAL(State::UNCLAIMED, f.state());
}

void test_refused_start_backs_off(void) {
    Flow f;
    TEST_ASSERT_FALSE(f.onStart(429, "{\"error\":\"too many codes\"}", 0));
    TEST_ASSERT_EQUAL_STRING("too many codes", f.lastError().c_str());
    TEST_ASSERT_FALSE(f.due(9999));
    TEST_ASSERT_TRUE(f.due(10000));
    f.onNetworkError("could not connect", 10000);
    TEST_ASSERT_FALSE(f.due(29999));
    TEST_ASSERT_TRUE(f.due(30000));
    // Not JSON at all (a captive portal of a hotel Wi-Fi)
    TEST_ASSERT_FALSE(f.onStart(200, "<html>log in</html>", 30000));
    TEST_ASSERT_EQUAL(State::UNCLAIMED, f.state());
}

void test_restore_and_reset(void) {
    Credentials c;
    c.device_id = "d";
    c.agent_token = "t";
    c.zabbix_server = "z";
    c.zabbix_host = "h";
    c.server_ca_pem = "s";
    c.cert_pem = "c";
    Flow f;
    f.restore(c);
    TEST_ASSERT_EQUAL(State::CLAIMED, f.state());
    f.reset();
    TEST_ASSERT_EQUAL(State::UNCLAIMED, f.state());
    TEST_ASSERT_TRUE(f.due(0));
}

void test_heartbeat_body(void) {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, heartbeatBody("1.13.0", 1700000000000ULL, "ok", "OL CHRG")));
    TEST_ASSERT_EQUAL_STRING("1.13.0", doc["agentVersion"]);
    TEST_ASSERT_EQUAL_STRING("ok", doc["status"]["link"]);
    TEST_ASSERT_EQUAL_STRING("OL CHRG", doc["status"]["upsStatus"]);
    TEST_ASSERT_TRUE(doc["status"]["startedAtMs"].as<uint64_t>() == 1700000000000ULL);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_start_body);
    RUN_TEST(test_starts_at_once_then_polls_on_interval);
    RUN_TEST(test_approved_keeps_the_credentials);
    RUN_TEST(test_incomplete_approval_is_not_kept);
    RUN_TEST(test_expired_code_starts_again);
    RUN_TEST(test_refused_start_backs_off);
    RUN_TEST(test_restore_and_reset);
    RUN_TEST(test_heartbeat_body);
    return UNITY_END();
}
