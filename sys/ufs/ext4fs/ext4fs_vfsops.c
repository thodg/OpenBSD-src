/* ext4fs
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
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
//#include <sys/socket.h>
#include <sys/mount.h>
//#include <sys/buf.h>
#include <sys/disk.h>
//#include <sys/mbuf.h>
#include <sys/fcntl.h>
//#include <sys/disklabel.h>
//#include <sys/ioctl.h>
#include <sys/errno.h>
#include <sys/malloc.h>
//#include <sys/pool.h>
//#include <sys/lock.h>
#include <sys/dkio.h>
#include <sys/specdev.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>
//#include <ufs/ufs/dir.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ext4fs/ext4fs.h>

#define PRINTF_FEATURES(mask, features)				\
	for (i = 0; i < nitems(features); i++)			\
		if ((mask) & (features)[i].f_mask)		\
			printf("%s ", (features)[i].f_name)

int	ext4fs_block_group_has_super_block(int);
int	ext4fs_mountfs(struct vnode *, struct mount *, struct proc *);
int	ext4fs_sbcheck(struct ext4fs *, int);
void	ext4fs_sbload(struct ext4fs *, struct m_ext4fs *);
int	ext4fs_sbfill(struct vnode *, struct m_ext4fs *);

const struct vfsops ext4fs_vfsops = {
	.vfs_mount	= ext4fs_mount,
	.vfs_start	= ufs_start,
	.vfs_unmount	= ext4fs_unmount,
	.vfs_root	= ufs_root,
	.vfs_quotactl	= ufs_quotactl,
	.vfs_statfs	= ext4fs_statfs,
	.vfs_sync	= ext4fs_sync,
	.vfs_vget	= ext4fs_vget,
	.vfs_fhtovp	= ext4fs_fhtovp,
	.vfs_vptofh	= ext4fs_vptofh,
	.vfs_init	= ext4fs_init,
	.vfs_sysctl	= ext4fs_sysctl,
	.vfs_checkexp	= ufs_check_export,
};

int
ext4fs_block_group_has_super_block(int group)
{
	int a3, a5, a7;

	if (group == 0 || group == 1)
		return 1;
	for (a3 = 3, a5 = 5, a7 = 7;
	    a3 <= group || a5 <= group || a7 <= group;
	    a3 *= 3, a5 *= 5, a7 *= 7)
		if (group == a3 || group == a5 || group == a7)
			return 1;
	return 0;
}

int
ext4fs_fhtovp(struct mount *mp, struct fid *fhp, struct vnode **vpp)
{
	(void)mp;
	(void)fhp;
	(void)vpp;
	printf("ext4fs_fhtovp: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_init(struct vfsconf *vfsp)
{
	(void)vfsp;
	printf("ext4fs_init: OK\n");
	return (0);
}

int
ext4fs_mount(struct mount *mp, const char *path, void *data,
	struct nameidata *ndp, struct proc *p)
{
	struct ufs_args *args;
	struct vnode *devvp;
	int error;
	struct m_ext4fs *mfs;
	char fname[MNAMELEN];
	char fspec[MNAMELEN];
	struct ufsmount *ump = NULL;
	args = data;
	error = copyinstr(args->fspec, fspec, sizeof(fspec), NULL);
	if (error)
		goto error;

	if (disk_map(fspec, fname, MNAMELEN, DM_OPENBLCK) == -1)
		memcpy(fname, fspec, sizeof(fname));

	NDINIT(ndp, LOOKUP, FOLLOW, UIO_SYSSPACE, fname, p);
	if ((error = namei(ndp)) != 0)
		goto error;
	devvp = ndp->ni_vp;

	if (devvp->v_type != VBLK) {
		error = ENOTBLK;
		goto error_devvp;
	}
	if (major(devvp->v_rdev) >= nblkdev) {
		error = ENXIO;
		goto error_devvp;
	}
	if ((mp->mnt_flag & MNT_UPDATE) == 0)
		error = ext4fs_mountfs(devvp, mp, p);
	else {
		if (devvp != ump->um_devvp)
			error = EINVAL;	/* XXX needs translation */
		else
			vrele(devvp);
	}
	if (error)
		goto error_devvp;
	ump = VFSTOUFS(mp);
	mfs = ump->um_e4fs;

	goto success;

error_devvp:
	/* Error with devvp held. */
	vrele(devvp);

error:
	/* Error with no state to backout. */

success:
	return (error);
}

