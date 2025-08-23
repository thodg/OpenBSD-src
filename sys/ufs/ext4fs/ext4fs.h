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
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/ucred.h>

struct fid;
struct nameidata;
struct statfs;
struct vfsconf;

#define EXT4FS_EXTENT_DEPTH_MAX		5
#define EXT4FS_FUNCTION_MAX		32
#define EXT4FS_REV_EXT2			0
#define EXT4FS_REV_DYNAMIC		1
#define EXT4FS_REV_MINOR		0
#define EXT4FS_LAST_MOUNTED_MAX		64
#define EXT4FS_LOG_MIN_BLOCK_SIZE	10
#define EXT4FS_MAGIC			0xEF53
#define EXT4FS_MOUNT_OPTS_MAX		64
#define EXT4FS_SUPER_BLOCK_OFFSET	1024
#define EXT4FS_SUPER_BLOCK_SIZE		1024
#define EXT4FS_VOLUME_NAME_MAX		16

#define	EXT4FS_DIRECT_ADDR_IN_INODE	12
#define	EXT4FS_INDIRECT_ADDR_IN_INODE	3
#define EXT4FS_SYMLINK_LEN_MAX \
	((EXT4FS_DIRECT_ADDR_IN_INODE +				\
	  EXT4FS_INDIRECT_ADDR_IN_INODE) * sizeof(u_int32_t))

#define	EXT4FS_NINDIR(fs)	((fs)->m_block_size / sizeof(u_int32_t))

#define EXT4FS_CHECKSUM_TYPE_NONE	0x0000
#define EXT4FS_CHECKSUM_TYPE_CRC32C	0x0001

#define EXT4FS_ENCODING_NONE	0x0000	// legacy behavior
#define EXT4FS_ENCODING_UTF8	0x0001	// UTF-8, Unicode 12.1.0

#define EXT4FS_ENCODING_FLAG_NONE	 0x0000
#define EXT4FS_ENCODING_FLAG_STRICT_MODE 0x0001 // Reject invalid encoding

#define EXT4FS_ERRORS_CONTINUE	1	// Log and keep going
#define EXT4FS_ERRORS_RO	2	// Remount read-only
#define EXT4FS_ERRORS_PANIC	3	// Kernel panic

#define EXT4FS_FEATURE_COMPAT_DIR_PREALLOC	0x0001
#define EXT4FS_FEATURE_COMPAT_IMAGIC_INODES	0x0002
#define EXT4FS_FEATURE_COMPAT_HAS_JOURNAL	0x0004
#define EXT4FS_FEATURE_COMPAT_EXT_ATTR		0x0008
#define EXT4FS_FEATURE_COMPAT_RESIZE_INODE	0x0010
#define EXT4FS_FEATURE_COMPAT_DIR_INDEX		0x0020

#define EXT4FS_FEATURE_INCOMPAT_COMPRESSION	0x00001
#define EXT4FS_FEATURE_INCOMPAT_FILETYPE	0x00002
#define EXT4FS_FEATURE_INCOMPAT_RECOVER		0x00004
#define EXT4FS_FEATURE_INCOMPAT_JOURNAL_DEV	0x00008
#define EXT4FS_FEATURE_INCOMPAT_META_BG		0x00010
#define EXT4FS_FEATURE_INCOMPAT_EXTENTS		0x00040
#define EXT4FS_FEATURE_INCOMPAT_64BIT		0x00080
#define EXT4FS_FEATURE_INCOMPAT_MMP		0x00100
#define EXT4FS_FEATURE_INCOMPAT_FLEX_BG		0x00200
#define EXT4FS_FEATURE_INCOMPAT_EA_INODE	0x00400
#define EXT4FS_FEATURE_INCOMPAT_DIRDATA		0x01000
#define EXT4FS_FEATURE_INCOMPAT_CSUM_SEED	0x02000
#define EXT4FS_FEATURE_INCOMPAT_LARGEDIR	0x04000
#define EXT4FS_FEATURE_INCOMPAT_INLINE_DATA	0x08000
#define EXT4FS_FEATURE_INCOMPAT_ENCRYPT		0x10000

