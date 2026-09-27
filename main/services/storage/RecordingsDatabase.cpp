#include "RecordingsDatabase.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <strings.h>

#include "app/audio/recording/RecordingProbe.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sd_storage/File.h"
#include "sd_storage/Fs.h"
#include "sd_storage/SdCard.h"

namespace Services {
namespace {

using ndb::recordings::RecordingDoc;

const char* const TAG = "RecordingsDb";
constexpr const char* RECORDINGS_DIR = "/sdcard/recordings";
constexpr time_t MIN_EPOCH = 1704067200;  // 2024-01-01; older file times mean the clock wasn't set
constexpr uint32_t OPEN_STACK = 8 * 1024;

// Serialises the directory check, adds, renames and deletes, so a file isn't
// listed twice or dropped while it is being added.
std::mutex g_mutex;

bool isRecordingName(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".opus") == 0 || strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".wav") == 0);
}

// Keys that are not a positive decimal id are skipped.
int parseId(std::string_view key) {
    if (key.empty() || key.size() > 9) return 0;
    int id = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return 0;
        id = id * 10 + (c - '0');
    }
    return id;
}

// Size, length, rate and channels from the file. The document keeps the
// size (and zero length) if the format isn't recognised.
bool probeFile(const char* path, RecordingDoc& doc) {
    sd_storage::File f = sd_storage::File::open(path, sd_storage::Mode::Read);
    if (!f) return false;
    long size = f.size();
    if (size < 0) return false;
    doc.size = uint32_t(size);

    uint8_t head[RecordingProbe::HEAD_BYTES];
    size_t head_len = f.read(head, sizeof(head));
    RecordingProbe::Info info;
    bool ok = false;
    if (head_len >= 4 && memcmp(head, "RIFF", 4) == 0) {
        ok = RecordingProbe::probeWav(head, head_len, uint64_t(size), info);
    } else if (head_len >= 4 && memcmp(head, "OggS", 4) == 0) {
        size_t tail_len = std::min(size_t(size), RecordingProbe::TAIL_BYTES);
        std::unique_ptr<uint8_t, decltype(&heap_caps_free)> tail(
            static_cast<uint8_t*>(heap_caps_malloc(tail_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), &heap_caps_free);
        ok = tail && f.seek(size - long(tail_len)) && f.readExact(tail.get(), tail_len) &&
             RecordingProbe::probeOpus(head, head_len, tail.get(), tail_len, info);
    }
    if (!ok) return false;
    doc.duration_ms = info.duration_ms;
    doc.channels = info.channels;
    doc.sample_rate = info.sample_rate;
    return true;
}

// AudioRecorder names files rec_YYYYMMDD_HHMMSS_<stereo|processed><rate>k.opus
// (rec_NNN_... when the clock wasn't set): the mode and encoded rate, which
// the file itself doesn't record, and the local start time.
void applyName(const char* name, RecordingDoc& doc) {
    const char* m = nullptr;
    int skip = 0;
    if ((m = strstr(name, "_stereo"))) {
        doc.mode = REC_MODE_STEREO;
        skip = 7;
    } else if ((m = strstr(name, "_processed"))) {
        doc.mode = REC_MODE_PROCESSED;
        skip = 10;
    }
    if (m) {
        char* end = nullptr;
        long khz = strtol(m + skip, &end, 10);
        if (khz > 0 && end && *end == 'k') doc.sample_rate = uint32_t(khz * 1000);
    }
    struct tm t = {};
    if (sscanf(name, "rec_%4d%2d%2d_%2d%2d%2d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min,
               &t.tm_sec) == 6 &&
        t.tm_year >= 2024) {
        t.tm_year -= 1900;
        t.tm_mon -= 1;
        t.tm_isdst = -1;
        time_t at = mktime(&t);
        if (at >= MIN_EPOCH) doc.started = uint32_t(at);
    }
}

// A file found on the card: everything from the file, its name and its time.
RecordingDoc describe(const char* name, time_t mtime) {
    RecordingDoc doc;
    doc.file = name;
    std::string path = std::string(RECORDINGS_DIR) + "/" + name;
    if (!probeFile(path.c_str(), doc)) ESP_LOGW(TAG, "Could not read the length of %s", name);
    applyName(name, doc);
    if (doc.mode == REC_MODE_UNKNOWN) {
        // Our stereo files are the two raw mics; mono ones the AFE output.
        if (doc.channels == 2) doc.mode = REC_MODE_STEREO;
        else if (doc.channels == 1) doc.mode = REC_MODE_PROCESSED;
    }
    // The file time is when writing ended.
    if (doc.started == 0 && mtime >= MIN_EPOCH) doc.started = uint32_t(mtime - doc.duration_ms / 1000);
    return doc;
}

struct DirFile {
    std::string name;
    uint32_t size;
    time_t mtime;
};

bool collectFile(const sd_storage::DirEntry& e, void* ctx) {
    if (!e.is_dir && isRecordingName(e.name)) {
        static_cast<std::vector<DirFile>*>(ctx)->push_back({e.name, uint32_t(e.size), e.mtime});
    }
    return true;
}

void reconcile(ndb::recordings::RecordingsDb& db) {
    int64_t t0 = esp_timer_get_time();
    std::vector<DirFile> files;
    sd_storage::Fs::list(RECORDINGS_DIR, nullptr, true, collectFile, &files);

    struct Listed {
        std::string key;
        int id;
        std::string file;
        uint32_t size;
    };
    std::vector<Listed> listed;
    db.recordings().forEach([&](std::string_view key, const RecordingDoc& doc) {
        listed.push_back({std::string(key), parseId(key), doc.file, doc.size});
        return true;
    });

    int next_id = 1;
    for (const auto& l : listed) next_id = std::max(next_id, l.id + 1);

    unsigned dropped = 0, updated = 0, added = 0;
    std::vector<bool> seen(files.size(), false);
    for (const auto& l : listed) {
        auto it = std::find_if(files.begin(), files.end(), [&](const DirFile& f) { return f.name == l.file; });
        size_t i = size_t(it - files.begin());
        if (l.id == 0 || it == files.end() || seen[i]) {
            // Gone, a duplicate, or a key that isn't an id.
            if (db.recordings().remove(l.key)) ++dropped;
            continue;
        }
        seen[i] = true;
        if (it->size != l.size) {
            // Changed on the card (or recorded while the database was closed):
            // read it again, keeping the id.
            if (db.recordings().put(l.key, describe(it->name.c_str(), it->mtime))) ++updated;
        }
    }
    for (size_t i = 0; i < files.size(); ++i) {
        if (seen[i]) continue;
        if (db.recordings().put(std::to_string(next_id), describe(files[i].name.c_str(), files[i].mtime))) {
            ++next_id;
            ++added;
        }
    }
    ESP_LOGI(TAG, "%u file(s): %u added, %u updated, %u dropped in %lld ms", unsigned(files.size()), added,
             updated, dropped, (esp_timer_get_time() - t0) / 1000);
}

void openAndReconcile() {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& db = recordingsDb();
    if (!db.open()) {
        ESP_LOGE(TAG, "Could not open recordings.ndb; the recordings list is unavailable");
        return;
    }
    reconcile(db);
}

// Nothing with a destructor may be in scope at vTaskDeleteWithCaps.
void openTask(void*) {
    openAndReconcile();
    vTaskDeleteWithCaps(nullptr);
}

// The new file name for `name`, keeping the extension of `old_file`; empty
// if `name` isn't allowed.
std::string cleanName(std::string name, const std::string& old_file) {
    size_t dot = old_file.rfind('.');
    std::string ext = dot == std::string::npos ? "" : old_file.substr(dot);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    if (!ext.empty() && name.size() > ext.size() &&
        strcasecmp(name.c_str() + name.size() - ext.size(), ext.c_str()) == 0) {
        name.resize(name.size() - ext.size());
    }
    if (name.empty() || name.size() > MAX_RECORDING_NAME || name[0] == '.') return "";
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  strchr(" -_.()", c) != nullptr;
        if (!ok) return "";
    }
    return name + ext;
}

