/* vi: set sw=4 ts=4: */
/*
 * Mini rpm applet for busybox
 *
 * Copyright (C) 2001,2002 by Laurence Anderson
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
//config:config RPM
//config:	bool "rpm (32 kb)"
//config:	default y
//config:	help
//config:	Mini RPM applet - queries and extracts RPM packages.

//applet:IF_RPM(APPLET(rpm, BB_DIR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_RPM) += rpm.o

#include "libbb.h"
#include "common_bufsiz.h"
#include "bb_archive.h"
#include "rpm.h"

#include <getopt.h>

#include <linux/fs.h>
#include <sys/ioctl.h>

#define RPM_CHAR_TYPE           1
#define RPM_INT8_TYPE           2
#define RPM_INT16_TYPE          3
#define RPM_INT32_TYPE          4
/* #define RPM_INT64_TYPE       5   ---- These aren't supported (yet) */
#define RPM_STRING_TYPE         6
#define RPM_BIN_TYPE            7
#define RPM_STRING_ARRAY_TYPE   8
#define RPM_I18NSTRING_TYPE     9

#define TAG_NAME                1000
#define TAG_VERSION             1001
#define TAG_RELEASE             1002
#define TAG_SUMMARY             1004
#define TAG_DESCRIPTION         1005
#define TAG_BUILDTIME           1006
#define TAG_BUILDHOST           1007
#define TAG_SIZE                1009
#define TAG_VENDOR              1011
#define TAG_LICENSE             1014
#define TAG_PACKAGER            1015
#define TAG_GROUP               1016
#define TAG_URL                 1020
#define TAG_ARCH                1022
#define TAG_PREIN               1023
#define TAG_POSTIN              1024
#define TAG_FILESIZES           1028
#define TAG_FILEMODES           1030
#define TAG_FILELINKTOS         1036
#define TAG_FILEFLAGS           1037
#define TAG_FILEUSERNAME        1039
#define TAG_FILEGROUPNAME       1040
#define TAG_SOURCERPM           1044
#define TAG_PREINPROG           1085
#define TAG_POSTINPROG          1086
#define TAG_FILEINODES          1096
#define TAG_PREFIXS             1098
#define TAG_DIRINDEXES          1116
#define TAG_BASENAMES           1117
#define TAG_DIRNAMES            1118
#define TAG_PAYLOADCOMPRESSOR   1125
#define TAG_FILENLINKS          5045


#define RPMFILE_CONFIG          (1 << 0)
#define RPMFILE_DOC             (1 << 1)
#define RPMFILE_GHOST           (1 << 6)

#define HEADER_DIR "/usr/lib/sysimage/rpm-headers"

enum rpm_functions_e {
	rpm_query = 1,
	rpm_install = 2,
	rpm_query_info = 4,
	rpm_query_package = 8,
	rpm_query_list = 16,
	rpm_query_list_doc = 32,
	rpm_query_list_config = 64,
	rpm_query_all = 128,
	rpm_install_reflink = 256,
};

typedef struct {
	uint32_t tag; /* 4 byte tag */
	uint32_t type; /* 4 byte type */
	uint32_t offset; /* 4 byte offset */
	uint32_t count; /* 4 byte count */
} rpm_index;

struct globals {
	void *map;
	rpm_index *mytags;
	int tagcount;
	unsigned mapsize;
	const char* install_root;
	char* header_dir;
	char* only_prefix;
	int force : 1;
	IF_VARIABLE_ARCH_PAGESIZE(unsigned pagesize;)
#define G_pagesize cached_pagesize(G.pagesize)
} FIX_ALIASING;
#define G (*(struct globals*)bb_common_bufsiz1)
#define INIT_G() do { setup_common_bufsiz(); } while (0)

