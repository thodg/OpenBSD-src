/*
 * Copyright (c) 2025 kmx.io.
 * Copyright (c) 1997 Manuel Bouyer.
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Modified for ext4fs by kmx.io.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/pool.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/lockf.h>
#include <sys/specdev.h>
#include <sys/unistd.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/dir.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ext4fs/ext4fs.h>
#include <ufs/ext4fs/ext4fs_crc32c.h>

/* Convert ext4 directory entry file type to BSD dirent type */
static const u_int8_t ext4fs_type_to_dt[EXT4FS_FT_MAX] = {
	[EXT4FS_FT_UNKNOWN]	= DT_UNKNOWN,
	[EXT4FS_FT_REG_FILE]	= DT_REG,
	[EXT4FS_FT_DIR]		= DT_DIR,
	[EXT4FS_FT_CHRDEV]	= DT_CHR,
	[EXT4FS_FT_BLKDEV]	= DT_BLK,
	[EXT4FS_FT_FIFO]	= DT_FIFO,
	[EXT4FS_FT_SOCK]	= DT_SOCK,
	[EXT4FS_FT_SYMLINK]	= DT_LNK,
};

/*
 * Look up the physical block number for a given logical block number
 * using the extent tree in the inode.
 * Returns 0 on success with the physical block stored in *pblk.
 */
static int
ext4fs_extent_pblk(struct inode *ip, u_int64_t lbn, u_int64_t *pblk,
    u_int64_t *ncontig)
{
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_extent_header *eh;
	struct m_ext4fs *fs = ip->i_e4fs;
	struct buf *bp = NULL;
	u_int16_t entries, depth;
	int error, found, i;

	/* Start with the extent header in the inode */
	eh = &din->i_extent_header;
	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC)
		return (EIO);

	depth = letoh16(eh->eh_depth);
	entries = letoh16(eh->eh_entries);

	/* Walk down the extent tree */
	while (depth > 0) {
		struct ext4fs_extent_idx *idx;
		u_int64_t child_blk;

		/* Index node: find the child that covers lbn */
		idx = (struct ext4fs_extent_idx *)(eh + 1);
		found = -1;
		for (i = 0; i < (int)entries; i++) {
			if (letoh32(idx[i].ei_block) <= lbn)
				found = i;
			else
				break;
		}
		if (found < 0) {
			if (bp != NULL)
				brelse(bp);
			return (EIO);
		}

		/* Read the child node block */
		child_blk = letoh32(idx[found].ei_leaf_lo);
		child_blk |= (u_int64_t)letoh16(idx[found].ei_leaf_hi) << 32;

		if (bp != NULL)
			brelse(bp);

		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, child_blk),
		    fs->m_block_size, &bp);
		if (error) {
			if (bp != NULL)
				brelse(bp);
			return (error);
		}

		eh = (struct ext4fs_extent_header *)bp->b_data;
		if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC) {
			brelse(bp);
			return (EIO);
		}
		depth = letoh16(eh->eh_depth);
		entries = letoh16(eh->eh_entries);
	}

	/* Leaf node: search for the extent containing lbn */
	{
		struct ext4fs_extent *ext;
		ext = (struct ext4fs_extent *)(eh + 1);
		for (i = 0; i < (int)entries; i++) {
			u_int32_t e_block = letoh32(ext[i].e_block);
			u_int16_t e_len = letoh16(ext[i].e_len);

			/* High bit of e_len marks uninitialized extents */
			if (e_len > 32768)
				e_len -= 32768;

			if (lbn >= e_block && lbn < e_block + e_len) {
				u_int64_t start = letoh32(ext[i].e_start_lo);
				start |=
				    (u_int64_t)letoh16(ext[i].e_start_hi) << 32;
				*pblk = start + (lbn - e_block);
				if (ncontig != NULL)
					*ncontig = e_len - (lbn - e_block);
				if (bp != NULL)
					brelse(bp);
				return (0);
			}
		}
	}

	if (bp != NULL)
		brelse(bp);
	return (EIO);
}

/*
 * Write inode back to disk with checksum update.
 */
int
ext4fs_update(struct inode *ip, int waitfor)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct buf *bp;
	u_int32_t inode_group, inode_index, block_in_table, offset_in_block;
	struct ext4fs_block_group_descriptor *gd;
	u_int64_t inode_table_block;
	daddr_t disk_block;
	u_int32_t csum;
	int error;

	printf("ext4fs_update: ino=%u flags=0x%x\n", ip->i_number, ip->i_flag);

	if (ITOV(ip)->v_mount->mnt_flag & MNT_RDONLY)
		return (0);

	EXT4FS_ITIMES(ip);

	if ((ip->i_flag & IN_MODIFIED) == 0) {
		printf("ext4fs_update: ino=%u not modified, skip\n", ip->i_number);
		return (0);
	}

	ip->i_flag &= ~IN_MODIFIED;

	/* Locate inode on disk */
	inode_group = (ip->i_number - 1) / fs->m_inodes_per_group;
	inode_index = (ip->i_number - 1) % fs->m_inodes_per_group;
	block_in_table = inode_index / fs->m_inodes_per_block;
	offset_in_block = (inode_index % fs->m_inodes_per_block) *
	    fs->m_inode_size;

	gd = &fs->m_gd[inode_group];
	inode_table_block = letoh32(gd->bgd_inode_table_block_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		inode_table_block |=
		    (u_int64_t)letoh32(gd->bgd_inode_table_block_hi) << 32;

	disk_block = (inode_table_block + block_in_table) <<
	    fs->m_fs_block_to_disk_block;

	printf("ext4fs_update: ino=%u bread dblk=%lld\n",
	    ip->i_number, (long long)disk_block);
	error = bread(ip->i_devvp, disk_block, fs->m_block_size, &bp);
	if (error) {
		printf("ext4fs_update: ino=%u bread error=%d\n",
		    ip->i_number, error);
		brelse(bp);
		return (error);
	}

	/* Recompute inode checksum */
	csum = ext4fs_inode_csum(fs, ip->i_e4din, ip->i_number);
	ip->i_e4din->dinode.i_checksum_lo = htole16(csum & 0xFFFF);
	ip->i_e4din->dinode.i_checksum_hi = htole16((csum >> 16) & 0xFFFF);

	/* Copy inode to buffer */
	memcpy((char *)bp->b_data + offset_in_block, ip->i_e4din,
	    sizeof(struct ext4fs_dinode_256));

	printf("ext4fs_update: ino=%u bwrite waitfor=%d\n",
	    ip->i_number, waitfor);
	if (waitfor) {
		error = bwrite(bp);
		printf("ext4fs_update: ino=%u bwrite done error=%d\n",
		    ip->i_number, error);
		return (error);
	}

	bdwrite(bp);
	printf("ext4fs_update: ino=%u bdwrite done\n", ip->i_number);
	return (0);
}

/*
 * Set inode size (both low and high 32-bit fields).
 */
void
ext4fs_setsize(struct inode *ip, u_int64_t size)
{
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;

	din->i_size_lo = htole32((u_int32_t)size);
	din->i_size_hi = htole32((u_int32_t)(size >> 32));
}

/*
 * Allocate a filesystem block.
 * Tries the group of the goal block first, then scans all groups.
 */
int
ext4fs_blkalloc(struct inode *ip, u_int64_t goal, u_int64_t *bnp)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_block_group_descriptor *gd;
	struct buf *bp;
	u_int64_t bitmap_blk;
	u_int32_t group, ngroups, blk_in_group, free_blocks;
	char *bbp;
	int error, i;

	*bnp = 0;

	if (fs->m_free_blocks_count == 0)
		return (ENOSPC);

	ngroups = fs->m_block_group_count;

	/* Pick starting group from goal */
	if (goal >= fs->m_first_data_block && goal < fs->m_blocks_count)
		group = (goal - fs->m_first_data_block) /
		    fs->m_blocks_per_group;
	else
		group = (ip->i_number - 1) / fs->m_inodes_per_group;

	for (i = 0; i < ngroups; i++) {
		u_int32_t g = (group + i) % ngroups;
		gd = &fs->m_gd[g];

		free_blocks = letoh16(gd->bgd_free_blocks_count_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			free_blocks |= (u_int32_t)
			    letoh16(gd->bgd_free_blocks_count_hi) << 16;
		if (free_blocks == 0)
			continue;

		/* Read block bitmap */
		bitmap_blk = letoh32(gd->bgd_block_bitmap_block_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			bitmap_blk |= (u_int64_t)
			    letoh32(gd->bgd_block_bitmap_block_hi) << 32;

		printf("ext4fs_blkalloc: group=%u bitmap_blk=%llu bread\n",
		    g, (unsigned long long)bitmap_blk);
		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, bitmap_blk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			continue;
		}
		printf("ext4fs_blkalloc: bread done\n");

		bbp = (char *)bp->b_data;

		/* Scan bitmap for free block */
		for (blk_in_group = 0;
		    blk_in_group < fs->m_blocks_per_group;
		    blk_in_group++) {
			if (isclr(bbp, blk_in_group)) {
				setbit(bbp, blk_in_group);

				/* Update block bitmap checksum in BGD */
				{
					u_int32_t bcsum =
					    ext4fs_bitmap_csum(fs, g, bbp,
					    fs->m_block_size);
					gd->bgd_block_bitmap_checksum_lo =
					    htole16(bcsum & 0xFFFF);
					if (fs->m_feature_incompat &
					    EXT4FS_FEATURE_INCOMPAT_64BIT)
						gd->bgd_block_bitmap_checksum_hi
						    = htole16(
						    (bcsum >> 16) & 0xFFFF);
				}

				printf("ext4fs_blkalloc: bwrite bitmap blk_in_group=%u\n",
				    blk_in_group);
				error = bwrite(bp);
				if (error)
					return (error);
				printf("ext4fs_blkalloc: bwrite done\n");

				/* Update BGD */
				free_blocks--;
				gd->bgd_free_blocks_count_lo =
				    htole16(free_blocks & 0xFFFF);
				if (fs->m_feature_incompat &
				    EXT4FS_FEATURE_INCOMPAT_64BIT)
					gd->bgd_free_blocks_count_hi =
					    htole16((free_blocks >> 16) &
					    0xFFFF);

				printf("ext4fs_blkalloc: bgd_write\n");
				ext4fs_bgd_write(fs, ip->i_devvp, g);
				printf("ext4fs_blkalloc: bgd_write done\n");

				/* Update superblock counters */
				fs->m_free_blocks_count--;
				fs->m_sble.sb_free_blocks_count_lo =
				    htole32((u_int32_t)
				    fs->m_free_blocks_count);
				fs->m_sble.sb_free_blocks_count_hi =
				    htole32((u_int32_t)
				    (fs->m_free_blocks_count >> 32));
				fs->m_fs_was_modified = 1;

				*bnp = (u_int64_t)g * fs->m_blocks_per_group +
				    blk_in_group + fs->m_first_data_block;
				return (0);
			}
		}

		brelse(bp);
	}

	return (ENOSPC);
}

/*
 * Free a filesystem block.
 */