/*
 * Common code for mount and mountroot
 */
int
ext4fs_mountfs(struct vnode *devvp, struct mount *mp, struct proc *p)
{
	struct ufsmount *ump;
	struct buf *bp;
	struct ext4fs *sble;
	struct m_ext4fs *mfs;
	dev_t dev;
	int error, ronly;
	struct ucred *cred;

	dev = devvp->v_rdev;
	cred = p ? p->p_ucred : NOCRED;
	/*
	 * Disallow multiple mounts of the same device.
	 * Disallow mounting of a device that is currently in use
	 * (except for root, which might share swap device for miniroot).
	 * Flush out any old buffers remaining from a previous use.
	 */
	if ((error = vfs_mountedon(devvp)) != 0)
		return (error);
	if (vcount(devvp) > 1 && devvp != rootvp)
		return (EBUSY);
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	error = vinvalbuf(devvp, V_SAVE, cred, p, 0, INFSLP);
	VOP_UNLOCK(devvp);
	if (error != 0)
		return (error);

	ronly = (mp->mnt_flag & MNT_RDONLY) != 0;
	error = VOP_OPEN(devvp, ronly ? FREAD : FREAD|FWRITE, FSCRED, p);
	if (error)
		return (error);

	bp = NULL;
	ump = NULL;

	/*
	 * Read the superblock from disk.
	 */
	error = bread(devvp, (daddr_t)(EXT4FS_SUPER_BLOCK_OFFSET /
				       DEV_BSIZE),
		      EXT4FS_SUPER_BLOCK_SIZE, &bp);
	if (error)
		goto out;
	sble = (struct ext4fs *)bp->b_data;
	error = ext4fs_sbcheck(sble, ronly);
	if (error)
		goto out;

	ump = malloc(sizeof *ump, M_UFSMNT, M_WAITOK | M_ZERO);
	mfs = ump->um_e4fs = malloc(sizeof(struct m_ext4fs), M_UFSMNT,
	    M_WAITOK | M_ZERO);

	/*
	 * Copy in the superblock, compute in-memory values
	 * and load group descriptors.
	 */
	ext4fs_sbload(sble, mfs);
	if ((error = ext4fs_sbfill(devvp, mfs)) != 0)
		goto out;
	brelse(bp);
	bp = NULL;
	sble = &mfs->m_sble;
	ump->um_e4fs->m_read_only = ronly;
	ump->um_fstype = UM_EXT4FS;

	if (ronly == 0) {
		if (mfs->m_state == EXT4FS_STATE_VALID)
			mfs->m_state = 0;
		else
			mfs->m_state = EXT4FS_STATE_ERROR;
		mfs->m_fs_was_modified = 1;
	}

	mp->mnt_data = ump;
	mp->mnt_stat.f_fsid.val[0] = (long)dev;
	mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
	mp->mnt_stat.f_namemax = MAXNAMLEN;
	mp->mnt_flag |= MNT_LOCAL;
	ump->um_mountp = mp;
	ump->um_dev = dev;
	ump->um_devvp = devvp;
	ump->um_nindir = EXT4FS_NINDIR(mfs);
	ump->um_bptrtodb = mfs->m_fs_block_to_disk_block;
	ump->um_seqinc = 1; /* no frags */
	ump->um_maxsymlinklen = EXT4FS_SYMLINK_LEN_MAX;
	devvp->v_specmountpoint = mp;
	return (0);
out:
	if (devvp->v_specinfo)
		devvp->v_specmountpoint = NULL;
	if (bp)
		brelse(bp);
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	(void)VOP_CLOSE(devvp, ronly ? FREAD : FREAD|FWRITE, cred, p);
	VOP_UNLOCK(devvp);
	if (ump) {
		free(mfs, M_UFSMNT, sizeof *mfs);
		free(ump, M_UFSMNT, sizeof *ump);
		mp->mnt_data = NULL;
	}
	return (error);
}

