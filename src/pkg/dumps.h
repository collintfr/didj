// The dumps didj runs and where archive.org hosts them (see docs/dumps.md).
#pragma once

#include <cstddef>

namespace didj::pkg {

enum class DumpKind {
    System,     // root filesystem and the Didj OS partition
    Cartridge,  // a cartridge image, inserted with --cart
    Package,    // a .lfp game package, installed into /Didj/ProgramFiles
};

struct DumpFile {
    const char* name;  // file name on archive.org and in the dump directory
    const char* item;  // archive.org item
    const char* md5;
    DumpKind kind;
    const char* title = "";  // what the game is called (cartridges and packages)
    bool cartridge() const { return kind == DumpKind::Cartridge; }
};

inline constexpr DumpFile kDumps[] = {
    {"erootfs.jffs2", "erootfs", "7bb34f01e4689309ef88ef8a29b6cbc9", DumpKind::System},
    {"Didj OS", "didj-roms", "5dc4bc5c60280dfae0248727c8addf01", DumpKind::System},
    {"DFR", "didj-roms", "fd514eebe0f2459dea1bd5a792b8af3b", DumpKind::Cartridge, "Tinker Bell and the Lost Treasure"},
    {"DRU", "didj-roms", "86aa74cf2f537a0e1d2cbbf346054df2", DumpKind::Cartridge, "Nancy Drew: Mystery in the Hollywood Hills"},
    {"FOF", "didj-roms", "ca98965b16c020f0fedbed760029e114", DumpKind::Cartridge, "SpongeBob SquarePants: Fists of Foam"},
    {"HAN", "didj-roms", "a824a136c4ffa578734056ec098977bb", DumpKind::Cartridge, "Hannah Montana"},
    {"HSM", "didj-roms", "698b7813df94f772b54ba520f093a88b", DumpKind::Cartridge, "High School Musical"},
    {"IND", "didj-roms", "026c1daf9efd8ccfd5c3b2d75bfb4156", DumpKind::Cartridge, "Indiana Jones and the Kingdom of the Crystal Skull"},
    {"JDI", "didj-roms", "d2d9910a5522f01352e49a377fff2b00", DumpKind::Cartridge, "Star Wars: The Clone Wars"},
    {"LNT", "didj-roms", "52e568dce719c4ae1e06e642ce9fb353", DumpKind::Cartridge, "Nicktoons: Android Invasion"},
    {"MAC", "didj-roms", "244aab639f4d9d085f12debdcee68f0d", DumpKind::Cartridge, "Foster's Home for Imaginary Friends"},
    {"NEO", "didj-roms", "d21b16de1615113806cf4828027e1c2c", DumpKind::Cartridge, "Neopets: Quizara's Curse"},
    {"NLG", "didj-roms", "d4f4238c68d1d5854b9de9553479534f", DumpKind::Cartridge, "Super Chicks"},
    {"NLR", "didj-roms", "28452e806b41436b43159aa0ca3d491c", DumpKind::Cartridge, "Didji Racing: Tiki Tropics"},
    {"SNC", "didj-roms", "2a005446936b4a037ed12af4f8b513f9", DumpKind::Cartridge, "Sonic the Hedgehog"},
    {"SW2", "didj-roms", "04b7efb8ba693e611d4f6966a0dda259", DumpKind::Cartridge, "Star Wars: Jedi Trials"},
    {"WVN", "didj-roms", "4e112497256d75545123960e3cab16ec", DumpKind::Cartridge, "Wolverine and the X-Men"},
    // JetPack Heroes, the game built into every Didj.
    {"POW.lfp", "didj-roms", "b560091d5e24509080f10d11a6a6600c", DumpKind::Package, "JetPack Heroes"},
};

inline constexpr const char* kRootfsName = "erootfs.jffs2";
inline constexpr const char* kOsName = "Didj OS";
inline constexpr const char* kBuiltInGame = "POW.lfp";

}  // namespace didj::pkg