static int rpm_gettags(const char *filename)
{
	rpm_index *tags;
	int fd;
	unsigned pass, idx;
	unsigned storepos;

	if (!filename) { /* rpm2cpio w/o filename? */
		filename = bb_msg_standard_output;
		fd = 0;
	} else {
		fd = xopen(filename, O_RDONLY);
	}

	storepos = xlseek(fd, 96, SEEK_CUR); /* Seek past the unused lead */
	G.tagcount = 0;
	tags = NULL;
	idx = 0;
	/* 1st pass is the signature headers, 2nd is the main stuff */
	for (pass = 0; pass < 2; pass++) {
		struct rpm_header header;
		unsigned cnt;

		xread(fd, &header, sizeof(header));
		if (header.magic_and_ver != htonl(RPM_HEADER_MAGICnVER))
			bb_error_msg_and_die("invalid RPM header magic in '%s'", filename);
		header.size = ntohl(header.size);
		cnt = ntohl(header.entries);
		storepos += sizeof(header) + cnt * 16;

		G.tagcount += cnt;
		tags = xrealloc(tags, sizeof(tags[0]) * G.tagcount);
		xread(fd, &tags[idx], sizeof(tags[0]) * cnt);
		while (cnt--) {
			rpm_index *tag = &tags[idx];
			tag->tag = ntohl(tag->tag);
			tag->type = ntohl(tag->type);
			tag->count = ntohl(tag->count);
			tag->offset = storepos + ntohl(tag->offset);
			if (pass == 0)
				tag->tag -= 743;
			idx++;
		}
		/* Skip padding to 8 byte boundary after reading signature headers */
		if (pass == 0)
			while (header.size & 7)
				header.size++;
		/* Seek past store */
		storepos = xlseek(fd, header.size, SEEK_CUR);
	}
	G.mytags = tags;

	/* Map the store */
	storepos = (storepos + G_pagesize) & -(int)G_pagesize;
	/* remember size for munmap */
	G.mapsize = storepos;
	/* some NOMMU systems prefer MAP_PRIVATE over MAP_SHARED */
	G.map = mmap_read(fd, storepos);
	if (G.map == MAP_FAILED)
		bb_perror_msg_and_die("mmap '%s'", filename);

	return fd;
}

static int bsearch_rpmtag(const void *key, const void *item)
{
	int *tag = (int *)key;
	rpm_index *tmp = (rpm_index *) item;
	return (*tag - tmp->tag);
}

static char *rpm_getstr(int tag, int itemindex)
{
	rpm_index *found;
	found = bsearch(&tag, G.mytags, G.tagcount, sizeof(G.mytags[0]), bsearch_rpmtag);
	if (!found || itemindex >= found->count)
		return NULL;
	if (found->type == RPM_STRING_TYPE
	 || found->type == RPM_I18NSTRING_TYPE
	 || found->type == RPM_STRING_ARRAY_TYPE
	) {
		int n;
		char *tmpstr = (char *) G.map + found->offset;
		for (n = 0; n < itemindex; n++)
			tmpstr = tmpstr + strlen(tmpstr) + 1;
		return tmpstr;
	}
	return NULL;
}
static char *rpm_getstr0(int tag)
{
	return rpm_getstr(tag, 0);
}

#if ENABLE_RPM

static int rpm_getint(int tag, int itemindex)
{
	rpm_index *found;
	char *tmpint;

	/* gcc throws warnings here when sizeof(void*)!=sizeof(int) ...
	 * it's ok to ignore it because tag won't be used as a pointer */
	found = bsearch(&tag, G.mytags, G.tagcount, sizeof(G.mytags[0]), bsearch_rpmtag);
	if (!found || itemindex >= found->count)
		return -1;

	tmpint = (char *) G.map + found->offset;
	if (found->type == RPM_INT32_TYPE) {
		tmpint += itemindex*4;
		return ntohl(*(int32_t*)tmpint);
	}
	if (found->type == RPM_INT16_TYPE) {
		tmpint += itemindex*2;
		return ntohs(*(int16_t*)tmpint);
	}
	if (found->type == RPM_INT8_TYPE) {
		tmpint += itemindex;
		return *(int8_t*)tmpint;
	}
	return -1;
}

