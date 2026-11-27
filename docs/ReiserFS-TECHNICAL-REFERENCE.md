# ReiserFS: technical reference

## History and format generations

ReiserFS, developed by Hans Reiser and Namesys, introduced a balanced-tree design with small-file packing and became a mainstream Linux filesystem in the 2.4 series. This reference covers ReiserFS 3.5 and 3.6, not Reiser4. It stores file metadata, directory items, packed file data and block-pointer items in one balanced tree; unformatted payload blocks are separate. A file has no fixed inode-table record. Its object key identifies all associated tree items. The 3.5 and 3.6 key/stat formats can coexist in a converted volume; per-item version matters.

## Superblock and geometry

All on-disk multibyte integers are little-endian. Usual superblock starts at byte 65536; historical placement is byte 8192. Relative fields: total blocks 0, free blocks 4, root block 8; eight journal parameter words from 12; 16-bit block size 44, object-ID map max/current sizes 46/48, unmount state 50; ten-byte magic at 52, fsck state 62, hash code 64, tree height 68, bitmap count 70, version 72, reserved journal blocks 74. The magic strings ReIsErFs, ReIsEr2Fs and ReIsEr3Fs identify format/journal variants. The extended superblock has inode generation at 76, flags 80, UUID 84, label 100. Validate geometry and journal location; old placement and bitmap layout require separate admission.

Admit the revision before interpreting keyed tree items. The Direct and indirect payload mapping section distinguishes packed data from externally allocated blocks.

## Allocation bitmaps and object IDs

Bitmaps represent allocation of filesystem blocks, not objects. In the ordinary distributed-bitmap layout the first bitmap follows the superblock and further bitmap blocks occur at block-size-times-eight intervals; historical layouts can store bitmaps together. One bit corresponds to one disk block, set meaning used. For 4096-byte blocks, bitmap coverage is 32768 blocks; block 33000 belongs to bitmap 1 at bit 232, byte 29. Exclude superblock, all bitmap and journal blocks from available space. The object-ID map stores ordered alternating interval boundaries for allocated and unallocated object IDs; validate even count, capacity and strictly increasing bounds. Allocate object identity separately from physical data blocks.

## Keys and object ordering

A disk key is sixteen bytes: directory locality ID at 0 and object ID at 4 (32-bit each), then an eight-byte version-dependent union. In 3.5 this is a 32-bit offset and 32-bit uniqueness field: stat 0, indirect 0xfffffffe, direct 0xffffffff, directory 500. In 3.6 the low 60 bits of the little-endian final word are offset and high four bits type: stat 0, indirect 1, direct 2, directory 3. Compare decoded components according to the native ordering, not raw bytes. File byte offsets in item keys start at one; stat data uses zero. The root directory's short key is locality ID 1, object ID 2. Hard links share the same object identity regardless of directory containing the name.

## Internal tree node structure

A formatted block begins with a 24-byte header: 16-bit level, item/key count, free byte count and reserved word at 0/2/4/6, followed by a legacy 16-byte right-delimiter key. Level 1 identifies a leaf; higher levels are internal. An internal block with n delimiter keys contains n sixteen-byte keys starting at 24, then n+1 eight-byte child records. Each child record stores block number at 0, used-space count at 4 and reserved word at 6. Select the child interval delimited by ordered keys, checking tree level decreases exactly one, child blocks are allocated and bounds are valid. Prevent cycles and verify the key interval against actual contents of the child.

## Leaf items and body organisation

A leaf with n items has n 24-byte item headers after its block header. Each item header begins with a 16-byte key; 16-bit entry count or indirect-tail/free-space field at 16, body length 18, body offset 20, key/version at 22. Version 0 denotes old encoding, 2 the newer one; the fsck temporary high bit is not ordinary clean-state data. Item bodies grow backward from the end of the block. Validate the header array, body intervals, free-space equation, nonoverlap and decoded key ordering. Body interpretation is determined by key type, not by a superficially plausible length. An item can hold part of an object's logical content; traverse following items with the same object key as needed.

## Stat-data versions

Old stat data is 32 bytes: 16-bit mode, link count, UID and GID at 0/2/4/6; size at 8, access/modify/change times at 12/16/20; device or block count at 24 and first direct byte at 28. New stat data is 44 bytes: mode 0, attribute flags 2, links 4, 64-bit size 8, UID/GID 16/20, access/modify/change 24/28/32, blocks 36, device or generation 40. Decode by the item version and mode. Do not use a direct-item key's version to choose its object's stat layout without looking up the actual stat item. Root and special objects must preserve their distinct semantics.

## Direct and indirect payload mapping

