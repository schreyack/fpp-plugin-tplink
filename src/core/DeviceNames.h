#pragma once

// Finding configured devices by the name a person gave them. FPP commands and
// playlists name a plug ("LED Roof"), never its IP; the name is the "name"
// field of plugin.tplink.json. A target is one name or a comma list of names.
// Matching ignores case and the spaces around each name. FPP-free, so
// tests/DeviceNamesTests.cpp builds it on a Mac.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace tplink {
namespace names {

inline std::string trim(std::string const& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

inline bool same(std::string const& a, std::string const& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// The names in a target, trimmed, blanks dropped.
inline std::vector<std::string> split(std::string const& target) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= target.size()) {
        std::size_t comma = target.find(',', pos);
        if (comma == std::string::npos) comma = target.size();
        std::string token = trim(target.substr(pos, comma - pos));
        if (!token.empty()) out.push_back(std::move(token));
        pos = comma + 1;
    }
    return out;
}

struct Resolved {
    std::vector<std::size_t> indices;  // into the names given to resolve, in target order, no repeats
    std::vector<std::string> unknown;  // the target's names that match nothing
};

// Resolves a target against the configured names. The first configured device
// with a matching name wins (load() warns about duplicates).
inline Resolved resolve(std::string const& target, std::vector<std::string> const& configured) {
    Resolved r;
    for (auto const& token : split(target)) {
        auto it = std::find_if(configured.begin(), configured.end(),
                               [&](std::string const& n) { return same(n, token); });
        if (it == configured.end()) {
            r.unknown.push_back(token);
            continue;
        }
        std::size_t idx = static_cast<std::size_t>(it - configured.begin());
        if (std::find(r.indices.begin(), r.indices.end(), idx) == r.indices.end()) r.indices.push_back(idx);
    }
    return r;
}

// The names that more than one configured device uses (ignoring case), each once.
inline std::vector<std::string> duplicates(std::vector<std::string> const& configured) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < configured.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (same(configured[i], configured[j])) {
                bool listed = std::any_of(out.begin(), out.end(),
                                          [&](std::string const& n) { return same(n, configured[i]); });
                if (!listed) out.push_back(configured[i]);
                break;
            }
        }
    }
    return out;
}

}  // namespace names
}  // namespace tplink
