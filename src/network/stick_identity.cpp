#include "network/stick_identity.h"
#include "core/app_logger.h"
#include <Preferences.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_csr.h>
#include <memory>

namespace {

const char* NVS_NAMESPACE = "stick";
const char* NVS_KEY = "key";

// A seeded DRBG for one operation; the ESP32's hardware RNG feeds the entropy
struct Drbg {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr;
    bool ok = false;
    Drbg() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr);
        static const char pers[] = "esp32-nut-stick";
        ok = mbedtls_ctr_drbg_seed(&ctr, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, sizeof(pers) - 1) == 0;
    }
    ~Drbg() {
        mbedtls_ctr_drbg_free(&ctr);
        mbedtls_entropy_free(&entropy);
    }
};

std::string makeKey() {
    Drbg drbg;
    if (!drbg.ok) return "";
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    std::string out;
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &drbg.ctr) == 0) {
        std::unique_ptr<unsigned char[]> buf(new unsigned char[800]);
        if (mbedtls_pk_write_key_pem(&pk, buf.get(), 800) == 0) out = (const char*)buf.get();
    }
    mbedtls_pk_free(&pk);
    return out;
}

} // namespace

namespace StickIdentity {

std::string privateKeyPem() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String saved = prefs.getString(NVS_KEY, "");
    prefs.end();
    if (saved.length()) return saved.c_str();

    std::string pem = makeKey();
    if (pem.empty()) {
        AppLogger::log("ERROR", "[STICK] Could not make the key pair");
        return "";
    }
    prefs.begin(NVS_NAMESPACE, false);
    bool ok = prefs.putString(NVS_KEY, pem.c_str()) == pem.size();
    prefs.end();
    if (!ok) {
        AppLogger::log("ERROR", "[STICK] Could not save the key pair");
        return "";
    }
    AppLogger::log("INFO", "[STICK] New key pair made");
    return pem;
}

std::string csrPem(const String& mac) {
    std::string key = privateKeyPem();
    if (key.empty()) return "";
    Drbg drbg;
    if (!drbg.ok) return "";

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_csr req;
    mbedtls_x509write_csr_init(&req);
    std::string out;
    String subject = "CN=stick-" + mac;
    subject.replace(":", "");
    if (mbedtls_pk_parse_key(&pk, (const unsigned char*)key.c_str(), key.size() + 1, nullptr, 0,
                             mbedtls_ctr_drbg_random, &drbg.ctr) == 0) {
        mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);
        mbedtls_x509write_csr_set_key(&req, &pk);
        if (mbedtls_x509write_csr_set_subject_name(&req, subject.c_str()) == 0) {
            std::unique_ptr<unsigned char[]> buf(new unsigned char[1024]);
            if (mbedtls_x509write_csr_pem(&req, buf.get(), 1024, mbedtls_ctr_drbg_random, &drbg.ctr) == 0) {
                out = (const char*)buf.get();
            }
        }
    }
    mbedtls_x509write_csr_free(&req);
    mbedtls_pk_free(&pk);
    if (out.empty()) AppLogger::log("ERROR", "[STICK] Could not make the certificate request");
    return out;
}

void erase() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.remove(NVS_KEY);
    prefs.end();
}

} // namespace StickIdentity
