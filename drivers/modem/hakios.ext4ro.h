// SPDX-License-Identifier: MIT-0
/*
 * A read-only reader of one file on an ext4 (or ext2/ext3) file system, for drivers that need a file of a partition
 * the kernel never mounts. First user: the Galaxy S9's modem driver (drivers/modem/hakios.shannon.c, cp_nv=2), which
 * takes the CP's non-volatile data, /efs/nv_data.bin, from the EFS partition at boot and keeps it in RAM.
 *
 * It only ever reads, through the caller's read function, and it has no write path at all: no journal replay, no
 * superblock update, nothing that would mark the file system as mounted. A file system whose journal would need a
 * replay is read as it is on the disk (what Linux calls "ro,noload").
 *
 * What it understands, from the on-disk format as documented (no code taken from any implementation): the superblock
 * at byte 1024, 32- and 64-byte group descriptors, inodes of any size, extent trees of any depth (uninitialised extents
 * read as zeros), the classic direct/indirect block map, and directories read linearly (an htree directory's index
 * blocks look like empty entries to a linear reader). Not: inline data, encryption. A path is looked up from the root,
 * one name per component.
 *
 * Header-only (static functions), so a driver includes it and a host-side test builds the same code.
 */
#ifndef HAKIOS_EXT4RO_H
#define HAKIOS_EXT4RO_H

/* the caller's reader: len bytes at byte offset off of the partition into dst; 0 or an errno */
typedef int (*ext4ro_read_fn)(void *ctx, uint64_t off, void *dst, uint32_t len);

struct ext4ro {
	ext4ro_read_fn read;
	void *ctx;
	uint32_t block_size, inode_size, inodes_per_group, desc_size, first_data_block;
	uint64_t blocks;
	bool is64;
	uint8_t *blk;           /* scratch, the caller's: block_size bytes (at most 64 KB) */
	uint8_t *blk2;          /* a second one (index and indirect blocks) */
};

#define EXT4RO_MAGIC            0xEF53
#define EXT4RO_INCOMPAT_64BIT   0x0080
#define EXT4RO_INCOMPAT_ENCRYPT 0x10000
#define EXT4RO_INODE_EXTENTS    0x00080000
#define EXT4RO_INODE_INLINE     0x10000000
#define EXT4RO_ROOT_INO         2
#define EXT4RO_MAX_DEPTH        5
#define EXT4RO_MAX_DIR_BYTES    (16u << 20)     /* a directory bigger than this is not looked through (EFBIG) */
#define EXT4RO_MAX_BLOCKS       (1ull << 47)    /* so that block * block_size (at most 64 KB) stays inside 64 bits */

static inline uint16_t
ext4ro_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t
ext4ro_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* the superblock: 0, or an errno (EINVAL: not ext2/3/4, ENOTSUP: a feature this reader does not have) */
static int
ext4ro_open(struct ext4ro *fs)
{
	uint8_t sb[1024];
	uint32_t log, incompat;
	int e;

	if ((e = fs->read(fs->ctx, 1024, sb, sizeof(sb))) != 0) {
		return e;
	}
	if (ext4ro_le16(sb + 0x38) != EXT4RO_MAGIC) {
		return EINVAL;
	}
	log = ext4ro_le32(sb + 0x18);
	if (log > 6) {
		return EINVAL;
	}
	fs->block_size = 1024u << log;
	fs->inodes_per_group = ext4ro_le32(sb + 0x28);
	fs->first_data_block = ext4ro_le32(sb + 0x14);
	incompat = ext4ro_le32(sb + 0x60);
	fs->inode_size = ext4ro_le32(sb + 0x4C) >= 1 ? ext4ro_le16(sb + 0x58) : 128;   /* revision 0: 128 */
	fs->is64 = (incompat & EXT4RO_INCOMPAT_64BIT) != 0;
	fs->desc_size = fs->is64 ? ext4ro_le16(sb + 0xFE) : 32;
	fs->blocks = ext4ro_le32(sb + 0x04) | (fs->is64 ? (uint64_t)ext4ro_le32(sb + 0x150) << 32 : 0);
	if (fs->inodes_per_group == 0 || fs->inode_size < 128 || fs->inode_size > fs->block_size ||
	    fs->desc_size < 32 || fs->desc_size > 1024 || (fs->desc_size & (fs->desc_size - 1)) != 0 ||
	    fs->blocks > EXT4RO_MAX_BLOCKS) {
		return EINVAL;
	}
	if (incompat & EXT4RO_INCOMPAT_ENCRYPT) {
		return ENOTSUP;
	}
	return 0;
}