#define EXT4FS_FEATURE_INCOMPAT_SUPPORTED	\
	(EXT4FS_FEATURE_INCOMPAT_FILETYPE |	\
	 EXT4FS_FEATURE_INCOMPAT_EXTENTS |	\
	 EXT4FS_FEATURE_INCOMPAT_64BIT |	\
	 EXT4FS_FEATURE_INCOMPAT_FLEX_BG)

#define EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER   0x0001
#define EXT4FS_FEATURE_RO_COMPAT_LARGE_FILE     0x0002
#define EXT4FS_FEATURE_RO_COMPAT_BTREE_DIR      0x0004
#define EXT4FS_FEATURE_RO_COMPAT_HUGE_FILE      0x0008
#define EXT4FS_FEATURE_RO_COMPAT_GDT_CSUM       0x0010
#define EXT4FS_FEATURE_RO_COMPAT_DIR_NLINK      0x0020
#define EXT4FS_FEATURE_RO_COMPAT_EXTRA_ISIZE    0x0040
#define EXT4FS_FEATURE_RO_COMPAT_HAS_SNAPSHOT   0x0080
#define EXT4FS_FEATURE_RO_COMPAT_QUOTA          0x0100
#define EXT4FS_FEATURE_RO_COMPAT_BIGALLOC       0x0200
#define EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM  0x0400
#define EXT4FS_FEATURE_RO_COMPAT_REPLICA        0x0800
#define EXT4FS_FEATURE_RO_COMPAT_READONLY       0x1000
#define EXT4FS_FEATURE_RO_COMPAT_PROJECT        0x2000

#define EXT4FS_FEATURE_RO_COMPAT_SUPPORTED		\
	(EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER |	\
	 EXT4FS_FEATURE_RO_COMPAT_LARGE_FILE |		\
	 EXT4FS_FEATURE_RO_COMPAT_HUGE_FILE |		\
	 EXT4FS_FEATURE_RO_COMPAT_DIR_NLINK |		\
	 EXT4FS_FEATURE_RO_COMPAT_EXTRA_ISIZE |		\
	 EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM)

#define EXT4FS_FLAG_SIGNED_HASH		0x0001
#define EXT4FS_FLAG_UNSIGNED_HASH	0x0002
#define EXT4FS_FLAG_TEST_FILESYS	0x0004
#define EXT4FS_FLAG_64BIT		0x0008
#define EXT4FS_FLAG_MOUNT_OPT_CHECK	0x0010

#define EXT4FS_INODE_BAD_BLOCKS		1
#define EXT4FS_INODE_ROOT_DIR		2
#define EXT4FS_INODE_USER_QUOTA		3
#define EXT4FS_INODE_GROUP_QUOTA	4
#define EXT4FS_INODE_BOOT_LOADER	5
#define EXT4FS_INODE_JOURNAL		8
#define EXT4FS_INODE_FIRST		11

#define EXTFS_INODE_FLAG_SECURE_RM			0x00000001
#define EXTFS_INODE_FLAG_UN_RM				0x00000002
#define EXTFS_INODE_FLAG_COMPRESSION			0x00000004
#define EXTFS_INODE_FLAG_SYNC				0x00000008
#define EXTFS_INODE_FLAG_IMMUTABLE			0x00000010
#define EXTFS_INODE_FLAG_APPEND				0x00000020
#define EXTFS_INODE_FLAG_NO_DUMP			0x00000040
#define EXTFS_INODE_FLAG_NO_ATIME			0x00000080
#define EXTFS_INODE_FLAG_DIRTY				0x00000100
#define EXTFS_INODE_FLAG_COMPRESSED_BLOCKS		0x00000200
#define EXTFS_INODE_FLAG_NO_COMPRESSION			0x00000400
#define EXTFS_INODE_FLAG_ENCRYPTED			0x00000800
#define EXTFS_INODE_FLAG_INDEX				0x00001000
#define EXTFS_INODE_FLAG_IMAGIC				0x00002000
#define EXTFS_INODE_FLAG_JOURNAL_DATA			0x00004000
#define EXTFS_INODE_FLAG_NO_TAIL			0x00008000
#define EXTFS_INODE_FLAG_DIR_SYNC			0x00010000
#define EXTFS_INODE_FLAG_TOP_DIR			0x00020000
#define EXTFS_INODE_FLAG_HUGE_FILE			0x00040000
#define EXTFS_INODE_FLAG_EXTENTS			0x00080000
#define EXTFS_INODE_FLAG_EXTENDED_ATTRIBUTES_INODE	0x00200000
#define EXTFS_INODE_FLAG_EOF_BLOCKS			0x00400000
#define EXTFS_INODE_FLAG_INLINE_DATA			0x10000000
#define EXTFS_INODE_FLAG_PROJECT_ID_INHERITANCE		0x20000000
#define EXTFS_INODE_FLAG_CASEFOLD			0x40000000

