#include "pkg/metainf.h"

namespace didj::pkg {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

MetaInf MetaInf::parse(std::string_view text) {
    MetaInf m;
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        size_t eq = line.find('=');
        if (line.empty() || line.front() == '#' || eq == std::string_view::npos) continue;
        std::string_view key = trim(line.substr(0, eq));
        std::string_view value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        m.entries_[std::string(key)] = std::string(value);
    }
    return m;
}

std::optional<std::string> MetaInf::get(const std::string& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

std::string MetaInf::get_or(const std::string& key, std::string fallback) const {
    auto v = get(key);
    return v ? *v : std::move(fallback);
}

}  // namespace didj::pkg
