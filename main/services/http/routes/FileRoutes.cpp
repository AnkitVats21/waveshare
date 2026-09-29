#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "sd_storage/File.h"
#include "sd_storage/Fs.h"
#include "sd_storage/PathPolicy.h"
#include "sd_storage/SdCard.h"
#include "services/alarm/AlarmSchedule.h"
#include "services/alarm/AudioSniff.h"
#include "services/alarm/ToneDownload.h"
#include "esp_heap_caps.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <strings.h>
#include <vector>

using namespace sd_storage;

namespace {

constexpr const char* BASE_PATH = CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH;
constexpr size_t CHUNK_SIZE = 4096;

// Files holding secrets (API key, Wi-Fi password). They are managed through
// /api/config/gemini and /api/wifi/configure, never read or replaced here.
// PathPolicy also refuses "." segments and FAT 8.3 aliases (GEMINI~1.JSO).
const char* const PROTECTED[] = { "/sdcard/gemini_config.json", "/sdcard/wifi_config.json" };

struct CapsFree { void operator()(void* p) const { heap_caps_free(p); } };

// Transfer buffer in PSRAM: a 4 KB vector would land in scarce internal RAM.
std::unique_ptr<char, CapsFree> chunkBuffer() {
    return std::unique_ptr<char, CapsFree>(
        static_cast<char*>(heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

const char* mimeType(const std::string& path) {
    const char* ext = strrchr(path.c_str(), '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".wav") == 0) return "audio/wav";
    if (strcasecmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(ext, ".opus") == 0 || strcasecmp(ext, ".ogg") == 0) return "audio/ogg";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".txt") == 0) return "text/plain";
    if (strcasecmp(ext, ".html") == 0) return "text/html";
    if (strcasecmp(ext, ".csv") == 0) return "text/csv";
    return "application/octet-stream";
}

// Reads `field` from the query string, or else from a small JSON body.
bool paramOrBodyField(httpd_req_t* req, const char* field, std::string& out, size_t max_body) {
    if (Http::queryParam(req, field, out) && !out.empty()) return true;
    std::string body;
    if (!Http::readBody(req, body, max_body - 1)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    const char* v = doc[field];
    if (!v) return false;
    out = v;
    return true;
}

bool sanitize(const std::string& raw, std::string& out) {
    return PathPolicy::sanitize(raw.c_str(), out, BASE_PATH);
}

bool isProtected(const std::string& path) {
    return PathPolicy::isProtected(path.c_str());
}

// Databases change only through their owners; /api/db serves them read-only.
bool isDbPath(const std::string& path) {
    return strncasecmp(path.c_str(), "/sdcard/db/", 11) == 0 || strcasecmp(path.c_str(), "/sdcard/db") == 0;
}

esp_err_t sendDbReadOnly(httpd_req_t* req) {
    return Http::sendError(req, 403, "Databases under /sdcard/db are read-only here; use GET /api/db/<name>");
}

bool mounted() {
    return SdCard::instance().isMounted();
}

esp_err_t sendProtected(httpd_req_t* req) {
    return Http::sendError(req, 403, "This file holds credentials and is not accessible through the file API");
}

esp_err_t storageInfoHandler(httpd_req_t* req) {
    uint64_t total_bytes = 0, free_bytes = 0;
    bool is_mounted = mounted();
    if (is_mounted) {
        Fs::info(total_bytes, free_bytes);
    }
    JsonDocument doc;
    doc["mounted"] = is_mounted;
    doc["total_bytes"] = total_bytes;
    doc["free_bytes"] = free_bytes;
    return Http::sendJson(req, 200, doc);
}

esp_err_t listHandler(httpd_req_t* req) {
    if (!mounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!Http::queryParam(req, "path", raw_path) || raw_path.empty()) {
        raw_path = BASE_PATH;
    }
    std::string path;
    if (!sanitize(raw_path, path)) return Http::sendError(req, 400, "Invalid path traversal");

    struct Entry {
        std::string name;
        size_t size;
        bool is_dir;
        time_t mtime;
    };
    std::vector<Entry> entries;
    if (!Fs::list(path.c_str(), nullptr, true, [](const DirEntry& e, void* ctx) {
            static_cast<std::vector<Entry>*>(ctx)->push_back({e.name, e.size, e.is_dir, e.mtime});
            return true;
        }, &entries)) {
        return Http::sendError(req, 404, "Directory not found");
    }
    // Folders first, then by name.
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.is_dir != b.is_dir ? a.is_dir : a.name < b.name;
    });

    JsonDocument doc;
    doc["path"] = path;
    JsonArray arr = doc["entries"].to<JsonArray>();
    for (const auto& item : entries) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = item.name;
        obj["size"] = item.size;
        obj["is_dir"] = item.is_dir;
        obj["mtime"] = static_cast<uint64_t>(item.mtime);
    }
    return Http::sendJson(req, 200, doc);
}

// "bytes=A-B", "bytes=A-" or "bytes=-N" (the last N bytes) against a file
// of `size` bytes. False if malformed or outside the file.
bool parseRange(const char* value, long size, long& first, long& last) {
    if (strncmp(value, "bytes=", 6) != 0 || size <= 0) return false;
    const char* p = value + 6;
    char* end = nullptr;
    if (*p == '-') {
        long n = strtol(p + 1, &end, 10);
        if (end == p + 1 || n <= 0) return false;
        first = n >= size ? 0 : size - n;
        last = size - 1;
        return true;
    }
    first = strtol(p, &end, 10);
    if (end == p || *end != '-' || first < 0 || first >= size) return false;
    p = end + 1;
    last = size - 1;
    if (*p != '\0') {
        long l = strtol(p, &end, 10);
        if (end == p || *end != '\0' || l < first) return false;
        if (l < last) last = l;
    }
    return true;
}

esp_err_t downloadHandler(httpd_req_t* req) {
    if (!mounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!Http::queryParam(req, "path", raw_path) || raw_path.empty()) return Http::sendError(req, 400, "Missing path");
    std::string path;
    if (!sanitize(raw_path, path)) return Http::sendError(req, 400, "Invalid path traversal");
    if (isProtected(path)) return sendProtected(req);

    File f = File::open(path.c_str(), Mode::Read);
    if (!f) return Http::sendError(req, 404, "File not found or busy");
    auto chunk = chunkBuffer();
    if (!chunk) return Http::sendError(req, 500, "Out of memory");

    // Range requests let a browser's <audio> seek without fetching the whole
    // file. One range only; the header strings must outlive the response.
    long size = f.size();
    long first = 0, last = size - 1;
    char range[48];
    char range_hdr[64];
    bool partial = httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK;
    if (partial) {
        if (!parseRange(range, size, first, last)) {
            snprintf(range_hdr, sizeof(range_hdr), "bytes */%ld", size);
            httpd_resp_set_hdr(req, "Content-Range", range_hdr);
            return Http::sendError(req, 416, "Range not satisfiable");
        }
        snprintf(range_hdr, sizeof(range_hdr), "bytes %ld-%ld/%ld", first, last, size);
        httpd_resp_set_status(req, "206 Partial Content");
        httpd_resp_set_hdr(req, "Content-Range", range_hdr);
        if (!f.seek(first)) return Http::sendError(req, 500, "Seek failed");
    }
    // After the error replies above, which set their own CORS header.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    httpd_resp_set_type(req, mimeType(path));

    const char* filename = strrchr(path.c_str(), '/');
    filename = (filename != nullptr) ? filename + 1 : path.c_str();
    char disp_hdr[128];
    snprintf(disp_hdr, sizeof(disp_hdr), "inline; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp_hdr);

    long remaining = last - first + 1;
    while (remaining > 0) {
        size_t n = f.read(chunk.get(), remaining < long(CHUNK_SIZE) ? size_t(remaining) : CHUNK_SIZE);
        if (n == 0) break;
        esp_err_t err = httpd_resp_send_chunk(req, chunk.get(), n);
        if (err != ESP_OK) return err;
        remaining -= long(n);
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t uploadHandler(httpd_req_t* req) {
    if (!mounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!Http::queryParam(req, "path", raw_path) || raw_path.empty()) return Http::sendError(req, 400, "Missing path");
    std::string path;
    if (!sanitize(raw_path, path)) return Http::sendError(req, 400, "Invalid path traversal");
    if (isProtected(path)) return sendProtected(req);
    if (isDbPath(path)) return sendDbReadOnly(req);

    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos && last_slash > 0) {
        Fs::mkdirs(path.substr(0, last_slash).c_str());
    }

    // The alarm folder (tones, briefing music) takes only what the player
    // decodes: its first bytes are checked before anything is written.
    const std::string alarm_dir = std::string(Services::ALARM_TONE_DIR) + "/";
    const bool alarm_file = path.compare(0, alarm_dir.size(), alarm_dir) == 0;
    if (alarm_file) {
        if (!Services::isValidToneFileName(path.substr(alarm_dir.size()))) {
            return Http::sendError(req, 400, "Alarm files need a plain name ending in .ogg, .opus or .webm "
                                             "(letters, digits, space, '-', '_', '.'; at most 48 characters)");
        }
        if (req->content_len > Services::ToneDownload::MAX_BYTES) {
            return Http::sendError(req, 413, "Alarm files are limited to 10 MB");
        }
    }

    auto chunk = chunkBuffer();
    if (!chunk) return Http::sendError(req, 500, "Out of memory");

    int head = 0;   // bytes already received into chunk (alarm files)
    if (alarm_file) {
        const int want = std::min<int>(req->content_len, CHUNK_SIZE);
        while (head < want) {
            int received = httpd_req_recv(req, chunk.get() + head, want - head);
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (received <= 0) return Http::sendError(req, 500, "Upload socket transfer failed");
            head += received;
        }
        const Services::AudioSniff sniff = Services::sniffAudio(reinterpret_cast<const uint8_t*>(chunk.get()), head);
        if (!sniff.playable) {
            std::string msg = std::string("This file is ") + sniff.format +
                              "; the device plays only Opus (.ogg, .opus or .webm). "
                              "Convert it: ffmpeg -i input.mp3 -ac 1 -c:a libopus -b:a 64k output.ogg";
            return Http::sendError(req, 415, msg.c_str());
        }
    }

    File f = File::open(path.c_str(), Mode::Write);
    if (!f) return Http::sendError(req, 500, "Cannot open target file for writing (missing folder or file busy)");
    if (head > 0 && !f.writeAll(chunk.get(), head)) {
        f.close();
        Fs::remove(path.c_str());
        return Http::sendError(req, 507, "Disk full or write failed");
    }

    int remaining = req->content_len - head;
    while (remaining > 0) {
        int to_read = (remaining < static_cast<int>(CHUNK_SIZE)) ? remaining : static_cast<int>(CHUNK_SIZE);
        int received = httpd_req_recv(req, chunk.get(), to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            f.close();
            Fs::remove(path.c_str());
            return Http::sendError(req, 500, "Upload socket transfer failed");
        }
        if (!f.writeAll(chunk.get(), received)) {
            f.close();
            Fs::remove(path.c_str());
            return Http::sendError(req, 507, "Disk full or write failed");
        }
        remaining -= received;
    }
    f.close();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "File uploaded successfully";
    doc["path"] = path;
    doc["bytes"] = req->content_len;
    return Http::sendJson(req, 200, doc);
}

esp_err_t mkdirHandler(httpd_req_t* req) {
    if (!mounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!paramOrBodyField(req, "path", raw_path, 512) || raw_path.empty()) {
        return Http::sendError(req, 400, "Missing path");
    }
    std::string path;
    if (!sanitize(raw_path, path)) return Http::sendError(req, 400, "Invalid path traversal");
    if (isDbPath(path)) return sendDbReadOnly(req);

    if (!Fs::mkdirs(path.c_str())) {
        return Http::sendError(req, 500, "Failed to create directory");
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Directory created";
    doc["path"] = path;
    return Http::sendJson(req, 200, doc);
}

// A file moved into the alarm folder must meet the upload's rules. Null if
// it does (or `to` is elsewhere, or `from` is a folder).
const char* alarmFileError(const std::string& to, const std::string& from) {
    const std::string alarm_dir = std::string(Services::ALARM_TONE_DIR) + "/";
    if (to.compare(0, alarm_dir.size(), alarm_dir) != 0 || !Fs::isFile(from.c_str())) return nullptr;
    if (!Services::isValidToneFileName(to.substr(alarm_dir.size()))) {
        return "Alarm files need a plain name ending in .ogg, .opus or .webm";
    }
    File f = File::open(from.c_str(), Mode::Read);
    auto chunk = chunkBuffer();
    if (!f || !chunk) return "Could not read the file to check its format";
    if (f.size() > long(Services::ToneDownload::MAX_BYTES)) return "Alarm files are limited to 10 MB";
    const size_t n = f.read(chunk.get(), CHUNK_SIZE);
    if (!Services::sniffAudio(reinterpret_cast<const uint8_t*>(chunk.get()), n).playable) {
        return "The device plays only Opus (.ogg, .opus or .webm) from the alarm folder";
    }
    return nullptr;
}

esp_err_t renameHandler(httpd_req_t* req) {
    if (!mounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string old_path, new_path;
    if (!Http::queryParam(req, "old_path", old_path) || !Http::queryParam(req, "new_path", new_path)) {
        std::string body;
        JsonDocument doc;
        if (Http::readBody(req, body, 1023) && !deserializeJson(doc, body)) {
            if (const char* op = doc["old_path"]) old_path = op;
            if (const char* np = doc["new_path"]) new_path = np;
        }
    }
    if (old_path.empty() || new_path.empty()) return Http::sendError(req, 400, "Missing old_path or new_path");

    std::string sanitized_old, sanitized_new;
    if (!sanitize(old_path, sanitized_old) || !sanitize(new_path, sanitized_new)) {
        return Http::sendError(req, 400, "Invalid path traversal");
    }
    if (isProtected(sanitized_old) || isProtected(sanitized_new)) return sendProtected(req);
    if (isDbPath(sanitized_old) || isDbPath(sanitized_new)) return sendDbReadOnly(req);
    if (const char* err = alarmFileError(sanitized_new, sanitized_old)) return Http::sendError(req, 415, err);
    if (!Fs::rename(sanitized_old.c_str(), sanitized_new.c_str())) {
        return Http::sendError(req, 500, "Failed to rename path");
    }
    return Http::sendOk(req, "Renamed successfully");
}

esp_err_t deleteHandler(httpd_req_t* req) {
    if (!mounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!Http::queryParam(req, "path", raw_path) || raw_path.empty()) return Http::sendError(req, 400, "Missing path");
    std::string path;
    if (!sanitize(raw_path, path)) return Http::sendError(req, 400, "Invalid path traversal");
    if (isProtected(path)) return sendProtected(req);
    if (isDbPath(path)) return sendDbReadOnly(req);

    if (path == BASE_PATH || path == std::string(BASE_PATH) + "/") {
        return Http::sendError(req, 403, "Cannot delete root SD mount point");
    }
    if (!Fs::removePath(path.c_str())) {
        return Http::sendError(req, 500, "Failed to delete target");
    }
    return Http::sendOk(req, "Deleted successfully");
}

} // namespace

void Routes::registerFiles(Http::Server& server) {
    PathPolicy::setProtected(PROTECTED, sizeof(PROTECTED) / sizeof(PROTECTED[0]));
    server.on("/api/storage/info", HTTP_GET, storageInfoHandler);
    server.on("/api/files", HTTP_GET, listHandler);
    server.on("/api/files/download", HTTP_GET, downloadHandler);
    server.on("/api/files/upload", HTTP_POST, uploadHandler);
    server.on("/api/files/mkdir", HTTP_POST, mkdirHandler);
    server.on("/api/files/rename", HTTP_POST, renameHandler);
    server.on("/api/files", HTTP_DELETE, deleteHandler);
}