#define EXT4FS_MOUNT_READONLY			0x0001
#define EXT4FS_MOUNT_NO_ATIME			0x0002
#define EXT4FS_MOUNT_DIRSYNC			0x0004
#define EXT4FS_MOUNT_DATA_JOURNAL		0x0008
#define EXT4FS_MOUNT_DATA_ORDERED		0x0010
#define EXT4FS_MOUNT_DATA_WRITEBACK		0x0020
#define EXT4FS_MOUNT_ERRORS_CONTINUE		0x0040
#define EXT4FS_MOUNT_ERRORS_REMOUNT_RO		0x0080
#define EXT4FS_MOUNT_ERRORS_PANIC		0x0100
#define EXT4FS_MOUNT_DISCARD			0x0200
#define EXT4FS_MOUNT_NO_BUFFER_HEADS		0x0400
#define EXT4FS_MOUNT_SKIP_JOURNAL		0x0800
#define EXT4FS_MOUNT_NOAUTO_DELAYED_ALLOC	0x1000

#define EXT4FS_OS_LINUX		0
#define EXT4FS_OS_HURD		1
#define EXT4FS_OS_MASIX		2
#define EXT4FS_OS_FREEBSD	3
#define EXT4FS_OS_LITES		4
#define EXT4FS_OS_OPENBSD	5

#define EXT4FS_STATE_VALID	0x0001  // Clean unmount
#define EXT4FS_STATE_ERROR	0x0002  // Errors detected (fsck needed)

#define EXT4FS_BGD_FLAG_INODE_UNINIT	0x0001
#define EXT4FS_BGD_FLAG_BLOCK_UNINIT	0x0002
#define EXT4FS_BGD_FLAG_INODE_ZEROED	0x0004
#define EXT4FS_BGD_FLAG_DIRTY		0x0008
#define EXT4FS_BGD_FLAG_BLOCK_ZEROED	0x0010
#define EXT4FS_BGD_FLAG_READ_ONLY	0x0020

