// Linux ARM (2.6.20, EABI) ABI constants as seen by the guest. These are
// defined here rather than taken from host headers so the emulator behaves
// the same on Windows and Linux hosts.
#pragma once

#include <cstdint>

namespace didj::emu::lx {

// errno values (asm-generic/errno-base.h, errno.h)
enum : int {
    kEPERM = 1, kENOENT = 2, kESRCH = 3, kEINTR = 4, kEIO = 5, kENXIO = 6, kE2BIG = 7, kENOEXEC = 8,
    kEBADF = 9, kECHILD = 10, kEAGAIN = 11, kENOMEM = 12, kEACCES = 13, kEFAULT = 14, kEBUSY = 16,
    kEEXIST = 17, kEXDEV = 18, kENODEV = 19, kENOTDIR = 20, kEISDIR = 21, kEINVAL = 22, kENFILE = 23,
    kEMFILE = 24, kENOTTY = 25, kEFBIG = 27, kENOSPC = 28, kESPIPE = 29, kEROFS = 30, kEMLINK = 31,
    kEPIPE = 32, kERANGE = 34, kEDEADLK = 35, kENAMETOOLONG = 36, kENOSYS = 38, kENOTEMPTY = 39,
    kELOOP = 40, kENOMSG = 42, kENOTSOCK = 88, kEOPNOTSUPP = 95, kEAFNOSUPPORT = 97, kETIMEDOUT = 110,
};

// open(2) flags; ARM differs from x86 for O_DIRECTORY/O_NOFOLLOW/O_LARGEFILE.
enum : uint32_t {
    kO_ACCMODE = 03, kO_RDONLY = 0, kO_WRONLY = 01, kO_RDWR = 02, kO_CREAT = 0100, kO_EXCL = 0200,
    kO_NOCTTY = 0400, kO_TRUNC = 01000, kO_APPEND = 02000, kO_NONBLOCK = 04000, kO_SYNC = 010000,
    kO_DIRECTORY = 040000, kO_NOFOLLOW = 0100000, kO_LARGEFILE = 0400000,
};

// st_mode type bits
enum : uint32_t {
    kS_IFMT = 0170000, kS_IFSOCK = 0140000, kS_IFLNK = 0120000, kS_IFREG = 0100000, kS_IFBLK = 0060000,
    kS_IFDIR = 0040000, kS_IFCHR = 0020000, kS_IFIFO = 0010000,
};

// dirent d_type
enum : uint8_t { kDT_UNKNOWN = 0, kDT_FIFO = 1, kDT_CHR = 2, kDT_DIR = 4, kDT_BLK = 6, kDT_REG = 8, kDT_LNK = 10, kDT_SOCK = 12 };

// lseek whence
enum : int { kSEEK_SET = 0, kSEEK_CUR = 1, kSEEK_END = 2 };

// poll events
enum : int16_t { kPOLLIN = 0x1, kPOLLPRI = 0x2, kPOLLOUT = 0x4, kPOLLERR = 0x8, kPOLLHUP = 0x10, kPOLLNVAL = 0x20 };

// access(2)
enum : int { kF_OK = 0, kX_OK = 1, kW_OK = 2, kR_OK = 4 };

// fcntl
enum : int {
    kF_DUPFD = 0, kF_GETFD = 1, kF_SETFD = 2, kF_GETFL = 3, kF_SETFL = 4, kF_GETLK = 5, kF_SETLK = 6,
    kF_SETLKW = 7, kF_GETLK64 = 12, kF_SETLK64 = 13, kF_SETLKW64 = 14,
};
constexpr uint32_t kFD_CLOEXEC = 1;

inline uint32_t dev_major(uint32_t rdev) { return (rdev >> 8) & 0xfff; }
inline uint32_t dev_minor(uint32_t rdev) { return (rdev & 0xff) | ((rdev >> 12) & 0xfff00); }
inline uint32_t make_dev(uint32_t major, uint32_t minor) {
    return (minor & 0xff) | ((major & 0xfff) << 8) | ((minor & ~0xffu) << 12);
}

}  // namespace didj::emu::lx