static int rpm_getcount(int tag)
{
	rpm_index *found;
	found = bsearch(&tag, G.mytags, G.tagcount, sizeof(G.mytags[0]), bsearch_rpmtag);
	if (!found)
		return 0;
	return found->count;
}

static void fileaction_dobackup(char *filename, int fileref)
{
	struct stat oldfile;
	int stat_res;
	char *newname;
	if (rpm_getint(TAG_FILEFLAGS, fileref) & RPMFILE_CONFIG) {
		/* Only need to backup config files */
		stat_res = lstat(filename, &oldfile);
		if (stat_res == 0 && S_ISREG(oldfile.st_mode)) {
			/* File already exists  - really should check MD5's etc to see if different */
			newname = xasprintf("%s.rpmorig", filename);
			copy_file(filename, newname, FILEUTILS_RECUR | FILEUTILS_PRESERVE_STATUS);
			remove_file(filename, FILEUTILS_RECUR | FILEUTILS_FORCE);
			free(newname);
		}
	}
}

static void fileaction_setowngrp(char *filename, int fileref)
{
	/* real rpm warns: "user foo does not exist - using <you>" */
	struct passwd *pw = getpwnam(rpm_getstr(TAG_FILEUSERNAME, fileref));
	int uid = pw ? pw->pw_uid : getuid(); /* or euid? */
	struct group *gr = getgrnam(rpm_getstr(TAG_FILEGROUPNAME, fileref));
	int gid = gr ? gr->gr_gid : getgid();
	chown(filename, uid, gid);
}

static void loop_through_files(int filetag, void (*fileaction)(char *filename, int fileref))
{
	int count = 0;
	while (rpm_getstr(filetag, count)) {
		char* filename = xasprintf("%s%s",
			rpm_getstr(TAG_DIRNAMES, rpm_getint(TAG_DIRINDEXES, count)),
			rpm_getstr(TAG_BASENAMES, count));
		fileaction(filename, count++);
		free(filename);
	}
}

#if 0 //DEBUG
static void print_all_tags(void)
{
	unsigned i = 0;
	while (i < G.tagcount) {
		rpm_index *tag = &G.mytags[i];
		if (tag->type == RPM_STRING_TYPE
		 || tag->type == RPM_I18NSTRING_TYPE
		 || tag->type == RPM_STRING_ARRAY_TYPE
		) {
			unsigned n;
			char *str = (char *) G.map + tag->offset;

			printf("tag[%u] %08x type %08x offset %08x count %d '%s'\n",
				i, tag->tag, tag->type, tag->offset, tag->count, str
			);
			for (n = 1; n < tag->count; n++) {
				str += strlen(str) + 1;
				printf("\t'%s'\n", str);
			}
		}
		i++;
	}
}
#else
#define print_all_tags() ((void)0)
#endif

// XXX: sysconf(_SC_PAGESIZE)
#define PAGE_SIZE 4096

#define bit(x, n) (x[n>>3]&(1<<(n&7)))
#define set_bit(x, n) (x[n>>3]|=(1<<(n&7)))

typedef struct { int inode; int fi; } ifi_t;

/* sort by inode. lowest file index first */
static int compare_inodes(const void* a, const void* b)
{
	if (((const ifi_t*)a)->inode == ((const ifi_t*)b)->inode)
		return ((const ifi_t*)a)->fi > ((const ifi_t*)b)->fi ? 1 : -1;

	if (((const ifi_t*)a)->inode > ((const ifi_t*)b)->inode)
		return 1;

	return -1;
}

static search_inode(const void* a, const void* b)
{
	int key = *(int*)a;
	const ifi_t* item = b;
	if (key == item->inode)
		return 0;
	if (key > item->inode)
		return 1;
	return -1;
}

