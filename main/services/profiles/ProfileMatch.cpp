#include "services/profiles/ProfileMatch.h"

#include <cctype>
#include <cstring>

namespace ProfileMatch {

namespace {

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// "the yoga one" -> "yoga"; never strips the query to nothing.
std::string stripFillers(std::string q) {
    for (const char* prefix : {"the ", "my "}) {
        if (startsWith(q, prefix) && q.size() > strlen(prefix)) q.erase(0, strlen(prefix));
    }
    for (const char* suffix : {" one", " profile", " persona", " mode"}) {
        if (endsWith(q, suffix) && q.size() > strlen(suffix)) q.erase(q.size() - strlen(suffix));
    }
    return q;
}

}  // namespace

std::string normalize(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool space = false;
    for (unsigned char c : in) {
        if (std::isspace(c)) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += char(std::tolower(c));
    }
    return out;
}

int match(const std::vector<std::string>& names, const std::string& query) {
    std::string q = normalize(query);
    if (q.empty()) return NONE;
    for (size_t i = 0; i < names.size(); ++i) {
        if (normalize(names[i]) == q) return int(i);
    }
    q = stripFillers(q);
    int found = NONE;
    for (size_t i = 0; i < names.size(); ++i) {
        std::string n = normalize(names[i]);
        if (n == q) return int(i);
        if (n.find(q) == std::string::npos) continue;
        if (found != NONE) return AMBIGUOUS;
        found = int(i);
    }
    return found;
}

bool sameName(const std::string& a, const std::string& b) {
    return normalize(a) == normalize(b);
}

} // namespace ProfileMatch
