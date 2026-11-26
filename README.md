# Filesystem-ReiserFS

## 1. Purpose and format scope

ReiserFS, a journaled tree-based filesystem. This repository is the designated independent implementation home for InfiltratorOS. **Documentation and migration scaffold only: no independent runtime provider or release is present.**

## 2. On-disk architecture and invariants

Superblock versions, keyed balanced trees, item ordering, tail packing, allocation and journal state determine the native layout. Distinguish packed tails from ordinary indirect data and reject inconsistent keys or unsupported replay states. These are design requirements for the native implementation, not claims about code already present in this repository.

## 3. Common V2 implementation and capabilities

No local Common V2 implementation or advertised operation set has yet been qualified. The target entry point is `infiltrator_filesystem_api_v2`, using bounded, partition-relative host I/O. Implement native probe first, then mount, stat, enumeration, read and volume query where applicable. Consumers must negotiate supported groups and profiles; an unsupported operation must not report success or delegate to a private driver.

## 4. Integrity, mutation, allocation and recovery

Mutation, formatting, check/repair and DEFRAG map/plan/execute/verify/recover require format-specific allocation and ordering invariants, exclusive writable host access and demonstrated interruption recovery. Until qualified, reject them before writing. Preserve native disk semantics and do not add a kernel or FUSE fallback as an alternative filesystem authority.

## 5. Build, installation and release

The target installation is `/System/Filesystems/REISERFS.so` and `/System/Resources/Documentation/Filesystem-ReiserFS.pdf`. Neither file is currently distributed by this repository. Once the implementation qualifies, the intended release is a portable amd64 DEB and CPU-native source-first RUN, with GitHub's automatic source archive. No install or build command is advertised before an executable build exists.

## 6. Qualification and limitations

Independent ReiserFS images, malformed items, invalid key ordering and dirty journals. Compare independent payloads and unchanged-image hashes; inject truncation, corruption and I/O faults. Establish exact-revision ABI, hosted and import-free freestanding qualification, then independently test BOOT and kernel integration. No local build, runtime or packaging qualification is claimed.

## 7. Technical references and provenance

- [Technical reference (PDF)](docs/ReiserFS.pdf) — original-format technical reference with historical appendices and [PDF stream source](docs/ReiserFS.pdfstreams); not an operational manual.

- [Pinned central source baseline](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/reiserfs)
- [Native design](native/filesystems/reiserfs/DESIGN.md)
- [Migration and qualification plan](MIGRATION_STATUS.md)
- [Original-format reference source](docs/ReiserFS.pdfstreams) and [PDF](docs/ReiserFS.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

Preserve inherited notices and licences on source extraction; new project-authored code uses GPL-3.0-or-later. The [technical reference](docs/ReiserFS-TECHNICAL-REFERENCE.md) expands original-format research. The PDF preserves both older documents as historical appendices; installation remains pending. Central code or test results are not evidence of an independent provider.