static void create_clone_from(const char* path, unsigned mode, int rpmfd, off_t off, size_t size)
{
		unsigned pad = (size & (PAGE_SIZE-1)) ? PAGE_SIZE - (size & (PAGE_SIZE-1)) : 0;
		//printf("%s off %lu, size %u + %u = %u\n", path, off, size, pad, size+pad);
		int fd = xopen3(path, O_WRONLY|O_CREAT|(G.force?O_TRUNC:O_EXCL), mode&07777);
		if (fd == -1)
			bb_perror_msg_and_die("failed to open %s", path);
		struct file_clone_range range = {
			.src_fd = rpmfd,
			.src_offset = off,
			.src_length = size + pad,
			.dest_offset = 0
		};
		int ret = ioctl(fd, FICLONERANGE, &range);
		if (ret) {
			unlink(path);
			bb_perror_msg_and_die("can't clone into %s", path);
		}
		ret = ftruncate(fd, size);
		if (ret) {
			unlink(path);
			bb_perror_msg_and_die("can't fix size of %s", path);
		}
		xclose(fd);
}

static void reflink_package(int rpmfd)
{
	int nfiles = rpm_getcount(TAG_BASENAMES);
	ifi_t* inodes = xmalloc(sizeof(ifi_t) * nfiles);
	for (int i = 0; i < nfiles; ++i) {
		inodes[i].fi = i;
		inodes[i].inode = rpm_getint(TAG_FILEINODES, i);
	}
	qsort(inodes, nfiles, sizeof(ifi_t), compare_inodes);
	int ninodes = 0;
	for (int i = 1; i < nfiles; ++i) {
		if (inodes[ninodes].inode == inodes[i].inode) {
#if 0
			printf("skip hardlink %s%s inode %d fi %d\n",
					rpm_getstr(TAG_DIRNAMES, rpm_getint(TAG_DIRINDEXES, inodes[i].fi)),
					rpm_getstr(TAG_BASENAMES, inodes[i].fi), inodes[i].inode, inodes[i].fi);
#endif
			continue;
		}

		++ninodes;
		inodes[ninodes].fi = inodes[i].fi;
		inodes[ninodes].inode = inodes[i].inode;
	}
	++ninodes;

	off_t off = xlseek(rpmfd, 0, SEEK_CUR);
	if (off & (PAGE_SIZE-1)) {
		off += PAGE_SIZE - (off & (PAGE_SIZE-1));
		xlseek(rpmfd, off, SEEK_SET);
	}

	mode_t org_mask = umask(022);
	for (int i = 0; i < nfiles; ++i) {
		int skip = 0;
		char* d = rpm_getstr(TAG_DIRNAMES, rpm_getint(TAG_DIRINDEXES, i));
		char* n = rpm_getstr(TAG_BASENAMES, i);
		unsigned flags = rpm_getint(TAG_FILEFLAGS, i);
		if (flags & RPMFILE_GHOST)
			continue;

		if (G.only_prefix && strncmp(d, G.only_prefix, strlen(G.only_prefix)))
			skip = 1;

		unsigned mode = rpm_getint(TAG_FILEMODES, i);

		if (skip && !S_ISREG(mode))
			continue;

		if (S_ISDIR(mode)) {
			char* path = xasprintf("%s%s%s", G.install_root, d, n);
			unsigned perms = mode&07777;
			if (getuid()) // if we're not root we need permissions
				perms |= 0700;
			bb_make_directory(path, perms, FILEUTILS_RECUR);
			free(path);
			continue;
		}
		if (S_ISLNK(mode)) {
			char* target = rpm_getstr(TAG_FILELINKTOS, i);
			char* path = xasprintf("%s%s", G.install_root, d);
			bb_make_directory(path, 0755, FILEUTILS_RECUR);
			free(path);
			path = xasprintf("%s%s%s", G.install_root, d, n);
			//printf("symlink %s -> %s\n", path, target);
			int ret = symlink(target, path);
			if (ret)
				bb_perror_msg_and_die("failed symlink %s -> %s", path, target);
			free(path);
			continue;
		}
		if (!S_ISREG(mode)) {
			printf("skip special file %s/%s\n", d, n);
			continue;
		}
		char* dir = xasprintf("%s%s", G.install_root, d);
		if (!skip)
			bb_make_directory(dir, 0755, FILEUTILS_RECUR);
		char* path = concat_path_file(dir, n);
		free(dir);
		int inode = rpm_getint(TAG_FILEINODES, i);
		ifi_t* found = bsearch(&inode, inodes, ninodes, sizeof(ifi_t), search_inode);
		if(!found) // can not happen
			bb_error_msg_and_die("inode %d not found\n", inode);
		if (found->fi != i) {
			if (skip)
				continue;
			char* od = rpm_getstr(TAG_DIRNAMES, rpm_getint(TAG_DIRINDEXES, found->fi));
			char* on = rpm_getstr(TAG_BASENAMES, found->fi);
			char* opath = xasprintf("%s%s%s", G.install_root, od, on);
			//printf("%d: hardlink %s -> %s\n", i, path, opath);
			if (link(opath, path) < 0)
				bb_perror_msg_and_die("failed to link %s -> %s", opath, path);
			free(path);
			free(opath);
			continue;
		}
		unsigned size = rpm_getint(TAG_FILESIZES, i);
		if (!skip)
			create_clone_from(path, mode, rpmfd, off, size);
		off += size;
		if (off & (PAGE_SIZE-1))
			off += PAGE_SIZE - (size & (PAGE_SIZE-1));
		free(path);
	}
	umask(org_mask);
	free(inodes);
}

