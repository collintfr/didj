// Parser for Didj package meta.inf files: one Key=Value per line, values
// optionally double-quoted (e.g. Name="Sonic the Hedgehog", Hidden=0).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace didj::pkg {

class MetaInf {
public:
    static MetaInf parse(std::string_view text);

    std::optional<std::string> get(const std::string& key) const;
    std::string get_or(const std::string& key, std::string fallback) const;
    const std::map<std::string, std::string>& entries() const { return entries_; }

private:
    std::map<std::string, std::string> entries_;
};

}  // namespace didj::pkg
