# Filesystem-QNX4

## 1. Purpose and format scope

QNX 4 filesystem, distinct from the QNX 6 Power-Safe format. This repository is the designated independent implementation home for InfiltratorOS. **Independent hosted native Common V2 implementation exists; release qualification remains incomplete.**

## 2. On-disk architecture and invariants

Superblock geometry, inode and directory records, allocation bitmaps and extent chains determine native file ownership. Validate record references and the entire extent chain within the partition. The hosted native provider implements these structures and checks their ownership.

## 3. Common V2 implementation and capabilities

The hosted implementation is exposed through `infiltrator_filesystem_api_v2`, using bounded, partition-relative host I/O. Native probe, mount, stat, enumeration, read, volume query, formatting, allocation mapping and journalled namespace and payload mutations are implemented. Consumers must negotiate supported groups and profiles; an unsupported operation must not report success or delegate to a private driver.

## 4. Integrity, mutation, allocation and recovery

Mutation, formatting, check/repair and DEFRAG map/plan/execute/verify/recover require format-specific allocation and ordering invariants, exclusive writable host access and demonstrated interruption recovery. Unsupported operations must be rejected before writing. Native relocation and persistent growth reservation are implemented; independent consumer qualification remains required. Preserve native disk semantics and do not add a kernel or FUSE fallback as an alternative filesystem authority.

## 5. Build, installation and release

The target installation is `/System/Filesystems/QNX4.so` and `/System/Resources/Documentation/Filesystem-QNX4.pdf`. Neither file is currently distributed by this repository. Once the implementation qualifies, the intended release is a portable amd64 DEB and CPU-native source-first RUN, with GitHub's automatic source archive. The source can be built and tested using the repository Makefile with the pinned Common dependency; no production installation is qualified.

## 6. Qualification and limitations

Independent QNX4 media, invalid inode pointers, malformed extents and allocation contradictions. Compare independent payloads and unchanged-image hashes; inject truncation, corruption and I/O faults. Establish exact-revision ABI, hosted and import-free freestanding qualification, then independently test BOOT and kernel integration. Hosted synthetic-image build, runtime and journal interruption tests have passed; independent foreign-media, freestanding, packaging and full consumer qualification remain outstanding.

## 7. Technical references and provenance

- [Technical reference (PDF)](docs/QNX4.pdf) — original-format technical reference as one integrated manual and [PDF stream source](docs/QNX4.pdfstreams); not an operational manual.

- [Pinned central source baseline](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/qnx4)
- [Native design](native/filesystems/qnx4/DESIGN.md)
- [Migration and qualification plan](MIGRATION_STATUS.md)
- [Original-format reference source](docs/QNX4.pdfstreams) and [PDF](docs/QNX4.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

Preserve inherited notices and licences on source extraction; new project-authored code uses GPL-3.0-or-later. The [technical reference](docs/QNX4-TECHNICAL-REFERENCE.md) expands original-format research. installation remains pending. Central code or test results are not evidence of an independent provider.

## Development implementation 0.1.0

A native hosted read-only Common V2 reader now implements root/bitmap discovery, direct and extended extents, directory and inode-file links, file reads, physical maps and allocation maps. Synthetic structure and corruption tests cover both 512 and 4096 byte host sectors. Independent media, full integrity reconciliation, writable operations, recovery, relocation, formatter, freestanding profile and publication remain outstanding. No unsupported mutation capability is advertised.

Version 0.1.1 adds the native nonbootable format and Common V2 format profile, root-block alias handling and the inode-file signature check. The formatter initialises the reserved bitmap, four-block directory and sixteen-block inode file, then publishes the root record after a durable barrier. Mount remains read-only. References: https://www.qnx.com/developers/docs/qnx_4.25_docs/qnx4/user_guide/recovery.html and https://codebrowser.dev/linux/linux/include/uapi/linux/qnx4_fs.h.html.

Version 0.1.2 adds an independent whole-volume check: reconcile all visited file and directory extents (including extent blocks), inode-file link reference counts, duplicate directory names and bitmap ownership. It is diagnostic only and never modifies a mounted image. Native mutations and durable recovery are still absent.

Version 0.1.4 introduces a native allocated journal-owner file and durable physical-page redo transactions, with writable mounts restricted to this provider-formatted profile. Native regular-file and directory creation use real inode-file link records and actual allocation. Existing directory/inode-file capacity is bounded; payload writes, dynamic expansion, removal/rename and interruption qualification are still outstanding.

## Development implementation 0.1.8

Versions 0.1.6–0.1.8 validate actual parent backlinks, implement native journalled rename/move and removal, and support transactional copy-on-write payload writes and truncation, including zero-filled extension. Existing unchanged extents are preserved; direct prepublication writes are restricted to wholly unallocated physical pages. Mixed pages and inode/bitmap changes use the redo transaction. Existing directory and inode-file capacity remains bounded. Foreign writable mounts, relocation, persistent growth reservations and full consumer qualification are not implemented or qualified. The exact [0.1.8 implementation CI run](https://github.com/Infiltrator-Projects/Filesystem-QNX4/actions/runs/45927194924) passed normal and sanitizer builds, including 270 journal interruption cases per build across 512- and 4096-byte sector sizes. This is synthetic-image evidence, not independent interoperability or package qualification.

## Development implementation 0.1.11

Native extent relocation, persistent 10% growth preallocation and reservation consumption are implemented. Plans are provider-owned and verified against native extents and payload digests. The exact [0.1.10 CI](https://github.com/Infiltrator-Projects/Filesystem-QNX4/actions/runs/45928052952) passed 462 interruption cases per normal and sanitizer build. The [0.1.11 CI](https://github.com/Infiltrator-Projects/Filesystem-QNX4/actions/runs/45928139049) also passed dynamic directory and inode-file growth across initial formatting boundaries, including reopen and removal. These are synthetic native tests. A separate pinned production-consumer job now gates the full 200 MiB corpus and both mutation policies; its result is not yet established. Foreign media mutation, independent interoperability, freestanding, BOOT and kernel qualification, operational manual and release packaging remain outstanding.