static void extract_cpio(int fd, const char *source_rpm)
{
	archive_handle_t *archive_handle;

	if (source_rpm != NULL) {
		/* Binary rpm (it was built from some SRPM), install to root */
		xchdir("/");
	} /* else: SRPM, install to current dir */

	/* Initialize */
	archive_handle = init_handle();
	archive_handle->seek = seek_by_read;
	archive_handle->action_data = data_extract_all;
#if 0 /* For testing (rpm -i only lists the files in internal cpio): */
	archive_handle->action_header = header_list;
	archive_handle->action_data = data_skip;
#endif
	archive_handle->ah_flags = ARCHIVE_RESTORE_DATE | ARCHIVE_CREATE_LEADING_DIRS
		/* compat: overwrite existing files.
		 * try "rpm -i foo.src.rpm" few times in a row -
		 * standard rpm will not complain.
		 */
		| ARCHIVE_REPLACE_VIA_RENAME;
	archive_handle->src_fd = fd;
	/*archive_handle->offset = 0; - init_handle() did it */

	setup_unzip_on_fd(archive_handle->src_fd, /*fail_if_not_compressed:*/ 1);
	while (get_header_cpio(archive_handle) == EXIT_SUCCESS)
		continue;
}

static void install_header(int rpm_fd)
{
	int fd;
	off_t payloadstart;
	char* path = xasprintf("%s/%s-%s-%s.%s.rpm", G.header_dir,
							rpm_getstr0(TAG_NAME), rpm_getstr0(TAG_VERSION),
							rpm_getstr0(TAG_RELEASE), rpm_getstr0(TAG_ARCH));
	/* hack to avoid copy */
	path[strlen(G.header_dir)] = 0;
	bb_make_directory(path, 0755, FILEUTILS_RECUR);
	path[strlen(G.header_dir)] = '/';

	payloadstart = xlseek(rpm_fd, 0, SEEK_CUR);
	xlseek(rpm_fd, 0, SEEK_SET);

	fd = xopen(path, O_WRONLY|O_CREAT|O_EXCL);
	bb_copyfd_exact_size(rpm_fd, fd, payloadstart);
	close(fd);
	if (payloadstart != xlseek(rpm_fd, 0, SEEK_CUR)) {
		unlink(path);
		bb_error_msg_and_die("failed to write header");
	}
}

