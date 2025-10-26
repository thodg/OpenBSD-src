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
//#include <sys/namei.h>
//#include <sys/resourcevar.h>
//#include <sys/kernel.h>
//#include <sys/file.h>
//#include <sys/stat.h>
//#include <sys/buf.h>
//#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
//#include <sys/malloc.h>
#include <sys/pool.h>
//#include <sys/dirent.h>
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

/* Stub implementations - all return EOPNOTSUPP for now */

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
	(void)v;
	printf("ext4fs_lookup: not implemented\n");
	return (EOPNOTSUPP);
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
	printf("ext4fs_open: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_access(void *v)
{
	(void)v;
	printf("ext4fs_access: not implemented\n");
	return (EOPNOTSUPP);
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
	(void)v;
	printf("ext4fs_read: not implemented\n");
	return (EOPNOTSUPP);
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
	(void)v;
	printf("ext4fs_readdir: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_readlink(void *v)
{
	(void)v;
	printf("ext4fs_readlink: not implemented\n");
	return (EOPNOTSUPP);
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

	printf("ext4fs_inactive: entry, vp=%p, ino=%llu, refcnt=%d\n",
	    vp, (unsigned long long)ip->i_number, vp->v_usecount);

	/*
	 * DIAGNOSTIC: vop_inactive should be called with v_usecount == 1
	 * (the last reference). If it's 0, something went wrong with
	 * reference counting.
	 */
	if (vp->v_usecount == 0) {
		printf("ext4fs_inactive: WARNING - vp->v_usecount is 0 for ino=%llu\n",
		    (unsigned long long)ip->i_number);
		printf("ext4fs_inactive: This indicates a reference counting bug!\n");
	}

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
	if (ip->i_e4din == NULL || letoh32(ip->i_e4din->dinode.i_dtime) != 0) {
		printf("ext4fs_inactive: vrecycle inode %llu\n",
		    (unsigned long long)ip->i_number);
		vrecycle(vp, ap->a_p);
	}

	printf("ext4fs_inactive: exit, ino=%llu\n", (unsigned long long)ip->i_number);
	return (error);
}

int
ext4fs_reclaim(void *v)
{
	struct vop_reclaim_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	int error;

	printf("ext4fs_reclaim: entry, vp=%p, ino=%llu, refcnt=%d\n",
	    vp, (unsigned long long)ip->i_number, vp->v_usecount);

	if ((error = ufs_reclaim(vp)) != 0) {
		printf("ext4fs_reclaim: ufs_reclaim failed with error %d\n", error);
		return (error);
	}

	if (ip->i_e4din != NULL)
		pool_put(&ext4fs_dinode_pool, ip->i_e4din);

	pool_put(&ext4fs_inode_pool, ip);

	vp->v_data = NULL;

	printf("ext4fs_reclaim: exit, ino=%llu\n", (unsigned long long)ip->i_number);
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