void
ext4fs_blkfree(struct inode *ip, u_int64_t bno)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_block_group_descriptor *gd;
	struct buf *bp;
	u_int64_t bitmap_blk;
	u_int32_t group, blk_in_group, free_blocks;
	char *bbp;
	int error;

	group = (bno - fs->m_first_data_block) / fs->m_blocks_per_group;
	blk_in_group = (bno - fs->m_first_data_block) %
	    fs->m_blocks_per_group;
	gd = &fs->m_gd[group];

	/* Read block bitmap */
	bitmap_blk = letoh32(gd->bgd_block_bitmap_block_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		bitmap_blk |=
		    (u_int64_t)letoh32(gd->bgd_block_bitmap_block_hi) << 32;

	error = bread(ip->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, bitmap_blk),
	    fs->m_block_size, &bp);
	if (error) {
		brelse(bp);
		return;
	}

	bbp = (char *)bp->b_data;
	clrbit(bbp, blk_in_group);

	/* Update block bitmap checksum in BGD */
	{
		u_int32_t bcsum = ext4fs_bitmap_csum(fs, group, bbp,
		    fs->m_block_size);
		gd->bgd_block_bitmap_checksum_lo = htole16(bcsum & 0xFFFF);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			gd->bgd_block_bitmap_checksum_hi =
			    htole16((bcsum >> 16) & 0xFFFF);
	}

	error = bwrite(bp);
	if (error)
		return;

	/* Update BGD */
	free_blocks = letoh16(gd->bgd_free_blocks_count_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		free_blocks |=
		    (u_int32_t)letoh16(gd->bgd_free_blocks_count_hi) << 16;
	free_blocks++;
	gd->bgd_free_blocks_count_lo = htole16(free_blocks & 0xFFFF);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		gd->bgd_free_blocks_count_hi =
		    htole16((free_blocks >> 16) & 0xFFFF);

	ext4fs_bgd_write(fs, ip->i_devvp, group);

	/* Update superblock counters */
	fs->m_free_blocks_count++;
	fs->m_sble.sb_free_blocks_count_lo =
	    htole32((u_int32_t)fs->m_free_blocks_count);
	fs->m_sble.sb_free_blocks_count_hi =
	    htole32((u_int32_t)(fs->m_free_blocks_count >> 32));
	fs->m_fs_was_modified = 1;
}

/*
 * Insert an extent into the inode's extent tree (depth 0 only).
 * Tries to merge with the last extent if contiguous.
 */
static int
ext4fs_extent_insert(struct inode *ip, u_int32_t lbn, u_int64_t pblk,
    u_int16_t len)
{
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_extent_header *eh = &din->i_extent_header;
	struct ext4fs_extent *ext = din->i_extent;
	u_int16_t entries, maxe;
	int i;

	printf("ext4fs_extent_insert: lbn=%u pblk=%llu len=%u\n",
	    lbn, (unsigned long long)pblk, len);

	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC) {
		printf("ext4fs_extent_insert: bad magic %x\n",
		    letoh16(eh->eh_magic));
		return (EIO);
	}
	if (letoh16(eh->eh_depth) != 0) {
		printf("ext4fs_extent_insert: depth=%u unsupported\n",
		    letoh16(eh->eh_depth));
		return (EOPNOTSUPP);
	}

	entries = letoh16(eh->eh_entries);
	maxe = letoh16(eh->eh_max);
	printf("ext4fs_extent_insert: entries=%u maxe=%u\n", entries, maxe);

	/* Try to merge with last extent */
	if (entries > 0) {
		struct ext4fs_extent *last = &ext[entries - 1];
		u_int32_t last_block = letoh32(last->e_block);
		u_int16_t last_len = letoh16(last->e_len);
		u_int64_t last_start = letoh32(last->e_start_lo) |
		    ((u_int64_t)letoh16(last->e_start_hi) << 32);

		if (last_block + last_len == lbn &&
		    last_start + last_len == pblk &&
		    last_len + len <= 32768) {
			last->e_len = htole16(last_len + len);
			ip->i_flag |= IN_CHANGE | IN_MODIFIED;
			return (0);
		}
	}

	/* Cannot merge - need a new entry */
	if (entries >= maxe) {
		printf("ext4fs_extent_insert: entries >= maxe\n");
		return (ENOSPC);
	}

	/* Find insertion point (keep sorted by lbn) */
	for (i = 0; i < entries; i++) {
		if (letoh32(ext[i].e_block) > lbn)
			break;
	}

	/* Shift entries to make room */
	if (i < entries)
		memmove(&ext[i + 1], &ext[i],
		    (entries - i) * sizeof(struct ext4fs_extent));

	/* Insert new extent */
	ext[i].e_block = htole32(lbn);
	ext[i].e_len = htole16(len);
	ext[i].e_start_lo = htole32((u_int32_t)pblk);
	ext[i].e_start_hi = htole16((u_int16_t)(pblk >> 32));

	eh->eh_entries = htole16(entries + 1);
	ip->i_flag |= IN_CHANGE | IN_MODIFIED;

	return (0);
}

/*
 * Allocate a buffer for a logical block.
 * If the block is already mapped, just read it.
 * Otherwise, allocate a new physical block and insert extent.
 */
static int
ext4fs_buf_alloc(struct inode *ip, u_int64_t lbn, int size,
    struct ucred *cred, struct buf **bpp, int flags)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int64_t pblk, goal, ncontig;
	u_int32_t i_blocks;
	int error;

	/* Check if already mapped */
	error = ext4fs_extent_pblk(ip, lbn, &pblk, &ncontig);
	if (error == 0) {
		/* Already mapped, just read */
		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, bpp);
		if (error)
			brelse(*bpp);
		return (error);
	}

	/* Not mapped - allocate a new block */
	/* Goal: try to be contiguous with last extent */
	goal = 0;
	if (letoh16(din->i_extent_header.eh_entries) > 0) {
		u_int16_t entries = letoh16(din->i_extent_header.eh_entries);
		struct ext4fs_extent *last = &din->i_extent[entries - 1];
		u_int64_t last_start = letoh32(last->e_start_lo) |
		    ((u_int64_t)letoh16(last->e_start_hi) << 32);
		goal = last_start + letoh16(last->e_len);
	}

	printf("ext4fs_buf_alloc: lbn=%llu blkalloc goal=%llu\n",
	    (unsigned long long)lbn, (unsigned long long)goal);
	error = ext4fs_blkalloc(ip, goal, &pblk);
	if (error)
		return (error);
	printf("ext4fs_buf_alloc: blkalloc done pblk=%llu\n",
	    (unsigned long long)pblk);

	/* Insert extent */
	error = ext4fs_extent_insert(ip, lbn, pblk, 1);
	if (error) {
		ext4fs_blkfree(ip, pblk);
		return (error);
	}
	printf("ext4fs_buf_alloc: extent_insert done\n");

	/* Update inode block count (i_blocks is in 512-byte sectors) */
	i_blocks = letoh32(din->i_blocks_lo);
	i_blocks += fs->m_block_size / DEV_BSIZE;
	din->i_blocks_lo = htole32(i_blocks);

	/* Set extents flag */
	din->i_flags |= htole32(EXTFS_INODE_FLAG_EXTENTS);

	ip->i_flag |= IN_CHANGE | IN_UPDATE;

	/* Get buffer for the new block */
	printf("ext4fs_buf_alloc: getblk pblk=%llu dblk=%llu\n",
	    (unsigned long long)pblk,
	    (unsigned long long)EXT4FS_FSBTODB(fs, pblk));
	*bpp = getblk(ip->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
	    fs->m_block_size, 0, INFSLP);
	printf("ext4fs_buf_alloc: getblk done\n");

	if (flags & B_CLRBUF)
		clrbuf(*bpp);

	printf("ext4fs_buf_alloc: clrbuf done\n");
	return (0);
}

/*
 * Truncate inode. Only truncate-to-0 is supported in Phase 2.
 */
int
ext4fs_truncate(struct inode *ip, off_t length, int flags, struct ucred *cred)
{
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_extent_header *eh = &din->i_extent_header;
	struct ext4fs_extent *ext;
	off_t cursize;
	u_int16_t entries;
	int i;

	cursize = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	if (length == cursize)
		return (0);

	/* Only support truncate to 0 for now */
	if (length != 0)
		return (EOPNOTSUPP);

	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC)
		return (EIO);

	/* Only handle depth-0 extent trees */
	if (letoh16(eh->eh_depth) != 0)
		return (EOPNOTSUPP);

	entries = letoh16(eh->eh_entries);
	ext = din->i_extent;

	/* Free all extents */
	for (i = 0; i < entries; i++) {
		u_int64_t start = letoh32(ext[i].e_start_lo) |
		    ((u_int64_t)letoh16(ext[i].e_start_hi) << 32);
		u_int16_t len = letoh16(ext[i].e_len);
		u_int16_t j;

		if (len > 32768)
			len -= 32768;

		for (j = 0; j < len; j++)
			ext4fs_blkfree(ip, start + j);
	}

	/* Zero extent entries */
	memset(ext, 0, 4 * sizeof(struct ext4fs_extent));
	eh->eh_entries = htole16(0);

	/* Update size and block count */
	ext4fs_setsize(ip, 0);
	din->i_blocks_lo = htole32(0);
	din->i_blocks_hi = htole16(0);

	ip->i_flag |= IN_CHANGE | IN_UPDATE;

	/* Purge cached data */
	uvm_vnp_setsize(ITOV(ip), 0);

	return (ext4fs_update(ip, 1));
}

/* Forward declarations */

int ext4fs_access(void *);
int ext4fs_advlock(void *);
int ext4fs_bmap(void *);
int ext4fs_chmod(struct vnode *, mode_t, struct ucred *);
int ext4fs_chown(struct vnode *, uid_t, gid_t, struct ucred *);
int ext4fs_create(void *);
int ext4fs_fsync(void *);
int ext4fs_getattr(void *);
int ext4fs_inactive(void *);
int ext4fs_link(void *);
int ext4fs_lookup(void *);
int ext4fs_mkdir(void *);
int ext4fs_mknod(void *);
int ext4fs_open(void *);
int ext4fs_pathconf(void *);
int ext4fs_print(void *);
int ext4fs_read(void *);
int ext4fs_readdir(void *);
int ext4fs_readlink(void *);
int ext4fs_reclaim(void *);
int ext4fs_remove(void *);
int ext4fs_rename(void *);
int ext4fs_rmdir(void *);
int ext4fs_setattr(void *);
int ext4fs_strategy(void *);
int ext4fs_symlink(void *);
int ext4fs_write(void *);

const struct vops ext4fs_vops = {
	.vop_lookup	= ext4fs_lookup,
	.vop_create	= ext4fs_create,
	.vop_mknod	= ext4fs_mknod,
	.vop_open	= ext4fs_open,
	.vop_close	= ufs_close,
	.vop_access	= ext4fs_access,
	.vop_getattr	= ext4fs_getattr,
	.vop_setattr	= ext4fs_setattr,
	.vop_read	= ext4fs_read,
	.vop_write	= ext4fs_write,
	.vop_ioctl	= ufs_ioctl,
	.vop_kqfilter	= ufs_kqfilter,
	.vop_revoke	= NULL,
	.vop_fsync	= ext4fs_fsync,
	.vop_remove	= ext4fs_remove,
	.vop_link	= ext4fs_link,
	.vop_rename	= ext4fs_rename,
	.vop_mkdir	= ext4fs_mkdir,
	.vop_rmdir	= ext4fs_rmdir,
	.vop_symlink	= ext4fs_symlink,
	.vop_readdir	= ext4fs_readdir,
	.vop_readlink	= ext4fs_readlink,
	.vop_abortop	= NULL,
	.vop_inactive	= ext4fs_inactive,
	.vop_reclaim	= ext4fs_reclaim,
	.vop_lock	= ufs_lock,
	.vop_unlock	= ufs_unlock,
	.vop_bmap	= ext4fs_bmap,
	.vop_strategy	= ext4fs_strategy,
	.vop_print	= ext4fs_print,
	.vop_pathconf	= ext4fs_pathconf,
	.vop_advlock	= ext4fs_advlock,
	.vop_bwrite	= NULL,
};

