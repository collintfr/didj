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
- OpenGL ES 1.x is reimplemented on the host: a generated stand-in for `libopengles_lite.so` hands every GL call to a software renderer.

## Getting the dumps

`didj` runs the Didj's original software, which has been preserved on archive.org ([didj-roms](https://archive.org/details/didj-roms) and [erootfs](https://archive.org/details/erootfs)). `didj-fetch` downloads it and checks every file against known-good MD5 sums:

```sh
didj-fetch            # root filesystem, Didj OS and all 15 cartridges (about 1.2 GB)
didj-fetch os SNC     # just the system and Sonic
didj-fetch --verify   # check what is already there
```

It needs `curl`, which comes with Windows 10 and later and with every Linux distribution. Files go to `%LOCALAPPDATA%\didj\dumps` on Windows and `~/.local/share/didj/dumps` on Linux, or to `$DIDJ_DUMPS` if it is set, and `didj-run` looks for them there. Running it again resumes an interrupted download.

| File | What it is |
| --- | --- |
| `erootfs.jffs2` | JFFS2 image of the Didj's root filesystem: uClibc, libstdc++ and the other system libraries |
| `Didj OS` | FAT16 image of the `/Didj` partition: the menu, Brio libraries, fonts, audio |
| `SNC`, `FOF`, `IND`, … | FAT32 images of game cartridges |

[docs/dumps.md](docs/dumps.md) has the checksums and more about each image.

## Building

You need CMake 3.16+ and a C++17 compiler: GCC, Clang or MSVC.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
```

## didj-run

`didj-run` runs the Didj's software headless, with the dumps mounted where the Didj mounts them. Writes go to a host directory (`--data`, default `./didj-data`). With no program it starts the Didj's own launcher, `RealAppManager`. `--cart` takes a fetched cartridge's name or a path to an image, and `--rootfs`/`--os` override the fetched images.

```sh
didj-run --cart SNC --run-seconds 20 --screenshot out.png   # boot to the home screen
didj-run --cart SNC -- /bin/ls -l /Cart/SNC                  # run any program on the Didj
didj-run --strace -- /bin/busybox echo hello                 # log every syscall
```

Headless runs can be scripted: `--input` presses buttons at given times (in seconds) and `--screenshot-at` saves the screen. With a profile already set up, this starts Sonic and takes a picture in Emerald Hill Zone:

```sh
didj-run --cart SNC --run-seconds 70 --input 15:a,20:down,24:a,32:a,42:a,46:a,52:a,58:a \
         --screenshot-at 66:sonic.png
```

Buttons are `up down left right a b l r home hint pause brightness`.

`--stub-devices` answers ioctls on device nodes that have no driver yet and logs them.

The build patches Unicorn (`cmake/patch_unicorn.cmake`): without it, stopping the CPU from another thread to time-slice threads can re-execute instructions and corrupt guest state.

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
3. **Done: user-mode Linux.** ARM926 CPU on Unicorn, ELF loader, a VFS over the dumps with host overlays for writes, about 150 syscalls, green threads with futexes, and signals. The original busybox runs (`ls`, `cat`, `sh`, `md5sum`), and `RealAppManager` loads all of its Brio and Lightning libraries.
4. **Done: `RealAppManager` headless.** POSIX message queues and timers, AF_UNIX sockets, real-time priority scheduling, and the LF1000 devices: evdev buttons, OSS audio, board sysfs, and the MLC/layer/DPC/GPIO display model with `/dev/mem` video memory. `RealAppManager` brings up every Brio module, streams audio, and runs the BLT home screen.
5. **Half done: display, input and audio.** The guest side works: LeapFrog's software drives the emulated display layers, button device and sound device. Still missing: a window, keyboard and gamepad input, and sound on the host.
6. **In progress: OpenGL ES 1.x.** A generated replacement for `libopengles_lite.so` sends every GL and EGL call to a software renderer and hands finished frames to LeapFrog's display library as the original did. The BLT home screen, its first-run setup and Sonic the Hedgehog's menus and gameplay render correctly, at full speed. Lighting, fog and some less common calls are not implemented yet; they are reported the first time a program uses them.
7. Compatibility pass across all cartridges, save data, packaging.
