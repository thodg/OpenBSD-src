/* kc3
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
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/mbuf.h>
#include <sys/fcntl.h>
#include <sys/disklabel.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/lock.h>
#include <sys/dkio.h>
#include <sys/specdev.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/dir.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ext4fs/ext4fs.h>

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

int ext4fs_init(struct vfsconf *vfsconf)
{
	(void) vfsconf;
	return 0;
}