/* Stub implementations */

int
ext4fs_lookup(void *v)
{
	struct vop_lookup_args *ap = v;
	struct vnode *vdp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	struct inode *dp = VTOI(vdp);
	struct m_ext4fs *fs = dp->i_e4fs;
	printf("ext4fs_lookup: dir_ino=%u name=%.*s\n",
	    dp->i_number, (int)cnp->cn_namelen, cnp->cn_nameptr);
	struct ext4fs_dinode *din = &dp->i_e4din->dinode;
	struct ext4fs_directory *ep;
	struct vnode *tdp;
	struct buf *bp;
	int flags = cnp->cn_flags;
	int nameiop = cnp->cn_nameiop;
	int lockparent = flags & LOCKPARENT;
	ino_t foundino = 0;
	off_t off, filesz;
	u_int64_t lbn, pblk, blkoff;
	u_int16_t reclen;
	int error;

	/* For CREATE: track free slot info */
	int slotfreespace = 0;
	int slotneeded = 0;
	int slotsize = 0;
	off_t slotoffset = -1;
	off_t prevoff = -1;

	*vpp = NULL;

	/* Check accessibility of directory */
	if ((error = VOP_ACCESS(vdp, VEXEC, cnp->cn_cred, cnp->cn_proc)) != 0) {
		printf("ext4fs_lookup: access denied error=%d\n", error);
		return (error);
	}

	if ((flags & ISLASTCN) && (vdp->v_mount->mnt_flag & MNT_RDONLY) &&
	    (nameiop == DELETE || nameiop == RENAME))
		return (EROFS);

	/* Check the name cache */
	printf("ext4fs_lookup: cache_lookup\n");
	if ((error = cache_lookup(vdp, vpp, cnp)) >= 0) {
		printf("ext4fs_lookup: cache hit error=%d\n", error);
		return (error);
	}
	printf("ext4fs_lookup: cache miss, searching dir\n");

	/* Search directory for the name */
	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	if (nameiop == CREATE || nameiop == RENAME)
		slotneeded = EXT4FS_DIRSIZ(cnp->cn_namelen);

	for (off = 0; off < filesz; ) {
		lbn = EXT4FS_LBLKNO(fs, off);

		error = ext4fs_extent_pblk(dp, lbn, &pblk, NULL);
		if (error)
			return (error);

		error = bread(dp->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			return (error);
		}

		blkoff = EXT4FS_BLKOFF(fs, off);

		while (blkoff < fs->m_block_size && off < filesz) {
			ep = (struct ext4fs_directory *)
			    ((char *)bp->b_data + blkoff);
			reclen = letoh16(ep->e4d_reclen);

			if (reclen == 0) {
				brelse(bp);
				return (EIO);
			}

			/* Skip directory checksum tail entry */
			if (letoh32(ep->e4d_ino) == 0 &&
			    ep->e4d_namlen == 0 &&
			    ep->e4d_type == EXT4FS_DIR_TAIL_FT &&
			    reclen == EXT4FS_DIR_TAIL_SIZE) {
				off += reclen;
				blkoff += reclen;
				continue;
			}

			/* Track free space for CREATE/RENAME */
			if ((nameiop == CREATE || nameiop == RENAME) &&
			    slotoffset == -1) {
				int entsz;

				if (letoh32(ep->e4d_ino) == 0) {
					slotfreespace += reclen;
				} else {
					entsz = EXT4FS_DIRSIZ(ep->e4d_namlen);
					slotfreespace += reclen - entsz;
				}
				if (slotfreespace >= slotneeded) {
					slotoffset = off;
					slotsize = reclen;
				}
			}

			if (letoh32(ep->e4d_ino) != 0 &&
			    ep->e4d_namlen == cnp->cn_namelen &&
			    memcmp(cnp->cn_nameptr, ep->e4d_name,
			    cnp->cn_namelen) == 0) {
				foundino = letoh32(ep->e4d_ino);
				dp->i_ino = foundino;
				dp->i_reclen = reclen;
				dp->i_offset = off;
				/* For DELETE: count = prev entry to this */
				if (nameiop == DELETE && prevoff != -1)
					dp->i_count = off - prevoff;
				else
					dp->i_count = 0;
				brelse(bp);
				goto found;
			}

			prevoff = off;
			off += reclen;
			blkoff += reclen;
		}

		brelse(bp);
	}

	/* Not found */
	printf("ext4fs_lookup: not found name=%.*s\n",
	    (int)cnp->cn_namelen, cnp->cn_nameptr);
	if ((nameiop == CREATE || nameiop == RENAME) && (flags & ISLASTCN)) {
		if (vdp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if ((error = VOP_ACCESS(vdp, VWRITE, cnp->cn_cred,
		    cnp->cn_proc)) != 0)
			return (error);
		/* Save free slot info for direnter */
		if (slotoffset == -1) {
			dp->i_offset = filesz;
			dp->i_count = 0;
		} else {
			dp->i_offset = slotoffset;
			dp->i_count = slotsize;
		}
		cnp->cn_flags |= SAVENAME;
		if (!lockparent) {
			VOP_UNLOCK(vdp);
			cnp->cn_flags |= PDIRUNLOCK;
		}
		return (EJUSTRETURN);
	}

	if ((cnp->cn_flags & MAKEENTRY) && nameiop != CREATE)
		cache_enter(vdp, *vpp, cnp);
	return (ENOENT);

found:
	printf("ext4fs_lookup: found ino=%u\n", (unsigned)foundino);
	if ((flags & ISLASTCN) && nameiop == LOOKUP)
		dp->i_diroff = EXT4FS_LBLKNO(fs, dp->i_offset) *
		    fs->m_block_size;

	/*
	 * If deleting, and at end of pathname, return parameters
	 * which can be used to remove file.  If the wantparent flag
	 * isn't set, we return only the directory (in ndp->ni_dvp),
	 * otherwise we go on and lock the inode, being careful with ".".
	 */
	if (nameiop == DELETE && (flags & ISLASTCN)) {
		if ((error = VOP_ACCESS(vdp, VWRITE, cnp->cn_cred,
		    cnp->cn_proc)) != 0)
			return (error);
		if (dp->i_number == foundino) {
			vref(vdp);
			*vpp = vdp;
			return (0);
		}
		if ((error = VFS_VGET(vdp->v_mount, foundino, &tdp)) != 0)
			return (error);
		*vpp = tdp;
		if (!lockparent) {
			VOP_UNLOCK(vdp);
			cnp->cn_flags |= PDIRUNLOCK;
		}
		return (0);
	}

	/*
	 * If rewriting (RENAME), return the inode and the
	 * information required to rewrite the present directory
	 * Must get inode of directory entry to verify it's a
	 * regular file, or empty directory.
	 */
	if (nameiop == RENAME && (flags & ISLASTCN)) {
		if ((error = VOP_ACCESS(vdp, VWRITE, cnp->cn_cred,
		    cnp->cn_proc)) != 0)
			return (error);
		if (dp->i_number == foundino)
			return (EISDIR);
		if ((error = VFS_VGET(vdp->v_mount, foundino, &tdp)) != 0)
			return (error);
		*vpp = tdp;
		cnp->cn_flags |= SAVENAME;
		if (!lockparent) {
			VOP_UNLOCK(vdp);
			cnp->cn_flags |= PDIRUNLOCK;
		}
		return (0);
	}

	if (flags & ISDOTDOT) {
		/* ".." - unlock parent, get child, optionally relock */
		VOP_UNLOCK(vdp);
		cnp->cn_flags |= PDIRUNLOCK;
		error = VFS_VGET(vdp->v_mount, foundino, &tdp);
		if (error) {
			if (vn_lock(vdp, LK_EXCLUSIVE | LK_RETRY) == 0)
				cnp->cn_flags &= ~PDIRUNLOCK;
			return (error);
		}
		if (lockparent && (flags & ISLASTCN)) {
			if ((error = vn_lock(vdp, LK_EXCLUSIVE)) != 0) {
				vput(tdp);
				return (error);
			}
			cnp->cn_flags &= ~PDIRUNLOCK;
		}
		*vpp = tdp;
	} else if (dp->i_number == foundino) {
		/* "." - return same vnode */
		printf("ext4fs_lookup: dot\n");
		vref(vdp);
		*vpp = vdp;
	} else {
		printf("ext4fs_lookup: vget ino=%u\n", (unsigned)foundino);
		if ((error = VFS_VGET(vdp->v_mount, foundino, &tdp)) != 0) {
			printf("ext4fs_lookup: vget error=%d\n", error);
			return (error);
		}
		printf("ext4fs_lookup: vget done\n");
		if (!lockparent || !(flags & ISLASTCN)) {
			VOP_UNLOCK(vdp);
			cnp->cn_flags |= PDIRUNLOCK;
		}
		*vpp = tdp;
	}

	if (cnp->cn_flags & MAKEENTRY)
		cache_enter(vdp, *vpp, cnp);
	printf("ext4fs_lookup: returning 0\n");
	return (0);
}

/*
 * Common code to create a new inode and enter it in a directory.
 */
static int
ext4fs_makeinode(int mode, struct vnode *dvp, struct vnode **vpp,
    struct componentname *cnp)
{
	struct inode *ip, *pdir;
	struct vnode *tvp;
	struct ext4fs_dinode *din;
	int error;

	pdir = VTOI(dvp);

	*vpp = NULL;
	if ((mode & S_IFMT) == 0)
		mode |= S_IFREG;

	printf("ext4fs_makeinode: inode_alloc mode=%o\n", mode);
	error = ext4fs_inode_alloc(pdir, mode, cnp->cn_cred, &tvp);
	if (error) {
		printf("ext4fs_makeinode: inode_alloc error=%d\n", error);
		pool_put(&namei_pool, cnp->cn_pnbuf);
		return (error);
	}

	ip = VTOI(tvp);
	din = &ip->i_e4din->dinode;
	printf("ext4fs_makeinode: ino=%u\n", ip->i_number);

	/* Set owner from cred and parent */
	din->i_uid_lo = htole16(cnp->cn_cred->cr_uid & 0xFFFF);
	din->i_uid_hi = htole16((cnp->cn_cred->cr_uid >> 16) & 0xFFFF);
	{
		gid_t gid = letoh16(pdir->i_e4din->dinode.i_gid_lo) |
		    ((gid_t)letoh16(pdir->i_e4din->dinode.i_gid_hi) << 16);
		din->i_gid_lo = htole16(gid & 0xFFFF);
		din->i_gid_hi = htole16((gid >> 16) & 0xFFFF);
	}

	ip->i_flag |= IN_ACCESS | IN_CHANGE | IN_UPDATE;
	din->i_mode = htole16(mode);
	tvp->v_type = IFTOVT(mode);
	ip->i_effnlink = 1;
	din->i_links_count = htole16(1);

	/* Clear SGID if not group member */
	if ((mode & ISGID) &&
	    !groupmember(letoh16(din->i_gid_lo) |
	    ((gid_t)letoh16(din->i_gid_hi) << 16), cnp->cn_cred) &&
	    suser_ucred(cnp->cn_cred))
		din->i_mode = htole16(letoh16(din->i_mode) & ~ISGID);

	/* Write inode to disk before directory entry */
	printf("ext4fs_makeinode: update\n");
	if ((error = ext4fs_update(ip, 1)) != 0) {
		printf("ext4fs_makeinode: update error=%d\n", error);
		goto bad;
	}
	printf("ext4fs_makeinode: direnter\n");
	error = ext4fs_direnter(ip, dvp, cnp);
	if (error != 0) {
		printf("ext4fs_makeinode: direnter error=%d\n", error);
		goto bad;
	}
	printf("ext4fs_makeinode: done ino=%u\n", ip->i_number);

	if ((cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, cnp->cn_pnbuf);
	*vpp = tvp;
	return (0);

bad:
	pool_put(&namei_pool, cnp->cn_pnbuf);
	ip->i_effnlink = 0;
	din->i_links_count = htole16(0);
	ip->i_flag |= IN_CHANGE;
	tvp->v_type = VNON;
	vput(tvp);
	return (error);
}

int
ext4fs_create(void *v)
{
	struct vop_create_args *ap = v;
	printf("ext4fs_create: name=%.*s\n",
	    (int)ap->a_cnp->cn_namelen, ap->a_cnp->cn_nameptr);
	return (ext4fs_makeinode(
	    MAKEIMODE(ap->a_vap->va_type, ap->a_vap->va_mode),
	    ap->a_dvp, ap->a_vpp, ap->a_cnp));
}

int
ext4fs_mknod(void *v)
{
	struct vop_mknod_args *ap = v;
	struct vnode **vpp = ap->a_vpp;
	printf("ext4fs_mknod: name=%.*s\n",
	    (int)ap->a_cnp->cn_namelen, ap->a_cnp->cn_nameptr);
	struct vnode *tvp;
	struct inode *ip;
	int error;

	error = ext4fs_makeinode(
	    MAKEIMODE(ap->a_vap->va_type, ap->a_vap->va_mode),
	    ap->a_dvp, &tvp, ap->a_cnp);
	if (error)
		return (error);

	ip = VTOI(tvp);

	/* Store device number */
	if (ap->a_vap->va_rdev != VNOVAL) {
		/* Old format in i_block[0], new format in i_block[1] */
		ip->i_e4din->dinode.i_block[0] =
		    htole32(ap->a_vap->va_rdev);
		ip->i_e4din->dinode.i_block[1] =
		    htole32(ap->a_vap->va_rdev);
	}

	ip->i_flag |= IN_CHANGE | IN_UPDATE;
	ext4fs_update(ip, 1);

	*vpp = tvp;
	return (0);
}

int
ext4fs_open(void *v)
{
	struct vop_open_args *ap = v;
	printf("ext4fs_open: ino=%u\n", VTOI(ap->a_vp)->i_number);
	return (0);
}

int
ext4fs_access(void *v)
{
	struct vop_access_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	printf("ext4fs_access: ino=%u mode=0%o\n", ip->i_number, ap->a_mode);
	mode_t mode;
	uid_t uid;
	gid_t gid;

	mode = letoh16(din->i_mode);
	uid = letoh16(din->i_uid_lo) |
	    ((uid_t)letoh16(din->i_uid_hi) << 16);
	gid = letoh16(din->i_gid_lo) |
	    ((gid_t)letoh16(din->i_gid_hi) << 16);

	return (vaccess(vp->v_type, mode, uid, gid, ap->a_mode, ap->a_cred));
}

int
ext4fs_getattr(void *v)
{
	struct vop_getattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	printf("ext4fs_getattr: ino=%u\n", ip->i_number);
	struct ext4fs_dinode_256 *din = ip->i_e4din;
	struct vattr *vap = ap->a_vap;

	/* Copy from inode table */
	vap->va_fsid = ip->i_dev;
	vap->va_fileid = ip->i_number;
	vap->va_mode = letoh16(din->dinode.i_mode) & ALLPERMS;
	vap->va_nlink = letoh16(din->dinode.i_links_count);
	vap->va_uid = letoh16(din->dinode.i_uid_lo);
	vap->va_uid |= (uid_t)letoh16(din->dinode.i_uid_hi) << 16;
	vap->va_gid = letoh16(din->dinode.i_gid_lo);
	vap->va_gid |= (gid_t)letoh16(din->dinode.i_gid_hi) << 16;
	vap->va_rdev = 0;
	vap->va_size = letoh32(din->dinode.i_size_lo);
	vap->va_size |= (off_t)letoh32(din->dinode.i_size_hi) << 32;

	/* Convert timestamps with nanosecond precision */
	vap->va_atime.tv_sec = letoh32(din->dinode.i_atime);
	vap->va_atime.tv_nsec = letoh32(din->dinode.i_atime_extra) >> 2;
	vap->va_mtime.tv_sec = letoh32(din->dinode.i_mtime);
	vap->va_mtime.tv_nsec = letoh32(din->dinode.i_mtime_extra) >> 2;
	vap->va_ctime.tv_sec = letoh32(din->dinode.i_ctime);
	vap->va_ctime.tv_nsec = letoh32(din->dinode.i_ctime_extra) >> 2;

	vap->va_flags = 0;
	vap->va_gen = letoh32(din->dinode.i_nfs_generation);

	/* Set appropriate block size */
	if (vp->v_type == VBLK)
		vap->va_blocksize = BLKDEV_IOSIZE;
	else if (vp->v_type == VCHR)
		vap->va_blocksize = MAXBSIZE;
	else
		vap->va_blocksize = vp->v_mount->mnt_stat.f_iosize;

	vap->va_bytes = letoh32(din->dinode.i_blocks_lo);
	vap->va_bytes |= (off_t)letoh16(din->dinode.i_blocks_hi) << 32;
	vap->va_bytes *= VFSTOUFS(vp->v_mount)->um_e4fs->m_block_size;
	vap->va_type = vp->v_type;
	vap->va_filerev = 0;

	return (0);
}

int
ext4fs_chmod(struct vnode *vp, mode_t mode, struct ucred *cred)
{
	struct inode *ip = VTOI(vp);
	printf("ext4fs_chmod: ino=%u mode=0%o\n", ip->i_number, mode);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	uid_t uid;
	gid_t gid;
	u_int16_t cur_mode;
	int error;

	uid = letoh16(din->i_uid_lo) |
	    ((uid_t)letoh16(din->i_uid_hi) << 16);
	gid = letoh16(din->i_gid_lo) |
	    ((gid_t)letoh16(din->i_gid_hi) << 16);

	if (cred->cr_uid != uid && (error = suser_ucred(cred)))
		return (error);
	if (cred->cr_uid) {
		if (vp->v_type != VDIR && (mode & S_ISTXT))
			return (EFTYPE);
		if (!groupmember(gid, cred) && (mode & ISGID))
			return (EPERM);
	}

	cur_mode = letoh16(din->i_mode);
	cur_mode &= ~ALLPERMS;
	cur_mode |= (mode & ALLPERMS);
	din->i_mode = htole16(cur_mode);
	ip->i_flag |= IN_CHANGE;

	if ((vp->v_flag & VTEXT) && (cur_mode & S_ISTXT) == 0)
		(void)uvm_vnp_uncache(vp);

	return (0);
}

int
ext4fs_chown(struct vnode *vp, uid_t uid, gid_t gid, struct ucred *cred)
{
	struct inode *ip = VTOI(vp);
	printf("ext4fs_chown: ino=%u uid=%u gid=%u\n", ip->i_number, uid, gid);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	uid_t ouid;
	gid_t ogid;
	u_int16_t mode;
	int error;

	ouid = letoh16(din->i_uid_lo) |
	    ((uid_t)letoh16(din->i_uid_hi) << 16);
	ogid = letoh16(din->i_gid_lo) |
	    ((gid_t)letoh16(din->i_gid_hi) << 16);

	if (uid == (uid_t)VNOVAL)
		uid = ouid;
	if (gid == (gid_t)VNOVAL)
		gid = ogid;

	if ((cred->cr_uid != ouid || uid != ouid ||
	    (gid != ogid && !groupmember(gid, cred))) &&
	    (error = suser_ucred(cred)))
		return (error);

	din->i_uid_lo = htole16(uid & 0xFFFF);
	din->i_uid_hi = htole16((uid >> 16) & 0xFFFF);
	din->i_gid_lo = htole16(gid & 0xFFFF);
	din->i_gid_hi = htole16((gid >> 16) & 0xFFFF);

	if (ouid != uid || ogid != gid)
		ip->i_flag |= IN_CHANGE;
	if (ouid != uid && cred->cr_uid != 0) {
		mode = letoh16(din->i_mode);
		mode &= ~S_ISUID;
		din->i_mode = htole16(mode);
	}
	if (ogid != gid && cred->cr_uid != 0) {
		mode = letoh16(din->i_mode);
		mode &= ~S_ISGID;
		din->i_mode = htole16(mode);
	}

	return (0);
}

int
ext4fs_setattr(void *v)
{
	struct vop_setattr_args *ap = v;
	struct vattr *vap = ap->a_vap;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ucred *cred = ap->a_cred;
	printf("ext4fs_setattr: ino=%u\n", ip->i_number);
	int error = 0;

	if ((vap->va_type != VNON) || (vap->va_nlink != VNOVAL) ||
	    (vap->va_fsid != VNOVAL) || (vap->va_fileid != VNOVAL) ||
	    (vap->va_blocksize != VNOVAL) || (vap->va_rdev != VNOVAL) ||
	    ((int)vap->va_bytes != VNOVAL) || (vap->va_gen != VNOVAL))
		return (EINVAL);

	if (vap->va_flags != VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if ((error = suser_ucred(cred)))
			return (error);
		u_int32_t iflags = letoh32(din->i_flags);
		iflags &= ~(EXTFS_INODE_FLAG_APPEND |
		    EXTFS_INODE_FLAG_IMMUTABLE);
		iflags |= (vap->va_flags & SF_APPEND) ?
		    EXTFS_INODE_FLAG_APPEND : 0;
		iflags |= (vap->va_flags & SF_IMMUTABLE) ?
		    EXTFS_INODE_FLAG_IMMUTABLE : 0;
		din->i_flags = htole32(iflags);
		ip->i_flag |= IN_CHANGE;
	}

	if (vap->va_uid != (uid_t)VNOVAL ||
	    vap->va_gid != (gid_t)VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		error = ext4fs_chown(vp, vap->va_uid, vap->va_gid, cred);
		if (error)
			return (error);
	}

	if (vap->va_size != VNOVAL) {
		switch (vp->v_type) {
		case VDIR:
			return (EISDIR);
		case VLNK:
		case VREG:
			if (vp->v_mount->mnt_flag & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			break;
		}
		error = ext4fs_truncate(ip, vap->va_size, 0, cred);
		if (error)
			return (error);
	}

	if ((vap->va_vaflags & VA_UTIMES_CHANGE) ||
	    vap->va_atime.tv_nsec != VNOVAL ||
	    vap->va_mtime.tv_nsec != VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		uid_t uid = letoh16(din->i_uid_lo) |
		    ((uid_t)letoh16(din->i_uid_hi) << 16);
		if (cred->cr_uid != uid &&
		    (error = suser_ucred(cred)) &&
		    ((vap->va_vaflags & VA_UTIMES_NULL) == 0 ||
		    (error = VOP_ACCESS(vp, VWRITE, cred, ap->a_p))))
			return (error);
		if (vap->va_mtime.tv_nsec != VNOVAL)
			ip->i_flag |= IN_CHANGE | IN_UPDATE;
		else if (vap->va_vaflags & VA_UTIMES_CHANGE)
			ip->i_flag |= IN_CHANGE;
		if (vap->va_atime.tv_nsec != VNOVAL)
			ip->i_flag |= IN_ACCESS;
		EXT4FS_ITIMES(ip);
		if (vap->va_mtime.tv_nsec != VNOVAL) {
			din->i_mtime =
			    htole32((u_int32_t)vap->va_mtime.tv_sec);
			din->i_mtime_extra =
			    htole32(vap->va_mtime.tv_nsec << 2);
		}
		if (vap->va_atime.tv_nsec != VNOVAL) {
			din->i_atime =
			    htole32((u_int32_t)vap->va_atime.tv_sec);
			din->i_atime_extra =
			    htole32(vap->va_atime.tv_nsec << 2);
		}
		ip->i_flag |= IN_MODIFIED;
		error = ext4fs_update(ip, 1);
		if (error)
			return (error);
	}

	if (vap->va_mode != (mode_t)VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		error = ext4fs_chmod(vp, vap->va_mode, cred);
	}

	return (error);
}

int
ext4fs_read(void *v)
{
	struct vop_read_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	printf("ext4fs_read: ino=%u resid=%zu offset=%lld\n",
	    ip->i_number, ap->a_uio->uio_resid,
	    (long long)ap->a_uio->uio_offset);
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct uio *uio = ap->a_uio;
	struct buf *bp;
	off_t filesz, bytesinfile;
	u_int64_t lbn, pblk, ncontig;
	int error, blkoffset, xfersize, size;

	if (vp->v_type == VDIR)
		return (EISDIR);
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);

	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	for (error = 0; uio->uio_resid > 0; ) {
		bytesinfile = filesz - uio->uio_offset;
		if (bytesinfile <= 0)
			break;

		lbn = EXT4FS_LBLKNO(fs, uio->uio_offset);
		blkoffset = EXT4FS_BLKOFF(fs, uio->uio_offset);

		error = ext4fs_extent_pblk(ip, lbn, &pblk, &ncontig);
		if (error)
			break;

		/* Read up to ncontig blocks, capped at MAXPHYS */
		size = ncontig * fs->m_block_size;
		if (size > MAXPHYS)
			size = MAXPHYS;

		xfersize = size - blkoffset;
		xfersize = MIN(xfersize, uio->uio_resid);
		xfersize = MIN(xfersize, bytesinfile);

		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    size, &bp);
		if (error) {
			brelse(bp);
			break;
		}

		size -= bp->b_resid;
		if (size < xfersize) {
			if (size == 0) {
				brelse(bp);
				break;
			}
			xfersize = size;
		}

		error = uiomove((char *)bp->b_data + blkoffset, xfersize, uio);
		brelse(bp);
		if (error)
			break;
	}

	if (!(vp->v_mount->mnt_flag & MNT_NOATIME))
		ip->i_flag |= IN_ACCESS;

	return (error);
}

int
ext4fs_write(void *v)
{
	struct vop_write_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct inode *ip = VTOI(vp);
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct buf *bp;
	off_t filesz;
	u_int64_t lbn;
	int ioflag = ap->a_ioflag;
	int blkoffset, xfersize;
	int error;
	size_t resid;
	ssize_t overrun;

	printf("ext4fs_write: ino=%u resid=%zu offset=%lld\n",
	    ip->i_number, uio->uio_resid, (long long)uio->uio_offset);

	if (uio->uio_resid == 0)
		return (0);

	switch (vp->v_type) {
	case VREG:
		break;
	case VLNK:
		break;
	case VDIR:
		return (EOPNOTSUPP);
	default:
		panic("ext4fs_write: type");
	}

	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	if (ioflag & IO_APPEND)
		uio->uio_offset = filesz;

	if (uio->uio_offset < 0)
		return (EINVAL);

	if ((error = vn_fsizechk(vp, uio, ioflag, &overrun)))
		return (error);

	resid = uio->uio_resid;

	for (error = 0; uio->uio_resid > 0; ) {
		lbn = EXT4FS_LBLKNO(fs, uio->uio_offset);
		blkoffset = EXT4FS_BLKOFF(fs, uio->uio_offset);
		xfersize = fs->m_block_size - blkoffset;
		if (uio->uio_resid < xfersize)
			xfersize = uio->uio_resid;

		printf("ext4fs_write: lbn=%llu buf_alloc\n",
		    (unsigned long long)lbn);
		error = ext4fs_buf_alloc(ip, lbn, fs->m_block_size,
		    ap->a_cred, &bp, B_CLRBUF);
		if (error)
			break;
		printf("ext4fs_write: lbn=%llu uiomove\n",
		    (unsigned long long)lbn);

		error = uiomove((char *)bp->b_data + blkoffset, xfersize,
		    uio);

		printf("ext4fs_write: lbn=%llu bwrite\n",
		    (unsigned long long)lbn);
		if (ioflag & IO_SYNC)
			(void)bwrite(bp);
		else if (xfersize + blkoffset == fs->m_block_size)
			bawrite(bp);
		else
			bdwrite(bp);
		printf("ext4fs_write: lbn=%llu bwrite done\n",
		    (unsigned long long)lbn);

		if (error || xfersize == 0)
			break;

		/* Update file size if we wrote past end */
		if (uio->uio_offset > filesz) {
			ext4fs_setsize(ip, uio->uio_offset);
			filesz = uio->uio_offset;
			printf("ext4fs_write: lbn=%llu uvm_vnp_setsize\n",
			    (unsigned long long)lbn);
			uvm_vnp_setsize(vp, filesz);
			printf("ext4fs_write: lbn=%llu uvm_vnp_setsize done\n",
			    (unsigned long long)lbn);
		}

		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}

	/* Clear setuid/setgid bits on write by non-root */
	if (resid > uio->uio_resid && ap->a_cred &&
	    ap->a_cred->cr_uid != 0) {
		u_int16_t mode = letoh16(din->i_mode);
		mode &= ~(S_ISUID | S_ISGID);
		din->i_mode = htole16(mode);
	}

	if (error == 0 && resid > uio->uio_resid && (ioflag & IO_SYNC))
		error = ext4fs_update(ip, 1);

	uio->uio_resid += overrun;
	return (error);
}

int
ext4fs_fsync(void *v)
{
	struct vop_fsync_args *ap = v;
	struct vnode *vp = ap->a_vp;
	printf("ext4fs_fsync: ino=%u\n", VTOI(vp)->i_number);

	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		return (0);

	vflushbuf(vp, ap->a_waitfor == MNT_WAIT);
	return (ext4fs_update(VTOI(vp), ap->a_waitfor == MNT_WAIT));
}

int
ext4fs_remove(void *v)
{
	struct vop_remove_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	struct inode *ip = VTOI(vp);
	printf("ext4fs_remove: ino=%u dir_ino=%u name=%.*s\n",
	    ip->i_number, VTOI(dvp)->i_number,
	    (int)ap->a_cnp->cn_namelen, ap->a_cnp->cn_nameptr);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int16_t nlink;
	int error;

	if (vp->v_type == VDIR) {
		error = EPERM;
		goto out;
	}

	error = ext4fs_dirremove(dvp, ap->a_cnp);
	if (error)
		goto out;

	nlink = letoh16(din->i_links_count);
	if (nlink > 0)
		nlink--;
	din->i_links_count = htole16(nlink);
	ip->i_effnlink = nlink;
	ip->i_flag |= IN_CHANGE;

out:
	return (error);
}

int
ext4fs_link(void *v)
{
	struct vop_link_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct vnode *vp = ap->a_vp;
	struct componentname *cnp = ap->a_cnp;
	struct inode *ip = VTOI(vp);
	printf("ext4fs_link: ino=%u dir_ino=%u name=%.*s\n",
	    ip->i_number, VTOI(dvp)->i_number,
	    (int)cnp->cn_namelen, cnp->cn_nameptr);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int16_t nlink;
	int error;

	if (vp->v_type == VDIR) {
		error = EPERM;
		goto out2;
	}
	if (dvp->v_mount != vp->v_mount) {
		error = EXDEV;
		goto out2;
	}

	nlink = letoh16(din->i_links_count);
	if (nlink >= EXT4FS_LINK_MAX) {
		error = EMLINK;
		goto out2;
	}

	if ((error = vn_lock(vp, LK_EXCLUSIVE)) != 0)
		goto out2;

	nlink++;
	din->i_links_count = htole16(nlink);
	ip->i_effnlink = nlink;
	ip->i_flag |= IN_CHANGE;
	error = ext4fs_update(ip, 1);
	if (error)
		goto out1;

	error = ext4fs_direnter(ip, dvp, cnp);
	if (error) {
		nlink--;
		din->i_links_count = htole16(nlink);
		ip->i_effnlink = nlink;
		ip->i_flag |= IN_CHANGE;
	}

out1:
	if (dvp != vp)
		VOP_UNLOCK(vp);
out2:
	vput(dvp);
	return (error);
}

int
ext4fs_rename(void *v)
{
	struct vop_rename_args *ap = v;
	struct vnode *tvp = ap->a_tvp;
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *fdvp = ap->a_fdvp;
	struct componentname *tcnp = ap->a_tcnp;
	struct componentname *fcnp = ap->a_fcnp;
	struct inode *ip, *xp, *dp;
	struct ext4fs_dinode *din;
	int doingdirectory = 0, oldparent = 0, newparent = 0;
	int error = 0;
	u_int16_t nlink;

	/* Check for cross-device rename */
	if ((fvp->v_mount != tdvp->v_mount) ||
	    (tvp && (fvp->v_mount != tvp->v_mount))) {
		error = EXDEV;
abortit:
		VOP_ABORTOP(tdvp, tcnp);
		if (tdvp == tvp)
			vrele(tdvp);
		else
			vput(tdvp);
		if (tvp)
			vput(tvp);
		VOP_ABORTOP(fdvp, fcnp);
		vrele(fdvp);
		vrele(fvp);
		return (error);
	}

	/* Lock source */
	if ((error = vn_lock(fvp, LK_EXCLUSIVE)) != 0)
		goto abortit;

	dp = VTOI(fdvp);
	ip = VTOI(fvp);
	din = &ip->i_e4din->dinode;

	nlink = letoh16(din->i_links_count);
	if ((letoh32(din->i_flags) &
	    (EXTFS_INODE_FLAG_IMMUTABLE | EXTFS_INODE_FLAG_APPEND))) {
		VOP_UNLOCK(fvp);
		error = EPERM;
		goto abortit;
	}

	if ((letoh16(din->i_mode) & S_IFMT) == S_IFDIR) {
		doingdirectory = 1;
		oldparent = dp->i_number;
	}

	/* Bump link count temporarily for crash safety */
	nlink++;
	din->i_links_count = htole16(nlink);
	ip->i_effnlink = nlink;
	ip->i_flag |= IN_CHANGE;
	if ((error = ext4fs_update(ip, 1)) != 0) {
		VOP_UNLOCK(fvp);
		goto abortit;
	}
	VOP_UNLOCK(fvp);

	/*
	 * 1. If target exists, remove/rewrite it.
	 * 2. If not, add new directory entry.
	 */
	dp = VTOI(tdvp);
	xp = NULL;
	if (tvp)
		xp = VTOI(tvp);

	if (tvp != NULL) {
		/* Target exists - rewrite the entry */
		error = ext4fs_dirrewrite(dp, ip, tcnp);
		if (error)
			goto bad;

		if (xp) {
			u_int16_t xnlink =
			    letoh16(xp->i_e4din->dinode.i_links_count);
			if (doingdirectory && ITOV(xp)->v_type == VDIR) {
				/* If target dir is not empty, fail */
				if (!ext4fs_dirempty(xp, dp->i_number,
				    tcnp->cn_cred)) {
					error = ENOTEMPTY;
					goto bad;
				}
				/* Remove ".." ref from parent */
				u_int16_t pnlink = letoh16(
				    dp->i_e4din->dinode.i_links_count);
				if (pnlink > 1) {
					pnlink--;
					dp->i_e4din->dinode.i_links_count =
					    htole16(pnlink);
					dp->i_effnlink = pnlink;
					dp->i_flag |= IN_CHANGE;
				}
			}
			if (xnlink > 0)
				xnlink--;
			xp->i_e4din->dinode.i_links_count = htole16(xnlink);
			xp->i_effnlink = xnlink;
			xp->i_flag |= IN_CHANGE;
			if (doingdirectory && xnlink == 0)
				ext4fs_truncate(xp, 0, 0, tcnp->cn_cred);
		}
		cache_purge(tvp);
	} else {
		/* Target doesn't exist - add entry */
		dp = VTOI(tdvp);
		error = ext4fs_direnter(ip, tdvp, tcnp);
		if (error)
			goto bad;
	}

	/* Remove source entry */
	dp = VTOI(fdvp);
	error = ext4fs_dirremove(fdvp, fcnp);
	if (error)
		goto bad;

	/* Decrement the bump we did earlier */
	nlink = letoh16(ip->i_e4din->dinode.i_links_count);
	if (nlink > 0)
		nlink--;
	ip->i_e4din->dinode.i_links_count = htole16(nlink);
	ip->i_effnlink = nlink;
	ip->i_flag |= IN_CHANGE;

	/* If directory moved to new parent, update ".." */
	if (doingdirectory) {
		newparent = VTOI(tdvp)->i_number;
		if (newparent != oldparent) {
			struct buf *dbp;
			struct ext4fs_directory *dotdot;
			u_int64_t dpblk;

			/* Update ".." in moved directory */
			error = ext4fs_extent_pblk(ip, 0, &dpblk, NULL);
			if (error == 0) {
				error = bread(ip->i_devvp,
				    (daddr_t)EXT4FS_FSBTODB(ip->i_e4fs,
				    dpblk), ip->i_e4fs->m_block_size, &dbp);
				if (error == 0) {
					dotdot = (struct ext4fs_directory *)
					    ((char *)dbp->b_data +
					    letoh16(((struct ext4fs_directory *)
					    dbp->b_data)->e4d_reclen));
					dotdot->e4d_ino =
					    htole32(newparent);
					ext4fs_dir_set_csum(ip->i_e4fs,
					    ip->i_number,
					    ip->i_e4din->dinode.i_nfs_generation,
					    dbp->b_data);
					bwrite(dbp);
				} else
					brelse(dbp);
			}

			/* Adjust parent link counts */
			{
				struct inode *odp = VTOI(fdvp);
				u_int16_t onlink = letoh16(
				    odp->i_e4din->dinode.i_links_count);
				if (onlink > 1) {
					onlink--;
					odp->i_e4din->dinode.i_links_count =
					    htole16(onlink);
					odp->i_effnlink = onlink;
					odp->i_flag |= IN_CHANGE;
				}
			}
			{
				struct inode *ndp = VTOI(tdvp);
				u_int16_t nnlink = letoh16(
				    ndp->i_e4din->dinode.i_links_count);
				nnlink++;
				ndp->i_e4din->dinode.i_links_count =
				    htole16(nnlink);
				ndp->i_effnlink = nnlink;
				ndp->i_flag |= IN_CHANGE;
				ext4fs_update(ndp, 1);
			}
		}
	}

	if (tvp)
		vput(tvp);
	vput(tdvp);
	vrele(fdvp);
	vrele(fvp);
	return (0);

bad:
	/* Restore link count */
	nlink = letoh16(ip->i_e4din->dinode.i_links_count);
	if (nlink > 0 && doingdirectory)
		nlink--;
	if (nlink > 0)
		nlink--;
	ip->i_e4din->dinode.i_links_count = htole16(nlink);
	ip->i_effnlink = nlink;
	ip->i_flag |= IN_CHANGE;

	if (tvp)
		vput(tvp);
	vput(tdvp);
	vrele(fdvp);
	vrele(fvp);
	return (error);
}

int
ext4fs_mkdir(void *v)
{
	struct vop_mkdir_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct vattr *vap = ap->a_vap;
	struct componentname *cnp = ap->a_cnp;
	struct inode *dp = VTOI(dvp);
	struct inode *ip;
	struct vnode *tvp;
	struct buf *bp;
	struct ext4fs_directory *dirp;
	struct ext4fs_dinode *din;
	struct m_ext4fs *fs = dp->i_e4fs;
	int error;
	u_int16_t nlink;

	nlink = letoh16(dp->i_e4din->dinode.i_links_count);
	if (nlink >= EXT4FS_LINK_MAX) {
		error = EMLINK;
		goto out;
	}

	/* Allocate inode for new directory */
	error = ext4fs_inode_alloc(dp, S_IFDIR | vap->va_mode,
	    cnp->cn_cred, &tvp);
	if (error)
		goto out;

	ip = VTOI(tvp);
	din = &ip->i_e4din->dinode;

	/* Set owner */
	din->i_uid_lo = htole16(cnp->cn_cred->cr_uid & 0xFFFF);
	din->i_uid_hi = htole16((cnp->cn_cred->cr_uid >> 16) & 0xFFFF);
	{
		gid_t gid = letoh16(dp->i_e4din->dinode.i_gid_lo) |
		    ((gid_t)letoh16(dp->i_e4din->dinode.i_gid_hi) << 16);
		din->i_gid_lo = htole16(gid & 0xFFFF);
		din->i_gid_hi = htole16((gid >> 16) & 0xFFFF);
	}

	ip->i_flag |= IN_ACCESS | IN_CHANGE | IN_UPDATE;
	din->i_mode = htole16(S_IFDIR | vap->va_mode);
	tvp->v_type = VDIR;
	ip->i_effnlink = 2;
	din->i_links_count = htole16(2);

	/* Allocate first block for "." and ".." */
	error = ext4fs_buf_alloc(ip, 0, fs->m_block_size, cnp->cn_cred,
	    &bp, B_CLRBUF);
	if (error)
		goto bad;

	/* Write "." entry */
	dirp = (struct ext4fs_directory *)bp->b_data;
	dirp->e4d_ino = htole32((u_int32_t)ip->i_number);
	dirp->e4d_reclen = htole16(12);
	dirp->e4d_namlen = 1;
	dirp->e4d_type = EXT4FS_FT_DIR;
	dirp->e4d_name[0] = '.';

	/* Write ".." entry */
	dirp = (struct ext4fs_directory *)((char *)bp->b_data + 12);
	dirp->e4d_ino = htole32((u_int32_t)dp->i_number);
	dirp->e4d_reclen = htole16(fs->m_block_size - 12 -
	    ((fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM) ? EXT4FS_DIR_TAIL_SIZE : 0));
	dirp->e4d_namlen = 2;
	dirp->e4d_type = EXT4FS_FT_DIR;
	dirp->e4d_name[0] = '.';
	dirp->e4d_name[1] = '.';

	ext4fs_dir_set_csum(fs, ip->i_number,
	    ip->i_e4din->dinode.i_nfs_generation, bp->b_data);
	printf("ext4fs_mkdir: before bwrite\n");
	error = bwrite(bp);
	printf("ext4fs_mkdir: after bwrite error=%d\n", error);
	if (error)
		goto bad;

	/* Set directory size */
	ext4fs_setsize(ip, fs->m_block_size);
	ip->i_flag |= IN_CHANGE | IN_UPDATE;

	/* Write inode before directory entry */
	if ((error = ext4fs_update(ip, 1)) != 0)
		goto bad;

	/* Increment parent's link count for ".." */
	nlink++;
	dp->i_e4din->dinode.i_links_count = htole16(nlink);
	dp->i_effnlink = nlink;
	dp->i_flag |= IN_CHANGE;
	if ((error = ext4fs_update(dp, 1)) != 0)
		goto bad;

	/* Enter new directory in parent */
	error = ext4fs_direnter(ip, dvp, cnp);
	if (error) {
		/* Undo parent nlink */
		nlink--;
		dp->i_e4din->dinode.i_links_count = htole16(nlink);
		dp->i_effnlink = nlink;
		dp->i_flag |= IN_CHANGE;
		goto bad;
	}

	if ((cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, cnp->cn_pnbuf);
	*ap->a_vpp = tvp;
	printf("ext4fs_mkdir: returning 0 ino=%u\n", ip->i_number);
	vput(dvp);
	return (0);

bad:
	pool_put(&namei_pool, cnp->cn_pnbuf);
	ip->i_effnlink = 0;
	din->i_links_count = htole16(0);
	ip->i_flag |= IN_CHANGE;
	tvp->v_type = VNON;
	vput(tvp);
out:
	vput(dvp);
	return (error);
}

int
ext4fs_rmdir(void *v)
{
	struct vop_rmdir_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct inode *ip = VTOI(vp);
	struct inode *dp = VTOI(dvp);
	printf("ext4fs_rmdir: ino=%u dir_ino=%u name=%.*s\n",
	    ip->i_number, dp->i_number,
	    (int)cnp->cn_namelen, cnp->cn_nameptr);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int16_t nlink;
	int error;

	/* Directory must be empty */
	if (!ext4fs_dirempty(ip, dp->i_number, cnp->cn_cred)) {
		error = ENOTEMPTY;
		goto out;
	}

	/* Remove entry from parent */
	error = ext4fs_dirremove(dvp, cnp);
	if (error)
		goto out;

	/* Decrement parent's link count ("..") */
	nlink = letoh16(dp->i_e4din->dinode.i_links_count);
	if (nlink > 1)
		nlink--;
	dp->i_e4din->dinode.i_links_count = htole16(nlink);
	dp->i_effnlink = nlink;
	dp->i_flag |= IN_CHANGE;

	cache_purge(dvp);

	/* Set target link count to 0 */
	din->i_links_count = htole16(0);
	ip->i_effnlink = 0;
	ip->i_flag |= IN_CHANGE;

	/* Truncate directory contents */
	error = ext4fs_truncate(ip, 0, 0, cnp->cn_cred);

	cache_purge(vp);

out:
	if (dvp == vp)
		vrele(vp);
	else
		vput(vp);
	vput(dvp);
	return (error);
}

int
ext4fs_symlink(void *v)
{
	struct vop_symlink_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	printf("ext4fs_symlink: dir_ino=%u name=%.*s\n",
	    VTOI(dvp)->i_number,
	    (int)ap->a_cnp->cn_namelen, ap->a_cnp->cn_nameptr);
	struct vattr *vap = ap->a_vap;
	struct componentname *cnp = ap->a_cnp;
	struct vnode **vpp = ap->a_vpp;
	struct inode *ip;
	int error, len;

	error = ext4fs_makeinode(S_IFLNK | vap->va_mode, dvp, vpp, cnp);
	if (error) {
		vput(dvp);
		return (error);
	}

	ip = VTOI(*vpp);
	len = strlen(ap->a_target);

	if (len <= EXT4FS_SYMLINK_LEN_MAX) {
		/* Fast symlink: store inline in i_block[] */
		memcpy(ip->i_e4din->dinode.i_block, ap->a_target, len);
		ext4fs_setsize(ip, len);
		/* Clear EXTENTS flag for fast symlinks */
		ip->i_e4din->dinode.i_flags &=
		    ~htole32(EXTFS_INODE_FLAG_EXTENTS);
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		error = ext4fs_update(ip, 1);
	} else {
		/* Slow symlink: write to data blocks */
		struct uio auio;
		struct iovec aiov;

		aiov.iov_base = ap->a_target;
		aiov.iov_len = len;
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_offset = 0;
		auio.uio_rw = UIO_WRITE;
		auio.uio_segflg = UIO_SYSSPACE;
		auio.uio_procp = NULL;
		auio.uio_resid = len;
		error = VOP_WRITE(*vpp, &auio, IO_NODELOCKED, ap->a_cnp->cn_cred);
	}

	if (error)
		vput(*vpp);
	vput(dvp);
	return (error);
}

int
ext4fs_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	struct uio *uio = ap->a_uio;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	printf("ext4fs_readdir: ino=%u\n", ip->i_number);
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_directory *ep;
	struct dirent dstd;
	struct buf *bp;
	off_t off, filesz;
	u_int64_t lbn, pblk, blkoff;
	u_int16_t reclen;
	int error = 0;

	if (vp->v_type != VDIR)
		return (ENOTDIR);

	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);
	off = uio->uio_offset;

	while (off < filesz && uio->uio_resid > 0) {
		lbn = EXT4FS_LBLKNO(fs, off);

		error = ext4fs_extent_pblk(ip, lbn, &pblk, NULL);
		if (error)
			break;

		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			break;
		}

		blkoff = EXT4FS_BLKOFF(fs, off);

		while (blkoff < fs->m_block_size && off < filesz) {
			ep = (struct ext4fs_directory *)
			    ((char *)bp->b_data + blkoff);
			reclen = letoh16(ep->e4d_reclen);

			if (reclen == 0) {
				error = EIO;
				brelse(bp);
				goto done;
			}

			if (letoh32(ep->e4d_ino) != 0) {
				memset(&dstd, 0, sizeof(dstd));
				dstd.d_fileno = letoh32(ep->e4d_ino);
				dstd.d_namlen = ep->e4d_namlen;

				if (ep->e4d_type < EXT4FS_FT_MAX)
					dstd.d_type =
					    ext4fs_type_to_dt[ep->e4d_type];
				else
					dstd.d_type = DT_UNKNOWN;

				memcpy(dstd.d_name, ep->e4d_name,
				    dstd.d_namlen);
				dstd.d_name[dstd.d_namlen] = '\0';
				dstd.d_reclen = DIRENT_SIZE(&dstd);
				dstd.d_off = off + reclen;

				if (dstd.d_reclen > uio->uio_resid) {
					brelse(bp);
					goto done;
				}

				error = uiomove(&dstd, dstd.d_reclen, uio);
				if (error) {
					brelse(bp);
					goto done;
				}
			}

			off += reclen;
			blkoff += reclen;
		}

		brelse(bp);
	}

