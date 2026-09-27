// /api/recordings: play, rename and delete the files listed in
// recordings.ndb. The list itself is GET /api/db/recordings.
#include "services/http/routes/Routes.h"

#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>

#include <ArduinoJson.h>

#include "http_server/HttpUtil.h"
#include "media_player/MusicPlaybackService.h"
#include "services/storage/RecordingsDatabase.h"

namespace {

using Services::RecordingResult;

std::string fileTrackId(int id) { return std::string(MusicPlaybackService::FILE_TRACK_PREFIX) + std::to_string(id); }

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

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

// POST {"id": 3}: plays the recording on the device's speaker, like a song
// (pause, seek, the player bar); it stops at the end, leaving the queue.
esp_err_t playHandler(httpd_req_t* req) {
    std::string body;
    JsonDocument in;
    if (!Http::readBody(req, body, 256) || deserializeJson(in, body)) return Http::sendError(req, 400, "Invalid JSON");
    int id = in["id"] | 0;
    ndb::recordings::RecordingDoc doc;
    std::string path;
    RecordingResult r = Services::findRecording(id, doc, path);
    if (r != RecordingResult::Ok) return sendResult(req, r);
    if (!endsWith(doc.file, ".opus") && !endsWith(doc.file, ".ogg")) {
        return Http::sendError(req, 415, "The device plays Ogg Opus recordings only");
    }
    InvidiousTrack track;
    track.videoId = fileTrackId(id);
    track.title = doc.file.substr(0, doc.file.rfind('.'));
    track.author = "Recording";
    track.durationSeconds = (doc.duration_ms + 500) / 1000;
    if (!MusicPlaybackService::getInstance().playFile(track, path.c_str())) {
        return Http::sendError(req, 500, "Could not start playback");
    }
    JsonDocument out;
    out["status"] = "ok";
    out["id"] = id;
    out["track_id"] = track.videoId;
    return Http::sendJson(req, 200, out);
}

// DELETE ?id=3
esp_err_t deleteHandler(httpd_req_t* req) {
    std::string val;
    if (!Http::queryParam(req, "id", val)) return Http::sendError(req, 400, "Missing id");
    const int id = atoi(val.c_str());
    // Deleting fails while the file is open (sd_storage): stop the player first.
    if (id > 0) MusicPlaybackService::getInstance().stopFileTrack(fileTrackId(id));
    RecordingResult r = Services::deleteRecording(id);
    if (r != RecordingResult::Ok) return sendResult(req, r);
    return Http::sendOk(req, "Deleted");
}

}  // namespace

void Routes::registerRecordings(Http::Server& server) {
    server.on("/api/recordings/play", HTTP_POST, playHandler);
    server.on("/api/recordings/rename", HTTP_POST, renameHandler);
    server.on("/api/recordings", HTTP_DELETE, deleteHandler);
}
