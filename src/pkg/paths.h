// Where didj-fetch puts the dumps and didj-run looks for them.
#pragma once

#include <filesystem>

namespace didj::pkg {

// $DIDJ_DUMPS if set; otherwise the per-user data directory:
// %LOCALAPPDATA%\didj\dumps on Windows, $XDG_DATA_HOME/didj/dumps (default
// ~/.local/share/didj/dumps) elsewhere. Returns an empty path if none of the
// variables it needs are set.
std::filesystem::path default_dump_dir();

// Where the windowed front end keeps what the Didj writes (profiles, saves):
// $DIDJ_DATA if set, otherwise "data" next to the default dump directory.
std::filesystem::path default_data_dir();

}  // namespace didj::pkg