std::string pathOf(const std::string& file) { return std::string(RECORDINGS_DIR) + "/" + file; }

}  // namespace

ndb::recordings::RecordingsDb& recordingsDb() {
    static ndb::recordings::RecordingsDb db;
    return db;
}

void openRecordingsDbAsync() {
    if (!sd_storage::SdCard::instance().isMounted()) return;
    if (xTaskCreatePinnedToCoreWithCaps(openTask, "recdb_open", OPEN_STACK, nullptr, 2, nullptr, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the recordings.ndb open task");
    }
}

bool addRecording(const char* path, uint32_t started, RecordingMode mode, uint32_t sample_rate) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& db = recordingsDb();
    if (!db.db().isOpen()) return false;
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;

    RecordingDoc doc;
    doc.file = name;
    if (!probeFile(path, doc)) ESP_LOGW(TAG, "Could not read the length of %s", name);
    doc.started = started;
    doc.mode = mode;
    if (sample_rate) doc.sample_rate = sample_rate;

    // Reuse the id if the file is somehow listed already.
    std::string key;
    int next_id = 1;
    db.recordings().forEach([&](std::string_view k, const RecordingDoc& d) {
        next_id = std::max(next_id, parseId(k) + 1);
        if (d.file == doc.file) key = std::string(k);
        return true;
    });
    if (key.empty()) key = std::to_string(next_id);
    if (!db.recordings().put(key, doc)) {
        ESP_LOGE(TAG, "Could not add %s", name);
        return false;
    }
    ESP_LOGI(TAG, "Added %s as id %s (%u ms, %u bytes)", name, key.c_str(), unsigned(doc.duration_ms),
             unsigned(doc.size));
    return true;
}

