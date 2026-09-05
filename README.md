# MkPFS PS5

This repository is a dedicated integration project based on [PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) and [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager). It preserves the Web File Manager payload, embedded HTTP server, file browser, background file tasks, responsive assets, and PS5 launcher reference while adding a native C implementation of the verified PFSC stream and the four-inode PS5 PFS wrapper used by the upstream exFAT workflow.

## Current implementation status

The native code now performs real upstream-compatible PFSC block encoding and verification. It uses the MkPFS PFSC header layout `<iiiiqqQq`, 64 KiB logical blocks, zlib streams, raw-block fallback when compression is not beneficial, monotonic block-offset tables, cancellation checks, progress callbacks, temporary output files, and atomic rename. The native PFSC output has been decoded successfully by the upstream MkPFS `decode_pfsc_payload` implementation.

The native code also builds the upstream-compatible four-inode PFS wrapper around a prepared raw exFAT image. The resulting wrapper was checked by the upstream MkPFS verifier with zero warnings and zero errors, reporting four inodes, one directory, one compressed file, the expected 512 KiB logical payload, and a valid data CRC and manifest.

The remaining boundary is the folder-to-exFAT serializer. `mkpfs_convert_folder` still returns `ENOTSUP` rather than pretending that a directory has been converted. The native wrapper can already consume an exFAT input, but the native folder scanner, exFAT allocator, boot-region serializer, FAT, allocation bitmap, up-case table, directory-entry generator, and streamed file-data emitter still need to be completed before the end-to-end folder conversion is production-ready.

## Host build and verification

Host-only targets do not require the PS5 SDK:

```sh
make test-native
make mkpfs-pfsc
make mkpfs-wrap-exfat
```

`make linux` builds the Linux file-manager payload and links the native PFSC implementation with zlib. The utility targets are useful for reproducible compatibility checks:

```sh
./tools/mkpfs-pfsc INPUT_PFS OUTPUT.ffpfsc 7
./tools/mkpfs-wrap-exfat INPUT.exfat OUTPUT.ffpfsc TITLEID.exfat
```

The upstream Python MkPFS verifier can validate the wrapped result:

```sh
python3 -m mkpfs verify OUTPUT.ffpfsc
```

The current native regression suite covers absolute-path normalization, traversal rejection, bounded folder scanning, native PFSC creation, upstream-compatible PFSC structural verification, corruption detection, cancellation-safe temporary cleanup, and the guarded folder-conversion boundary.

## PS5 build status

The project keeps the reference PS5 SDK convention:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

A public Prospero SDK checkout was inspected during this work. In the current sandbox its wrapper scripts and target sysroot were incomplete for this project: the wrapper required an additional host compiler path and the target sysroot lacked headers needed by the inherited file manager. Consequently, no PS5 ELF is claimed as successfully built or runtime-tested in this revision.

## Preserved file-manager features

The inherited payload provides browsing and sorting, copy, move, delete, rename, folder creation, text editing, multi-selection, upload and download, background progress, cancellation, responsive English/Chinese UI, embedded assets, startup notifications, and the reference Home Screen launcher flow. The HTTP server listens on port `8888` by default and attempts the next available port when needed.

## Attribution and licensing

The project is distributed under GPLv3-or-later. The original Web File Manager notices and third-party attribution are preserved. MkPFS is GPLv3-or-later. libmicrohttpd is LGPL; redistributions must satisfy its applicable terms. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
