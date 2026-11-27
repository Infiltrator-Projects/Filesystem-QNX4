# QNX4: technical reference

## Format history and design

QNX 4 uses an extent-based disk format designed for a real-time operating system. Unlike QNX6 Power-Safe, it has no alternate copy-on-write root transaction mechanism. Ordinary file metadata can live directly inside a directory entry, avoiding an additional inode lookup for short single-link names. Extended inode records support multiple names and longer names. Native metadata structures carry recognizable signatures to assist forensic reconstruction. The filesystem may occupy a whole floppy or a selected hard-disk partition; a partition type alone is not proof of valid media.

## Volume layout and addressing

The logical format uses 512-byte blocks regardless of host transfer geometry. On disk block addresses start at one; block b corresponds to partition-relative byte offset (b-1)*512. Zero is therefore not the first valid extent address. Physical block zero is the loader, physical block one the root block; the bitmap follows, then the root directory and remaining allocations. If the volume contains N complete 512-byte blocks, the bitmap needs ceil(N/4096) blocks. Check the root block's directory/inode records and special-file references independently. Host sector alignment must be handled by bounded byte I/O; never reinterpret a 4 KiB host block as one QNX4 block.

The Linux VFS QNX4_SUPER_MAGIC value is not proof of a unique on-disk signature. Admit the disk only after validating actual root, bitmap and extent structures. QNX6's alternate-root publication must not be assumed.

## Root and reserved metadata

The root block contains records describing the root directory, inode file, primary boot image and alternate boot image. The root directory contains dot and dot-dot links, .bitmap, .inodes, .boot and .altboot. The bitmap is itself represented as a read-only file. The .inodes file begins with a reserved identifying record. Validate the root's self-reference, parent, special-file extents and bitmap size before any general directory traversal. The QNX4_SUPER_MAGIC used by operating-system drivers is not necessarily a unique signature stored at a particular disk offset.

## Inode and short directory record layout

Each record is 64 bytes; eight fit in a 512-byte block. Little-endian fields, offsets in bytes from record start: name 0..15; size 16..19; first extent start 20..23 and count 24..27; extent-block pointer 28..31; creation/ftime 32..35; modification 36..39; access 40..43; change 44..47; extent count 48..49; mode 50..51; UID 52..53; GID 54..55; hard-link count 56..57; reserved 58..61; type 62; status 63. Decode fields explicitly rather than relying on compiler packing. Check that all 64 bytes are present before interpreting the status and that file sizes fit within the mapped runs.

## Long name and hard-link records

A link entry uses 48 name bytes at offset 0, little-endian inode-block number at 48, inode slot at 52, ten spare bytes from 53 and status at 63. Slot is zero-based within the referenced block and must be below eight. Follow the one-based block address to a validated inode entry in the inode file; avoid treating an arbitrary block as an inode table. Status bits include used 0x01, modified 0x02, busy 0x04, link 0x08, inode 0x10 and clean 0x20. In directory interpretation the used/link combination distinguishes an unused entry, a direct entry and an indirect link; both used and link set is invalid under the original guide. Multiple names of one inode do not represent duplicate physical ownership.

## Extent block layout

An extent record consists of a 32-bit start block followed by a 32-bit length in blocks. The first extent lives in the inode. An extension block is exactly 512 bytes: next address at 0, previous at 4, one-byte count at 8, three spare bytes at 9, 32-bit total covered blocks at 12, up to 60 eight-byte extents beginning at 16, eight-byte identifier at 496, redundant first extent at 504. Verify the first seven bytes of the identifier against IamXblk; interpret remaining byte according to original format. Validate forward/backward links, extent count, block-count sum and redundant first extent, and bound chain traversal by declared extents and volume length. Reject zero-length allocated extents, range overflow, invalid addresses and cycles.

## File mapping and worked example

Take a file of 1500 bytes whose first extent is two blocks starting at disk address 100 and second extent is one block at address 900. The first 1024 file bytes occupy partition offsets 50688 through 51711; the remaining 476 bytes occupy offsets 460288 through 460763. The unused tail of the last block is not file content. To read any logical offset, divide by 512 to select a logical block and remainder; subtract extent lengths until the containing run is found, then translate its disk address. Return EOF at logical file size even when allocation extends beyond it. A missing range is an error, not a silently fabricated sparse hole.