struct ext4fs {
	u_int32_t	sb_inodes_count;
	u_int32_t	sb_blocks_count_lo;
	u_int32_t	sb_reserved_blocks_count_lo;
	u_int32_t	sb_free_blocks_count_lo;
	// 0x10
	u_int32_t	sb_free_inodes_count;
	u_int32_t	sb_first_data_block;
	u_int32_t	sb_log_block_size;	// log2(block size) - 10
	u_int32_t	sb_log_cluster_size;	// log2(cluster size) - 10
	// 0x20
	u_int32_t	sb_blocks_per_group;
	u_int32_t	sb_clusters_per_group;
	u_int32_t	sb_inodes_per_group;
	u_int32_t	sb_mount_time_lo;
	// 0x30
	u_int32_t	sb_write_time_lo;
	u_int16_t	sb_mount_count;
	int16_t		sb_max_mount_count_before_fsck;
	u_int16_t	sb_magic;
	u_int16_t	sb_state;		// EXT4FS_STATE_*
	u_int16_t	sb_errors;		// EXT4FS_ERRORS_*
	u_int16_t	sb_revision_level_minor;
	// 0x40
	u_int32_t	sb_check_time_lo;
	u_int32_t	sb_check_interval;
	u_int32_t	sb_creator_os;		// EXT4FS_OS_*
	u_int32_t	sb_revision_level;
	// 0x50
	u_int16_t	sb_default_reserved_uid;
	u_int16_t	sb_default_reserved_gid;
	u_int32_t	sb_first_non_reserved_inode;
	u_int16_t	sb_inode_size;
	u_int16_t	sb_block_group_id;
	u_int32_t	sb_feature_compat;
	// 0x60
	u_int32_t	sb_feature_incompat;
	u_int32_t	sb_feature_ro_compat;
	u_int8_t	sb_uuid[16];
	char		sb_volume_name[EXT4FS_VOLUME_NAME_MAX];
	char		sb_last_mounted[EXT4FS_LAST_MOUNTED_MAX];
	u_int32_t	sb_algorithm_usage_bitmap;
	u_int8_t	sb_preallocate_blocks;
	u_int8_t	sb_preallocate_dir_blocks;
	u_int16_t	sb_reserved_bgdt_blocks;
	// 0xD0
	u_int8_t	sb_journal_uuid[16];     // UUID of journal superblock
	// 0xE0
	u_int32_t	sb_journal_inode_number;
	u_int32_t	sb_journal_device_number;
	u_int32_t	sb_last_orphan;
	u_int32_t	sb_hash_seed[4];
	u_int8_t	sb_default_hash_version;
	u_int8_t	sb_journal_backup_type;
	u_int16_t	sb_block_group_descriptor_size;
	// 0x100
	u_int32_t	sb_default_mount_opts;
	u_int32_t	sb_first_meta_block_group;
	u_int32_t	sb_newfs_time_lo;
	u_int32_t	sb_jnl_blocks[17];       // Backup of journal inode
	// 0x150
	u_int32_t	sb_blocks_count_hi;
	u_int32_t	sb_reserved_blocks_count_hi;
	u_int32_t	sb_free_blocks_count_hi;
	u_int16_t	sb_inode_size_extra_min;
	u_int16_t	sb_inode_size_extra_want;
	// 0x160
	u_int32_t	sb_flags;
	u_int16_t	sb_raid_stride_block_count;
	u_int16_t	sb_mmp_interval;
	u_int64_t	sb_mmp_block;
	// 0x170
	u_int32_t	sb_raid_stripe_width_block_count;
	u_int8_t	sb_log_groups_per_flex;
	u_int8_t	sb_checksum_type;
	u_int16_t	sb_reserved_176;
	u_int64_t	sb_kilobytes_written;
	// 0x180
	u_int32_t	sb_ext3_snapshot_inode;
	u_int32_t	sb_ext3_snapshot_id;
	u_int64_t	sb_ext3_snapshot_reserved_blocks_count;
	// 0x190
	u_int32_t	sb_ext3_snapshot_list;
	u_int32_t	sb_error_count;
	u_int32_t	sb_first_error_time_lo;
	u_int32_t	sb_first_error_inode;
	// 0x1A0
	u_int64_t	sb_first_error_block;
	char		sb_first_error_function[EXT4FS_FUNCTION_MAX];
	u_int32_t	sb_first_error_line;
	u_int32_t	sb_last_error_time_lo;
	// 0x1D0
	u_int32_t	sb_last_error_inode;
	u_int32_t	sb_last_error_line;
	u_int64_t	sb_last_error_block;
	// 0x1E0
	char		sb_last_error_function[EXT4FS_FUNCTION_MAX];
	// 0x200
	char		sb_mount_opts[EXT4FS_MOUNT_OPTS_MAX];
	// 0x240
	u_int32_t	sb_user_quota_inode;
	u_int32_t	sb_group_quota_inode;
	u_int32_t	sb_overhead_clusters;
	u_int32_t	sb_backup_block_groups[2];
	u_int8_t	sb_encrypt_algos[4];
	u_int8_t	sb_encrypt_pw_salt[16];
	u_int32_t	sb_lost_and_found_inode;
	u_int32_t	sb_project_quota_inode;
	// 0x270
	u_int32_t	sb_checksum_seed;
	u_int8_t	sb_write_time_hi;
	u_int8_t	sb_mount_time_hi;
	u_int8_t	sb_newfs_time_hi;
	u_int8_t	sb_check_time_hi;
	u_int8_t	sb_first_error_time_hi;
	u_int8_t	sb_last_error_time_hi;
	u_int8_t	sb_first_error_code;
	u_int8_t	sb_last_error_code;
	u_int16_t	sb_encoding;
	u_int16_t	sb_encoding_flags;
	// 0x280
	u_int16_t	sb_orphan_file_inode;
	u_int16_t	sb_reserved_284;
	u_int32_t	sb_reserved_288[94];
	u_int32_t	sb_checksum;
} __attribute__((packed));