//usage:#define rpm_trivial_usage
//usage:       "-i PACKAGE.rpm; rpm -qp[ildc] PACKAGE.rpm"
//usage:#define rpm_full_usage "\n\n"
//usage:       "Manipulate RPM packages\n"
//usage:     "\nCommands:"
//usage:     "\n	-i	Install package"
//usage:     "\n	-q	Query package"
//usage:     "\n\nQuery Options:"
//usage:     "\n	-p	Query package file"
//usage:     "\n	-a	Query all installed packages"
//usage:     "\n	-i	Show information"
//usage:     "\n	-l	List contents"
//usage:     "\n	-d	List documents"
//usage:     "\n	-c	List config files"

/* RPM version 4.13.0.1:
 * Unlike -q, -i seems to imply -p: -i, -ip and -pi work the same.
 * OTOH, with -q order is important: "-piq FILE.rpm" works as -qp, not -qpi
 * (IOW: shows only package name, not package info).
 * "-iq ARG" works as -q: treats ARG as package name, not a file.
 *
 * "man rpm" on -l option and options implying it:
 * -l, --list		List files in package.
 * -c, --configfiles	List only configuration files (implies -l).
 * -d, --docfiles	List only documentation files (implies -l).
 * -L, --licensefiles	List only license files (implies -l).
 * --dump	Dump file information as follows (implies -l):
 *		path size mtime digest mode owner group isconfig isdoc rdev symlink
 * -s, --state	Display the states of files in the package (implies -l).
 *		The state of each file is one of normal, not installed, or replaced.
 *
 * Looks like we can switch to getopt32 here: in practice, people
 * do place -q first if they intend to use it (misinterpreting "-piq" wouldn't matter).
 */