done:
	uio->uio_offset = off;
	*ap->a_eofflag = (off >= filesz);
	return (error);
}

int
ext4fs_readlink(void *v)
{
	struct vop_readlink_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	printf("ext4fs_readlink: ino=%u\n", ip->i_number);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int64_t filesz;

	filesz = (u_int64_t)letoh32(din->i_size_lo) |
	    ((u_int64_t)letoh32(din->i_size_hi) << 32);

	/* Fast symlink: target stored inline in i_block[] area */
	if (filesz <= EXT4FS_SYMLINK_LEN_MAX &&
	    !(letoh32(din->i_flags) & EXTFS_INODE_FLAG_EXTENTS)) {
		return (uiomove((char *)din->i_block, filesz, ap->a_uio));
	}

	/* Slow symlink: target stored in data blocks */
	return (VOP_READ(vp, ap->a_uio, 0, ap->a_cred));
}

/*
 * Enter a directory entry for inode ip into directory dvp.
 */
int
ext4fs_direnter(struct inode *ip, struct vnode *dvp,
    struct componentname *cnp)
{
	struct inode *dp = VTOI(dvp);
	struct m_ext4fs *fs = dp->i_e4fs;
	printf("ext4fs_direnter: ino=%u dir_ino=%u name=%.*s\n",
	    ip->i_number, dp->i_number,
	    (int)cnp->cn_namelen, cnp->cn_nameptr);
	struct ext4fs_dinode *ddin = &dp->i_e4din->dinode;
	struct ext4fs_directory *ep, *nep;
	struct buf *bp;
	u_int64_t pblk;
	off_t filesz;
	int entrysize, error, loc;
	u_int16_t reclen, mode;

