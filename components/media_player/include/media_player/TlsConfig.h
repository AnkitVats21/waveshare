#pragma once
#include "esp_crt_bundle.h"
#include "esp_http_client.h"

namespace Tls {

// Server verification for every HTTPS client: the certificate chain against
// ESP-IDF's CA bundle, and the hostname (which also sends SNI). All servers
// used (googlevideo and its ISP caches, i.ytimg.com, Invidious instances)
// chain to roots in the bundle (GTS Root R1/R4, ISRG Root X1/X2). The build
// has no unverified fallback (CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY off), so
// a client without this fails to connect instead of trusting anyone.
// Certificate dates are not checked (CONFIG_MBEDTLS_HAVE_TIME_DATE off).
inline void secure(esp_http_client_config_t& config) {
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = false;
}

}  // namespace Tls
