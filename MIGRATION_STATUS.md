# QNX4 Common V2 migration status

## Source and ownership

[Exact central source baseline](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/qnx4). Editable Linux v6.12.107-derived source remains centrally. Extract and adapt the existing implementation; preserve original licensing, attribution and provenance. New project-authored code uses GPL-3.0-or-later. The independent project-authored native implementation now resides in this repository.

## Qualification plan

1. Enumerate supported on-disk variants, validate geometry and features; implement bounded CORE probe.
2. Provide CORE mount/stat/list/read and VOLUME query using native superblock and geometry, inode and directory records, allocation bitmaps and extent chains.
3. Test independent QNX4 media, invalid inode pointers, malformed extents and allocation contradictions; compare independent payloads and unchanged-image hashes; inject malformed media and I/O faults.
4. Implement mutation, format, check/repair and DEFRAG map/plan/execute/verify/recover only with proven allocation, durability and interruption/recovery semantics. Unsupported operations must refuse before writing.
5. Qualify exact packaged Common V2 ABI, hosted and import-free freestanding profiles, then actual BOOT and kernel integration independently.

Hosted native development has passed the exact-revision [v0.1.8 CI run](https://github.com/Infiltrator-Projects/Filesystem-QNX4/actions/runs/45927194924), including native format/read, integrity checks, journalled create/rename/remove/write/truncate and 270 interruption cases per build across both sector sizes. Subsequent [0.1.10 CI](https://github.com/Infiltrator-Projects/Filesystem-QNX4/actions/runs/45928052952) passed native relocation, persistent growth reservation and consumption, including 462 interruption cases per build. [0.1.11 CI](https://github.com/Infiltrator-Projects/Filesystem-QNX4/actions/runs/45928139049) passed dynamic directory and inode-file expansion. Independent foreign-media interoperability, freestanding and full production-consumer qualification, operational PDF manual updates and packaging remain pending. No production release is qualified.

The [design and qualification reference](docs/QNX4.pdf) is available. It is not an operational manual; release and installation qualification remain pending.
