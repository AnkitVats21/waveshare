// GET /api/db/<name>: the raw nexus_db file, decoded by the dashboard
// (tools/starc/js/ndb.js). See docs/nexus-db-design.md.
#include "services/http/routes/Routes.h"

#include <cstring>
#include <memory>

#include "esp_heap_caps.h"
#include "http_server/HttpUtil.h"
#include "media_player/CatalogDB.h"
#include "sd_storage/File.h"
#include "services/storage/RecordingsDatabase.h"
#include "services/storage/SystemDatabase.h"

namespace {

constexpr size_t CHUNK_SIZE = 4096;

nexus_db::Database* findDb(const char* name) {
    if (strcmp(name, "system") == 0) return &Services::systemDb().db();
    if (strcmp(name, "recordings") == 0) return &Services::recordingsDb().db();
    if (strcmp(name, "music") == 0) return &CatalogDB::getInstance().database();
    return nullptr;
}

esp_err_t dbHandler(httpd_req_t* req) {
    const char* name = req->uri + strlen("/api/db/");
    const char* query = strchr(name, '?');
    char buf[16] = {};
    size_t len = query ? size_t(query - name) : strlen(name);
    if (len == 0 || len >= sizeof(buf)) return Http::sendError(req, 404, "Unknown database");
    memcpy(buf, name, len);

    nexus_db::Database* db = findDb(buf);
    if (!db || !db->isOpen()) return Http::sendError(req, 404, "Unknown or unavailable database");

    // Read beside the database's own write handle. While this is open, a
    // cleanup can't swap the file; appends continue but aren't sent, because
    // the length is fixed here. A record cut off at that point fails its CRC
    // in the reader and is dropped, as at boot.
    sd_storage::File f = sd_storage::File::open(db->options().path, sd_storage::Mode::Read,
                                                sd_storage::Share::FollowWriter);
    if (!f) return Http::sendError(req, 503, "Database file busy");
    long remaining = f.size();

    std::unique_ptr<char, decltype(&heap_caps_free)> chunk(
        static_cast<char*>(heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), &heap_caps_free);
    if (!chunk) return Http::sendError(req, 500, "Out of memory");

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    while (remaining > 0) {
        size_t want = remaining < long(CHUNK_SIZE) ? size_t(remaining) : CHUNK_SIZE;
        size_t n = f.read(chunk.get(), want);
        if (n == 0) break;
        esp_err_t err = httpd_resp_send_chunk(req, chunk.get(), n);
        if (err != ESP_OK) return err;
        remaining -= long(n);
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

}  // namespace

void Routes::registerDb(Http::Server& server) {
    server.on("/api/db/*", HTTP_GET, dbHandler);
}
