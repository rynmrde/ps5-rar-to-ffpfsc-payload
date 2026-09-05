# MkPFS for PS5 — final verified report

## Outcome

The dedicated repository is [rynmrde/mkpfs-ps5](https://github.com/rynmrde/mkpfs-ps5). The latest implementation commit is `51648f7`, titled `Implement native PFSC encoder and PFS wrapper`.

This revision preserves the upstream GPLv3-or-later Web File Manager base and adds a real native PFSC encoder and verifier, a native four-inode PFS wrapper around a prepared exFAT image, host utilities, regression coverage, compatibility documentation, and updated build instructions.

The complete requested folder-to-`.ffpfsc` workflow is **not yet complete**. The native `mkpfs_convert_folder` entry point remains guarded with `ENOTSUP`, so the project does not claim that an arbitrary source directory can already be converted on PS5. It also does not claim a PS5 ELF or runtime verification.

## Repository status

| Item | Verified value |
|---|---|
| Repository | [https://github.com/rynmrde/mkpfs-ps5](https://github.com/rynmrde/mkpfs-ps5) |
| Branch | `main` |
| Latest commit | `51648f7` — `Implement native PFSC encoder and PFS wrapper` |
| Existing release | [v0.1.0-alpha](https://github.com/rynmrde/mkpfs-ps5/releases/tag/v0.1.0-alpha) |
| Native PFSC output | Implemented and verified against upstream decoder |
| Native four-inode PFS wrapper | Implemented and verified by upstream MkPFS verifier |
| Folder-to-exFAT serializer | Not yet implemented; conversion remains guarded |
| PS5 ELF | Not produced or runtime-tested |

## Verified build and compatibility results

The following commands passed in the sandbox:

```sh
make test-native
make linux
make mkpfs-pfsc mkpfs-wrap-exfat
```

A native-generated PFSC payload was decoded by the upstream MkPFS `decode_pfsc_payload` implementation and reproduced all `589824` source bytes. A native-generated four-inode wrapper around an upstream-generated exFAT fixture was checked by the upstream MkPFS verifier with zero warnings and zero errors. The verifier reported four inodes, one directory, one compressed file, 524288 logical bytes, CRC32 `0xEB87F8DD`, and manifest SHA-256 `54e2a33b28fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.

The native PFSC path uses the upstream `<iiiiqqQq` header layout, 64 KiB logical blocks, zlib streams, raw-block fallback, monotonic offset tables, bounded streaming buffers, cancellation checks, progress callbacks, temporary output files, atomic rename, and structural verification.

## Host utilities

The repository now provides these host-side utilities:

```sh
./tools/mkpfs-pfsc INPUT_PFS OUTPUT.ffpfsc 7
./tools/mkpfs-wrap-exfat INPUT.exfat OUTPUT.ffpfsc TITLEID.exfat
```

The resulting wrapper can be checked with the upstream MkPFS checkout using:

```sh
python3 -m mkpfs verify OUTPUT.ffpfsc
```

The wrapper consumes a prepared raw exFAT image. It does not yet generate that exFAT image from an arbitrary folder in native code.

## Remaining work

The main unimplemented component is the native folder-to-exFAT serializer and its integration with the existing file-manager task/API/UI workflow. That work includes the native directory-tree allocator, exFAT boot regions, FAT, allocation bitmap, up-case table, directory entry sets, streamed file payloads, title-ID naming, conversion-task progress and cancellation, and end-to-end fixture testing from a source folder through a PS5-compatible `.ffpfsc` image.

The public Prospero SDK repository was inspected during the work, but the available checkout was incomplete for this build: the wrapper required additional host compiler setup and the target sysroot lacked headers needed by the inherited file manager. Therefore no PS5 ELF is claimed.

## Preserved application features

The inherited payload still provides browsing and sorting, copy, move, delete, rename, folder creation, text editing, multi-selection, upload and download, background task progress, cancellation, responsive English/Chinese UI, embedded assets, startup notifications, and the reference Home Screen launcher flow.

## Licensing and attribution

The project remains GPLv3-or-later. MkPFS is GPLv3-or-later, and libmicrohttpd is LGPL. The original notices and third-party attribution are preserved in `LICENSE` and `THIRD_PARTY_NOTICES.md`.
