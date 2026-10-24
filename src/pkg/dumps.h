// The dumps didj runs and where archive.org hosts them (see docs/dumps.md).
#pragma once

#include <cstddef>

namespace didj::pkg {

struct DumpFile {
    const char* name;  // file name on archive.org and in the dump directory
    const char* item;  // archive.org item
    const char* md5;
    bool cartridge;
};

inline constexpr DumpFile kDumps[] = {
    {"erootfs.jffs2", "erootfs", "7bb34f01e4689309ef88ef8a29b6cbc9", false},
    {"Didj OS", "didj-roms", "5dc4bc5c60280dfae0248727c8addf01", false},
    {"DFR", "didj-roms", "fd514eebe0f2459dea1bd5a792b8af3b", true},
    {"DRU", "didj-roms", "86aa74cf2f537a0e1d2cbbf346054df2", true},
    {"FOF", "didj-roms", "ca98965b16c020f0fedbed760029e114", true},
    {"HAN", "didj-roms", "a824a136c4ffa578734056ec098977bb", true},
    {"HSM", "didj-roms", "698b7813df94f772b54ba520f093a88b", true},
    {"IND", "didj-roms", "026c1daf9efd8ccfd5c3b2d75bfb4156", true},
    {"JDI", "didj-roms", "d2d9910a5522f01352e49a377fff2b00", true},
    {"LNT", "didj-roms", "52e568dce719c4ae1e06e642ce9fb353", true},
    {"MAC", "didj-roms", "244aab639f4d9d085f12debdcee68f0d", true},
    {"NEO", "didj-roms", "d21b16de1615113806cf4828027e1c2c", true},
    {"NLG", "didj-roms", "d4f4238c68d1d5854b9de9553479534f", true},
    {"NLR", "didj-roms", "28452e806b41436b43159aa0ca3d491c", true},
    {"SNC", "didj-roms", "2a005446936b4a037ed12af4f8b513f9", true},
    {"SW2", "didj-roms", "04b7efb8ba693e611d4f6966a0dda259", true},
    {"WVN", "didj-roms", "4e112497256d75545123960e3cab16ec", true},
};

inline constexpr const char* kRootfsName = "erootfs.jffs2";
inline constexpr const char* kOsName = "Didj OS";

}  // namespace didj::pkg
