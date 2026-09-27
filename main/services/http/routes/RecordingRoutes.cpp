// /api/recordings: rename and delete the files listed in recordings.ndb. The
// list itself is GET /api/db/recordings.
#include "services/http/routes/Routes.h"

#include <cstdlib>
#include <string>

#include <ArduinoJson.h>

#include "http_server/HttpUtil.h"
#include "services/storage/RecordingsDatabase.h"

namespace {

using Services::RecordingResult;

esp_err_t sendResult(httpd_req_t* req, RecordingResult r) {
    switch (r) {
    case RecordingResult::Unavailable: return Http::sendError(req, 503, "Recordings list unavailable");
    case RecordingResult::NotFound: return Http::sendError(req, 404, "No recording with that id");
    case RecordingResult::BadName:
        return Http::sendError(req, 400, "Name must be 1-60 letters, digits, spaces or - _ . ( )");
    case RecordingResult::Taken: return Http::sendError(req, 409, "A recording with that name exists");
    case RecordingResult::Failed: return Http::sendError(req, 500, "Could not change the file (in use?)");
    default: return Http::sendError(req, 500, "Unexpected result");
    }
}

// POST {"id": 3, "name": "kitchen test"} -> {"status":"ok","id":3,"file":"kitchen test.opus"}
esp_err_t renameHandler(httpd_req_t* req) {
    std::string body;
    JsonDocument in;
    if (!Http::readBody(req, body, 512) || deserializeJson(in, body)) return Http::sendError(req, 400, "Invalid JSON");
    int id = in["id"] | 0;
    const char* name = in["name"] | "";
    std::string file;
    RecordingResult r = Services::renameRecording(id, name, file);
    if (r != RecordingResult::Ok) return sendResult(req, r);
    JsonDocument out;
    out["status"] = "ok";
    out["id"] = id;
    out["file"] = file;
    return Http::sendJson(req, 200, out);
}

// DELETE ?id=3
esp_err_t deleteHandler(httpd_req_t* req) {
    std::string val;
    if (!Http::queryParam(req, "id", val)) return Http::sendError(req, 400, "Missing id");
    RecordingResult r = Services::deleteRecording(atoi(val.c_str()));
    if (r != RecordingResult::Ok) return sendResult(req, r);
    return Http::sendOk(req, "Deleted");
}

}  // namespace

void Routes::registerRecordings(Http::Server& server) {
    server.on("/api/recordings/rename", HTTP_POST, renameHandler);
    server.on("/api/recordings", HTTP_DELETE, deleteHandler);
}
