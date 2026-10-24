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
- The C/C++ runtime that lived on the Didj's internal flash isn't in the public dumps, so it is rebuilt from open source (uClibc, libstdc++, libpng 1.2, zlib).

## What you need

Your own dumps, in the format of the [didj-roms](https://archive.org/details/didj-roms) archive:

| File | What it is |
| --- | --- |
| `Didj OS` | FAT16 image of the Didj's `/Didj` partition: the menu, Brio libraries, fonts, audio |
| `SNC`, `FOF`, `IND`, … | FAT32 images of game cartridges |

No game or system files are included in or distributed with this project.

## Building

You need CMake 3.16+ and a C++17 compiler: GCC, Clang or MSVC.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
```

## didjtool

`didjtool` reads dumps directly. You don't need to mount them.

```sh
didjtool info SNC                       # volume info and the cartridge's meta.inf
didjtool ls "Didj OS" /Base -r          # list files
didjtool cat SNC /SNC/meta.inf          # print a file
didjtool extract SNC /SNC out/          # copy files to the host
didjtool elf SNC:/SNC/App.so            # NEEDED libraries, imports and exports
didjtool coverage "Didj OS:/Base" SNC:/SNC   # imports that nothing provides
```

`coverage` reports the symbols and libraries the rebuilt runtime has to supply. For the Didj OS plus Sonic, that's [docs/required-symbols.txt](docs/required-symbols.txt).

## Status

1. **Done: skeleton and tools.** FAT12/16/32 reader, ELF dynamic-section reader, `didjtool`, CI on Linux and Windows. Every file in the OS and Sonic dumps matches LeapFrog's own `packagefiles.md5` checksums.
2. Rebuilt ARM sysroot: uClibc, libstdc++, libgcc_s, libpng12, zlib.
3. ARM CPU (Unicorn), ELF loader, Linux syscall emulation.
4. `RealAppManager` running headless.
5. Display, buttons and audio, which brings up the menu.
6. OpenGL ES 1.x translation, which makes games playable.
7. Compatibility pass across all cartridges, save data, packaging.
