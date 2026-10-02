#include "services/storage/TuningFile.h"
#include "core_sysdb/Tuning.h"
#include "common/AppLogger.h"
#include "sd_storage/File.h"
#include <ArduinoJson.h>
#include <string>

namespace Services {

void loadTuningFile() {
    sd_storage::File f = sd_storage::File::open(TUNING_FILE, sd_storage::Mode::Read);
    if (!f) return;
    constexpr long kMaxBytes = 2048;  // a handful of flags
    const long size = f.size();
    if (size <= 0 || size > kMaxBytes) {
        LOGW_SYSTEM("%s: %ld bytes, expected 1-%ld; ignored", TUNING_FILE, size, kMaxBytes);
        return;
    }
    std::string text(static_cast<size_t>(size), '\0');
    if (!f.readExact(text.data(), text.size())) {
        LOGW_SYSTEM("%s: read failed; ignored", TUNING_FILE);
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, text) || !doc.is<JsonObjectConst>()) {
        LOGW_SYSTEM("%s: not a JSON object; ignored", TUNING_FILE);
        return;
    }
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
        JsonVariantConst v = kv.value();
        int32_t value;
        if (v.is<bool>()) {
            value = v.as<bool>() ? 1 : 0;
        } else if (v.is<int32_t>()) {
            value = v.as<int32_t>();
        } else {
            LOGW_SYSTEM("%s: %s is not an integer; ignored", TUNING_FILE, kv.key().c_str());
            continue;
        }
        Tuning::set(kv.key().c_str(), value);
        LOGI_SYSTEM("Tuning flag %s = %ld", kv.key().c_str(), (long)value);
    }
}

}  // namespace Services