A direct item contains packed file bytes inside the tree leaf. An indirect item contains a sequence of little-endian 32-bit physical block numbers; each covers one filesystem block and zero denotes a hole. If an indirect item's one-based key offset is 1 and block size 4096, logical byte offset 9000 selects pointer 2 and in-block offset 808. If pointer 2 is block 1234, read at 1234*4096+808=5055272. For direct item key offset 12289, logical offset 12300 selects byte 12 in that item. Clip to stat size and item coverage. Tails may convert between packed direct items and separately allocated blocks; preserve all bytes and zero semantics across that conversion.

## Directory items and collision resolution

A directory item consists of sixteen-byte entry headers followed by names packed backward in its body. Header offsets: 32-bit hash/offset at 0, referenced object's locality and object ID at 4/8, 16-bit name location at 12 and state at 14. The item header's entry count determines the header-array length. Derive each name span from adjacent locations and item end; account for version-specific padding and reject overlap. State bit 2 marks visible entries; hidden entries are not normal enumeration. Dot and dot-dot use special offsets 1 and 2. Other offsets encode a native filename hash and collision-generation bits; the superblock selects the hash algorithm (TEA, Yura or R5). Scan colliding candidates and compare actual names rather than treating a hash as identity.

## Tree updates and allocation invariants

Insertion may insert a complete item or extend an existing one, split leaves, move items between siblings and adjust internal delimiter keys. Deletion may shrink or merge nodes, change root height and release storage. Tail packing and unpacking change both metadata and block ownership. A writer must reserve metadata headroom before balancing; otherwise a simple file growth could fail halfway through a structural update. Preserve object keys, stat size, directory visibility and hard-link count. Orphan/safe-link items support crash recovery for unlink and truncate. Never reclaim data solely because an object lacks a currently visible directory name.

## Journal and recovery

Journal parameters at superblock offsets 12..43 identify start, device, size, transaction limits, journal magic and batching/age limits. The log is a circular block area. A descriptor begins with transaction ID, payload block count and mount ID at 0/4/8; target-block numbers follow, with descriptor magic ReIsErLB in its format-defined tail. A commit begins with matching transaction ID and length, plus remaining target addresses. The journal header records last fully flushed transaction, first unflushed log offset and mount ID, followed by journal parameters. Validate matching descriptor/commit, bounds, target list and mount sequence; replay complete committed transactions in native order. Preserve log blocks until corresponding home writes are durable. Exact wrap and transaction-ID comparison semantics need independent tests.

## Defragmentation and forensic qualification

Defrag must distinguish packed bytes, unformatted payload blocks and tree metadata. Moving an ordinary block changes the owning indirect pointer; moving a formatted node requires updating parent and balancing invariants. Read all source bytes, reserve destination, journal metadata updates and reclaim old storage only after publication. Rebuild allocation ownership from tree structure and indirect items, then compare bitmap and object-ID map. Test both key versions, converted media, direct-tail transitions, sparse files, hash collisions, hidden entries, mixed-version stat data, malformed child intervals, corrupted bitmap and interrupted journal transactions. Use independent mkreiserfs/reiserfsck and historical kernel images as interoperability oracles.

## Implementation and qualification boundary

The independent provider uses Common V2: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate actual operation masks and media profiles. Distinguish unsupported, corrupt, I/O and stale-handle results. Require bounded host I/O, exclusive unmounted ownership and durable flush before mutation. Test Media and Defragmenter consume the same engine, not separate format parsers. Hosted and freestanding binaries, relocation, recovery, BOOT, kernel integration and packages require separate exact-source evidence. Current support belongs to the repository README and qualification records. The intended package paths are /System/Filesystems/ and /System/Resources/Documentation/; the reference is not an installation or release-status claim.

## References and scope boundary

Pinned original implementation and structure definitions: https://github.com/torvalds/linux/tree/v6.12/fs/reiserfs (reiserfs.h, stree.c, do_balan.c, fix_node.c, item_ops.c, namei.c, hashes.c, bitmap.c, journal.c). Historical user-space tools: https://git.kernel.org/pub/scm/linux/kernel/git/jeffm/reiserfsprogs.git/ . Linux v6.12 is a useful historical reference for the retired kernel implementation. Exact bitmap placement for old images, key conversion rules, journal target encoding, directory padding and safe-link recovery require further source and fixture validation. Reiser4 is out of scope. Original-format engineering notes do not establish independent runtime support.

Test packed tails, tree splits, corrupt item lengths and dirty journals against independent historical tools.

Retained source: https://github.com/Infiltrator-Projects/Filesystems/blob/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/reiserfs/linux/reiserfs.h (blob 12fc20af8e).