int
ext4fs_sbcheck(struct ext4fs *sble, int ronly)
{
	u_int32_t mask, tmp;
	int i;

	tmp = letoh16(sble->sb_magic);
	if (tmp != EXT4FS_MAGIC) {
		printf("ext2fs: wrong magic number 0x%x\n", tmp);
		return (EIO);		/* XXX needs translation */
	}

	tmp = letoh32(sble->sb_log_block_size);
	if (tmp > 2) {
		/* skewed log(block size): 1024 -> 0 | 2048 -> 1 | 4096 -> 2 */
		tmp += 10;
		printf("ext2fs: wrong log2(block size) %d\n", tmp);
		return (EIO);	   /* XXX needs translation */
	}

	if (sble->sb_blocks_per_group == 0) {
		printf("ext2fs: zero blocks per group\n");
		return (EIO);
	}

	tmp = letoh32(sble->sb_revision_level);
	if (tmp != EXT4FS_REV_DYNAMIC) {
		printf("ext2fs: wrong revision number 0x%x\n", tmp);
		return (EIO);		/* XXX needs translation */
	}
	/*
	else if (tmp == E2FS_REV0)
		return (0);
	*/

	tmp = letoh32(sble->sb_first_non_reserved_inode);
	if (tmp != EXT4FS_INODE_FIRST) {
		printf("ext4fs: first inode at 0x%x\n", tmp);
		return (EINVAL);      /* XXX needs translation */
	}

	tmp = letoh32(sble->sb_block_group_descriptor_size);
	if (tmp != sizeof(struct ext4fs_block_group_descriptor)) {
		printf("ext4fs: block group descriptor size is 0x%x\n",
		       tmp);
		return (EINVAL);
	}

	tmp = letoh32(sble->sb_feature_incompat);
	mask = tmp & ~EXT4FS_FEATURE_INCOMPAT_SUPPORTED;
	if (mask) {
		printf("ext4fs: unsupported incompat features: ");
		PRINTF_FEATURES(mask, ext4fs_feature_incompat);
		printf("\n");
		return (EINVAL);      /* XXX needs translation */
	}

	if (!ronly && (tmp & EXT4FS_FEATURE_RO_COMPAT_SUPPORTED)) {
		printf("ext4fs: only read-only support right now\n");
		return (EROFS);      /* XXX needs translation */
	}

	if (tmp & EXT4FS_FEATURE_INCOMPAT_RECOVER) {
		printf("ext4fs: your file system says it needs"
		       " recovery\n");
		if (!ronly)
			return (EROFS);	/* XXX needs translation */
	}

	tmp = letoh32(sble->sb_feature_ro_compat) &
		~EXT4FS_FEATURE_RO_COMPAT_SUPPORTED;
	if (!ronly && tmp) {
		printf("ext4fs: unsupported R/O compat features: ");
		PRINTF_FEATURES(tmp, ext4fs_feature_ro_compat);
		printf("\n");
		return (EROFS);      /* XXX needs translation */
	}

	return (0);
}

int
ext4fs_sbfill(struct vnode *devvp, struct m_ext4fs *mfs)
{
	(void)devvp;

	mfs->m_block_group_count = howmany(mfs->m_blocks_count -
					   mfs->m_first_data_block,
					   mfs->m_blocks_per_group);

	mfs->m_block_size_shift = EXT4FS_LOG_MIN_BLOCK_SIZE +
		mfs->m_log_block_size;
	mfs->m_block_size = 1 << mfs->m_block_size_shift;
	mfs->m_block_group_descriptor_blocks_count =
		howmany(mfs->m_block_group_count,
			mfs->m_block_size /
			sizeof(struct ext4fs_block_group_descriptor));
	mfs->m_fs_block_to_disk_block = mfs->m_log_block_size + 1;
	mfs->m_inodes_per_block = mfs->m_block_size / mfs->m_inode_size;
	mfs->m_inode_table_blocks_per_group = mfs->m_inodes_per_group /
		mfs->m_inodes_per_block;

	printf("ext4fs_sbfill: OK\n");
	return (0);
}