## Allocation bitmap and consistency

One bitmap bit represents one 512-byte logical block. Bit zero of byte zero corresponds to disk block 1; a set bit denotes allocation. For disk address b, bitmap byte index is floor((b-1)/8) and bit index is (b-1) mod 8. Padding bits representing no actual block are set. A checker traverses root, directories, inode references and extension blocks, recording ownership of metadata and data; count each hard-linked inode once. Compare reconstructed allocation with the persistent bitmap; distinguish leaked allocated blocks from referenced but marked-free blocks. Never reclaim ambiguous space, or treat file data as a valid inode merely because a signature-like pattern occurs.

## Namespace, metadata and traversal

Directory data is a sequence of 64-byte records. Map the entire directory using its extents and respect its byte size. Direct names have a 16-byte maximum; indirect names can reach 48 bytes. Do not require a terminator when a field occupies its entire width. Decode type, mode, link count and native timestamps from validated inode records; keep native metadata distinct from the host representation. Dot and dot-dot identify self and parent and must not cause infinite recursion. Use visited directory identities when constructing a volume-wide map. Rename and link operations must preserve the identity of the inode and all references to it, not duplicate its extents.

## Writes, formatting and recovery

Growth may extend the final extent in adjacent free space, or allocate a new run and additional extension blocks. Allocation, inode and directory changes span multiple disk blocks; do not claim atomicity simply because each write is sector aligned. A robust independent implementation must define recoverable ordering, flushes, allocation rollback and link-count transitions, with tests for every interrupted write. Preserve the native busy/modified state and respect existing reserved files. Native dinit creates the loader, root block, bitmap, root directory and inode file. A new formatter must reproduce a validated profile and reserved-state rules before it is interoperable. QNX4 is not QNX6; its native checker reconstructs ownership and compares allocation, not a modern COW transaction log.

## Repair and forensic semantics

Native chkfsys traverses the directory graph and compares a reconstructed bitmap against on-disk allocation. Recovery is not synonymous with a clean mount. Preserve an original immutable image, enumerate errors with physical offsets, and classify uncertain records rather than silently patching them. Orphaned extension blocks can provide evidence because they contain a redundant initial extent; their content is not proof of current ownership. Deleted files may be recoverable only when directory metadata and blocks remain unmodified. Recovered names and payloads should carry provenance and confidence; do not write recovered allocations onto the source by default. Defrag must validate destination ownership, payload equivalence and all metadata references before releasing the source run.

## Implementation and qualification boundary

The independent provider uses Common V2: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate actual operation masks and media profiles. Distinguish unsupported, corrupt, I/O and stale-handle results. Require bounded host I/O, exclusive unmounted ownership and durable flush before mutation. Test Media and Defragmenter consume the same engine, not separate format parsers. Hosted and freestanding binaries, relocation, recovery, BOOT, kernel integration and packages require separate exact-source evidence. Current support belongs to the repository README and qualification records. The intended package paths are /System/Filesystems/ and /System/Resources/Documentation/; the reference is not an installation or release-status claim.

## Independent implementation tests and references

Use a QNX-native dinit image and compare bytes and metadata with an independent reader. Fixtures: short and full-width names, long link records, hard-link aliases, 0/1/60/61 extension entries, truncated root, invalid slot 8, last-volume block, overlapping extents, corrupt reverse pointer, inconsistent counts, bitmap padding, busy files and injected read/write/flush failures. Source: QNX 4 User Guide, Overview of QNX disk structure, https://www.qnx.com/developers/docs/qnx_4.25_docs/qnx4/user_guide/recovery.html . Binary layout cross-check: https://codebrowser.dev/linux/linux/include/uapi/linux/qnx4_fs.h.html ; address mapping: https://github.com/torvalds/linux/tree/v6.12/fs/qnx4 . Exact original write ordering, all legacy flags and timestamp edge cases require native-reference verification. Historical design status is identified in Implementation and qualification boundary; this reference does not establish runtime qualification.

The retained reader resolves raw inode entries, root directory and extended extent blocks; validate link and extent cycles against real QNX4 media.

Retained source: https://github.com/Infiltrator-Projects/Filesystems/blob/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/qnx4/linux/inode.c (blob e399e2dd3a).
