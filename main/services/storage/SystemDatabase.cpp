#include "SystemDatabase.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <ArduinoJson.h>

#include "common/ParserUtils.h"
#include "core_sysdb/led_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_storage/Fs.h"
#include "sd_storage/SdCard.h"

namespace Services {
namespace {

const char* const TAG = "SystemDb";

constexpr const char* STATE_KEY = "state";
constexpr const char* SETTINGS_KEY = "settings";
constexpr const char* OLD_STATE_FILE = "/sdcard/state_sync.txt";
constexpr const char* OLD_SETTINGS_FILE = "/sdcard/settings.txt";
constexpr const char* OLD_GEMINI_FILE = "/sdcard/gemini_config.json";

void renameToBak(const char* path) {
    std::string bak = std::string(path) + ".bak";
    sd_storage::Fs::remove(bak.c_str());
    if (!sd_storage::Fs::rename(path, bak.c_str())) ESP_LOGW(TAG, "Could not rename %s to .bak", path);
}

void onStatePair(const std::string& key, const std::string& val, void* ctx) {
    auto* s = static_cast<ndb::system::SavedState*>(ctx);
    if (key == "speaker_volume") {
        s->speaker_volume = atoi(val.c_str());
    } else if (key == "led_color") {
        int r = 0, g = 0, b = 0;
        sscanf(val.c_str(), "%d,%d,%d", &r, &g, &b);
        s->led_color = {uint8_t(r), uint8_t(g), uint8_t(b)};
    } else if (key == "autoplay") {
        s->autoplay = (val == "1" || val == "true");
    } else if (key == "cache_downloads" || key == "caching") {
        s->cache_downloads = (val == "1" || val == "true");
    }
}

// state_sync.txt -> the "state" document. It never stored the LED mode or
// mic gain; the old loader always switched the LED to solid.
void migrateState(ndb::system::SystemDb& db) {
    if (!sd_storage::Fs::isFile(OLD_STATE_FILE)) return;
    if (!db.state().contains(STATE_KEY)) {
        ndb::system::SavedState s;
        s.led_mode = uint8_t(LedMode::SOLID);
        Utils::ParserUtils::parseKeyValueStream(sd_storage::Fs::readText(OLD_STATE_FILE), onStatePair, &s);
        if (!db.state().put(STATE_KEY, s)) {
            ESP_LOGE(TAG, "Could not import %s; left in place", OLD_STATE_FILE);
            return;
        }
        ESP_LOGI(TAG, "Imported %s", OLD_STATE_FILE);
    }
    renameToBak(OLD_STATE_FILE);
}

void onSettingsPair(const std::string& key, const std::string& val, void* ctx) {
    if (key == "system.timezone") static_cast<ndb::system::Settings*>(ctx)->timezone = val;
}

// settings.txt -> timezone. Its only other lines were unused Wi-Fi and MQTT
// credentials, so the file is deleted rather than kept as .bak.
void migrateSettingsFile(ndb::system::SystemDb& db) {
    if (!sd_storage::Fs::isFile(OLD_SETTINGS_FILE)) return;
    ndb::system::Settings s;
    s.timezone.clear();
    Utils::ParserUtils::parseKeyValueStream(sd_storage::Fs::readText(OLD_SETTINGS_FILE), onSettingsPair, &s);
    if (!s.timezone.empty() && !db.settings().merge(SETTINGS_KEY, s, ndb::system::Settings::F_TIMEZONE)) {
        ESP_LOGE(TAG, "Could not import %s; left in place", OLD_SETTINGS_FILE);
        return;
    }
    sd_storage::Fs::remove(OLD_SETTINGS_FILE);
    ESP_LOGI(TAG, "Imported and deleted %s", OLD_SETTINGS_FILE);
}

// gemini_config.json -> model, voice, system prompt. Also the way to set them
// on a fresh device: drop the file on the card. credentials::importFromSdCard
// has already moved any API key to NVS; a file still holding one is left
// alone so the key is never lost.
void migrateGeminiFile(ndb::system::SystemDb& db) {
    std::string content = sd_storage::Fs::readText(OLD_GEMINI_FILE);
    if (content.empty()) {
        if (sd_storage::Fs::isFile(OLD_GEMINI_FILE)) sd_storage::Fs::remove(OLD_GEMINI_FILE);
        return;
    }
    if (content.find("\"api_key\"") != std::string::npos) {
        ESP_LOGW(TAG, "%s still holds an API key; not importing it", OLD_GEMINI_FILE);
        return;
    }
    JsonDocument doc;
    if (!deserializeJson(doc, content) && doc.is<JsonObject>()) {
        ndb::system::Settings s;
        uint64_t fields = 0;
        auto take = [&](const char* key, std::string& dst, uint64_t bit) {
            if (doc[key].is<const char*>()) {
                dst = doc[key].as<const char*>();
                fields |= bit;
            }
        };
        take("model", s.gemini_model, ndb::system::Settings::F_GEMINI_MODEL);
        take("voice", s.gemini_voice, ndb::system::Settings::F_GEMINI_VOICE);
        take("system_prompt", s.gemini_system_prompt, ndb::system::Settings::F_GEMINI_SYSTEM_PROMPT);
        if (fields && !db.settings().merge(SETTINGS_KEY, s, fields)) {
            ESP_LOGE(TAG, "Could not import %s; left in place", OLD_GEMINI_FILE);
            return;
        }
    } else {
        ESP_LOGW(TAG, "%s is not valid JSON; deleting it", OLD_GEMINI_FILE);
    }
    sd_storage::Fs::remove(OLD_GEMINI_FILE);
    ESP_LOGI(TAG, "Imported and deleted %s", OLD_GEMINI_FILE);
}

// Written by an agent rule, read by nothing.
void deleteUnusedFiles() {
    for (const char* path : {"/sdcard/playback.txt", "/sdcard/playback_history.txt"}) {
        if (sd_storage::Fs::isFile(path) && sd_storage::Fs::remove(path)) ESP_LOGI(TAG, "Deleted unused %s", path);
    }
}

std::string idKey(int id) { return std::to_string(id); }

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

template <typename Doc>
std::vector<std::pair<int, Doc>> listById(nexus_db::Collection<Doc> coll) {
    std::vector<std::pair<int, Doc>> out;
    coll.forEach([&](std::string_view key, const Doc& doc) {
        if (int id = parseId(key)) out.emplace_back(id, doc);
        return true;
    });
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

template <typename Doc>
int nextId(nexus_db::Collection<Doc> coll) {
    int max_id = 0;
    coll.forEach([&](std::string_view key, const Doc&) {
        max_id = std::max(max_id, parseId(key));
        return true;
    });
    return max_id + 1;
}

}  // namespace

ndb::system::SystemDb& systemDb() {
    static ndb::system::SystemDb db;
    return db;
}

bool openSystemDb() {
    if (!sd_storage::SdCard::instance().isMounted()) return false;
    // Opening reads the whole log and may run a cleanup, which is too deep
    // for the 3.5 KB main task; do it on a short-lived task and wait.
    struct Job {
        TaskHandle_t caller;
        bool ok;
    } job{xTaskGetCurrentTaskHandle(), false};
    auto work = [](void* arg) {
        auto* j = static_cast<Job*>(arg);
        auto& db = systemDb();
        j->ok = db.open();
        if (j->ok) {
            migrateState(db);
            migrateSettingsFile(db);
            migrateGeminiFile(db);
            deleteUnusedFiles();
        } else {
            ESP_LOGE(TAG, "Could not open system.ndb; settings will not be saved");
        }
        xTaskNotifyGive(j->caller);
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(work, "sysdb_open", 8 * 1024, &job, 5, nullptr) != pdPASS) return false;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return job.ok;
}

ndb::system::Settings loadSettings() {
    ndb::system::Settings s;
    auto& db = systemDb();
    if (db.db().isOpen()) db.settings().get(SETTINGS_KEY, s);
    return s;
}

bool saveSettings(const ndb::system::Settings& settings, uint64_t fields) {
    auto& db = systemDb();
    return db.db().isOpen() && db.settings().merge(SETTINGS_KEY, settings, fields);
}

bool loadAlertConfig(const char* name, ndb::system::AlertConfig& config) {
    auto& db = systemDb();
    return db.db().isOpen() && db.alerts().get(name, config);
}

bool saveAlertConfig(const char* name, const ndb::system::AlertConfig& config, uint64_t fields) {
    auto& db = systemDb();
    return db.db().isOpen() && db.alerts().merge(name, config, fields);
}

bool resetAlertConfig(const char* name) {
    auto& db = systemDb();
    return db.db().isOpen() && db.alerts().remove(name);
}

std::vector<std::pair<int, ndb::system::AlarmDoc>> listAlarms() {
    auto& db = systemDb();
    if (!db.db().isOpen()) return {};
    return listById(db.alarms());
}

bool loadAlarm(int id, ndb::system::AlarmDoc& doc) {
    auto& db = systemDb();
    return db.db().isOpen() && db.alarms().get(idKey(id), doc);
}

bool saveAlarm(int id, const ndb::system::AlarmDoc& doc) {
    auto& db = systemDb();
    return id > 0 && db.db().isOpen() && db.alarms().put(idKey(id), doc);
}

bool mergeAlarm(int id, const ndb::system::AlarmDoc& doc, uint64_t fields) {
    auto& db = systemDb();
    return id > 0 && db.db().isOpen() && db.alarms().merge(idKey(id), doc, fields);
}

bool removeAlarm(int id) {
    auto& db = systemDb();
    return db.db().isOpen() && db.alarms().remove(idKey(id));
}

int nextAlarmId() {
    auto& db = systemDb();
    return db.db().isOpen() ? nextId(db.alarms()) : 1;
}

std::vector<std::pair<int, ndb::system::ReminderDoc>> listReminders() {
    auto& db = systemDb();
    if (!db.db().isOpen()) return {};
    return listById(db.reminders());
}

bool loadReminder(int id, ndb::system::ReminderDoc& doc) {
    auto& db = systemDb();
    return db.db().isOpen() && db.reminders().get(idKey(id), doc);
}

bool saveReminder(int id, const ndb::system::ReminderDoc& doc) {
    auto& db = systemDb();
    return id > 0 && db.db().isOpen() && db.reminders().put(idKey(id), doc);
}

bool mergeReminder(int id, const ndb::system::ReminderDoc& doc, uint64_t fields) {
    auto& db = systemDb();
    return id > 0 && db.db().isOpen() && db.reminders().merge(idKey(id), doc, fields);
}

bool removeReminder(int id) {
    auto& db = systemDb();
    return db.db().isOpen() && db.reminders().remove(idKey(id));
}

int nextReminderId() {
    auto& db = systemDb();
    return db.db().isOpen() ? nextId(db.reminders()) : 1;
}

}  // namespace Services