	entrysize = EXT4FS_DIRSIZ(cnp->cn_namelen);
	mode = letoh16(ip->i_e4din->dinode.i_mode);

	filesz = (off_t)letoh32(ddin->i_size_lo) |
	    ((off_t)letoh32(ddin->i_size_hi) << 32);

	printf("ext4fs_direnter: filesz=%lld i_count=%d i_offset=%lld\n",
	    (long long)filesz, dp->i_count, (long long)dp->i_offset);

	if (dp->i_count == 0) {
		/*
		 * No free slot found - append at end of directory.
		 * Allocate a new block if needed.
		 */
		u_int64_t lbn = EXT4FS_LBLKNO(fs, filesz);
		u_int64_t blkoff = EXT4FS_BLKOFF(fs, filesz);

		if (blkoff == 0) {
			/* Need a new block */
			error = ext4fs_buf_alloc(dp, lbn, fs->m_block_size,
			    cnp->cn_cred, &bp, B_CLRBUF);
			if (error)
				return (error);
		} else {
			error = ext4fs_extent_pblk(dp, lbn, &pblk, NULL);
			if (error)
				return (error);
			error = bread(dp->i_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
			    fs->m_block_size, &bp);
			if (error) {
				brelse(bp);
				return (error);
			}
		}

		/* Write entry at end */
		ep = (struct ext4fs_directory *)
		    ((char *)bp->b_data + blkoff);
		ep->e4d_ino = htole32((u_int32_t)ip->i_number);
		{
			int tail = (fs->m_feature_ro_compat &
			    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM) ?
			    EXT4FS_DIR_TAIL_SIZE : 0;
			if (blkoff == 0)
				ep->e4d_reclen =
				    htole16(fs->m_block_size - tail);
			else
				ep->e4d_reclen =
				    htole16(fs->m_block_size - blkoff - tail);
		}
		ep->e4d_namlen = cnp->cn_namelen;
		ep->e4d_type = ext4fs_mode_to_ft(mode);
		memcpy(ep->e4d_name, cnp->cn_nameptr, cnp->cn_namelen);

		ext4fs_dir_set_csum(fs, dp->i_number,
		    dp->i_e4din->dinode.i_nfs_generation, bp->b_data);
		printf("ext4fs_direnter: new block bwrite\n");
		error = bwrite(bp);
		printf("ext4fs_direnter: new block bwrite done error=%d\n", error);
		if (error)
			return (error);

		/* Update directory size */
		if (blkoff == 0)
			ext4fs_setsize(dp, filesz + fs->m_block_size);
		else
			ext4fs_setsize(dp, filesz + entrysize);
		dp->i_flag |= IN_CHANGE | IN_UPDATE;
		return (ext4fs_update(dp, 1));
	}