int rpm_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int rpm_main(int argc, char **argv)
{
	int opt, option_index, func = 0;
	struct dirent *ent;
	DIR* rpms = NULL;
	int justfs = 0;

	INIT_G();
	INIT_PAGESIZE(G.pagesize);

	static struct option long_options[] = {
		{"install",      no_argument,       0,  'i' },
		{"query",        no_argument,       0,  'q' },
		{"force",        no_argument,       0,  1 },
		{"nodeps",       no_argument,       0,  0 },
		{"nodigest",     no_argument,       0,  0 },
		{"nosignature",  no_argument,       0,  0 },
		{"root",         required_argument, 0,  'r' },
		{"only-prefix",  required_argument, 0,  2 },
		{"justfs",       no_argument,       0,  3 },
		{0,              0,                 0,  0 }
	};

	while ((opt = getopt_long(argc, argv, "iqpldcaUr:", long_options, &option_index)) != -1) {
		switch (opt) {
		case 0: /* ignore */
			break;
		case 1:
			G.force = 1;
			break;
		case 2:
			G.only_prefix = xstrdup(optarg);
			break;
		case 3:
			justfs = 1;
			break;
		case 'U':
			if (func) bb_show_usage();
			func = rpm_install;
			break;
		case 'i': /* First arg: Install mode, with q: Information */
			if (!func) func = rpm_install;
			else func |= rpm_query_info;
			break;
		case 'q': /* First arg: Query mode */
			if (func) bb_show_usage();
			func = rpm_query;
			break;
		case 'p': /* Query a package (IOW: .rpm file, we are not querying RPMDB) */
			func |= rpm_query_package;
			break;
		case 'l': /* List files in a package */
			func |= rpm_query_list;
			break;
		case 'd': /* List doc files in a package (implies -l) */
			func |= rpm_query_list;
			func |= rpm_query_list_doc;
			break;
		case 'c': /* List config files in a package (implies -l) */
			func |= rpm_query_list;
			func |= rpm_query_list_config;
			break;
		case 'a': /* query all packages */
			func |= rpm_query_all;
		case 'r': /* install root */
			G.install_root = xstrdup(optarg);
			break;
		default:
			bb_show_usage();
		}
	}
	argv += optind;
	//argc -= optind;
	if (!(func & rpm_query_all) && !argv[0]) {
		bb_show_usage();
	}

	G.header_dir = xasprintf("%s%s", G.install_root?G.install_root:"", HEADER_DIR);

	if (func & rpm_query && (func | rpm_query_package) != func )
		rpms = xopendir(G.header_dir);

	for (;;) {
		int rpm_fd;
		const char *source_rpm;

		/* query installed package */
		if (func & rpm_query && (func | rpm_query_package) != func ) {
			char* path = NULL;
			ent = readdir(rpms);
			if (!ent)
				break;
			if (DOT_OR_DOTDOT(ent->d_name))
				continue; /* . or .. */
			path = concat_path_file(G.header_dir, ent->d_name);
			rpm_fd = rpm_gettags(path);
			free(path);
			if (!(func & rpm_query_all) && strcmp(rpm_getstr0(TAG_NAME), *argv)) {
				munmap(G.map, G.mapsize);
				free(G.mytags);
				continue;
			}
		} else
			rpm_fd = rpm_gettags(*argv);
		print_all_tags();

		source_rpm = rpm_getstr0(TAG_SOURCERPM);

		if (func & rpm_install) {
			/* -i (and not -qi) */

			if (!justfs)
				install_header(rpm_fd);

			int marker;
			xread(rpm_fd, &marker, 4);
			if (ntohl(marker) == 12245589) {
				reflink_package(rpm_fd);
			} else {
				xlseek(rpm_fd, -4, SEEK_CUR);
				/* Backup any config files */
				loop_through_files(TAG_BASENAMES, fileaction_dobackup);
				/* Extact the archive */
				extract_cpio(rpm_fd, source_rpm);
			}
			/* Set the correct file uid/gid's */
			loop_through_files(TAG_BASENAMES, fileaction_setowngrp);
		}
		else
		if (func & rpm_query) {
			/* -q */

			if (!(func & (rpm_query_info|rpm_query_list))) {
				/* If just a straight query, just give package name */
				printf("%s-%s-%s.%s\n", rpm_getstr0(TAG_NAME), rpm_getstr0(TAG_VERSION),
						rpm_getstr0(TAG_RELEASE), rpm_getstr0(TAG_ARCH));
			}
			if (func & rpm_query_info) {
				/* Do the nice printout */
				time_t bdate_time;
				struct tm *bdate_ptm;
				char bdatestring[50];
				const char *p;

				printf("%-12s: %s\n", "Name"        , rpm_getstr0(TAG_NAME));
				/* TODO compat: add "Epoch" here */
				printf("%-12s: %s\n", "Version"     , rpm_getstr0(TAG_VERSION));
				printf("%-12s: %s\n", "Release"     , rpm_getstr0(TAG_RELEASE));
				/* add "Architecture" */
				/* printf("%-12s: %s\n", "Install Date", "(not installed)"); - we don't know */
				printf("%-12s: %s\n", "Group"       , rpm_getstr0(TAG_GROUP));
				printf("%-12s: %d\n", "Size"        , rpm_getint(TAG_SIZE, 0));
				printf("%-12s: %s\n", "License"     , rpm_getstr0(TAG_LICENSE));
				/* add "Signature" */
				printf("%-12s: %s\n", "Source RPM"  , source_rpm ? source_rpm : "(none)");
				bdate_time = rpm_getint(TAG_BUILDTIME, 0);
				bdate_ptm = localtime(&bdate_time);
				strftime(bdatestring, 50, "%a %d %b %Y %T %Z", bdate_ptm);
				printf("%-12s: %s\n", "Build Date"  , bdatestring);
				printf("%-12s: %s\n", "Build Host"  , rpm_getstr0(TAG_BUILDHOST));
				p = rpm_getstr0(TAG_PREFIXS);
				printf("%-12s: %s\n", "Relocations" , p ? p : "(not relocatable)");
				/* add "Packager" */
				p = rpm_getstr0(TAG_VENDOR);
				if (p) /* rpm 4.13.0.1 does not show "(none)" for Vendor: */
				printf("%-12s: %s\n", "Vendor"      , p);
				p = rpm_getstr0(TAG_URL);
				if (p) /* rpm 4.13.0.1 does not show "(none)"/"(null)" for URL: */
				printf("%-12s: %s\n", "URL"         , p);
				printf("%-12s: %s\n", "Summary"     , rpm_getstr0(TAG_SUMMARY));
				printf("Description :\n%s\n", rpm_getstr0(TAG_DESCRIPTION));
			}
			if (func & rpm_query_list) {
				int count, it, flags;
				count = rpm_getcount(TAG_BASENAMES);
				for (it = 0; it < count; it++) {
					flags = rpm_getint(TAG_FILEFLAGS, it);
					switch (func & (rpm_query_list_doc|rpm_query_list_config)) {
					case rpm_query_list_doc:
						if (!(flags & RPMFILE_DOC)) continue;
						break;
					case rpm_query_list_config:
						if (!(flags & RPMFILE_CONFIG)) continue;
						break;
					case rpm_query_list_doc|rpm_query_list_config:
						if (!(flags & (RPMFILE_CONFIG|RPMFILE_DOC))) continue;
						break;
					}
					printf("%s%s\n",
						rpm_getstr(TAG_DIRNAMES, rpm_getint(TAG_DIRINDEXES, it)),
						rpm_getstr(TAG_BASENAMES, it));
				}
			}
		} else {
			/* Unsupported (help text shows what we support) */
			bb_show_usage();
		}
		munmap(G.map, G.mapsize);
		free(G.mytags);
		close(rpm_fd);
		if (!(func & rpm_query_all) && !*++argv)
			break;
	}
	if (rpms)
		closedir(rpms);

	free(G.install_root);
	free(G.header_dir);
	free(G.only_prefix);

	return 0;
}