static int
ext4ro_read_block(struct ext4ro *fs, uint64_t block, uint8_t *dst)
{
	if (block >= fs->blocks) {
		return EIO;
	}
	return fs->read(fs->ctx, block * fs->block_size, dst, fs->block_size);
}

/* an inode's first 256 bytes (fewer on disk: the rest zero) */
static int
ext4ro_inode(struct ext4ro *fs, uint32_t ino, uint8_t inode[256])
{
	uint32_t group, index, n = fs->inode_size < 256 ? fs->inode_size : 256;
	uint64_t desc_off, table;
	uint8_t desc[64];
	int e;

	if (ino == 0) {
		return EINVAL;
	}
	group = (ino - 1) / fs->inodes_per_group;
	index = (ino - 1) % fs->inodes_per_group;
	desc_off = (uint64_t)(fs->first_data_block + 1) * fs->block_size + (uint64_t)group * fs->desc_size;
	if ((e = fs->read(fs->ctx, desc_off, desc, fs->desc_size < 64 ? fs->desc_size : 64)) != 0) {
		return e;
	}
	table = ext4ro_le32(desc + 0x08) | (fs->desc_size >= 64 ? (uint64_t)ext4ro_le32(desc + 0x28) << 32 : 0);
	if (table == 0 || table >= fs->blocks) {
		return EIO;
	}
	bzero(inode, 256);
	return fs->read(fs->ctx, table * fs->block_size + (uint64_t)index * fs->inode_size, inode, n);
}

static uint64_t
ext4ro_size(const uint8_t *inode)
{
	return ext4ro_le32(inode + 0x04) | (uint64_t)ext4ro_le32(inode + 0x6C) << 32;
}

/*
 * The physical block of logical block lblk of an inode; 0 for a hole or an uninitialised extent (read as zeros).
 * Extent trees: the 60 bytes of i_block are the root node; index entries are followed down to a leaf.
 */
static int
ext4ro_map(struct ext4ro *fs, const uint8_t *inode, uint32_t lblk, uint64_t *pblk)
{
	uint32_t flags = ext4ro_le32(inode + 0x20);
	const uint8_t *node = inode + 0x28;
	int e;

	*pblk = 0;
	if (flags & EXT4RO_INODE_INLINE) {
		return ENOTSUP;
	}
	if (flags & EXT4RO_INODE_EXTENTS) {
		for (int level = 0; level <= EXT4RO_MAX_DEPTH; level++) {
			uint16_t entries = ext4ro_le16(node + 2), depth = ext4ro_le16(node + 6);
			uint32_t room = level == 0 ? 60u : fs->block_size;
			const uint8_t *best = NULL;
			uint64_t child;
			uint8_t *buf;

			if (ext4ro_le16(node) != 0xF30A || 12u + 12u * entries > room) {
				return EIO;
			}
			for (uint16_t i = 0; i < entries; i++) {
				const uint8_t *x = node + 12 + 12 * i;

				if (ext4ro_le32(x) > lblk) {
					break;
				}
				best = x;
			}
			if (best == NULL) {
				return 0;               /* before the first extent: a hole */
			}
			if (depth == 0) {
				uint32_t len = ext4ro_le16(best + 4), first = ext4ro_le32(best);
				uint64_t start = ext4ro_le32(best + 8) | (uint64_t)ext4ro_le16(best + 6) << 32;
				bool uninit = len > 32768;

				if (uninit) {
					len -= 32768;
				}
				if (lblk - first < len && !uninit) {
					*pblk = start + (lblk - first);
				}
				return 0;
			}
			child = ext4ro_le32(best + 4) | (uint64_t)ext4ro_le16(best + 8) << 32;
			buf = (level & 1) ? fs->blk2 : fs->blk;
			if ((e = ext4ro_read_block(fs, child, buf)) != 0) {
				return e;
			}
			node = buf;
		}
		return EIO;
	}
	/* the block map: 12 direct blocks, then single, double and triple indirect */
	uint32_t per = fs->block_size / 4, idx[3] = { 0, 0, 0 };
	uint64_t b, l = lblk;
	int levels;

	if (l < 12) {
		*pblk = ext4ro_le32(inode + 0x28 + 4 * l);
		return 0;
	}
	l -= 12;
	if (l < per) {
		levels = 1;
		idx[0] = (uint32_t)l;
		b = ext4ro_le32(inode + 0x28 + 4 * 12);
	} else if ((l -= per) < (uint64_t)per * per) {
		levels = 2;
		idx[0] = (uint32_t)(l / per);
		idx[1] = (uint32_t)(l % per);
		b = ext4ro_le32(inode + 0x28 + 4 * 13);
	} else {
		l -= (uint64_t)per * per;
		if (l >= (uint64_t)per * per * per) {
			return EFBIG;           /* past what a block map can name: idx[0] would leave the block */
		}
		levels = 3;
		idx[0] = (uint32_t)(l / per / per);
		idx[1] = (uint32_t)((l / per) % per);
		idx[2] = (uint32_t)(l % per);
		b = ext4ro_le32(inode + 0x28 + 4 * 14);
	}
	for (int i = 0; i < levels && b != 0; i++) {
		if ((e = ext4ro_read_block(fs, b, fs->blk2)) != 0) {
			return e;
		}
		b = ext4ro_le32(fs->blk2 + 4 * idx[i]);
	}
	*pblk = b;
	return 0;
}

