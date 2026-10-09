/* Isolated loopback test fixture. Never use its clock or credentials in firmware. */
#include "dtls_probe.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <avsystem/commons/avs_crypto_pki.h>
#include <avsystem/commons/avs_prng.h>
#include <avsystem/commons/avs_socket.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/pem.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/timing.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#ifdef NDEBUG
#error "This test requires assertions to execute and validate its checks"
#endif

#define CHECK(call) do { \
    int result_ = (call); \
    if (result_) { \
        printf("PROBE: failure at line %d, code=%d\n", __LINE__, result_); \
        abort(); \
    } \
} while (0)

typedef struct {
    mbedtls_pk_context root_key, other_root_key, server_key, client_key;
    char root[1536], other_root[1536], server[3072], client[3072];
    char client_private[2048];
} credentials_t;

typedef struct {
    credentials_t *credentials;
    SemaphoreHandle_t ready, done;
    int suite;
    int handshake_result;
    uint32_t verify_flags;
    bool echoed;
} server_t;

static uint32_t client_verify_flags;

static void generate_key(mbedtls_pk_context *key) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    CHECK(psa_generate_key(&attributes, &id));
    mbedtls_pk_init(key);
    CHECK(mbedtls_pk_copy_from_psa(id, key));
    CHECK(psa_destroy_key(id));
    psa_reset_key_attributes(&attributes);
}

static void generate_rsa_key(mbedtls_pk_context *key) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attributes, 2048);
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    CHECK(psa_generate_key(&attributes, &id));
    mbedtls_pk_init(key);
    CHECK(mbedtls_pk_copy_from_psa(id, key));
    CHECK(psa_destroy_key(id));
    psa_reset_key_attributes(&attributes);
}

static void write_rsa_pkcs8(credentials_t *credentials) {
    unsigned char der[2048];
    int length = mbedtls_pk_write_key_der(&credentials->client_key, der, sizeof(der));
    assert(length > 256 && length + 26 < sizeof(der));
    /* Test-only PKCS#8 wrapper around the SDK's PKCS#1 RSA export. */
    unsigned char prefix[] = {
        0x30, 0x82, (unsigned char)((length + 22) >> 8), (unsigned char)(length + 22),
        0x02, 0x01, 0x00, 0x30, 0x0d, 0x06, 0x09,
        0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00,
        0x04, 0x82, (unsigned char)(length >> 8), (unsigned char)length
    };
    unsigned char *start = der + sizeof(der) - length - sizeof(prefix);
    memcpy(start, prefix, sizeof(prefix));
    size_t written;
    CHECK(mbedtls_pem_write_buffer("-----BEGIN PRIVATE KEY-----\n",
                                   "-----END PRIVATE KEY-----\n",
                                   start, length + sizeof(prefix),
                                   (unsigned char *)credentials->client_private,
                                   sizeof(credentials->client_private), &written));
    mbedtls_platform_zeroize(der, sizeof(der));
}

static void reject_mismatched_rsa_key(credentials_t *credentials) {
    avs_crypto_prng_ctx_t *prng = avs_crypto_prng_new(NULL, NULL);
    assert(prng);
    avs_net_certificate_info_t certificates = {
        .server_cert_validation = true,
        .trusted_certs = avs_crypto_certificate_chain_info_from_buffer(
                credentials->root, strlen(credentials->root)),
        .client_cert = avs_crypto_certificate_chain_info_from_buffer(
                credentials->client, strlen(credentials->client)),
        .client_key = avs_crypto_private_key_info_from_buffer(
                credentials->client_private, strlen(credentials->client_private), NULL)
    };
    avs_net_ssl_configuration_t configuration = {
        .security = avs_net_security_info_from_certificates(certificates),
        .prng_ctx = prng
    };
    avs_net_socket_t *socket = NULL;
    assert(avs_is_err(avs_net_dtls_socket_create(&socket, &configuration)));
    avs_net_socket_cleanup(&socket);
    avs_crypto_prng_free(&prng);
    puts("PROBE: reject mismatched PKCS#8 RSA private key PASS");
}

