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
#include <sys/types.h>
#include <sys/mount.h>

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mntopts.h"

void ext4fs_usage(void);

static const struct mntopt mopts[] = {
	MOPT_STDOPTS,
	MOPT_UPDATE,
	{ NULL }
};

int
main(int argc, char *argv[])
{
	struct ufs_args args;		/* XXX ffs_args */
	int ch, mntflags;
	char fs_name[PATH_MAX], *errcause;

	mntflags = 0;
	optind = optreset = 1;		/* Reset for parse of new argv. */
	while ((ch = getopt(argc, argv, "o:")) != -1)
		switch (ch) {
		case 'o':
			getmntopts(optarg, mopts, &mntflags);
			break;
		default:
			ext4fs_usage();
		}
	argc -= optind;
	argv += optind;

	if (argc != 2)
		ext4fs_usage();

	args.fspec = argv[0];		/* The name of the device file. */
	if (realpath(argv[1], fs_name) == NULL)	/* The mount point. */
		err(1, "realpath %s", argv[1]);

	#define DEFAULT_ROOTUID	-2
	args.export_info.ex_root = DEFAULT_ROOTUID;

	if (mntflags & MNT_RDONLY)
		args.export_info.ex_flags = MNT_EXRDONLY;
	else
		args.export_info.ex_flags = 0;
	if (mount(MOUNT_EXT4FS, fs_name, mntflags, &args) == -1) {
		switch (errno) {
		case EMFILE:
			errcause = "mount table full";
			break;
		case EINVAL:
			errcause =
			    "specified device does not match mounted device";
			break;
		case EOPNOTSUPP:
			errcause = "filesystem not supported by kernel";
			break;
		default:
			errcause = strerror(errno);
			break;
		}
		errx(1, "%s on %s: %s", args.fspec, fs_name, errcause);
	}
	exit(0);
}

void
ext4fs_usage(void)
{
	(void)fprintf(stderr,
		"usage: mount_ext4fs [-o options] special node\n");
	exit(1);
}
