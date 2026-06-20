#pragma once

#include <string>
#include <sstream>

namespace Utils {

class ParserUtils {
public:
    // Trims leading and trailing whitespace and carriage returns
    static std::string trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, last - first + 1);
    }

    // Splits a string into key and value by a delimiter (e.g. '=')
    static bool splitKeyValue(const std::string& line, char delimiter, std::string& key, std::string& value) {
        size_t pos = line.find(delimiter);
        if (pos == std::string::npos) return false;
        key = trim(line.substr(0, pos));
        value = trim(line.substr(pos + 1));
        return true;
    }

    // Callback function type for key-value pairs
    typedef void (*KeyValueCallback)(const std::string& key, const std::string& value, void* ctx);

    // Parses a multi-line string stream, executing a callback for each key-value pair
    static void parseKeyValueStream(const std::string& stream, KeyValueCallback cb, void* ctx) {
        if (!cb) return;
        std::stringstream ss(stream);
        std::string line;
        while (std::getline(ss, line)) {
            std::string trimmed_line = trim(line);
            
            // Skip comments and empty lines
            if (trimmed_line.empty() || trimmed_line[0] == '#' || trimmed_line[0] == ';') {
                continue;
            }

            std::string key, val;
            if (splitKeyValue(trimmed_line, '=', key, val)) {
                if (!key.empty()) {
                    cb(key, val, ctx);
                }
            }
        }
    }
};

} // namespace Utils