struct m_ext4fs {
	/* little-endian super-block */
	struct ext4fs	m_sble;
	/* computed from little-endian super-block */
	u_int32_t	m_inodes_count;
	u_int64_t	m_blocks_count;
	u_int64_t	m_reserved_blocks_count;
	u_int64_t	m_free_blocks_count;
	u_int32_t	m_free_inodes_count;
	u_int32_t	m_first_data_block;
	u_int32_t	m_log_block_size;       // log2(block size) - 10
	u_int32_t	m_log_cluster_size;     // log2(cluster size) - 10
	u_int32_t	m_blocks_per_group;
	u_int32_t	m_clusters_per_group;
	u_int32_t	m_inodes_per_group;
	u_int64_t	m_mount_time;
	u_int32_t	m_write_time;
	u_int16_t	m_mount_count;
	int16_t		m_max_mount_count_before_fsck;
	u_int16_t	m_state;                // EXT4FS_STATE_*
	u_int16_t	m_errors;               // EXT4FS_ERRORS_*
	u_int16_t	m_revision_level_minor;
	u_int64_t	m_check_time;
	u_int32_t	m_check_interval;
	u_int32_t	m_creator_os;           // EXT4FS_OS_*
	u_int32_t	m_revision_level;
	u_int16_t	m_default_reserved_uid;
	u_int16_t	m_default_reserved_gid;
	u_int32_t	m_first_non_reserved_inode;
	u_int16_t	m_inode_size;
	u_int16_t	m_block_group_id;
	u_int32_t	m_feature_compat;
	u_int32_t	m_feature_incompat;
	u_int32_t	m_feature_ro_compat;
	u_int32_t	m_algorithm_usage_bitmap;
	u_int16_t	m_reserved_bgdt_blocks;
	u_int32_t	m_journal_inode_number;
	u_int32_t	m_journal_device_number;
	u_int32_t	m_last_orphan;
	u_int16_t	m_block_group_descriptor_size;
	u_int32_t	m_default_mount_opts;
	u_int32_t	m_first_meta_block_group;
	u_int64_t	m_newfs_time;
	u_int16_t	m_inode_size_extra_min;
	u_int16_t	m_inode_size_extra_want;
	u_int32_t	m_flags;
	u_int16_t	m_raid_stride_block_count;
	u_int16_t	m_mmp_interval;
	u_int64_t	m_mmp_block;
	u_int32_t	m_raid_stripe_width_block_count;
	u_int64_t	m_kilobytes_written;
	u_int32_t	m_error_count;
	u_int64_t	m_first_error_time;
	u_int32_t	m_first_error_inode;
	u_int64_t	m_first_error_block;
	u_int32_t	m_first_error_line;
	u_int64_t	m_last_error_time;
	u_int32_t	m_last_error_inode;
	u_int32_t	m_last_error_line;
	u_int64_t	m_last_error_block;
	u_int32_t	m_user_quota_inode;
	u_int32_t	m_group_quota_inode;
	u_int32_t	m_overhead_clusters;
	u_int32_t	m_backup_block_groups[2];
	u_int32_t	m_lost_and_found_inode;
	u_int32_t	m_project_quota_inode;
	u_int32_t	m_checksum_seed;
	u_int16_t	m_encoding;
	u_int16_t	m_encoding_flags;
	u_int16_t	m_orphan_file_inode;
	int		m_read_only;
	int		m_fs_was_modified;
	/* computed by ext4fs_sbfill */
	u_int64_t       m_block_group_descriptor_blocks_count;
	u_int64_t	m_block_group_count;
	u_int64_t	m_block_size;
	u_int64_t	m_block_size_shift;
	u_int32_t	m_fs_block_to_disk_block;
	u_int32_t	m_inodes_per_block;
	u_int32_t	m_inode_table_blocks_per_group;
};

