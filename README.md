# didj

An emulator for the **LeapFrog Didj** (2008) for Linux and Windows. It is meant to keep the Didj's games playable.

No Didj emulator exists yet. MAME's `leapfrog_didj` driver is an empty stub. This project is at an early stage. See [Status](#status).

## Approach

The Didj runs a small Linux system on a LeapFrog LF1000 SoC (ARM926EJ-S). Each game is an ARM Linux shared library (`App.so`). It uses three layers below it:
- LeapFrog's "Brio" SDK libraries (display, audio, buttons, events, fonts).
- An OpenGL ES 1.x driver (`libopengles_lite.so`).
- uClibc and libstdc++.

Rather than emulating the whole SoC (kernel, NAND, an undocumented 3D core), `didj` is a **user-mode emulator**, along the same lines as `qemu-user`:

- The original ARM code runs unmodified: the game, LeapFrog's libraries, and the `RealAppManager` menu.
- Linux system calls and the LF1000 device nodes (display layers, buttons, audio) are emulated on the host.
- OpenGL ES 1.x calls are translated to host OpenGL.

## What you need

Your own dumps of the following. None of them are included in or distributed with this project.

| File | What it is |
| --- | --- |
| `erootfs.jffs2` | JFFS2 image of the Didj's root filesystem: uClibc, libstdc++ and the other system libraries |
| `Didj OS` | FAT16 image of the `/Didj` partition: the menu, Brio libraries, fonts, audio |
| `SNC`, `FOF`, `IND`, … | FAT32 images of game cartridges |

[docs/dumps.md](docs/dumps.md) lists known-good checksums and explains how to verify a set.

## Building

You need CMake 3.16+ and a C++17 compiler: GCC, Clang or MSVC.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
```

## didjtool

`didjtool` reads dumps directly, both FAT and JFFS2. You don't need to mount them.

```sh
didjtool info SNC                       # volume info and the cartridge's meta.inf
didjtool ls erootfs.jffs2 /dev          # symlinks and device nodes are shown
didjtool ls "Didj OS" /Base -r          # list files
didjtool cat SNC /SNC/meta.inf          # print a file
didjtool extract SNC /SNC out/          # copy files to the host
didjtool elf SNC:/SNC/App.so            # NEEDED libraries, imports and exports
didjtool coverage "Didj OS:/Base" SNC:/SNC   # imports that nothing provides
```

`coverage` reports any imported symbol or library that none of the given files provide.

## Status

1. **Done: skeleton and tools.** FAT12/16/32 reader, ELF dynamic-section reader, `didjtool`, CI on Linux and Windows. Every file in the OS and Sonic dumps matches LeapFrog's own `packagefiles.md5` checksums.
2. **Done: system libraries.** A JFFS2 reader with a built-in inflate loads the original root filesystem. With it, every import from the OS partition and Sonic resolves.
3. ARM CPU (Unicorn), ELF loader, Linux syscall emulation.
4. `RealAppManager` running headless.
5. Display, buttons and audio, which brings up the menu.
6. OpenGL ES 1.x translation, which makes games playable.
7. Compatibility pass across all cartridges, save data, packaging.
