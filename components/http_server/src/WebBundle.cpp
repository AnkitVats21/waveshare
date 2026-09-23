#include "http_server/WebBundle.h"

#include "esp_log.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace Http {

namespace {

constexpr const char* TAG = "WebBundle";
constexpr const char* SLOT_LABELS[WebBundle::SLOT_COUNT] = { "www_0", "www_1" };
constexpr const char* NVS_NS = "www";
constexpr const char* NVS_KEY_SLOT = "slot";

constexpr char     MAGIC[4] = { 'N', 'X', 'W', 'B' };
constexpr uint16_t FORMAT = 1;
constexpr size_t   PATH_LEN = 104;
constexpr uint32_t FLAG_GZIP = 1u << 0;

struct __attribute__((packed)) Header {
    char     magic[4];
    uint16_t format;
    uint16_t file_count;
    uint32_t total_size;
    uint32_t build_time;
    char     version[32];
    uint8_t  sha256[32];
};
static_assert(sizeof(Header) == 80, "bundle header layout");

struct __attribute__((packed)) Entry {
    char     path[PATH_LEN];
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
    uint8_t  etag[8];
    uint32_t reserved;
};
static_assert(sizeof(Entry) == 128, "bundle entry layout");

const Header* header(const uint8_t* base) { return reinterpret_cast<const Header*>(base); }
const Entry* entries(const uint8_t* base) { return reinterpret_cast<const Entry*>(base + sizeof(Header)); }

// Structural checks that don't need the whole bundle mapped.
bool headerLooksValid(const Header& h, size_t part_size) {
    if (memcmp(h.magic, MAGIC, sizeof(MAGIC)) != 0 || h.format != FORMAT) return false;
    if (h.total_size > part_size || h.file_count == 0) return false;
    return sizeof(Header) + size_t(h.file_count) * sizeof(Entry) <= h.total_size;
}

bool entriesValid(const uint8_t* base) {
    const Header& h = *header(base);
    for (uint16_t i = 0; i < h.file_count; ++i) {
        const Entry& e = entries(base)[i];
        if (memchr(e.path, '\0', PATH_LEN) == nullptr || e.path[0] != '/') return false;
        if (e.offset > h.total_size || e.size > h.total_size - e.offset) return false;
    }
    return true;
}

// SHA-256 of the bundle with its sha256 field treated as zeros.
bool shaMatches(const uint8_t* base, char* hex_out) {
    const Header& h = *header(base);
    static const uint8_t zeros[32] = {};
    constexpr size_t SHA_OFF = offsetof(Header, sha256);

    uint8_t digest[32];
    size_t len = 0;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS &&
              psa_hash_update(&op, base, SHA_OFF) == PSA_SUCCESS &&
              psa_hash_update(&op, zeros, sizeof(zeros)) == PSA_SUCCESS &&
              psa_hash_update(&op, base + sizeof(Header), h.total_size - sizeof(Header)) == PSA_SUCCESS &&
              psa_hash_finish(&op, digest, sizeof(digest), &len) == PSA_SUCCESS;
    if (!ok) {
        psa_hash_abort(&op);
        return false;
    }
    if (hex_out) {
        for (int i = 0; i < 32; ++i) snprintf(hex_out + 2 * i, 3, "%02x", h.sha256[i]);
    }
    return memcmp(digest, h.sha256, sizeof(digest)) == 0;
}

// Maps [0, size) of a partition; nullptr on failure.
const uint8_t* mapRange(const esp_partition_t* part, size_t size, esp_partition_mmap_handle_t& handle) {
    const void* ptr = nullptr;
    if (esp_partition_mmap(part, 0, size, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK) return nullptr;
    return static_cast<const uint8_t*>(ptr);
}

// Fully validates a slot. On success leaves it mapped (caller owns `handle`).
const uint8_t* mapIfValid(const esp_partition_t* part, esp_partition_mmap_handle_t& handle, WebBundle::SlotInfo* info) {
    if (!part) return nullptr;

    Header h;
    if (esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK || !headerLooksValid(h, part->size)) return nullptr;

    const uint8_t* base = mapRange(part, h.total_size, handle);
    if (!base) return nullptr;

    char sha_hex[65] = {};
    if (!entriesValid(base) || !shaMatches(base, sha_hex)) {
        esp_partition_munmap(handle);
        return nullptr;
    }
    if (info) {
        info->valid = true;
        memcpy(info->version, h.version, sizeof(h.version));
        info->version[sizeof(h.version)] = '\0';
        info->build_time = h.build_time;
        info->size = h.total_size;
        info->files = h.file_count;
        memcpy(info->sha256, sha_hex, sizeof(sha_hex));
    }
    return base;
}

const char* mimeType(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    static const struct { const char* ext; const char* type; } TYPES[] = {
        { ".html", "text/html" },            { ".js", "text/javascript" },
        { ".css", "text/css" },              { ".json", "application/json" },
        { ".svg", "image/svg+xml" },         { ".png", "image/png" },
        { ".jpg", "image/jpeg" },            { ".ico", "image/x-icon" },
        { ".webp", "image/webp" },           { ".woff2", "font/woff2" },
        { ".txt", "text/plain" },            { ".webmanifest", "application/manifest+json" },
    };
    for (const auto& t : TYPES) {
        if (strcasecmp(ext, t.ext) == 0) return t.type;
    }
    return "application/octet-stream";
}

} // namespace

WebBundle& WebBundle::instance() {
    static WebBundle bundle;
    return bundle;
}

void WebBundle::init() {
    for (int i = 0; i < SLOT_COUNT; ++i) {
        m_parts[i] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, SLOT_LABELS[i]);
    }
    if (!m_parts[0] || !m_parts[1]) {
        ESP_LOGW(TAG, "No www_0/www_1 partitions — serving the built-in page only");
        return;
    }