	/*
	 * Found a free slot at dp->i_offset with dp->i_count bytes.
	 * Read the block and compact entries to make room.
	 */
	{
		u_int64_t lbn = EXT4FS_LBLKNO(fs, dp->i_offset);

		error = ext4fs_extent_pblk(dp, lbn, &pblk, NULL);
		if (error)
			return (error);

		error = bread(dp->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			return (error);
		}
	}

	loc = EXT4FS_BLKOFF(fs, dp->i_offset);
	ep = (struct ext4fs_directory *)((char *)bp->b_data + loc);
	reclen = letoh16(ep->e4d_reclen);

	if (letoh32(ep->e4d_ino) == 0) {
		/* Unused entry - just overwrite */
		ep->e4d_ino = htole32((u_int32_t)ip->i_number);
		/* Keep reclen as is */
		ep->e4d_namlen = cnp->cn_namelen;
		ep->e4d_type = ext4fs_mode_to_ft(mode);
		memcpy(ep->e4d_name, cnp->cn_nameptr, cnp->cn_namelen);
	} else {
		/* Compact: shrink current entry, add new one after it */
		int oldentsz = EXT4FS_DIRSIZ(ep->e4d_namlen);

		nep = (struct ext4fs_directory *)
		    ((char *)ep + oldentsz);
		nep->e4d_ino = htole32((u_int32_t)ip->i_number);
		nep->e4d_reclen = htole16(reclen - oldentsz);
		nep->e4d_namlen = cnp->cn_namelen;
		nep->e4d_type = ext4fs_mode_to_ft(mode);
		memcpy(nep->e4d_name, cnp->cn_nameptr, cnp->cn_namelen);
		ep->e4d_reclen = htole16(oldentsz);
	}

