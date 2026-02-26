/* ext4fs_vnops.c
 * Copyright 2025 kmx.io <contact@kmx.io>
 *
 * Permission is hereby granted to use this software granted the above
 * copyright notice and this permission paragraph are included in all
 * copies and substantial portions of this software.
 *
 * THIS SOFTWARE IS PROVIDED "AS-IS" WITHOUT ANY GUARANTEE OF
 * PURPOSE AND PERFORMANCE. IN NO EVENT WHATSOEVER SHALL THE
 * AUTHOR BE CONSIDERED LIABLE FOR THE USE AND PERFORMANCE OF
 * THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
//#include <sys/resourcevar.h>
//#include <sys/kernel.h>
//#include <sys/file.h>
//#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
//#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/dirent.h>
//#include <sys/fcntl.h>
//#include <sys/lockf.h>
//#include <sys/uio.h>
//#include <sys/unistd.h>

//#include <miscfs/specfs/specdev.h>
//#include <miscfs/fifofs/fifo.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/dir.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ext4fs/ext4fs.h>

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

/* Stub implementations - remaining ops return EOPNOTSUPP */

int ext4fs_lookup(void *);
int ext4fs_create(void *);
int ext4fs_mknod(void *);
int ext4fs_open(void *);
int ext4fs_access(void *);
int ext4fs_getattr(void *);
int ext4fs_setattr(void *);
int ext4fs_read(void *);
int ext4fs_write(void *);
int ext4fs_fsync(void *);
int ext4fs_remove(void *);
int ext4fs_link(void *);
int ext4fs_rename(void *);
int ext4fs_mkdir(void *);
int ext4fs_rmdir(void *);
int ext4fs_symlink(void *);
int ext4fs_readdir(void *);
int ext4fs_readlink(void *);
int ext4fs_inactive(void *);
int ext4fs_reclaim(void *);
int ext4fs_bmap(void *);
int ext4fs_strategy(void *);
int ext4fs_print(void *);
int ext4fs_pathconf(void *);
int ext4fs_advlock(void *);

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

	*vpp = NULL;

	/* Check accessibility of directory */
	if ((error = VOP_ACCESS(vdp, VEXEC, cnp->cn_cred, cnp->cn_proc)) != 0)
		return (error);

	if ((flags & ISLASTCN) && (vdp->v_mount->mnt_flag & MNT_RDONLY) &&
	    (nameiop == DELETE || nameiop == RENAME))
		return (EROFS);

	/* Check the name cache */
	if ((error = cache_lookup(vdp, vpp, cnp)) >= 0)
		return (error);

	/* Search directory for the name */
	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

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

			if (letoh32(ep->e4d_ino) != 0 &&
			    ep->e4d_namlen == cnp->cn_namelen &&
			    memcmp(cnp->cn_nameptr, ep->e4d_name,
			    cnp->cn_namelen) == 0) {
				foundino = letoh32(ep->e4d_ino);
				dp->i_ino = foundino;
				dp->i_reclen = reclen;
				dp->i_offset = off;
				brelse(bp);
				goto found;
			}

			off += reclen;
			blkoff += reclen;
		}

		brelse(bp);
	}

	/* Not found */
	if ((nameiop == CREATE || nameiop == RENAME) && (flags & ISLASTCN)) {
		if (vdp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if ((error = VOP_ACCESS(vdp, VWRITE, cnp->cn_cred,
		    cnp->cn_proc)) != 0)
			return (error);
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
	/*
	 * Found the entry. Handle ".", "..", and normal names
	 * following the same locking protocol as ext2fs.
	 */

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
		vref(vdp);
		*vpp = vdp;
	} else {
		/* Normal entry */
		error = VFS_VGET(vdp->v_mount, foundino, &tdp);
		if (error)
			return (error);
		if (!lockparent || !(flags & ISLASTCN)) {
			VOP_UNLOCK(vdp);
			cnp->cn_flags |= PDIRUNLOCK;
		}
		*vpp = tdp;
	}

	/* Cache the result */
	if (cnp->cn_flags & MAKEENTRY)
		cache_enter(vdp, *vpp, cnp);
	return (0);
}