    uint8_t preferred = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY_SLOT, &preferred);
        nvs_close(nvs);
    }
    if (preferred >= SLOT_COUNT) preferred = 0;

    const int order[] = { int(preferred), int(1 - preferred) };
    for (int slot : order) {
        if (mapSlot(slot)) {
            ESP_LOGI(TAG, "Serving frontend from %s (version \"%.32s\")", SLOT_LABELS[slot], header(m_base)->version);
            return;
        }
    }
    ESP_LOGW(TAG, "No valid frontend bundle — serving the built-in page");
}

int WebBundle::uploadSlot() const {
    if (!m_parts[0] || !m_parts[1]) return NO_SLOT;
    return (m_active == 0) ? 1 : 0;
}

const esp_partition_t* WebBundle::partition(int slot) const {
    return (slot >= 0 && slot < SLOT_COUNT) ? m_parts[slot] : nullptr;
}

WebBundle::SlotInfo WebBundle::info(int slot) const {
    SlotInfo out;
    esp_partition_mmap_handle_t handle;
    if (mapIfValid(partition(slot), handle, &out)) {
        esp_partition_munmap(handle);
    }
    return out;
}

bool WebBundle::mapSlot(int slot) {
    esp_partition_mmap_handle_t handle;
    const uint8_t* base = mapIfValid(partition(slot), handle, nullptr);
    if (!base) return false;
    unmap();
    m_base = base;
    m_mmap = handle;
    m_active = slot;
    return true;
}

void WebBundle::unmap() {
    if (m_base) {
        esp_partition_munmap(m_mmap);
        m_base = nullptr;
        m_active = NO_SLOT;
    }
}

esp_err_t WebBundle::activate(int slot) {
    if (!mapSlot(slot)) {
        ESP_LOGE(TAG, "%s does not hold a valid bundle", SLOT_LABELS[slot < 0 ? 0 : slot]);
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, NVS_KEY_SLOT, static_cast<uint8_t>(slot));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        // Still serving the new slot now; only the choice after reboot is lost.
        ESP_LOGW(TAG, "Could not persist active slot: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Activated frontend %s (version \"%.32s\")", SLOT_LABELS[slot], header(m_base)->version);
    return ESP_OK;
}

esp_err_t WebBundle::serve(httpd_req_t* req) {
    if (!m_base) return ESP_ERR_NOT_FOUND;

    // Path without the query string; "/" means the app shell.
    char path[PATH_LEN];
    size_t len = strcspn(req->uri, "?#");
    if (len >= sizeof(path)) return ESP_ERR_NOT_FOUND;
    memcpy(path, req->uri, len);
    path[len] = '\0';
    if (strcmp(path, "/") == 0) strcpy(path, "/index.html");

    const Header& h = *header(m_base);
    const Entry* found = nullptr;
    for (int pass = 0; pass < 2 && !found; ++pass) {
        for (uint16_t i = 0; i < h.file_count; ++i) {
            if (strcmp(entries(m_base)[i].path, path) == 0) {
                found = &entries(m_base)[i];
                break;
            }
        }
        // Client-side route (no file extension in the last segment): app shell.
        const char* last = strrchr(path, '/');
        if (!found && pass == 0 && last && !strchr(last, '.')) {
            strcpy(path, "/index.html");
        } else {
            break;
        }
    }
    if (!found) return ESP_ERR_NOT_FOUND;

    char etag[19];
    etag[0] = '"';
    for (int i = 0; i < 8; ++i) snprintf(etag + 1 + 2 * i, 3, "%02x", found->etag[i]);
    etag[17] = '"';
    etag[18] = '\0';

    // Hashed build assets never change under the same name; everything else
    // (index.html) is revalidated with the ETag on each load.
    bool immutable = strncmp(found->path, "/assets/", 8) == 0;
    httpd_resp_set_hdr(req, "Cache-Control", immutable ? "public, max-age=31536000, immutable" : "no-cache");
    httpd_resp_set_hdr(req, "ETag", etag);

    char inm[24];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK && strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, nullptr, 0);
    }

    httpd_resp_set_type(req, mimeType(found->path));
    if (found->flags & FLAG_GZIP) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    return httpd_resp_send(req, reinterpret_cast<const char*>(m_base + found->offset), found->size);
}

} // namespace Http