	ext4fs_dir_set_csum(fs, dp->i_number,
	    dp->i_e4din->dinode.i_nfs_generation, bp->b_data);
	printf("ext4fs_direnter: compact bwrite\n");
	error = bwrite(bp);
	printf("ext4fs_direnter: compact bwrite done error=%d\n", error);
	dp->i_flag |= IN_CHANGE | IN_UPDATE;
	if (error == 0)
		error = ext4fs_update(dp, 1);
	printf("ext4fs_direnter: returning error=%d\n", error);
	return (error);
}

/*
 * Remove a directory entry.
 */
int
ext4fs_dirremove(struct vnode *dvp, struct componentname *cnp)
{
	struct inode *dp = VTOI(dvp);
	struct m_ext4fs *fs = dp->i_e4fs;
	struct ext4fs_directory *ep, *prevep;
	struct buf *bp;
	u_int64_t lbn, pblk;
	int error, loc;

	lbn = EXT4FS_LBLKNO(fs, dp->i_offset);

	error = ext4fs_extent_pblk(dp, lbn, &pblk, NULL);
	if (error)
		return (error);

	error = bread(dp->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
	    fs->m_block_size, &bp);
	if (error) {
		brelse(bp);
		return (error);
	}

	loc = EXT4FS_BLKOFF(fs, dp->i_offset);
	ep = (struct ext4fs_directory *)((char *)bp->b_data + loc);

	if (dp->i_count == 0) {
		/* First entry in block: just zero the inode field */
		ep->e4d_ino = 0;
	} else {
		/* Merge with previous entry */
		int prevloc = EXT4FS_BLKOFF(fs, dp->i_offset - dp->i_count);
		prevep = (struct ext4fs_directory *)
		    ((char *)bp->b_data + prevloc);
		prevep->e4d_reclen = htole16(
		    letoh16(prevep->e4d_reclen) + letoh16(ep->e4d_reclen));
	}

	ext4fs_dir_set_csum(fs, dp->i_number,
	    dp->i_e4din->dinode.i_nfs_generation, bp->b_data);
	error = bwrite(bp);
	dp->i_flag |= IN_CHANGE | IN_UPDATE;
	return (error);
}