struct ext4fs_block_group_descriptor {
  u_int32_t bgd_block_bitmap_block_lo;
  u_int32_t bgd_inode_bitmap_block_lo;
  u_int32_t bgd_inode_table_block_lo;
  u_int16_t bgd_free_blocks_count_lo;
  u_int16_t bgd_free_inodes_count_lo;
  // 0x10
  u_int16_t bgd_used_dirs_count_lo;
  u_int16_t bgd_flags;
  u_int32_t bgd_exclude_bitmap_block_lo;
  u_int16_t bgd_block_bitmap_checksum_lo;
  u_int16_t bgd_inode_bitmap_checksum_lo;
  u_int16_t bgd_inode_table_unused_lo;
  u_int16_t bgd_checksum;
  // 0x20
  u_int32_t bgd_block_bitmap_block_hi;
  u_int32_t bgd_inode_bitmap_block_hi;
  u_int32_t bgd_inode_table_block_hi;
  u_int16_t bgd_free_blocks_count_hi;
  u_int16_t bgd_free_inodes_count_hi;
  // 0x30
  u_int16_t bgd_used_dirs_count_hi;
  u_int16_t bgd_inode_table_unused_hi;
  u_int32_t bgd_exclude_bitmap_block_hi;
  u_int16_t bgd_block_bitmap_checksum_hi;
  u_int16_t bgd_inode_bitmap_checksum_hi;
  u_int32_t bgd_reserved_3c;
  // 0x40
} __attribute__((packed));

#define EXT4FS_EXTENT_HEADER_MAGIC  0xF30A

struct ext4fs_extent_header {
  u_int16_t eh_magic;
  u_int16_t eh_entries;
  u_int16_t eh_max;
  u_int16_t eh_depth;
  u_int32_t eh_generation;
} __attribute__((packed));

struct ext4fs_extent {
  u_int32_t e_block;
  u_int16_t e_len;
  u_int16_t e_start_hi;
  u_int32_t e_start_lo;
} __attribute__((packed));

struct ext4fs_extent_idx {
  u_int32_t ei_block;
  u_int32_t ei_leaf_lo;
  u_int16_t ei_leaf_hi;
  u_int16_t ei_unused;
} __attribute__((packed));

struct ext4fs_dinode {
  u_int16_t i_mode;
  u_int16_t i_uid_lo;
  u_int32_t i_size_lo;
  u_int32_t i_atime;
  u_int32_t i_ctime;
  // 0x10
  u_int32_t i_mtime;
  u_int32_t i_dtime;
  u_int16_t i_gid_lo;
  u_int16_t i_links_count;
  u_int32_t i_blocks_lo;
  // 0x20
  u_int32_t i_flags;
  u_int32_t i_version;
  union {
    u_int32_t i_block[15];
    struct {
      struct ext4fs_extent_header i_extent_header;
      union {
        struct ext4fs_extent i_extent[4];
        struct ext4fs_extent_idx i_extent_idx[4];
      };
    };
  };
  u_int32_t i_nfs_generation;
  u_int32_t i_extended_attributes_lo;
  u_int32_t i_size_hi;
  // 0x70
  u_int32_t i_fragment_address;
  u_int16_t i_blocks_hi;
  u_int16_t i_extended_attributes_hi;
  u_int16_t i_uid_hi;
  u_int16_t i_gid_hi;
  u_int16_t i_checksum_lo;
  u_int16_t i_reserved_7e;
  // 0x80
  u_int16_t i_extra_isize;
  u_int16_t i_checksum_hi;
  u_int32_t i_ctime_extra;
  u_int32_t i_mtime_extra;
  u_int32_t i_atime_extra;
  // 0x90
  u_int32_t i_crtime;
  u_int32_t i_crtime_extra;
  u_int32_t i_version_hi;
  u_int32_t i_project_id;
  // 0xA0
} __attribute__((packed));

