// Downloads the Didj dumps from archive.org and verifies them (didj-fetch,
// and the front end's first-run download).
//
// Downloads go through the system curl, which ships with Windows 10 and
// later and with every Linux distribution.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "pkg/dumps.h"

namespace didj::pkg {

std::string dump_url(const DumpFile& d);

// The dumps named by words: "all", "os" (the system and JetPack Heroes), a
// cartridge's name, or a package's name with or without ".lfp". Unless
// verify_only, the system images are always added, since a game is useless
// without them. Returns false and sets *unknown for a word that names nothing.
bool select_dumps(const std::vector<std::string>& words, bool verify_only, std::vector<const DumpFile*>& out,
                  std::string* unknown);

struct FetchOptions {
    bool verify_only = false;
    bool show_progress = true;  // curl draws its progress bar on the console
    const std::atomic<bool>* cancel = nullptr;
    // Called with a status line ("ok        SNC", "download  SNC", ...).
    std::function<void(const std::string&)> log;
};

// Makes dir/d.name present with the right MD5, downloading it into
// <name>.part (resumed if it is already there) when it is missing or wrong.
// Returns true if the file ends up present and verified; otherwise *err says
// why, unless it was cancelled.
bool fetch_dump(const DumpFile& d, const std::filesystem::path& dir, const FetchOptions& opt, std::string* err);

}  // namespace didj::pkg