/*
 * Check if a directory is empty (contains only "." and "..").
 */
int
ext4fs_dirempty(struct inode *ip, ufsino_t parentino, struct ucred *cred)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_directory *ep;
	struct buf *bp;
	off_t off, filesz;
	u_int64_t lbn, pblk, blkoff;
	u_int16_t reclen;
	int error;

	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	for (off = 0; off < filesz; ) {
		lbn = EXT4FS_LBLKNO(fs, off);

		error = ext4fs_extent_pblk(ip, lbn, &pblk, NULL);
		if (error)
			return (0);

		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			return (0);
		}

		blkoff = EXT4FS_BLKOFF(fs, off);

		while (blkoff < fs->m_block_size && off < filesz) {
			ep = (struct ext4fs_directory *)
			    ((char *)bp->b_data + blkoff);
			reclen = letoh16(ep->e4d_reclen);

			if (reclen == 0) {
				brelse(bp);
				return (0);
			}

			if (letoh32(ep->e4d_ino) != 0) {
				if (ep->e4d_namlen > 2) {
					brelse(bp);
					return (0);
				}
				if (ep->e4d_name[0] != '.') {
					brelse(bp);
					return (0);
				}
				if (ep->e4d_namlen == 1) {
					/* "." - ok */
				} else if (ep->e4d_name[1] == '.') {
					/* ".." - ok */
				} else {
					brelse(bp);
					return (0);
				}
			}

			off += reclen;
			blkoff += reclen;
		}

		brelse(bp);
	}

	return (1);
}

/*
 * Rewrite an existing directory entry to point to a new inode.
 */
int
ext4fs_dirrewrite(struct inode *dp, struct inode *ip,
    struct componentname *cnp)
{
	struct m_ext4fs *fs = dp->i_e4fs;
	struct ext4fs_directory *ep;
	struct buf *bp;
	u_int64_t lbn, pblk;
	u_int16_t mode;
	int error, loc;

	lbn = EXT4FS_LBLKNO(fs, dp->i_offset);

	error = ext4fs_extent_pblk(dp, lbn, &pblk, NULL);
	if (error)
		return (error);

	error = bread(dp->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
	    fs->m_block_size, &bp);
	if (error) {
		brelse(bp);
		return (error);
	}

	loc = EXT4FS_BLKOFF(fs, dp->i_offset);
	ep = (struct ext4fs_directory *)((char *)bp->b_data + loc);
	ep->e4d_ino = htole32((u_int32_t)ip->i_number);
	mode = letoh16(ip->i_e4din->dinode.i_mode);
	ep->e4d_type = ext4fs_mode_to_ft(mode);

	ext4fs_dir_set_csum(fs, dp->i_number,
	    dp->i_e4din->dinode.i_nfs_generation, bp->b_data);
	error = bwrite(bp);
	dp->i_flag |= IN_CHANGE | IN_UPDATE;
	return (error);
}

int
ext4fs_inactive(void *v)
{
	struct vop_inactive_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	u_int16_t mode, nlink;
	int error = 0;
	printf("ext4fs_inactive: ino=%u flags=0x%x\n",
	    ip->i_number, ip->i_flag);
#ifdef DIAGNOSTIC
	extern int prtactive;

	if (prtactive && vp->v_usecount != 0)
		vprint("ext4fs_inactive: pushing active", vp);
#endif

	/*
	 * Ignore inodes related to stale file handles.
	 */
	if (ip->i_e4din == NULL) {
		printf("ext4fs_inactive: ino=%u no din\n", ip->i_number);
		goto out;
	}

	mode = letoh16(ip->i_e4din->dinode.i_mode);
	if (mode == 0) {
		printf("ext4fs_inactive: ino=%u mode=0\n", ip->i_number);
		goto out;
	}

	/*
	 * If the inode was deleted (dtime != 0), skip further processing.
	 */
	if (letoh32(ip->i_e4din->dinode.i_dtime) != 0) {
		printf("ext4fs_inactive: ino=%u already deleted\n", ip->i_number);
		goto out;
	}

	nlink = letoh16(ip->i_e4din->dinode.i_links_count);
	printf("ext4fs_inactive: ino=%u nlink=%u flags=0x%x\n",
	    ip->i_number, nlink, ip->i_flag);

	/*
	 * Handle file deletion: if nlink == 0, truncate data,
	 * free inode, and mark as deleted.
	 */
	if (nlink == 0 && (vp->v_mount->mnt_flag & MNT_RDONLY) == 0) {
		struct timespec ts;

		printf("ext4fs_inactive: ino=%u truncating\n", ip->i_number);
		(void)ext4fs_truncate(ip, 0, 0, NOCRED);
		printf("ext4fs_inactive: ino=%u inode_free\n", ip->i_number);
		ext4fs_inode_free(ip, ip->i_number, mode);

		getnanotime(&ts);
		ip->i_e4din->dinode.i_dtime =
		    htole32((u_int32_t)ts.tv_sec);
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}

	if (ip->i_flag & (IN_ACCESS | IN_CHANGE | IN_MODIFIED | IN_UPDATE)) {
		printf("ext4fs_inactive: ino=%u calling update\n", ip->i_number);
		ext4fs_update(ip, 0);
	}

out:
	printf("ext4fs_inactive: ino=%u unlock\n", ip->i_number);
	VOP_UNLOCK(vp);
	printf("ext4fs_inactive: ino=%u unlock done\n", ip->i_number);

	/*
	 * If we are done with the inode, reclaim it
	 * so that it can be reused immediately.
	 */
	if (ip->i_e4din == NULL || letoh32(ip->i_e4din->dinode.i_dtime) != 0) {
		printf("ext4fs_inactive: ino=%u vrecycle\n", ip->i_number);
		vrecycle(vp, ap->a_p);
		printf("ext4fs_inactive: ino=%u vrecycle done\n", ip->i_number);
	}

	printf("ext4fs_inactive: ino=%u returning\n", ip->i_number);
	return (error);
}

int
ext4fs_reclaim(void *v)
{
	struct vop_reclaim_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	int error;

	printf("ext4fs_reclaim: ino=%u\n", ip->i_number);
	if ((error = ufs_reclaim(vp)) != 0) {
		printf("ext4fs_reclaim: ufs_reclaim error=%d\n", error);
		return (error);
	}
	printf("ext4fs_reclaim: ufs_reclaim done\n");

	if (ip->i_e4din != NULL)
		pool_put(&ext4fs_dinode_pool, ip->i_e4din);

	pool_put(&ext4fs_inode_pool, ip);

	vp->v_data = NULL;

	printf("ext4fs_reclaim: done\n");
	return (0);
}

int
ext4fs_bmap(void *v)
{
	struct vop_bmap_args *ap = v;
	struct inode *ip = VTOI(ap->a_vp);
	struct m_ext4fs *fs = ip->i_e4fs;
	printf("ext4fs_bmap: ino=%u lbn=%lld\n",
	    ip->i_number, (long long)ap->a_bn);
	u_int64_t pblk, ncontig;
	int error;

	if (ap->a_vpp != NULL)
		*ap->a_vpp = ip->i_devvp;
	if (ap->a_bnp == NULL)
		return (0);

	error = ext4fs_extent_pblk(ip, (u_int64_t)ap->a_bn, &pblk, &ncontig);
	if (error) {
		*ap->a_bnp = -1;
		return (error);
	}

	*ap->a_bnp = (daddr_t)EXT4FS_FSBTODB(fs, pblk);

	if (ap->a_runp != NULL) {
		int maxrun = MAXBSIZE / fs->m_block_size - 1;
		*ap->a_runp = MIN((int)(ncontig - 1), maxrun);
		if (*ap->a_runp < 0)
			*ap->a_runp = 0;
	}

	return (0);
}

int
ext4fs_strategy(void *v)
{
	struct vop_strategy_args *ap = v;
	struct buf *bp = ap->a_bp;
	struct vnode *vp = bp->b_vp;
	struct inode *ip;
	int error;
	int s;

	ip = VTOI(vp);
	printf("ext4fs_strategy: ino=%u blkno=%lld\n",
	    ip->i_number, (long long)bp->b_blkno);
	if (vp->v_type == VBLK || vp->v_type == VCHR)
		panic("ext4fs_strategy: spec");

	if (bp->b_blkno == bp->b_lblkno) {
		error = VOP_BMAP(vp, bp->b_lblkno, NULL, &bp->b_blkno,
		    NULL);
		if (error) {
			bp->b_error = error;
			bp->b_flags |= B_ERROR;
			s = splbio();
			biodone(bp);
			splx(s);
			return (error);
		}
		if (bp->b_blkno == -1)
			clrbuf(bp);
	}
	if (bp->b_blkno == -1) {
		s = splbio();
		biodone(bp);
		splx(s);
		return (0);
	}
	vp = ip->i_devvp;
	bp->b_dev = vp->v_rdev;
	VOP_STRATEGY(vp, bp);
	return (0);
}

int
ext4fs_print(void *v)
{
	struct vop_print_args *ap = v;
	struct inode *ip = VTOI(ap->a_vp);

	printf("tag VT_EXT4FS, ino %llu, on dev %d, %d",
	    (unsigned long long)ip->i_number,
	    major(ip->i_dev), minor(ip->i_dev));
	printf(" flags 0x%x, effnlink %d\n",
	    ip->i_flag, ip->i_effnlink);
	return (0);
}

int
ext4fs_pathconf(void *v)
{
	struct vop_pathconf_args *ap = v;
	printf("ext4fs_pathconf: name=%d\n", ap->a_name);

	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = EXT4FS_LINK_MAX;
		break;
	case _PC_NAME_MAX:
		*ap->a_retval = EXT4FS_MAXNAMLEN;
		break;
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		break;
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		break;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		break;
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		break;
	case _PC_TIMESTAMP_RESOLUTION:
		*ap->a_retval = 1;
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

int
ext4fs_advlock(void *v)
{
	struct vop_advlock_args *ap = v;
	struct inode *ip = VTOI(ap->a_vp);
	printf("ext4fs_advlock: ino=%u op=%d\n", ip->i_number, ap->a_op);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	off_t filesz;

	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);
	return (lf_advlock(&ip->i_lockf, filesz, ap->a_id, ap->a_op,
	    ap->a_fl, ap->a_flags));
}