RecordingResult renameRecording(int id, const std::string& name, std::string& new_file) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& db = recordingsDb();
    if (!db.db().isOpen()) return RecordingResult::Unavailable;
    std::string key = std::to_string(id);
    RecordingDoc doc;
    if (id <= 0 || !db.recordings().get(key, doc)) return RecordingResult::NotFound;
    new_file = cleanName(name, doc.file);
    if (new_file.empty()) return RecordingResult::BadName;
    if (new_file == doc.file) return RecordingResult::Ok;

    std::string from = pathOf(doc.file), to = pathOf(new_file);
    // FAT names are case-insensitive: a change of case alone is the same file.
    bool case_only = strcasecmp(new_file.c_str(), doc.file.c_str()) == 0;
    if (!case_only && sd_storage::Fs::exists(to.c_str())) return RecordingResult::Taken;
    if (!sd_storage::Fs::rename(from.c_str(), to.c_str())) {
        ESP_LOGW(TAG, "Could not rename %s to %s", doc.file.c_str(), new_file.c_str());
        return RecordingResult::Failed;
    }
    RecordingDoc update;
    update.file = new_file;
    if (!db.recordings().merge(key, update, RecordingDoc::F_FILE)) {
        // Put the file back so the list stays true.
        sd_storage::Fs::rename(to.c_str(), from.c_str());
        return RecordingResult::Failed;
    }
    ESP_LOGI(TAG, "Renamed id %d: %s -> %s", id, doc.file.c_str(), new_file.c_str());
    return RecordingResult::Ok;
}

RecordingResult findRecording(int id, RecordingDoc& doc, std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& db = recordingsDb();
    if (!db.db().isOpen()) return RecordingResult::Unavailable;
    if (id <= 0 || !db.recordings().get(std::to_string(id), doc)) return RecordingResult::NotFound;
    path = pathOf(doc.file);
    return RecordingResult::Ok;
}

RecordingResult deleteRecording(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& db = recordingsDb();
    if (!db.db().isOpen()) return RecordingResult::Unavailable;
    std::string key = std::to_string(id);
    RecordingDoc doc;
    if (id <= 0 || !db.recordings().get(key, doc)) return RecordingResult::NotFound;
    std::string path = pathOf(doc.file);
    if (sd_storage::Fs::exists(path.c_str()) && !sd_storage::Fs::remove(path.c_str())) {
        ESP_LOGW(TAG, "Could not delete %s", doc.file.c_str());
        return RecordingResult::Failed;
    }
    // If this write fails, the next boot's check drops the entry.
    db.recordings().remove(key);
    ESP_LOGI(TAG, "Deleted id %d: %s", id, doc.file.c_str());
    return RecordingResult::Ok;
}

}  // namespace Services
