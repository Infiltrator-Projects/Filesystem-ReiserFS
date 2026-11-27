# Filesystem-ReiserFS

## 1. Purpose and format scope

ReiserFS, a journaled tree-based filesystem. This repository is the designated independent implementation home for InfiltratorOS. **Independent Common V2 read-only development provider. No published release yet.**

## 2. On-disk architecture and invariants

Superblock versions, keyed balanced trees, item ordering, tail packing, allocation and journal state determine the native layout. Distinguish packed tails from ordinary indirect data and reject inconsistent keys or unsupported replay states. The implemented profile is limited to clean modern-layout ReiserFS 3.5/3.6 volumes with distributed bitmaps and an internal journal.

## 3. Common V2 implementation and capabilities

The C engine exports only `infiltrator_filesystem_api_v2`, with native CORE probe, mount, unmount, stat, enumeration and read, VOLUME query/label and METADATA mode/readlink. Hosted and import-free AMD64 firmware profiles share this source. Journal replay, relocated or external journals, legacy 8 KiB superblocks, writes and defragmentation are not implemented.

## 4. Integrity, mutation, allocation and recovery

Mutation, formatting, check/repair and DEFRAG map/plan/execute/verify/recover require format-specific allocation and ordering invariants, exclusive writable host access and demonstrated interruption recovery. Until qualified, reject them before writing. Preserve native disk semantics and do not add a kernel or FUSE fallback as an alternative filesystem authority.

## 5. Build, installation and release

The target installation is `/System/Filesystems/REISERFS.so` and `/System/Resources/Documentation/Filesystem-ReiserFS.pdf`. Neither file is currently distributed by this repository. Once the implementation qualifies, the intended release is a portable amd64 DEB and CPU-native source-first RUN, with GitHub's automatic source archive. For development, check out the exact Libraries revision pinned in CI and run `make carrier` or `make check`. These targets are not installable releases.

## 6. Qualification and limitations

Independent ReiserFS images, malformed items, invalid key ordering and dirty journals. Compare independent payloads and unchanged-image hashes; inject truncation, corruption and I/O faults. Establish exact-revision ABI, hosted and import-free freestanding qualification, then independently test BOOT and kernel integration. The [first hosted and firmware fixture and sanitizer run](https://github.com/Infiltrator-Projects/Filesystem-ReiserFS/actions/runs/45955661353) passed at revision `fa9da0a9bb8dea45e7b8fca08ed9feae253e4b9a`. It covers mixed key formats, internal tree nodes, packed tails, explicit sparse pointers, symbolic links and malformed tree rejection. Independent-media and signed BOOT qualification must be recorded separately.

## 7. Technical references and provenance

- [Technical reference (PDF)](docs/ReiserFS.pdf) — original-format technical reference as one integrated manual and [PDF stream source](docs/ReiserFS.pdfstreams); not an operational manual.

- [Pinned central source baseline](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/reiserfs)
- [Native design](native/filesystems/reiserfs/DESIGN.md)
- [Migration and qualification plan](MIGRATION_STATUS.md)
- [Original-format reference source](docs/ReiserFS.pdfstreams) and [PDF](docs/ReiserFS.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

Preserve inherited notices and licences on source extraction; new project-authored code uses GPL-3.0-or-later. The [technical reference](docs/ReiserFS-TECHNICAL-REFERENCE.md) expands original-format research. The PDF preserves both older documents as historical appendices; installation remains pending. Central code or test results are not evidence of an independent provider.