/* the inode number of name in the directory dir_ino */
static int
ext4ro_lookup(struct ext4ro *fs, uint32_t dir_ino, const char *name, size_t name_len, uint32_t *ino)
{
	uint8_t inode[256];
	uint64_t size;
	int e;

	*ino = 0;
	if ((e = ext4ro_inode(fs, dir_ino, inode)) != 0) {
		return e;
	}
	if ((ext4ro_le16(inode) & 0xF000) != 0x4000) {
		return ENOTDIR;
	}
	size = ext4ro_size(inode);
	if (size > EXT4RO_MAX_DIR_BYTES) {
		return EFBIG;                   /* (a size made up on the disk would otherwise be walked block by block) */
	}
	for (uint32_t lblk = 0; (uint64_t)lblk * fs->block_size < size; lblk++) {
		uint64_t pblk;

		if ((e = ext4ro_map(fs, inode, lblk, &pblk)) != 0) {
			return e;
		}
		if (pblk == 0) {
			continue;
		}
		if ((e = ext4ro_read_block(fs, pblk, fs->blk)) != 0) {
			return e;
		}
		for (uint32_t off = 0; off + 8 <= fs->block_size;) {
			const uint8_t *d = fs->blk + off;
			uint32_t entry = ext4ro_le32(d);
			uint16_t rec = ext4ro_le16(d + 4);
			uint8_t nl = d[6];

			if (rec < 8 || off + rec > fs->block_size) {
				break;
			}
			if (entry != 0 && nl == name_len && 8u + nl <= rec && memcmp(d + 8, name, name_len) == 0) {
				*ino = entry;
				return 0;
			}
			off += rec;
		}
	}
	return ENOENT;
}

/*
 * The regular file at path (e.g. "/nv_data.bin") into dst, at most cap bytes; *size gets its size. ENOSPC when it is
 * bigger than cap (nothing read then). fs->blk and fs->blk2 must point at the caller's scratch blocks.
 */
static int
ext4ro_read_file(struct ext4ro *fs, const char *path, uint8_t *dst, uint64_t cap, uint64_t *size)
{
	uint32_t ino = EXT4RO_ROOT_INO;
	uint8_t inode[256];
	const char *p = path;
	int e;

	*size = 0;
	while (*p) {
		const char *q;

		while (*p == '/') {
			p++;
		}
		if (!*p) {
			break;
		}
		for (q = p; *q && *q != '/'; q++) {
		}
		if ((e = ext4ro_lookup(fs, ino, p, (size_t)(q - p), &ino)) != 0) {
			return e;
		}
		p = q;
	}
	if ((e = ext4ro_inode(fs, ino, inode)) != 0) {
		return e;
	}
	if ((ext4ro_le16(inode) & 0xF000) != 0x8000) {
		return EISDIR;
	}
	*size = ext4ro_size(inode);
	if (*size > cap) {
		return ENOSPC;
	}
	for (uint64_t done = 0; done < *size;) {
		uint64_t pblk;
		uint32_t n = (uint32_t)((*size - done) < fs->block_size ? (*size - done) : fs->block_size);

		if ((e = ext4ro_map(fs, inode, (uint32_t)(done / fs->block_size), &pblk)) != 0) {
			return e;
		}
		if (pblk == 0) {
			bzero(dst + done, n);
		} else if ((e = ext4ro_read_block(fs, pblk, fs->blk)) != 0) {
			return e;
		} else {
			memcpy(dst + done, fs->blk, n);
		}
		done += n;
	}
	return 0;
}

#endif /* HAKIOS_EXT4RO_H */
