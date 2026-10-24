// Where didj-fetch puts the dumps and didj-run looks for them.
#pragma once

#include <filesystem>

namespace didj::pkg {

// $DIDJ_DUMPS if set; otherwise the per-user data directory:
// %LOCALAPPDATA%\didj\dumps on Windows, $XDG_DATA_HOME/didj/dumps (default
// ~/.local/share/didj/dumps) elsewhere. Returns an empty path if none of the
// variables it needs are set.
std::filesystem::path default_dump_dir();

}  // namespace didj::pkg
