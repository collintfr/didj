# Dumps

`didj` runs the Didj's original software, so it needs three kinds of dump. `didj-fetch` downloads them from archive.org and checks them against the sums below. All three are bare filesystem images, with no partition table.

| Dump | Filesystem | Mounted on the Didj at | Contents |
| --- | --- | --- | --- |
| `erootfs.jffs2` | JFFS2 | `/` | Root filesystem: uClibc 0.9.29, libstdc++ 6.0.8 (GCC 4.1), libpng 1.2, zlib, busybox, `/dev` nodes |
| `Didj OS` | FAT16, label `Didj` | `/Didj` | `RealAppManager`, the BLT home screen, Brio SDK libraries and modules, `libopengles_lite.so`, fonts, audio, profiles |
| cartridge (`SNC`, …) | FAT32, label `Cartridge` | `/Cart` | `/<3LD>/App.so`, its assets, and `meta.inf` |

## Known-good checksums (MD5)

These come from the archive.org items [erootfs](https://archive.org/details/erootfs) (firmware 1.35.2.4222) and [didj-roms](https://archive.org/details/didj-roms).

```
7bb34f01e4689309ef88ef8a29b6cbc9  erootfs.jffs2
5dc4bc5c60280dfae0248727c8addf01  Didj OS
fd514eebe0f2459dea1bd5a792b8af3b  DFR
86aa74cf2f537a0e1d2cbbf346054df2  DRU
ca98965b16c020f0fedbed760029e114  FOF
a824a136c4ffa578734056ec098977bb  HAN
698b7813df94f772b54ba520f093a88b  HSM
026c1daf9efd8ccfd5c3b2d75bfb4156  IND
d2d9910a5522f01352e49a377fff2b00  JDI
52e568dce719c4ae1e06e642ce9fb353  LNT
244aab639f4d9d085f12debdcee68f0d  MAC
d21b16de1615113806cf4828027e1c2c  NEO
d4f4238c68d1d5854b9de9553479534f  NLG
28452e806b41436b43159aa0ca3d491c  NLR
2a005446936b4a037ed12af4f8b513f9  SNC
04b7efb8ba693e611d4f6966a0dda259  SW2
4e112497256d75545123960e3cab16ec  WVN
```

## Checking a set of dumps

Every package on the FAT volumes carries a `packagefiles.md5`. Extract a volume and verify it like this:

```sh
didjtool extract SNC out/
(cd out/SNC && md5sum -c packagefiles.md5)
```

To check that every library import resolves, run `coverage` over the system libraries plus a game. The only symbols it should report are in `libthread_db` and `libmudflap`, which are debugging libraries that games never load:

```sh
didjtool coverage erootfs.jffs2:/lib erootfs.jffs2:/usr/lib "Didj OS:/Base" SNC:/SNC
```
