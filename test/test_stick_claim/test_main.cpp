#include <unity.h>
#include <ArduinoJson.h>
#include "StickClaim.h"
#include <string>

using namespace StickClaim;

static const char* START_OK =
    "{\"userCode\":\"XFV7-S224\",\"verifyUrl\":\"https://connect.tescom.gr/?link-stick=XFV7-S224\","
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
    TEST_ASSERT_FALSE(doc["secret"].is<const char*>()); // no factory secret, none sent
    r.secret = "7JPMNKMIXTAO4PG5";
    TEST_ASSERT_FALSE(deserializeJson(doc, startBody(r)));
    TEST_ASSERT_EQUAL_STRING("7JPMNKMIXTAO4PG5", doc["secret"]);
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

void test_approved_without_zabbix_yet(void) {
    // The UPS behind the stick is not added (or has no plan): no Zabbix, still claimed
    Flow f;
    f.onStart(201, START_OK, 0);
    TEST_ASSERT_TRUE(f.onPoll(200,
                              "{\"status\":\"approved\",\"deviceId\":\"dev-1\",\"agentToken\":\"jwt\",\"zabbix\":null,"
                              "\"next\":\"ups\",\"manageUrl\":\"https://connect.tescom.gr/?stick=dev-1\","
                              "\"tls\":{\"ca\":\"CA\",\"serverCa\":\"SCA\",\"cert\":\"CERT\"}}",
                              5000));
    const Credentials& c = f.credentials();
    TEST_ASSERT_TRUE(c.zabbix().empty());
    TEST_ASSERT_EQUAL_STRING("ups", c.next.c_str());
    TEST_ASSERT_EQUAL_STRING("https://connect.tescom.gr/?stick=dev-1", c.manage_url.c_str());
}

void test_pre_approved_start_polls_soon(void) {
    Flow f;
    TEST_ASSERT_TRUE(f.onStart(201, "{\"userCode\":\"AAAA-BBBB\",\"pollToken\":\"t\",\"interval\":1,\"approved\":true}", 0));
    TEST_ASSERT_TRUE(f.preApproved());
    TEST_ASSERT_TRUE(f.due(1000));
    Flow g;
    g.onStart(201, "{\"userCode\":\"AAAA-BBBB\",\"pollToken\":\"t\",\"interval\":1}", 0);
    TEST_ASSERT_FALSE(g.preApproved());
    TEST_ASSERT_FALSE(g.due(1999)); // a plain code never polls faster than 2 s
}

void test_heartbeat_answer(void) {
    HeartbeatAnswer a;
    TEST_ASSERT_TRUE(parseHeartbeat("{\"ok\":true,\"zabbix\":{\"server\":\"z.example\",\"port\":10052,\"host\":\"tc-1\"},\"next\":\"sending\"}", a));
    TEST_ASSERT_TRUE(a.known);
    TEST_ASSERT_EQUAL_STRING("z.example", a.zabbix.server.c_str());
    TEST_ASSERT_EQUAL_UINT16(10052, a.zabbix.port);
    TEST_ASSERT_EQUAL_STRING("tc-1", a.zabbix.host.c_str());
    TEST_ASSERT_EQUAL_STRING("sending", a.next.c_str());
    TEST_ASSERT_TRUE(parseHeartbeat("{\"ok\":true,\"zabbix\":null,\"next\":\"plan\"}", a));
    TEST_ASSERT_TRUE(a.known);
    TEST_ASSERT_TRUE(a.zabbix.empty());
    TEST_ASSERT_EQUAL_STRING("plan", a.next.c_str());
    // An older cloud says nothing about Zabbix: keep what there is
    TEST_ASSERT_TRUE(parseHeartbeat("{\"ok\":true}", a));
    TEST_ASSERT_FALSE(a.known);
    TEST_ASSERT_FALSE(parseHeartbeat("not json", a));
}

void test_pending_code_survives_restart(void) {
    Flow f;
    PendingCode p;
    TEST_ASSERT_FALSE(f.pending(p, 0));  // nothing shown yet
    TEST_ASSERT_TRUE(f.onStart(201, START_OK, 1000));
    TEST_ASSERT_TRUE(f.pending(p, 61000));
    TEST_ASSERT_EQUAL_STRING("XFV7-S224", p.user_code.c_str());
    TEST_ASSERT_EQUAL_STRING("tok", p.poll_token.c_str());
    TEST_ASSERT_EQUAL_UINT32(840, p.expires_in_s);
    TEST_ASSERT_EQUAL_UINT32(5, p.interval_s);

    // After the restart: the same code, polled at once, the time left kept
    Flow g;
    TEST_ASSERT_TRUE(g.resume(p, 500));
    TEST_ASSERT_TRUE(g.state() == State::PENDING);
    TEST_ASSERT_EQUAL_STRING("XFV7-S224", g.userCode().c_str());
    TEST_ASSERT_EQUAL_STRING("https://connect.tescom.gr/?link-stick=XFV7-S224", g.verifyUrl().c_str());
    TEST_ASSERT_TRUE(g.due(500));
    TEST_ASSERT_EQUAL_UINT32(840, g.expiresInS(500));
    TEST_ASSERT_TRUE(g.onPoll(200, POLL_OK, 600));
    TEST_ASSERT_TRUE(g.state() == State::CLAIMED);
}

void test_pending_code_not_kept_when_useless(void) {
    Flow f;
    PendingCode p;
    // Approved at once (factory secret): no code to keep
    TEST_ASSERT_TRUE(f.onStart(201, "{\"userCode\":\"A\",\"pollToken\":\"t\",\"approved\":true}", 0));
    TEST_ASSERT_FALSE(f.pending(p, 10));
    // Expired, or incomplete: not resumed
    PendingCode old;
    old.user_code = "XFV7-S224";
    old.poll_token = "tok";
    old.expires_in_s = 0;
    Flow g;
    TEST_ASSERT_FALSE(g.resume(old, 0));
    TEST_ASSERT_TRUE(g.state() == State::UNCLAIMED);
    old.expires_in_s = 60;
    old.poll_token = "";
    TEST_ASSERT_FALSE(g.resume(old, 0));
    // A claimed stick keeps its credentials
    Flow h;
    TEST_ASSERT_TRUE(h.onStart(201, START_OK, 0));
    TEST_ASSERT_TRUE(h.onPoll(200, POLL_OK, 10));
    old.poll_token = "tok";
    TEST_ASSERT_FALSE(h.resume(old, 20));
    TEST_ASSERT_TRUE(h.state() == State::CLAIMED);
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
    RUN_TEST(test_approved_without_zabbix_yet);
    RUN_TEST(test_pre_approved_start_polls_soon);
    RUN_TEST(test_heartbeat_answer);
    RUN_TEST(test_pending_code_survives_restart);
    RUN_TEST(test_pending_code_not_kept_when_useless);
    return UNITY_END();
}