static void issue_certificate(char *output, size_t capacity,
                              mbedtls_pk_context *subject_key,
                              const char *subject,
                              mbedtls_pk_context *issuer_key,
                              const char *issuer, bool ca, bool expired) {
    mbedtls_x509write_cert certificate;
    mbedtls_x509write_crt_init(&certificate);
    mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&certificate, subject_key);
    mbedtls_x509write_crt_set_issuer_key(&certificate, issuer_key);
    CHECK(mbedtls_x509write_crt_set_subject_name(&certificate, subject));
    CHECK(mbedtls_x509write_crt_set_issuer_name(&certificate, issuer));
    unsigned char serial[16];
    CHECK(psa_generate_random(serial, sizeof(serial)));
    serial[0] = 1;
    CHECK(mbedtls_x509write_crt_set_serial_raw(&certificate, serial, sizeof(serial)));
    CHECK(mbedtls_x509write_crt_set_validity(&certificate,
                                          expired ? "20200101000000" : "20260101000000",
                                          expired ? "20210101000000" : "20270101000000"));
    CHECK(mbedtls_x509write_crt_set_basic_constraints(&certificate, ca, ca ? 1 : -1));
    CHECK(mbedtls_x509write_crt_set_key_usage(&certificate,
              ca ? MBEDTLS_X509_KU_KEY_CERT_SIGN : MBEDTLS_X509_KU_DIGITAL_SIGNATURE));
    memset(output, 0, capacity);
    CHECK(mbedtls_x509write_crt_pem(&certificate, (unsigned char *) output, capacity));
    mbedtls_x509write_crt_free(&certificate);
}

static void make_server_certificate(credentials_t *credentials, bool expired) {
    issue_certificate(credentials->server, sizeof(credentials->server),
                      &credentials->server_key, "CN=probe.local",
                      &credentials->root_key, "CN=Probe Root", false, expired);
    assert(strlen(credentials->server) + strlen(credentials->root) + 1
           <= sizeof(credentials->server));
    strcat(credentials->server, credentials->root);
}

static void server_task(void *argument) {
    server_t *server = argument;
    credentials_t *credentials = server->credentials;
    mbedtls_net_context listener, peer;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config configuration;
    mbedtls_x509_crt chain, trust;
    mbedtls_timing_delay_context timer = {0};
    mbedtls_net_init(&listener);
    mbedtls_net_init(&peer);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&configuration);
    mbedtls_x509_crt_init(&chain);
    mbedtls_x509_crt_init(&trust);
    CHECK(mbedtls_x509_crt_parse(&chain, (unsigned char *) credentials->server,
                                strlen(credentials->server) + 1));
    CHECK(mbedtls_x509_crt_parse(&trust, (unsigned char *) credentials->root,
                                strlen(credentials->root) + 1));
    CHECK(mbedtls_ssl_config_defaults(&configuration, MBEDTLS_SSL_IS_SERVER,
                                      MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT));
    int suites[] = {server->suite, 0};
    mbedtls_ssl_conf_ciphersuites(&configuration, suites);
    mbedtls_ssl_conf_authmode(&configuration, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&configuration, &trust, NULL);
    CHECK(mbedtls_ssl_conf_own_cert(&configuration, &chain, &credentials->server_key));
    /* A single loopback peer: cookie/DoS protection is not exercised by this fixture. */
    mbedtls_ssl_conf_dtls_cookies(&configuration, NULL, NULL, NULL);
    mbedtls_ssl_conf_handshake_timeout(&configuration, 1000, 3000);
    mbedtls_ssl_conf_read_timeout(&configuration, 3000);
    CHECK(mbedtls_ssl_setup(&ssl, &configuration));
    mbedtls_ssl_set_timer_cb(&ssl, &timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);
    CHECK(mbedtls_net_bind(&listener, "127.0.0.1", "15686", MBEDTLS_NET_PROTO_UDP));
    xSemaphoreGive(server->ready);
    CHECK(mbedtls_net_accept(&listener, &peer, NULL, 0, NULL));
    mbedtls_ssl_set_bio(&ssl, &peer, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);
    do {
        server->handshake_result = mbedtls_ssl_handshake(&ssl);
    } while (server->handshake_result == MBEDTLS_ERR_SSL_WANT_READ
             || server->handshake_result == MBEDTLS_ERR_SSL_WANT_WRITE);
    server->verify_flags = mbedtls_ssl_get_verify_result(&ssl);
    if (!server->handshake_result) {
        unsigned char data[1200];
        int length = mbedtls_ssl_read(&ssl, data, sizeof(data));
        if (length > 0) {
            server->echoed = mbedtls_ssl_write(&ssl, data, length) == length;
        }
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&configuration);
    mbedtls_x509_crt_free(&chain);
    mbedtls_x509_crt_free(&trust);
    mbedtls_net_free(&peer);
    mbedtls_net_free(&listener);
    xSemaphoreGive(server->done);
    vTaskDelete(NULL);
}