void
ext4fs_sbload(struct ext4fs *sble, struct m_ext4fs *dest)
{
	int feature_incompat_64bit;
	feature_incompat_64bit = letoh32(sble->sb_feature_incompat) &
		EXT4FS_FEATURE_INCOMPAT_64BIT;
	dest->m_inodes_count = letoh32(sble->sb_inodes_count);
	dest->m_blocks_count = letoh32(sble->sb_blocks_count_lo);
	dest->m_reserved_blocks_count =
		letoh32(sble->sb_reserved_blocks_count_lo);
	dest->m_free_blocks_count =
		letoh32(sble->sb_free_blocks_count_lo);
	dest->m_free_inodes_count = letoh32(sble->sb_free_inodes_count);
	dest->m_first_data_block = letoh32(sble->sb_first_data_block);
	dest->m_log_block_size = letoh32(sble->sb_log_block_size);
	dest->m_log_cluster_size = letoh32(sble->sb_log_cluster_size);
	dest->m_blocks_per_group = letoh32(sble->sb_blocks_per_group);
	dest->m_clusters_per_group = letoh32(sble->sb_clusters_per_group);
	dest->m_inodes_per_group = letoh32(sble->sb_inodes_per_group);
	dest->m_mount_time = letoh32(sble->sb_mount_time_lo);
	dest->m_write_time = letoh32(sble->sb_write_time_lo);
	dest->m_mount_count = letoh16(sble->sb_mount_count);
	dest->m_max_mount_count_before_fsck = (int16_t)letoh16(sble->sb_max_mount_count_before_fsck);
	dest->m_state = letoh16(sble->sb_state);
	dest->m_errors = letoh16(sble->sb_errors);
	dest->m_revision_level_minor = letoh16(sble->sb_revision_level_minor);
	dest->m_check_time = letoh32(sble->sb_check_time_lo);
	dest->m_check_interval = letoh32(sble->sb_check_interval);
	dest->m_creator_os = letoh32(sble->sb_creator_os);
	dest->m_revision_level = letoh32(sble->sb_revision_level);
	dest->m_default_reserved_uid = letoh16(sble->sb_default_reserved_uid);
	dest->m_default_reserved_gid = letoh16(sble->sb_default_reserved_gid);
	dest->m_first_non_reserved_inode = letoh32(sble->sb_first_non_reserved_inode);
	dest->m_inode_size = letoh16(sble->sb_inode_size);
	dest->m_block_group_id = letoh16(sble->sb_block_group_id);
	dest->m_feature_compat = letoh32(sble->sb_feature_compat);
	dest->m_feature_incompat = letoh32(sble->sb_feature_incompat);
	dest->m_feature_ro_compat = letoh32(sble->sb_feature_ro_compat);
	dest->m_algorithm_usage_bitmap = letoh32(sble->sb_algorithm_usage_bitmap);
	dest->m_reserved_bgdt_blocks = letoh16(sble->sb_reserved_bgdt_blocks);
	dest->m_journal_inode_number = letoh32(sble->sb_journal_inode_number);
	dest->m_journal_device_number = letoh32(sble->sb_journal_device_number);
	dest->m_last_orphan = letoh32(sble->sb_last_orphan);
	dest->m_block_group_descriptor_size = letoh16(sble->sb_block_group_descriptor_size);
	dest->m_default_mount_opts = letoh32(sble->sb_default_mount_opts);
	dest->m_first_meta_block_group = letoh32(sble->sb_first_meta_block_group);
	dest->m_newfs_time = letoh32(sble->sb_newfs_time_lo);
	if (letoh32(sble->sb_feature_incompat) & EXT4FS_FEATURE_INCOMPAT_64BIT)
	dest->m_inode_size_extra_min = letoh16(sble->sb_inode_size_extra_min);
	dest->m_inode_size_extra_want = letoh16(sble->sb_inode_size_extra_want);
	dest->m_flags = letoh32(sble->sb_flags);
	dest->m_raid_stride_block_count = letoh16(sble->sb_raid_stride_block_count);
	dest->m_mmp_interval = letoh16(sble->sb_mmp_interval);
	dest->m_mmp_block = letoh64(sble->sb_mmp_block);
	dest->m_raid_stripe_width_block_count = letoh32(sble->sb_raid_stripe_width_block_count);
	dest->m_kilobytes_written = letoh64(sble->sb_kilobytes_written);
	dest->m_error_count = letoh32(sble->sb_error_count);
	dest->m_first_error_time = letoh32(sble->sb_first_error_time_lo);
	dest->m_first_error_inode = letoh32(sble->sb_first_error_inode);
	dest->m_first_error_block = letoh64(sble->sb_first_error_block);
	dest->m_first_error_line = letoh32(sble->sb_first_error_line);
	dest->m_last_error_time = letoh32(sble->sb_last_error_time_lo);
	dest->m_last_error_inode = letoh32(sble->sb_last_error_inode);
	dest->m_last_error_line = letoh32(sble->sb_last_error_line);
	dest->m_last_error_block = letoh64(sble->sb_last_error_block);
	dest->m_user_quota_inode = letoh32(sble->sb_user_quota_inode);
	dest->m_group_quota_inode = letoh32(sble->sb_group_quota_inode);
	dest->m_overhead_clusters = letoh32(sble->sb_overhead_clusters);
	dest->m_backup_block_groups[0] = letoh32(sble->sb_backup_block_groups[0]);
	dest->m_backup_block_groups[1] = letoh32(sble->sb_backup_block_groups[1]);
	dest->m_lost_and_found_inode = letoh32(sble->sb_lost_and_found_inode);
	dest->m_project_quota_inode = letoh32(sble->sb_project_quota_inode);
	dest->m_checksum_seed = letoh32(sble->sb_checksum_seed);
	dest->m_encoding = letoh16(sble->sb_encoding);
	dest->m_encoding_flags = letoh16(sble->sb_encoding_flags);
	dest->m_orphan_file_inode = letoh16(sble->sb_orphan_file_inode);
	if (feature_incompat_64bit) {
		dest->m_blocks_count |= (u_int64_t)
			letoh32(sble->sb_blocks_count_hi) << 32;
		dest->m_reserved_blocks_count |=	(u_int64_t)
			letoh32(sble->sb_reserved_blocks_count_hi)
			<< 32;
		dest->m_free_blocks_count |= (u_int64_t)
			letoh32(sble->sb_free_blocks_count_hi) << 32;
		dest->m_mount_time |= (u_int64_t)
			letoh32(sble->sb_mount_time_hi) << 32;
		dest->m_check_time |= (u_int64_t)
			letoh32(sble->sb_check_time_hi) << 32;
		dest->m_newfs_time |= (u_int64_t)
			letoh32(sble->sb_newfs_time_hi) << 32;
		dest->m_first_error_time |= (u_int64_t)
			letoh32(sble->sb_first_error_time_hi) << 32;
		dest->m_last_error_time |= (u_int64_t)
			letoh32(sble->sb_last_error_time_hi) << 32;
	}
}

