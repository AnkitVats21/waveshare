#include "services/http/routes/Routes.h"
#include "services/http/routes/MusicStatus.h"
#include "http_server/HttpUtil.h"
#include "media_player/MusicPlaybackService.h"
#include "media_player/CatalogDB.h"
#include "media_player/NexusPlayer.h"

#include <cstdlib>
#include <cstring>

namespace {

esp_err_t playHandler(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > 8192) {
        return Http::sendError(req, 400, "Invalid payload size");
    }
    std::string body;
    if (!Http::readBody(req, body, 8192)) {
        return Http::sendError(req, 400, "Failed to read request body");
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        return Http::sendError(req, 400, "Invalid JSON payload");
    }

    const char* stream_url = doc["stream_url"];
    if (!stream_url || strlen(stream_url) == 0) {
        return Http::sendError(req, 400, "stream_url is required");
    }

    InvidiousTrack track;
    track.videoId = doc["id"] | doc["videoId"] | "";
    track.title = doc["title"] | "Unknown Title";
    track.author = doc["artist"] | doc["author"] | "Unknown Artist";
    track.durationSeconds = doc["duration"] | doc["durationSeconds"] | 0;

    if (!MusicPlaybackService::getInstance().playDirect(track, stream_url)) {
        return Http::sendError(req, 500, "Failed to start direct playback");
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["message"] = "Playback started";
    resp["id"] = track.videoId;
    resp["title"] = track.title;
    return Http::sendJson(req, 200, resp);
}

esp_err_t playLocalHandler(httpd_req_t* req) {
    std::string target;
    if (!Http::queryParam(req, "id", target) && !Http::queryParam(req, "path", target)) {
        std::string body;
        JsonDocument doc;
        if (Http::readBody(req, body, 2047) && !deserializeJson(doc, body)) {
            if (const char* id = doc["id"]) target = id;
            else if (const char* path = doc["path"]) target = path;
        }
    }
    if (target.empty()) {
        return Http::sendError(req, 400, "Missing id or path parameter");
    }
    if (!MusicPlaybackService::getInstance().playLocal(target.c_str())) {
        return Http::sendError(req, 404, "Track file not found or failed to play");
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["target"] = target;
    return Http::sendJson(req, 200, resp);
}

esp_err_t controlHandler(httpd_req_t* req) {
    std::string action;
    int int_val = 0;
    bool bool_val = false;

    if (Http::queryParam(req, "action", action)) {
        std::string val;
        if (Http::queryParam(req, "value", val)) {
            int_val = atoi(val.c_str());
            bool_val = (val == "1" || val == "true");
        }
    } else {
        std::string body;
        JsonDocument doc;
        if (Http::readBody(req, body, 2047) && !deserializeJson(doc, body)) {
            if (const char* a = doc["action"]) action = a;
            if (doc["value"].is<int>()) {
                int_val = doc["value"].as<int>();
            } else if (doc["value"].is<bool>()) {
                bool_val = doc["value"].as<bool>();
            }
        }
    }
    if (action.empty()) {
        return Http::sendError(req, 400, "Missing action parameter");
    }

    auto& music = MusicPlaybackService::getInstance();
    if (action == "pause") {
        music.pause();
    } else if (action == "resume") {
        music.resume();
    } else if (action == "toggle") {
        music.postCommand(MediaCmdType::TOGGLE_PLAY_PAUSE);
    } else if (action == "next") {
        music.next();
    } else if (action == "prev" || action == "previous") {
        music.previous();
    } else if (action == "stop") {
        music.stop();
    } else if (action == "clear_queue") {
        music.clearQueue();
    } else if (action == "shuffle") {
        music.shuffleQueue();
    } else if (action == "repeat") {
        music.setRepeatMode(static_cast<RepeatMode>(int_val));
    } else if (action == "autoplay") {
        music.setAutoplay(bool_val);
    } else if (action == "caching") {
        music.setCaching(bool_val);
    } else if (action == "seek") {
        music.seekTo(static_cast<uint32_t>(int_val));
    } else {
        return Http::sendError(req, 400, "Unknown action");
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["action"] = action;
    return Http::sendJson(req, 200, resp);
}

esp_err_t statusHandler(httpd_req_t* req) {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    MusicStatus::fill(root);

    auto q = MusicPlaybackService::getInstance().getQueue();
    doc["queue_count"] = q.size();
    JsonArray qa = doc["queue"].to<JsonArray>();
    int count = 0;
    for (const auto& item : q) {
        if (++count > 10) break;
        JsonObject obj = qa.add<JsonObject>();
        obj["id"] = item.videoId;
        obj["title"] = item.title;
        obj["artist"] = item.author;
    }
    return Http::sendJson(req, 200, doc);
}

esp_err_t libraryScanHandler(httpd_req_t* req) {
    JsonDocument doc;
    doc["status"] = "ok";
    doc["scanned_count"] = CatalogDB::getInstance().scanAndSync();
    return Http::sendJson(req, 200, doc);
}

esp_err_t libraryDeleteHandler(httpd_req_t* req) {
    std::string id;
    if (!Http::queryParam(req, "id", id) || id.empty()) {
        std::string body;
        JsonDocument doc;
        if (Http::readBody(req, body, 511) && !deserializeJson(doc, body)) {
            if (const char* v = doc["id"]) id = v;
        }
    }
    if (id.empty()) {
        return Http::sendError(req, 400, "Missing id parameter");
    }

    bool ok = CatalogDB::getInstance().remove(id.c_str());
    JsonDocument doc;
    doc["status"] = ok ? "ok" : "error";
    doc["id"] = id;
    if (!ok) doc["message"] = "Track not found in library";
    return Http::sendJson(req, ok ? 200 : 404, doc);
}

} // namespace

void MusicStatus::fill(JsonObject out) {
    PlayerState state = NexusPlayer::getInstance().getState();
    const char* state_str = "IDLE";
    switch (state) {
        case STATE_STREAMING_AND_CACHING: state_str = "STREAMING"; break;
        case STATE_LOCAL_PLAYBACK:        state_str = "LOCAL"; break;
        case STATE_PAUSED:                state_str = "PAUSED"; break;
        default:                          state_str = "IDLE"; break;
    }

    auto& music = MusicPlaybackService::getInstance();
    InvidiousTrack cur = music.getCurrentTrack();

    out["state"] = state_str;
    out["position_ms"] = music.getPositionMs();
    out["duration_ms"] = cur.durationSeconds * 1000;
    out["seekable"] = (state == STATE_LOCAL_PLAYBACK || state == STATE_STREAMING_AND_CACHING || state == STATE_PAUSED);
    JsonObject t = out["current_track"].to<JsonObject>();
    t["id"] = cur.videoId;
    t["title"] = cur.title;
    t["artist"] = cur.author;
    t["duration"] = cur.durationSeconds;
    out["repeat_mode"] = static_cast<int>(music.getRepeatMode());
    out["autoplay"] = music.isAutoplayEnabled();
    out["caching"] = music.isCachingEnabled();
}

void Routes::registerMusic(Http::Server& server) {
    server.on("/api/music/play", HTTP_POST, playHandler);
    server.on("/api/music/play_local", HTTP_POST, playLocalHandler);
    server.on("/api/music/control", HTTP_POST, controlHandler);
    server.on("/api/music/status", HTTP_GET, statusHandler);
    server.on("/api/music/library/scan", HTTP_POST, libraryScanHandler);
    server.on("/api/music/library", HTTP_DELETE, libraryDeleteHandler);
}