#endif /* RPM */

/*
 * Mini rpm2cpio implementation for busybox
 *
 * Copyright (C) 2001 by Laurence Anderson
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
//config:config RPM2CPIO
//config:	bool "rpm2cpio (21 kb)"
//config:	default y
//config:	help
//config:	Converts a RPM file into a CPIO archive.

//applet:IF_RPM2CPIO(APPLET(rpm2cpio, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_RPM2CPIO) += rpm.o

//usage:#define rpm2cpio_trivial_usage
//usage:       "PACKAGE.rpm"
//usage:#define rpm2cpio_full_usage "\n\n"
//usage:       "Output a cpio archive of the rpm file"

#if ENABLE_RPM2CPIO

/* No getopt required */
int rpm2cpio_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int rpm2cpio_main(int argc UNUSED_PARAM, char **argv)
{
	const char *str;
	int rpm_fd;

	INIT_G();
	INIT_PAGESIZE(G.pagesize);

	rpm_fd = rpm_gettags(argv[1]);

	//if (SEAMLESS_COMPRESSION) - we do this at the end instead.
	//	/* We need to know whether child (gzip/bzip/etc) exits abnormally */
	//	signal(SIGCHLD, check_errors_in_children);

	if (ENABLE_FEATURE_SEAMLESS_LZMA
	 && (str = rpm_getstr0(TAG_PAYLOADCOMPRESSOR)) != NULL
	 && strcmp(str, "lzma") == 0
	) {
		// lzma compression can't be detected
		// set up decompressor without detection
		setup_lzma_on_fd(rpm_fd);
	} else {
		setup_unzip_on_fd(rpm_fd, /*fail_if_not_compressed:*/ 1);
	}

	if (bb_copyfd_eof(rpm_fd, STDOUT_FILENO) < 0)
		bb_simple_error_msg_and_die("error unpacking");

	if (ENABLE_FEATURE_CLEAN_UP) {
		close(rpm_fd);
	}

	if (SEAMLESS_COMPRESSION) {
		check_errors_in_children(0);
		return bb_got_signal;
	}
	return EXIT_SUCCESS;
}

#endif /* RPM2CPIO */