int
ext4fs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p)
{
	struct ufsmount *ump;
	struct m_ext4fs *mfs;
	const u_int32_t overhead_per_group_block_bitmap = 1;
	const u_int32_t overhead_per_group_inode_bitmap = 1;
	u_int32_t overhead, overhead_per_group;
	int ngroups;

	(void)p;
	ump = VFSTOUFS(mp);
	mfs = ump->um_e4fs;

	overhead_per_group = overhead_per_group_block_bitmap +
		overhead_per_group_inode_bitmap +
		mfs->m_inode_table_blocks_per_group;
	overhead = mfs->m_first_data_block +
		mfs->m_block_group_count * overhead_per_group;
	if (mfs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER) {
		int i;
		for (i = 0, ngroups = 0; i < mfs->m_block_group_count; i++) {
			if (ext4fs_block_group_has_super_block(i))
				ngroups++;
		}
	} else {
		ngroups = mfs->m_block_group_count;
	}
	overhead += ngroups *
		(1 + mfs->m_block_group_descriptor_blocks_count);

	sbp->f_bsize = mfs->m_block_size;
	sbp->f_iosize = mfs->m_block_size;
	sbp->f_blocks = mfs->m_blocks_count - overhead;
	sbp->f_bfree = mfs->m_free_blocks_count;
	sbp->f_bavail = sbp->f_bfree - mfs->m_reserved_blocks_count;
	sbp->f_files = mfs->m_inodes_count;
	sbp->f_favail = sbp->f_ffree = mfs->m_free_inodes_count;
	copy_statfs_info(sbp, mp);

	return (0);
}

int
ext4fs_sync(struct mount *mp, int waitfor, int stall, struct ucred *cred, struct proc *p)
{
	(void)mp;
	(void)waitfor;
	(void)stall;
	(void)cred;
	(void)p;
	printf("ext4fs_sync: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen, struct proc *p)
{
	(void)name;
	(void)namelen;
	(void)oldp;
	(void)oldlenp;
	(void)newp;
	(void)newlen;
	(void)p;
	printf("ext4fs_sysctl: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_unmount(struct mount *mp, int mntflags, struct proc *p)
{
	(void)mp;
	(void)mntflags;
	(void)p;
	printf("ext4fs_unmount: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_vget(struct mount *mp, ino_t ino, struct vnode **vpp)
{
	(void)mp;
	(void)ino;
	(void)vpp;
	printf("ext4fs_vget: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_vptofh(struct vnode *vp, struct fid *fhp)
{
	(void)vp;
	(void)fhp;
	printf("ext4fs_vptofh: not implemented\n");
	return (EOPNOTSUPP);
}