static int record_verification(void *argument, mbedtls_x509_crt *certificate,
                               int depth, uint32_t *flags) {
    (void) argument;
    (void) certificate;
    (void) depth;
    client_verify_flags |= *flags;
    return 0; /* Preserve all verification flags; never override rejection. */
}

static int configure_client(void *configuration) {
    mbedtls_ssl_conf_verify(configuration, record_verification, NULL);
    return 0;
}

static void run_case(credentials_t *credentials, const char *name, int suite,
                     bool wrong_root, bool wrong_hostname,
                     uint32_t expected_client_flags, bool reject_client) {
    server_t server = {
        .credentials = credentials,
        .ready = xSemaphoreCreateBinary(),
        .done = xSemaphoreCreateBinary(),
        .suite = suite
    };
    assert(server.ready && server.done);
    avs_crypto_prng_ctx_t *prng = avs_crypto_prng_new(NULL, NULL);
    assert(prng);
    const char *root = wrong_root ? credentials->other_root : credentials->root;
    avs_net_certificate_info_t certificates = {
        .server_cert_validation = true,
        .ignore_system_trust_store = true,
        /* Omit the trailing NUL to match the application's certificate loader. */
        .trusted_certs = avs_crypto_certificate_chain_info_from_buffer(root, strlen(root)),
        .client_cert = avs_crypto_certificate_chain_info_from_buffer(
            credentials->client, strlen(credentials->client)),
        .client_key = avs_crypto_private_key_info_from_buffer(
            credentials->client_private, strlen(credentials->client_private), NULL)
    };
    uint32_t suites[] = {(uint32_t) suite};
    const avs_net_dtls_handshake_timeouts_t timeouts = {
        .min = {.seconds = 1}, .max = {.seconds = 3}
    };
    avs_net_ssl_configuration_t configuration = {
        .version = AVS_NET_SSL_VERSION_TLSv1_2,
        .security = avs_net_security_info_from_certificates(certificates),
        .prng_ctx = prng,
        .dtls_handshake_timeouts = &timeouts,
        .server_name_indication = wrong_hostname ? "wrong.local" : "probe.local",
        .additional_configuration_clb = configure_client,
        .ciphersuites = {.ids = suites, .num_ids = 1},
        .backend_configuration = {.address_family = AVS_NET_AF_INET4, .forced_mtu = 1200}
    };
    client_verify_flags = 0;
    avs_net_socket_t *socket = NULL;
    assert(avs_is_ok(avs_net_dtls_socket_create(&socket, &configuration)));
    assert(xTaskCreate(server_task, "dtls_fixture", 16384, &server, 5, NULL) == pdPASS);
    assert(xSemaphoreTake(server.ready, pdMS_TO_TICKS(10000)) == pdTRUE);
    avs_error_t result = avs_net_socket_connect(socket, "127.0.0.1", "15686");
    bool success = !expected_client_flags && !reject_client;
    assert(avs_is_ok(result) == success);
    if (success) {
        assert(!client_verify_flags);
        avs_net_socket_opt_value_t inner_mtu;
        assert(avs_is_ok(avs_net_socket_get_opt(socket, AVS_NET_SOCKET_OPT_INNER_MTU,
                                              &inner_mtu)));
        assert(inner_mtu.mtu > 1000 && inner_mtu.mtu < 1200);
        unsigned char payload[1200], reply[1200];
        for (size_t i = 0; i < sizeof(payload); ++i) {
            payload[i] = (unsigned char) i;
        }
        assert(avs_is_ok(avs_net_socket_send(socket, payload, inner_mtu.mtu)));
        size_t length = 0;
        assert(avs_is_ok(avs_net_socket_receive(socket, &length, reply, sizeof(reply))));
        assert(length == (size_t) inner_mtu.mtu && !memcmp(reply, payload, length));
        printf("PROBE: negotiated payload limit=%d bytes\n", inner_mtu.mtu);
    } else if (expected_client_flags) {
        assert((client_verify_flags & expected_client_flags) == expected_client_flags);
    }
    avs_net_socket_cleanup(&socket);
    avs_crypto_prng_free(&prng);
    assert(xSemaphoreTake(server.done, pdMS_TO_TICKS(15000)) == pdTRUE);
    if (success) {
        assert(!server.handshake_result && !server.verify_flags && server.echoed);
    } else if (reject_client) {
        assert(server.handshake_result != 0);
        assert(server.verify_flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    }
    vSemaphoreDelete(server.ready);
    vSemaphoreDelete(server.done);
    /* Allow the idle task to reclaim the completed server task. */
    vTaskDelay(pdMS_TO_TICKS(20));
    printf("PROBE: %s PASS; client_flags=0x%08lx server_flags=0x%08lx\n",
           name, (unsigned long) client_verify_flags, (unsigned long) server.verify_flags);
}

void run_dtls_probe(void) {
    /* Fixed test time only. Production must preserve SNTP before secure traffic. */
    struct timeval now = {.tv_sec = 1789948800}; /* 2026-09-21 00:00:00 UTC */
    CHECK(settimeofday(&now, NULL));
    CHECK(psa_crypto_init());
    credentials_t *credentials = calloc(1, sizeof(*credentials));
    assert(credentials);
    generate_key(&credentials->root_key);
    generate_key(&credentials->other_root_key);
    generate_key(&credentials->server_key);
    generate_key(&credentials->client_key);
    issue_certificate(credentials->root, sizeof(credentials->root),
                      &credentials->root_key, "CN=Probe Root",
                      &credentials->root_key, "CN=Probe Root", true, false);
    issue_certificate(credentials->other_root, sizeof(credentials->other_root),
                      &credentials->other_root_key, "CN=Other Root",
                      &credentials->other_root_key, "CN=Other Root", true, false);
    issue_certificate(credentials->client, sizeof(credentials->client),
                      &credentials->client_key, "CN=Probe Device",
                      &credentials->root_key, "CN=Probe Root", false, false);
    strcat(credentials->client, credentials->root);
    CHECK(mbedtls_pk_write_key_pem(&credentials->client_key,
                                   (unsigned char *) credentials->client_private,
                                   sizeof(credentials->client_private)));
    make_server_certificate(credentials, false);
    const int ccm = MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8;
    const int cbc = MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256;
    run_case(credentials, "mutual X.509 CCM-8 and PEM chain", ccm, false, false, 0, false);
    run_case(credentials, "mutual X.509 CBC-SHA256", cbc, false, false, 0, false);
    run_case(credentials, "reject wrong server trust root", ccm, true, false,
             MBEDTLS_X509_BADCERT_NOT_TRUSTED, false);
    run_case(credentials, "reject wrong server hostname", ccm, false, true,
             MBEDTLS_X509_BADCERT_CN_MISMATCH, false);
    make_server_certificate(credentials, true);
    run_case(credentials, "reject expired server certificate", ccm, false, false,
             MBEDTLS_X509_BADCERT_EXPIRED, false);
    make_server_certificate(credentials, false);
    issue_certificate(credentials->client, sizeof(credentials->client),
                      &credentials->client_key, "CN=Probe Device",
                      &credentials->other_root_key, "CN=Other Root", false, false);
    strcat(credentials->client, credentials->other_root);
    run_case(credentials, "reject untrusted device certificate", ccm, false, false, 0, true);

    mbedtls_pk_free(&credentials->client_key);
    generate_rsa_key(&credentials->client_key);
    issue_certificate(credentials->client, sizeof(credentials->client),
                      &credentials->client_key, "CN=Probe RSA Device",
                      &credentials->root_key, "CN=Probe Root", false, false);
    strcat(credentials->client, credentials->root);
    write_rsa_pkcs8(credentials);
    run_case(credentials, "PKCS#8 RSA device with ECDSA server CCM-8", ccm, false, false, 0, false);
    run_case(credentials, "PKCS#8 RSA device with ECDSA server CBC-SHA256", cbc, false, false, 0, false);
    mbedtls_pk_free(&credentials->client_key);
    generate_rsa_key(&credentials->client_key);
    write_rsa_pkcs8(credentials);
    reject_mismatched_rsa_key(credentials);
    mbedtls_pk_free(&credentials->root_key);
    mbedtls_pk_free(&credentials->other_root_key);
    mbedtls_pk_free(&credentials->server_key);
    mbedtls_pk_free(&credentials->client_key);
    mbedtls_platform_zeroize(credentials, sizeof(*credentials));
    free(credentials);
    printf("PROBE: all DTLS cases PASS; free heap=%u minimum heap=%u\n",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}
