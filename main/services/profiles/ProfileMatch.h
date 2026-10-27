#pragma once
// Portable helpers for assistant profiles (host-tested).

#include <string>
#include <vector>

namespace ProfileMatch {

constexpr int NONE = -1;
constexpr int AMBIGUOUS = -2;

// Lower case (ASCII), trimmed, inner spaces collapsed.
std::string normalize(const std::string& in);

// Which of `names` the spoken `query` means: its index, NONE or AMBIGUOUS.
// An exact match (ignoring case and spaces) wins; else the query without
// fillers ("the yoga one" -> "yoga", "technical profile" -> "technical")
// is looked for inside each name, and one name containing it is the match.
int match(const std::vector<std::string>& names, const std::string& query);

// The same name ignoring case and spaces: profile names must differ so.
bool sameName(const std::string& a, const std::string& b);

} // namespace ProfileMatch
