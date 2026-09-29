#include "services/alarm/ToneDownload.h"

#include "app/media_player/NexusPlayer.h"
#include "common/thread_config.h"
#include "media_player/CatalogDB.h"
#include "media_player/HttpClientStream.h"
#include "media_player/MusicPlaybackService.h"
#include "media_player/StreamManager.h"
#include "sd_storage/File.h"
#include "sd_storage/Fs.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>

namespace Services {
namespace {

const char* TAG = "ToneDownload";
constexpr size_t CHUNK = 8 * 1024;
constexpr uint32_t MIN_BYTES = 32 * 1024;   // the music cache deletes smaller files as broken

std::mutex s_mutex;
ToneDownload::Status s_status;
std::string s_artist;

void setState(ToneDownload::State st, const char* error = nullptr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_status.state = st;
    if (error) s_status.error = error;
}

bool isVideoId(const std::string& id) {
    if (id.size() != 11) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// Returns an error, or nullptr once the file is in the library.
const char* download(const std::string& id, const std::string& title, const std::string& artist) {
    std::string url;
    if (MusicPlaybackService::getInstance().getInvidiousClient().resolveWebMOpusStreamUrl(id, url) != ESP_OK ||
        url.empty()) {
        return "could not resolve the audio stream";
    }
    const uint32_t duration_ms = (uint32_t)(StreamManager::urlNumberParam(url.c_str(), "dur") * 1000);
    setState(ToneDownload::State::Downloading);

    HttpClientStream http;
    if (!http.open(url)) {
        ESP_LOGW(TAG, "Stream open failed: HTTP %d", http.lastStatus());   // no URL: it is signed
        return "could not open the audio stream";
    }
    if (http.contentLength() > (int64_t)ToneDownload::MAX_BYTES) return "too long for an alarm tone (over 10 MB)";
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.total = http.contentLength() > 0 ? (uint32_t)http.contentLength() : 0;
    }

    const std::string tmp = "/sdcard/music/" + id + ".webm.tmp";
    const std::string path = "/sdcard/music/" + id + ".webm";
    if (!sd_storage::Fs::mkdirs("/sdcard/music")) return "no music folder on the card";
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM));
    if (!buf) return "out of memory";
    const char* err = nullptr;
    uint32_t total = 0;
    {
        auto file = sd_storage::File::open(tmp.c_str(), sd_storage::Mode::Write);
        if (!file) {
            err = "could not create the file";
        } else {
            for (;;) {
                const int n = http.read(buf, CHUNK);
                if (n == 0) break;
                if (n < 0) { err = "download interrupted"; break; }
                if (total + n > ToneDownload::MAX_BYTES) { err = "too long for an alarm tone (over 10 MB)"; break; }
                if (!file.writeAll(buf, n)) { err = "card write failed"; break; }
                total += n;
                std::lock_guard<std::mutex> lock(s_mutex);
                s_status.bytes = total;
            }
        }
    }
    heap_caps_free(buf);
    http.close();
    if (!err && total < MIN_BYTES) err = "download too short";
    if (!err && !sd_storage::Fs::rename(tmp.c_str(), path.c_str())) err = "could not save the file";
    if (err) {
        sd_storage::Fs::remove(tmp.c_str());
        return err;
    }
    CatalogDB::getInstance().noteTrack(id.c_str(), title.empty() ? id : title, artist, duration_ms);
    CatalogDB::getInstance().setSaved(id.c_str(), total);
    return nullptr;
}

void worker(void*) {
    {
        std::string id, title, artist;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            id = s_status.id;
            title = s_status.title;
            artist = s_artist;
        }
        const int64_t t0 = esp_timer_get_time();
        const char* err = download(id, title, artist);
        const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        uint32_t bytes;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            bytes = s_status.bytes;
        }
        if (err) {
            ESP_LOGW(TAG, "%s: %s (%u bytes, %u ms)", id.c_str(), err, (unsigned)bytes, (unsigned)ms);
            setState(ToneDownload::State::Failed, err);
        } else {
            ESP_LOGI(TAG, "%s saved to the library: %u bytes in %u ms (stack left %u B)", id.c_str(),
                     (unsigned)bytes, (unsigned)ms, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
            setState(ToneDownload::State::Done);
        }
    }
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

bool ToneDownload::start(const std::string& id, const std::string& title, const std::string& artist,
                         std::string* error) {
    auto fail = [error](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (!isVideoId(id)) return fail("not a YouTube video id (11 characters)");
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_status.state == State::Resolving || s_status.state == State::Downloading) {
        return fail("another download is running");
    }
    s_status = Status{};
    s_status.id = id;
    s_status.title = title;
    s_artist = artist;
    if (NexusPlayer::getInstance().getStorageManager().fileExists(id.c_str())) {
        if (!CatalogDB::getInstance().exists(id.c_str())) {
            CatalogDB::getInstance().noteTrack(id.c_str(), title.empty() ? id : title, artist, 0);
        }
        s_status.state = State::Done;
        return true;
    }
    s_status.state = State::Resolving;
    // TLS and the Invidious JSON need more than the 8 KB MCP workers use.
    if (xTaskCreatePinnedToCoreWithCaps(worker, "tone_dl", 12 * 1024, nullptr, ThreadConfig::LOW, nullptr,
                                        ThreadConfig::CORE_NETWORK, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_status.state = State::Failed;
        s_status.error = "could not start the download task";
        return fail("could not start the download task");
    }
    return true;
}

ToneDownload::Status ToneDownload::status() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_status;
}

const char* ToneDownload::stateName(State s) {
    switch (s) {
    case State::Resolving: return "resolving";
    case State::Downloading: return "downloading";
    case State::Done: return "done";
    case State::Failed: return "failed";
    default: return "idle";
    }
}

} // namespace Services
