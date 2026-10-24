#include "pkg/paths.h"

#include <cstdlib>

namespace didj::pkg {

namespace {

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

}  // namespace

std::filesystem::path default_dump_dir() {
    if (const char* d = env("DIDJ_DUMPS")) return std::filesystem::u8path(d);
#ifdef _WIN32
    if (const char* d = env("LOCALAPPDATA")) return std::filesystem::u8path(d) / "didj" / "dumps";
#else
    if (const char* d = env("XDG_DATA_HOME")) return std::filesystem::u8path(d) / "didj" / "dumps";
    if (const char* d = env("HOME")) return std::filesystem::u8path(d) / ".local" / "share" / "didj" / "dumps";
#endif
    return {};
}

}  // namespace didj::pkg