struct ext4fs_dinode_256 {
  struct ext4fs_dinode dinode;
  u_int8_t extended_attributes[256 - sizeof(struct ext4fs_dinode)];
};

struct ext4fs_feature {
	int		f_mask;
	const char *	f_name;
};

static const struct ext4fs_feature ext4fs_feature_incompat[] = {
  {EXT4FS_FEATURE_INCOMPAT_COMPRESSION, "compression"},
  {EXT4FS_FEATURE_INCOMPAT_FILETYPE,    "filetype"},
  {EXT4FS_FEATURE_INCOMPAT_RECOVER,     "recover"},
  {EXT4FS_FEATURE_INCOMPAT_JOURNAL_DEV, "journal_dev"},
  {EXT4FS_FEATURE_INCOMPAT_META_BG,     "meta_bg"},
  {EXT4FS_FEATURE_INCOMPAT_EXTENTS,     "extents"},
  {EXT4FS_FEATURE_INCOMPAT_64BIT,       "64bit"},
  {EXT4FS_FEATURE_INCOMPAT_MMP,         "mmp"},
  {EXT4FS_FEATURE_INCOMPAT_FLEX_BG,     "flex_bg"},
  {EXT4FS_FEATURE_INCOMPAT_EA_INODE,    "ea_inode"},
  {EXT4FS_FEATURE_INCOMPAT_DIRDATA,     "dirdata"},
  {EXT4FS_FEATURE_INCOMPAT_CSUM_SEED,   "csum_seed"},
  {EXT4FS_FEATURE_INCOMPAT_LARGEDIR,    "largedir"},
  {EXT4FS_FEATURE_INCOMPAT_INLINE_DATA, "inline_data"},
  {EXT4FS_FEATURE_INCOMPAT_ENCRYPT,     "encrypt"},
};

static const struct ext4fs_feature ext4fs_feature_ro_compat[] = {
  {EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER,  "sparse-super"},
  {EXT4FS_FEATURE_RO_COMPAT_LARGE_FILE,    "large-file"},
  {EXT4FS_FEATURE_RO_COMPAT_BTREE_DIR,     "btree-dir"},
  {EXT4FS_FEATURE_RO_COMPAT_HUGE_FILE,     "huge-file"},
  {EXT4FS_FEATURE_RO_COMPAT_GDT_CSUM,      "gdt-csum"},
  {EXT4FS_FEATURE_RO_COMPAT_DIR_NLINK,     "dir-nlink"},
  {EXT4FS_FEATURE_RO_COMPAT_EXTRA_ISIZE,   "extra-isize"},
  {EXT4FS_FEATURE_RO_COMPAT_HAS_SNAPSHOT,  "has-snapshot"},
  {EXT4FS_FEATURE_RO_COMPAT_QUOTA,         "quota"},
  {EXT4FS_FEATURE_RO_COMPAT_BIGALLOC,      "bigalloc"},
  {EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM, "metadata-csum"},
  {EXT4FS_FEATURE_RO_COMPAT_REPLICA,       "replica"},
  {EXT4FS_FEATURE_RO_COMPAT_READONLY,      "readonly"},
  {EXT4FS_FEATURE_RO_COMPAT_PROJECT,       "project"},
};

/* VFS operations */
int ext4fs_fhtovp(struct mount *, struct fid *, struct vnode **);
int ext4fs_init(struct vfsconf *);
int ext4fs_mount(struct mount *, const char *, void *,
	struct nameidata *, struct proc *);
int ext4fs_statfs(struct mount *, struct statfs *, struct proc *);
int ext4fs_sync(struct mount *, int, int, struct ucred *,
	struct proc *);
int ext4fs_sysctl(int *, u_int, void *, size_t *, void *, size_t,
	struct proc *);
int ext4fs_unmount(struct mount *, int, struct proc *);
int ext4fs_vget(struct mount *, ino_t, struct vnode **);
int ext4fs_vptofh(struct vnode *, struct fid *);

/* VNode operations */

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
