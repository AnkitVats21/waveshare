#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/storage/StorageService.h"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <vector>

using Services::StorageService;

namespace {

constexpr const char* BASE_PATH = CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH;
constexpr size_t CHUNK_SIZE = 4096;

const char* mimeType(const std::string& path) {
    const char* ext = strrchr(path.c_str(), '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".wav") == 0) return "audio/wav";
    if (strcasecmp(ext, ".mp3") == 0) return "audio/mpeg";
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
    return StorageService::sanitizePath(raw.c_str(), out, BASE_PATH);
}

// Files holding secrets (API key, Wi-Fi password). They are managed through
// /api/config/gemini and /api/wifi/configure, never read or replaced here.
// Also refuses "." segments and FAT 8.3 aliases (GEMINI~1.JSO) that would
// otherwise reach the same file under a different name.
bool isProtected(const std::string& path) {
    if (path.find("/./") != std::string::npos ||
        (path.size() >= 2 && path.compare(path.size() - 2, 2, "/.") == 0)) {
        return true;
    }
    static const char* const PROTECTED[] = { "/sdcard/gemini_config.json", "/sdcard/wifi_config.json" };
    static const char* const ALIAS_PREFIX[] = { "/sdcard/gemini", "/sdcard/wifi_c" };
    for (const char* p : PROTECTED) {
        if (strcasecmp(path.c_str(), p) == 0) return true;
    }
    if (path.find('~') != std::string::npos) {
        for (const char* p : ALIAS_PREFIX) {
            if (strncasecmp(path.c_str(), p, strlen(p)) == 0) return true;
        }
    }
    return false;
}

esp_err_t sendProtected(httpd_req_t* req) {
    return Http::sendError(req, 403, "This file holds credentials and is not accessible through the file API");
}

esp_err_t storageInfoHandler(httpd_req_t* req) {
    uint64_t total_bytes = 0, free_bytes = 0;
    bool mounted = StorageService::getInstance().isMounted();
    if (mounted) {
        StorageService::getInstance().getStorageInfo(BASE_PATH, total_bytes, free_bytes);
    }
    JsonDocument doc;
    doc["mounted"] = mounted;
    doc["total_bytes"] = total_bytes;
    doc["free_bytes"] = free_bytes;
    return Http::sendJson(req, 200, doc);
}

esp_err_t listHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!Http::queryParam(req, "path", raw_path) || raw_path.empty()) {
        raw_path = BASE_PATH;
    }
    std::string path;
    if (!sanitize(raw_path, path)) return Http::sendError(req, 400, "Invalid path traversal");

    std::vector<StorageService::FileEntryInfo> entries =
        StorageService::getInstance().listDirectoryDetailed(path.c_str());

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

esp_err_t downloadHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!Http::queryParam(req, "path", raw_path) || raw_path.empty()) return Http::sendError(req, 400, "Missing path");
    std::string path;
    if (!sanitize(raw_path, path)) return Http::sendError(req, 400, "Invalid path traversal");
    if (isProtected(path)) return sendProtected(req);

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return Http::sendError(req, 404, "File not found");

    httpd_resp_set_type(req, mimeType(path));
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    const char* filename = strrchr(path.c_str(), '/');
    filename = (filename != nullptr) ? filename + 1 : path.c_str();
    char disp_hdr[128];
    snprintf(disp_hdr, sizeof(disp_hdr), "inline; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp_hdr);

    std::vector<char> chunk(CHUNK_SIZE);
    auto& storage = StorageService::getInstance();
    while (true) {
        storage.lock();
        size_t n = fread(chunk.data(), 1, CHUNK_SIZE, f);
        storage.unlock();
        if (n == 0) break;

        esp_err_t err = httpd_resp_send_chunk(req, chunk.data(), n);
        if (err != ESP_OK) {
            fclose(f);
            return err;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t uploadHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!Http::queryParam(req, "path", raw_path) || raw_path.empty()) return Http::sendError(req, 400, "Missing path");
    std::string path;
    if (!sanitize(raw_path, path)) return Http::sendError(req, 400, "Invalid path traversal");
    if (isProtected(path)) return sendProtected(req);

    auto& storage = StorageService::getInstance();
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos && last_slash > 0) {
        storage.createDirectory(path.substr(0, last_slash).c_str());
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return Http::sendError(req, 500, "Cannot open target file for writing");

    std::vector<char> chunk(CHUNK_SIZE);
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = (remaining < static_cast<int>(CHUNK_SIZE)) ? remaining : static_cast<int>(CHUNK_SIZE);
        int received = httpd_req_recv(req, chunk.data(), to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            fclose(f);
            remove(path.c_str());
            return Http::sendError(req, 500, "Upload socket transfer failed");
        }

        storage.lock();
        size_t written = fwrite(chunk.data(), 1, received, f);
        storage.unlock();
        if (written != static_cast<size_t>(received)) {
            fclose(f);
            remove(path.c_str());
            return Http::sendError(req, 507, "Disk full or write failed");
        }
        remaining -= received;
    }
    fclose(f);

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "File uploaded successfully";
    doc["path"] = path;
    doc["bytes"] = req->content_len;
    return Http::sendJson(req, 200, doc);
}

esp_err_t mkdirHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!paramOrBodyField(req, "path", raw_path, 512) || raw_path.empty()) {
        return Http::sendError(req, 400, "Missing path");
    }
    std::string path;
    if (!sanitize(raw_path, path)) return Http::sendError(req, 400, "Invalid path traversal");

    if (!StorageService::getInstance().createDirectory(path.c_str())) {
        return Http::sendError(req, 500, "Failed to create directory");
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Directory created";
    doc["path"] = path;
    return Http::sendJson(req, 200, doc);
}

esp_err_t renameHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return Http::sendError(req, 500, "SD Card not mounted");

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
    if (!StorageService::getInstance().renamePath(sanitized_old.c_str(), sanitized_new.c_str())) {
        return Http::sendError(req, 500, "Failed to rename path");
    }
    return Http::sendOk(req, "Renamed successfully");
}

esp_err_t deleteHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return Http::sendError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!Http::queryParam(req, "path", raw_path) || raw_path.empty()) return Http::sendError(req, 400, "Missing path");
    std::string path;
    if (!sanitize(raw_path, path)) return Http::sendError(req, 400, "Invalid path traversal");
    if (isProtected(path)) return sendProtected(req);

    if (path == BASE_PATH || path == std::string(BASE_PATH) + "/") {
        return Http::sendError(req, 403, "Cannot delete root SD mount point");
    }
    if (!StorageService::getInstance().deletePath(path.c_str())) {
        return Http::sendError(req, 500, "Failed to delete target");
    }
    return Http::sendOk(req, "Deleted successfully");
}

} // namespace

void Routes::registerFiles(Http::Server& server) {
    server.on("/api/storage/info", HTTP_GET, storageInfoHandler);
    server.on("/api/files", HTTP_GET, listHandler);
    server.on("/api/files/download", HTTP_GET, downloadHandler);
    server.on("/api/files/upload", HTTP_POST, uploadHandler);
    server.on("/api/files/mkdir", HTTP_POST, mkdirHandler);
    server.on("/api/files/rename", HTTP_POST, renameHandler);
    server.on("/api/files", HTTP_DELETE, deleteHandler);
}