int
ext4fs_create(void *v)
{
	(void)v;
	printf("ext4fs_create: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_mknod(void *v)
{
	(void)v;
	printf("ext4fs_mknod: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_open(void *v)
{
	(void)v;
	return (0);
}

int
ext4fs_access(void *v)
{
	struct vop_access_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
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
ext4fs_setattr(void *v)
{
	(void)v;
	printf("ext4fs_setattr: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_read(void *v)
{
	struct vop_read_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
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
	(void)v;
	printf("ext4fs_write: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_fsync(void *v)
{
	struct vop_fsync_args *ap = v;
	struct vnode *vp = ap->a_vp;

	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		return (0);

	vflushbuf(vp, ap->a_waitfor == MNT_WAIT);
	return (0);
}

int
ext4fs_remove(void *v)
{
	(void)v;
	printf("ext4fs_remove: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_link(void *v)
{
	(void)v;
	printf("ext4fs_link: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_rename(void *v)
{
	(void)v;
	printf("ext4fs_rename: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_mkdir(void *v)
{
	(void)v;
	printf("ext4fs_mkdir: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_rmdir(void *v)
{
	(void)v;
	printf("ext4fs_rmdir: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_symlink(void *v)
{
	(void)v;
	printf("ext4fs_symlink: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	struct uio *uio = ap->a_uio;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
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

int
ext4fs_inactive(void *v)
{
	struct vop_inactive_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	u_int16_t mode, nlink;
	int error = 0;
#ifdef DIAGNOSTIC
	extern int prtactive;

	if (prtactive && vp->v_usecount != 0)
		vprint("ext4fs_inactive: pushing active", vp);
#endif

	/*
	 * Ignore inodes related to stale file handles.
	 */
	if (ip->i_e4din == NULL)
		goto out;

	mode = letoh16(ip->i_e4din->dinode.i_mode);
	if (mode == 0)
		goto out;

	/*
	 * If the inode was deleted (dtime != 0), skip further processing.
	 */
	if (letoh32(ip->i_e4din->dinode.i_dtime) != 0)
		goto out;

	nlink = letoh16(ip->i_e4din->dinode.i_links_count);

	/*
	 * Handle file deletion: if nlink == 0, truncate and free the inode.
	 */
	if (nlink == 0 && (vp->v_mount->mnt_flag & MNT_RDONLY) == 0) {
		printf("ext4fs_inactive: inode %llu has nlink=0, needs deletion\n",
		    (unsigned long long)ip->i_number);

		/* TODO: implement truncate and inode freeing
		 * For now, just set dtime to mark it as deleted
		 */
		printf("ext4fs_inactive: WARNING - truncate/free not implemented yet\n");

		/* Mark inode as deleted by setting dtime */
		/* This would require write support:
		struct timespec ts;
		getnanotime(&ts);
		ip->i_e4din->dinode.i_dtime = htole32(ts.tv_sec);
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		*/
	}

	/*
	 * Update inode if any flags are set.
	 * TODO: implement ext4fs_update when write support is added
	 */
	if (ip->i_flag & (IN_ACCESS | IN_CHANGE | IN_MODIFIED | IN_UPDATE)) {
		printf("ext4fs_inactive: inode %llu needs update (flags=0x%x)\n",
		    (unsigned long long)ip->i_number, ip->i_flag);
		/* ext4fs_update(ip, 0); */
	}

out:
	VOP_UNLOCK(vp);

	/*
	 * If we are done with the inode, reclaim it
	 * so that it can be reused immediately.
	 */
	if (ip->i_e4din == NULL || letoh32(ip->i_e4din->dinode.i_dtime) != 0)
		vrecycle(vp, ap->a_p);

	return (error);
}

int
ext4fs_reclaim(void *v)
{
	struct vop_reclaim_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	int error;

	if ((error = ufs_reclaim(vp)) != 0)
		return (error);

	if (ip->i_e4din != NULL)
		pool_put(&ext4fs_dinode_pool, ip->i_e4din);

	pool_put(&ext4fs_inode_pool, ip);

	vp->v_data = NULL;

	return (0);
}

int
ext4fs_bmap(void *v)
{
	(void)v;
	printf("ext4fs_bmap: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_strategy(void *v)
{
	(void)v;
	printf("ext4fs_strategy: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_print(void *v)
{
	(void)v;
	printf("ext4fs_print: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_pathconf(void *v)
{
	(void)v;
	printf("ext4fs_pathconf: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_advlock(void *v)
{
	(void)v;
	printf("ext4fs_advlock: not implemented\n");
	return (EOPNOTSUPP);
}
