/*
Copyright (c) 2007,2008 Rene Ladan <r.c.ladan@gmail.com> All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.

See uxtaf.txt for usage information.

*/

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#ifdef HAVE_SYS_UTSNAME_H
#include <sys/utsname.h>
#endif

#include <sys/types.h>

/* Undefine this if you have a big endian box */
/* XXX yeah I know this is ugly... */
#define LITTLE_ENDIAN_BOX


#define CLUST_2_SECT(c, firstclustsect)	\
	(firstclustsect - 1 + (c * 32))

#define SECT_2_INODE(firstdatasect, s)    \
    ((s - firstdatasect) * 8 + 3)

uint16_t bswap16(uint16_t x) {
	return(
#ifdef LITTLE_ENDIAN_BOX
		((x & 0x00ff) << 8) +
		((x & 0xff00) >> 8)
#else
		x
#endif
	);
}

uint32_t bswap32(uint32_t x) {
	return(
#ifdef LITTLE_ENDIAN_BOX
		bswap16((x & 0xffff0000) >> 16) +
		(bswap16(x & 0x0000ffff) << 16)

#else
		x
#endif
	);
}

/* Slightly ugly :-) */
#define INFONAME "./uxtaf.info"

#define FAT32_MASK 0x0fffffff
#define FAT16_MASK 0x0000ffff
#define DOT_NOT_FOUND 0xfffffff0

struct boot_s { /* 20 bytes */
	char magic[4]; /* should be "XTAF" */
	uint32_t volid; /* volume id */
	uint32_t spc; /* sectors/cluster */
	uint32_t nfat; /* should be 1 */
	uint16_t zero; /* should be 0 */
};

struct direntry_s { /* 64 bytes */
	uint8_t fnl; /* Theoretically, File Name Length. 0x00 / 0xff -> unused, 0xe5 -> deleted */
	uint8_t attr;
	char name[42];
	uint32_t fstart; /* cluster, 0 (i.e. "root") for nul-files */
	uint32_t fsize; /* 0 for directories */
	uint16_t cdate;
	uint16_t ctime;
	uint16_t adate;
	uint16_t atime;
	uint16_t udate;
	uint16_t utime;
};

struct datetime_s { /* 12 bytes */
	uint16_t year;
	uint16_t month;
	uint16_t day;
	uint16_t hour;
	uint16_t minute;
	uint16_t second;
};

struct dot_table_s {
	uint32_t this;
	uint32_t parent;
	struct dot_table_s *next;
};

/* 120*1024^3/512 < 2^32, so only define mediasize as uint64_t */
struct info_s {
	struct boot_s bootinfo;
	uint32_t pwd; /* sector of curdir */
	uint32_t fatmask;
	uint8_t fatmult;
	uint32_t fatstart;
	uint32_t fatsize;
	uint32_t rootstart;
	uint32_t firstcluster;
	uint32_t maxcluster;
	uint32_t numclusters;
	uint64_t partitionsize; /* Unit: Bytes */
	uint64_t mediasize; /* TODO Distinguish in code between media size and partition size */
	uint32_t fatsecs;
	uint64_t imageoffset; /* offset within the image file (for a partition within a disk image) */
	char imagename[256]; /* max file name length */
};

struct fat_s { /* 32 bits indeed... */
	uint32_t nextval;
	struct fat_s *next;
};

/* Prototype */
int dfxmlify(FILE *f, char *argv, struct info_s *info, struct dot_table_s **dot_table);

struct datetime_s dosdati(uint16_t date, uint16_t time) {
	struct datetime_s dt;

	dt.year = (date >> 9) + 1980;
	/* wikipedia says s/1980/2000 for FATX, seems wrong */
	dt.month = date >> 5 & 0x000f;
	dt.day = date & 0x001f;
	dt.hour = time >> 11;
	dt.minute = time >> 5 & 0x003f;
	dt.second = (time & 0x001f) << 1;
	return(dt);
}

int read_boot(FILE *f, struct boot_s *b) {
	size_t s;

	s = fread(b->magic, sizeof(char), 4, f);
	if (s != 4 || strncmp(b->magic, "XTAF", 4)) {
		fprintf(stderr, "read_boot: magic cmp = %i\n",
		    strncmp(b->magic, "XTAF", 4));
		return(1);
	}

	s = fread(&b->volid, sizeof(uint32_t), 1, f);
	b->volid = bswap32(b->volid);
	if (s != 1) {
		fprintf(stderr, "read_boot: volid: s = %zu\n", s);
		return(1);
	}

	s = fread(&b->spc, sizeof(uint32_t), 1, f);
	b->spc = bswap32(b->spc);
	if (s != 1) {
		fprintf(stderr, "read_boot: spc: s = %zu\n", s);
		return(1);
	}

	s = fread(&b->nfat, sizeof(uint32_t), 1, f);
	b->nfat = bswap32(b->nfat);
	if (s != 1 || b->nfat != 1) {
		fprintf(stderr, "read_boot: nfat: s = %zu nfat = %u\n", s,
		    b->nfat);
		return(1);
	}

	s = fread(&b->zero, sizeof(uint16_t), 1, f);
	b->zero = bswap16(b->zero);
	if (s != 1) {
		fprintf(stderr, "read_boot: zero: s = %zu\n", s);
		return(1);
	}
	return(0);
}

struct fat_s *build_fat_chain(FILE *f, struct info_s *info, uint32_t start,
    uint32_t size, uint8_t dentry_attr) {
	int rc;
	struct fat_s *head, *list, *this;
	size_t s;
	uint32_t cluster, nc;


        //Debug fprintf(stderr, "build_fat_chain: Debug: start = %d\n", (int) start);
	head = calloc(1, sizeof(struct fat_s));
	head->nextval = (start - 1) * info->bootinfo.spc + info->rootstart;
	list = head;
	cluster = start;

	nc = size / (512 * info->bootinfo.spc);
	if (size % (512 * info->bootinfo.spc) > 0)
		nc++;
	for (;;) {
		rc = fseeko(f, (uint64_t)(info->imageoffset + info->fatstart * 512 + cluster *
		    info->fatmult), SEEK_SET);
		if (rc < 0) {
			fprintf(stderr, "build_fat_chain: fseeko error, errno %d", errno);
		}
		s = fread(&cluster, info->fatmult, 1, f);
		if (s != 1) {
			fprintf(stderr, "build_fat_chain: s = %zu\n", s);
			fprintf(stderr, "build_fat_chain: (start = %zu)\n", start);
			fprintf(stderr, "build_fat_chain: (size = %zu)\n", size);
			fprintf(stderr, "build_fat_chain: (nc = %zu)\n", nc);
			fprintf(stderr, "build_fat_chain: (cluster = %zu)\n", cluster);
			return(NULL);
		}
                //Debug fprintf(stderr, "build_fat_chain: Debug: clust = %d\n", (int) cluster);
		cluster = info->fatmult == 2 ? bswap16(cluster) :
		    bswap32(cluster);
		cluster &= info->fatmask;
		nc--;

		/* from wikpedia::File_Allocation_Table :
		   0 = free cluster
		   2 .. 0x?fffffef = pointer to next, used
		   1, 0x?ffffff0 .. 0x?ffffff6 = reserved value
		   0x?ffffff7 = bad sector in cluster or reserved cluster
		   0x?ffffff8 .. 0x?fffffff = last cluster in file
		*/
		if (cluster < 2 || cluster > (0xffffffef & info->fatmask))
			break;

		this = calloc(1, sizeof(struct fat_s));
		this->nextval = (cluster - 1) * info->bootinfo.spc +
		    info->rootstart; /* convert to sector */
		list->next = this;
		list = list->next;
	}
	/* Die if file size mismatches; directories don't report size,
           so fat chain is the authority on length */
	if (nc > 0 && ! (dentry_attr & 16)) {
		fprintf(stderr, "build_fat_chain: %u clusters left in regular file\n", nc);
		exit(1);
	}
	list->next = NULL;
	return(head);
}

uint32_t find_dot_entry(struct dot_table_s *dot_table, uint32_t startcluster) {
	struct dot_table_s *dot;

	for (dot = dot_table; dot != NULL; dot = dot->next)
		if (dot->this == startcluster)
			return(dot->parent);
	return(DOT_NOT_FOUND);
}

void add_dot_entry(struct info_s *info, struct dot_table_s **dot_table, uint32_t cluster,
    uint32_t parent, int check) {
	struct dot_table_s *newdot;
    
	if (!check || find_dot_entry(*dot_table, cluster) == DOT_NOT_FOUND) {
		if (info->bootinfo.spc * 512 * cluster < info->partitionsize) { /*AJN Prevent segfaults caused by reading outside image or partition size*/
			newdot = calloc(1, sizeof(struct dot_table_s));
			newdot->this = cluster;
			newdot->parent = parent;
			newdot->next = *dot_table;
			*dot_table = newdot;
		} else {
			fprintf(stderr, "add_dot_entry: Warning: Skipped adding a dot entry for out-of-bounds cluster %zu", cluster);
		}
	}
}

int attach(struct info_s *info, struct dot_table_s **dot_table) {
	int i;
	uint8_t quirkblk[4096];
	size_t s;
	FILE *f;
	int rc;

	fprintf(stderr, "Opening %s in 'rb' mode\n", info->imagename);
	f = fopen(info->imagename, "rb");
	if (f == NULL) {
		fprintf(stderr, "Error opening %s: %i\n",
		    info->imagename, errno);
		return(errno);
	}
	rc = fseeko(f, 0LL, SEEK_END);
	if (rc < 0) {
		fprintf(stderr, "attach: fseeko (first call) error, errno %d", errno);
	}
	info->mediasize = 0;
	if (ftello(f) != -1)
		info->mediasize = ftello(f);
	if (info->mediasize > 0) {
		rc = fseeko(f, 0LL, SEEK_SET);
		if (rc < 0) {
			fprintf(stderr, "attach: fseeko (second call) error, errno %d", errno);
		}
	} else {
		fprintf(stderr, "attach: fseeko: errno = %i\n", errno);
		fclose(f);
		return(errno);
	}

	/* Partition sizes are as in TSK's tsk3/vs/xtaf.c */
	/* While a switch might look better here, some of the offsets are bigger than a 4-byte int can store, which even caused a problem on a 64-bit CPU for some reason. */
	if (info->imageoffset == 0x0) {
		info->partitionsize = info->mediasize;
	} else if (info->imageoffset == 0x80000) {
		info->partitionsize = 2147483648;
	} else if (info->imageoffset == 0x80080000) {
		info->partitionsize = 2348810240;
	} else if (info->imageoffset == 0x10c080000) {
		info->partitionsize = 216203264;
	} else if (info->imageoffset == 0x118eb0000) {
		info->partitionsize = 134217728;
	} else if (info->imageoffset == 0x120eb0000) {
		info->partitionsize = 268435456;
	} else if (info->imageoffset == 0x130eb0000) {
		/* TODO Populate this according to switch on media size */
		info->partitionsize = info->mediasize - info->imageoffset;
	} else {
		fprintf(stderr, "Warning: Unknown partition offset; defaulting to remaining media size.  This probably won't end well.\n"); /*TODO Add partition to this*/
		info->partitionsize = info->mediasize - info->imageoffset;
	}

	rc = fseeko(f, info->imageoffset, SEEK_SET);
	if (rc < 0) {
		fprintf(stderr, "attach: fseeko (partition initializing) error, errno %d", errno);
	}

	if (read_boot(f, &info->bootinfo)) {
		fclose(f);
		return(1);
	}

	/* some logic checks */
	if (info->bootinfo.spc == 0 || info->mediasize == 0 ||
	    info->bootinfo.spc & (info->bootinfo.spc - 1)) {
		fprintf(stderr, "attach: geometry error!\n");
		fclose(f);
		return(1);
	}

	/* calculate # of FAT sectors in sectors */
	info->numclusters = info->partitionsize / (512 * info->bootinfo.spc);
	if (info->numclusters >= 0xfff4) {
		info->fatmask = FAT32_MASK;
		info->fatmult = 4;
	} else {
		info->fatmask = FAT16_MASK;
		info->fatmult = 2;
	}
	info->fatsize = info->numclusters * info->fatmult;
	if (info->fatsize % 4096 != 0)
		info->fatsize = ((info->fatsize / 4096) + 1) * 4096;
	info->fatsecs = info->fatsize / 512; /* convert to sectors */
	info->fatstart = 8;

	info->rootstart = info->fatsecs + info->fatstart;
	fprintf(stderr, "rootstart before: div=%i mod=%i\n",
	    info->rootstart / info->bootinfo.spc,
	    info->rootstart % info->bootinfo.spc);
	/* correct for hd quirk */
	rc = fseeko(f, (uint64_t)info->imageoffset + info->rootstart * 512, SEEK_SET);
	if (rc < 0) {
		fprintf(stderr, "attach: fseeko (quirk block call) error, errno %d", errno);
	}
	s = fread(quirkblk, sizeof(uint8_t), 4096, f);
	if (s != 4096) {
		fprintf(stderr, "attach: block read error!\n");
		fclose(f);
		return(1);
	}
	for (i = 0; i < 4096 && quirkblk[i] == 0; i++)
		;
	fprintf(stderr, "quirk: i = %i quirkblk[i] = %u\n", i, quirkblk[i]);
	if (i == 4096)
		info->rootstart += 8;
	fprintf(stderr, "rootstart after : div=%i mod=%i\n",
	    info->rootstart / info->bootinfo.spc,
	    info->rootstart % info->bootinfo.spc);

	info->firstcluster = info->rootstart + info->bootinfo.spc;
	info->maxcluster = (info->partitionsize/ 512 - info->firstcluster) /
	    info->bootinfo.spc + 1;
	if (info->maxcluster >= info->numclusters) {
		fprintf(stderr, "attach: numclusters (%u) exceeds FAT capacity "
		    "(%u), adjusting\n",
		info->maxcluster + 1, info->numclusters);
		info->maxcluster = info->numclusters - 1;
	}

	fclose(f);

	info->pwd = info->rootstart; /* sensible start */
	*dot_table = NULL;
	add_dot_entry(info, dot_table, 1, 1, 0);
	fprintf(stderr, "attach: done\n");
	return(0);
}

struct direntry_s get_entry(struct info_s *info, uint32_t clust, char *filename)
{
	FILE *f;
	int rc;
	struct direntry_s de;
	size_t s;
	struct fat_s *fatptr;
	int entry;
	char fname[43];

	f = fopen(info->imagename, "rb");
	if (f == NULL) {
		fprintf(stderr, "Error opening %s: %i\n",
		    info->imagename, errno);
		de.fnl = 0;
		return(de);
	}
	for (fatptr = build_fat_chain(f, info, clust, 512 * info->bootinfo.spc, 0);
	    fatptr != NULL; fatptr = fatptr->next) {
		rc = fseek(f, (uint64_t)(info->imageoffset + 512 * fatptr->nextval), SEEK_SET);
		if (rc < 0) {
			fprintf(stderr, "get_entry: fseek error, errno %d", errno);
		}
		for (entry = 0; entry < info->bootinfo.spc; entry++) {
			s = fread(&de, sizeof(struct direntry_s), 1, f);
			if (s != 1) {
				fprintf(stderr, "get_entry: s = %zu\n", s);
				de.fnl = 0;
				return(de);
			}
			if (de.fnl != 0 && de.fnl != 0xff && de.fnl != 0xe5) {
				bzero(fname, 43 * sizeof(char));
				strncpy(fname, de.name, de.fnl);
				if (!strcmp(fname, filename)) {
					fclose(f);
					de.fstart = bswap32(de.fstart);
					de.fsize = bswap32(de.fsize);
					/* date/time only in ls */
					return(de); /* found */
				}
			}
		}
	}
	fclose(f);
	de.fnl = 0;
	return(de);
}

struct direntry_s resolve_path(struct info_s *info,
	struct dot_table_s *dot_table, char *pathname) {
	struct direntry_s de;
	uint32_t clust;
	char *part, *path;

	bzero(&de, sizeof(struct direntry_s));
	if (pathname == NULL || strlen(pathname) == 0) {
		fprintf(stderr, "resolve_path: empty path\n");
		return(de);
	}

	clust = pathname[0] == '/' ? 1 :
	    (info->pwd - info->rootstart) / info->bootinfo.spc + 1;
	de.fnl = 1;
	de.fstart = clust;

	path = strdup(pathname);
	for (; (part = strsep(&path, "/")) != NULL; ) {
		if (*part == '\0' || !strcmp(part, "."))
			continue;
		else if (!strcmp(part, "..")) {
			clust = find_dot_entry(dot_table, clust);
			if (clust == DOT_NOT_FOUND) {
				de.fnl = 0;
				return(de);
			}
			de.fnl = 1;
			de.fstart = clust;
		} else {
			de = get_entry(info, clust, part);
			if (de.fnl == 0)
				return(de);
			clust = de.fstart;
		}
	}
	return(de);
}

int ls(struct info_s *info, struct dot_table_s **dot_table) {
	struct direntry_s de;
	char fname[43];
	struct datetime_s da, dc, du;
	int i, entry;
	size_t s;
	FILE *f;
	int rc;
	struct fat_s *fatptr;
	int freq[256];
	uint32_t clust;
	int is_dent;

	for (i = 0; i < 256; i++)
		freq[i] = 0;

	f = fopen(info->imagename, "rb");
	if (f == NULL) {
		fprintf(stderr, "Error opening %s: %i\n",
		    info->imagename, errno);
		return(errno);
	}
	clust = (info->pwd - info->rootstart) / info->bootinfo.spc + 1;
	for (fatptr = build_fat_chain(f, info, clust, 512 * info->bootinfo.spc, 0);
	    fatptr != NULL; fatptr = fatptr->next) {
		rc = fseek(f, (uint64_t)(info->imageoffset + 512 * fatptr->nextval), SEEK_SET);
		if (rc < 0) {
			fprintf(stderr, "ls: fseeko error, errno %d", errno);
		}

		printf("entry fnl rhsvda startclust   filesize    "
		    "create_date_time    access_date_time    update_date_time "
		    "filename\n");
		for (entry = 0; entry < info->bootinfo.spc; entry++) {
			s = fread(&de, sizeof(struct direntry_s), 1, f);
			if (s != 1) {
				fprintf(stderr, "ls: s = %zu\n", s);
				return(1);
			}

			if (de.fnl == 0 || de.fnl == 0xff)
				continue; /* to next slot */

			de.fstart = bswap32(de.fstart);
			de.fsize = bswap32(de.fsize);
			bzero(fname, 43 * sizeof(char));
			if (de.fnl == 0xe5 || de.fnl > 42)
				for (i = 0; i < 42; i++) {
					fname[i] = de.name[i];
					if (de.name[i] == 0x00 ||
					    (de.name[i] & 0xff) == 0xff) {
						fname[i] = 0;
						break;
					}
				}
			else
				strncpy(fname, de.name, de.fnl);
            /*AJN Check name for unprintable characters. That's a good indication that this isn't supposed to be a directory entry.*/
            is_dent = 1;
            for (i = 0; i < 42; i++)
                if (!isprint(fname[i]) && fname[i] != 0)
                    is_dent = 0;
            if (is_dent == 0)
                continue; /*AJN Of course, the rest of the directory's probably dead at this point.*/
			dc = dosdati(bswap16(de.cdate), bswap16(de.ctime));
			da = dosdati(bswap16(de.adate), bswap16(de.atime));
			du = dosdati(bswap16(de.udate), bswap16(de.utime));
			printf("%5u %3u %c%c%c%c%c%c %10u %10u %04u-%02u-%02u "
			    "%02u:%02u:%02u %04u-%02u-%02u %02u:%02u:%02u "
			    "%04u-%02u-%02u %02u:%02u:%02u %s\n",
			    entry, de.fnl,
			    (de.attr & 1 ? 'r' : '-'),
			    (de.attr & 2 ? 'h' : '-'),
			    (de.attr & 4 ? 's' : '-'),
			    (de.attr & 8 ? 'v' : '-'),
			    (de.attr & 16 ? 'd' : '-'),
			    (de.attr & 32 ? 'a' : '-'),
			    de.fstart, de.fsize, dc.year, dc.month, dc.day,
			    dc.hour, dc.minute, dc.second, da.year, da.month,
			    da.day, da.hour, da.minute, da.second, du.year,
			    du.month, du.day, du.hour, du.minute, du.second,
			    fname);

			if (de.fnl != 0xe5 && de.attr & 16)
				add_dot_entry(info, dot_table, de.fstart, clust, 1);

			for (i = 0; i < strlen(fname); i++)
				freq[(uint8_t)fname[i]] = 1;
		}
	}
	fclose(f);

	printf("file name characters: ");
	for (i = 0; i < 256; i++)
		if (freq[i] == 1)
			printf("%4i", i);
	printf("\n");
	return(0);
}

void show_info(struct info_s *info) {
	int i;

	printf("magic        = ");
	for (i = 0; i < 4; i++)
		printf("%c", info->bootinfo.magic[i]);
	printf("\n");
	printf("volid        = 0x%08x\n", info->bootinfo.volid);
	printf("spc          = %u\n", info->bootinfo.spc);
	printf("nfat         = %u\n", info->bootinfo.nfat);
	printf("zero         = %u\n", info->bootinfo.zero);
	printf("pwd          = %u sectors  @ 0x%llx bytes\n", info->pwd,
	    (uint64_t)info->pwd * 512);
	printf("fatmask      = 0x%08x\n", info->fatmask);
	printf("%u bits\n", info->fatmult * 8);
	printf("fatstart     = %u sectors  @ 0x%llx bytes\n", info->fatstart,
	    (uint64_t)info->fatstart * 512);
	printf("fatsize      = %u bytes\n", info->fatsize);
	printf("rootstart    = %u sectors  @ 0x%llx bytes\n", info->rootstart,
	    (uint64_t)(info->rootstart * 512));
	printf("firstcluster = %u sectors  @ 0x%llx bytes\n",
	    info->firstcluster, (uint64_t)(info->firstcluster * 512));
	printf("maxcluster   = %u clusters @ 0x%llx bytes\n", info->maxcluster,
	    (uint64_t)(info->maxcluster * 512 * info->bootinfo.spc));
	printf("numclusters  = %u\n", info->numclusters);
	printf("mediasize    = %llu bytes\n", info->mediasize);
	printf("partitionsize= %llu bytes\n", info->partitionsize);
	printf("fatsecs      = %u sectors\n", info->fatsecs);
	printf("image offset = %u\n", info->imageoffset);
	printf("image name   = %s\n", info->imagename);
}

void cd(char *argv, struct info_s *info, struct dot_table_s *dot_table) {
	struct direntry_s de;

	de = resolve_path(info, dot_table, argv);
	if (de.fnl == 0)
		fprintf(stderr, "cd: pathname not found: %s\n", argv);
	else if (de.fstart < 2)
		info->pwd = info->rootstart; /* use 0 or 1 for root directory */
	else
		info->pwd = (de.fstart - 1) * info->bootinfo.spc +
		    info->rootstart;

	fprintf(stderr, "cd: Debug: new pwd = %u sectors @ 0x%llx bytes into partition at %llu bytes of media\n", info->pwd,
	    (uint64_t)(info->pwd * 512), info->imageoffset);
}

int cat(char *argv, struct info_s *info, struct dot_table_s *dot_table) {
	size_t s;
	FILE *f;
	int rc;
	struct fat_s *fatptr;
	char *buf;
	struct direntry_s de;
	uint32_t rest;

	de = resolve_path(info, dot_table, argv);
	if (de.fnl == 0) {
		fprintf(stderr, "cat: path not found: %s\n", argv);
		return(ENOENT);
	}
	rest = de.fsize % (512 * info->bootinfo.spc);

	f = fopen(info->imagename, "rb");
	if (f == NULL) {
		fprintf(stderr, "Error opening %s: %i\n",
		    info->imagename, errno);
		return(errno);
	}
	buf = calloc(512 * info->bootinfo.spc, sizeof(char));
	for (fatptr = build_fat_chain(f, info, de.fstart, de.fsize, de.attr);
	    fatptr != NULL; fatptr = fatptr->next) {
		rc = fseek(f, (uint64_t)(info->imageoffset + 512 * fatptr->nextval), SEEK_SET);
		if (rc < 0) {
			fprintf(stderr, "cat: fseeko error, errno %d", errno);
		}

		s = fread(buf, sizeof(char), 512 * info->bootinfo.spc, f);
		if (s != 512 * info->bootinfo.spc) {
			fprintf(stderr, "cat: s = %zu\n", s);
			return(1);
		}
		fwrite(buf, sizeof(char), fatptr->next != NULL || rest == 0 ?
		    512 * info->bootinfo.spc : rest, stdout);
	}
	fclose(f);
	return(0);
}

void show_dot_table(struct dot_table_s *dot_table) {
	struct dot_table_s *dot;

	printf("this\tparent\n");
	for (dot = dot_table; dot != NULL; dot = dot->next)
		printf("%u\t%u\n", dot->this, dot->parent);
}

int usage(void) {
	printf("See uxtaf.txt for usage information.\n");
	return(1);
}

void read_infofile(struct info_s *info, struct dot_table_s **dot_table) {
	FILE *infofile;
	size_t s = 0;
	uint32_t this, parent;

	infofile = fopen(INFONAME, "rb");
	if (infofile == NULL) {
		fprintf(stderr, "Error opening %s: %i\n",
		    info->imagename, errno);
		exit(errno);
	}
	if (infofile != NULL) {
		s = fread(info, sizeof(struct info_s), 1, infofile);
		if (s != 1) {
			printf("Could not read %s, aborting.\n", INFONAME);
			exit(1);
		}
		/* read dot_table */
		*dot_table = NULL;
		while (!feof(infofile)) {
			s = fread(&this, sizeof(uint32_t), 1, infofile);
			if (s == 1)
				s = fread(&parent, sizeof(uint32_t), 1, infofile);
			if (s == 1)
				add_dot_entry(info, dot_table, this, parent, 0);
		}
		fclose(infofile);
	} else {
		printf("%s does not exist, use attach command.\n", INFONAME);
		exit(ENOENT);
	}

}

void write_infofile(struct info_s *info, struct dot_table_s **dot_table) {
	FILE *infofile;
	struct dot_table_s *dot, *tmp;
	size_t s = 0;

	infofile = fopen(INFONAME, "wb");
	if (infofile == NULL) {
		fprintf(stderr, "Could not open %s for writing.\n", INFONAME);
		exit(errno);
	}
	s = fwrite(info, sizeof(struct info_s), 1, infofile);
	if (s != 1)
		fprintf(stderr, "Could not save info.\n");
	else {
		/* write/clean dot table */
		for (dot = *dot_table; dot != NULL; ) {
			fwrite(&(dot->this), sizeof(uint32_t), 1, infofile);
			fwrite(&(dot->parent), sizeof(uint32_t), 1, infofile);
			tmp = dot;
			dot = dot->next;
			free(tmp);
		}
	}
	fclose(infofile);
}

int dfxml(struct info_s *info, struct dot_table_s *dot_table, int argc, char *argv[]) {
    int retval;

    printf("<?xml version='1.0' encoding='UTF-8'?>\n");
    printf("<dfxml>\n");

    printf("  <creator version='1.0'>\n");
    printf("    <program>%s</program>\n", PACKAGE);
    printf("    <version>%s</version>\n", PACKAGE_VERSION);
    printf("    <build_environment>\n");
    printf("      <compiler>GCC %d.%d</compiler>\n",__GNUC__, __GNUC_MINOR__);
    printf("    </build_environment>\n");
    printf("    <execution_environment>\n");
#ifdef HAVE_SYS_UTSNAME_H
    struct utsname name;
    if(uname(&name)==0){
	printf("      <os_sysname>%s</os_sysname>\n",name.sysname);
	printf("      <os_release>%s</os_release>\n",name.release);
	printf("      <os_version>%s</os_version>\n",name.version);
	printf("      <host>%s</host>\n",name.nodename);
	printf("      <arch>%s</arch>\n",name.machine);
    }
#else
    printf("<!--No sys/utsname, cannot print sys info-->\n");
#endif
    printf("    </execution_environment>\n");
      int i;
      printf("    <source>%s</source>\n", info->imagename);
      printf("    <command_line>");
      for(i = 0; i < argc; i++){
          if (i > 0)
            printf(" ");
          printf("%s", argv[i]);
      }
      printf("    </command_line>\n");
    printf("  </creator>\n");

	//Start processing file
	FILE *f;
	f = fopen(info->imagename, "rb");
	if (f == NULL) {
		fprintf(stderr, "Error opening %s: %i\n",
                info->imagename, errno);
		return(errno);
	}
	printf("  <sectorsize>512</sectorsize>\n");
	printf("  <volume offset=\"%zu\">\n", info->imageoffset);
	printf("    <partition_offset>%zu</partition_offset>\n", info->imageoffset);
	printf("    <block_size>%zu</block_size>\n", (info->bootinfo.spc) * 512);
	if (info->fatmask == FAT32_MASK) {
		printf("    <ftype_str>XTAF32</ftype_str>\n");
	} else if (info->fatmask == FAT16_MASK) {
		printf("    <ftype_str>XTAF16</ftype_str>\n");
	} else {
		printf("    <!--Warning: Unknown if this is XTAF16, 32, or something new.-->\n");
		printf("    <ftype_str>XTAF</ftype_str>\n");
	}
/* TODO Add these other volume elements
    <ftype>256</ftype> (Probably not this one, it's a number only TSK defines)
    <block_count>235516</block_count> (Is this in sectors or blocks(==clusters)?)
    <first_block>0</first_block> (Why would this not be 0?)
    <last_block>235515</last_block>

*/
	retval = dfxmlify(f, "/", info, &dot_table);
	if (retval != 0) printf("    <!--Error: dfxml conversion of this partition failed.-->");
	printf("  </volume>\n");
        fclose(f);

	printf("</dfxml>\n");
	return retval;
}

/* Convert an entire partition to DFXML */
int dfxmlify(FILE *f, char *argv, struct info_s *info, struct dot_table_s **dot_table) {
	int rc;
	int retval = 0;
	struct direntry_s de;
	char fname[43];
	struct datetime_s da, dc, du;
	int i, entry;
	size_t s;
	off_t dir_off;
	off_t dent_off;
	struct fat_s *fatptr, *sizefatptr;
	uint32_t clust;
	int is_dir;
	char full_path[4096]; /*AJN Maybe overkill?*/
	int concat_retval;
	int is_dent;
	int is_alloc;
	uint32_t this_cluster;
	char name_type[2];
	*(name_type+1) = 0; /* Null-terminate string */

	uint32_t prevpwd = info->pwd; /*AJN: Note that cd() only mutates info->pwd*/
	cd(argv, info, *dot_table);
	/*BEGIN COPY*/

	clust = (info->pwd - info->rootstart) / info->bootinfo.spc + 1;
	printf("    <!-- Starting clust: %d -->\n", clust);
	/* Loop over clusters of the directory */
	for (
	  fatptr = build_fat_chain(f, info, clust, 512 * info->bootinfo.spc, 0);
	  fatptr != NULL;
	  fatptr = fatptr->next) {
		rc = fseek(f, (uint64_t)(info->imageoffset + 512 * fatptr->nextval), SEEK_SET);
		if (rc < 0) {
			fprintf(stderr, "dfxmlify: fseeko error, errno %d", errno);
		}
		dir_off = ftello(f);
		fprintf(stderr, "dfxmlify: Debug: Changed directory, cursor offset %llu bytes, path %s\n", dir_off, argv);
		if (dir_off == -1); //TODO Handle ftello failing.

		/*printf("entry fnl rhsvda startclust   filesize    "
		"create_date_time    access_date_time    update_date_time "
		"filename\n");*/
		/* Loop over entries in the directory */
		/* AJN TODO Is this loop condition actually right? Why SPC, instead of dents per cluster? */
		for (entry = 0; entry < info->bootinfo.spc; entry++) {
//			rc = fseek(f, (uint64_t)(info->imageoffset + dir_off), SEEK_SET); /*Reset file pointer, in case build_fat_chain reads and doesn't clean up state*/
			dent_off = ftello(f);
			s = fread(&de, sizeof(struct direntry_s), 1, f);
			if (s != 1) {
				fprintf(stderr, "dfxmlify: s = %zu\n", s);
				return(1);
			}

			if (de.fnl == 0 || de.fnl == 0xff) {
				fprintf(stderr, "dfxmlify: Note: Skipping directory entry (index %d, image offset %llu) due to fnl %u\n", entry, dent_off, de.fnl);
				continue; /* to next slot */
			}

			/* Denote deleted entries */
			is_alloc = (de.fnl == 0xe5) ? 0 : 1;

			de.fstart = bswap32(de.fstart);

			/* Special consideration: directories have their sizes recorded as 0, but Fiwalk records the size of the clusters used (not considering the proportion of the clusters used).  Record here directory size as their fat chain length. This differs from the copy-pasted logic.*/
			//TODO build_fat_chain isn't liking this; debug.
//			if (de.attr & 16) {
//				de.fsize = 0;
//				for (
//				  sizefatptr = build_fat_chain(f, info, de.fstart, 0, de.attr);
//				  sizefatptr != NULL;
//				  sizefatptr = sizefatptr->next
//				) {
//					de.fsize += 512 * info->bootinfo.spc;
//				}
//			} else {
				de.fsize = bswap32(de.fsize);
//			}

			bzero(fname, 43 * sizeof(char));
			/* Extract name */
			if (de.fnl == 0xe5 || de.fnl > 42)
				for (i = 0; i < 42; i++) {
					fname[i] = de.name[i];
					if (de.name[i] == 0x00 ||
					    (de.name[i] & 0xff) == 0xff) {
						fname[i] = 0;
						break;
					}
				}
			else
				strncpy(fname, de.name, (de.fnl < 42 ? de.fnl : 42));
			/*AJN Check name for unprintable characters. That's a good indication that this isn't supposed to be a directory entry.*/
			is_dent = 1;
			for (i = 0; i < 42; i++)
				if (!isprint(fname[i]) && fname[i] != 0)
					is_dent = 0;
			if (is_dent == 0) {
				fprintf(stderr, "dfxmlify: Note: Skipping directory entry (index %d) due to unprintable character\n", entry);
				continue; /*AJN Of course, the rest of the directory's probably dead at this point. */
			}

			printf("    <fileobject>\n");

			dc = dosdati(bswap16(de.cdate), bswap16(de.ctime));
			da = dosdati(bswap16(de.adate), bswap16(de.atime));
			du = dosdati(bswap16(de.udate), bswap16(de.utime));

			is_dir = de.attr & 16;
			*name_type = 0;
			if (is_dir) *name_type = 'd';
			if (de.attr & 1) *name_type = 'r'; /* TODO This is not the right check for a regular file */

			/*Define full path*/
			bzero(full_path, 4096 * sizeof(char));
			concat_retval = snprintf(full_path, 4096, "%s%s%s", argv, strcmp(argv,"/") ? "/" : "", fname);
			if (concat_retval < 0) {
				retval = concat_retval;
				fprintf(stderr, "dfxmlify: snprintf: Some kind of error forming the full path.\n");
			} else {
				/*TODO guarantee that len(full_path) >= 1*/
				printf("      <filename>%s</filename>\n",full_path+1); /*AJN DFXML has a history of not starting paths with '/' */
				printf("      <xtaf:filenamelength>%u</xtaf:filenamelength>\n", de.fnl);
				if (*name_type) printf("      <name_type>%s</name_type>\n", name_type);
				printf("      <filesize>%d</filesize>\n", de.fsize);
				printf("      <alloc>%d</alloc>\n", is_alloc);
				printf("      <crtime>%04u-%02u-%02uT%02u:%02u:%02uZ</crtime>\n", dc.year, dc.month, dc.day, dc.hour, dc.minute, dc.second);
				printf("      <atime>%04u-%02u-%02uT%02u:%02u:%02uZ</atime>\n", da.year, da.month, da.day, da.hour, da.minute, da.second);
				printf("      <mtime>%04u-%02u-%02uT%02u:%02u:%02uZ</mtime>\n", du.year, du.month, du.day, du.hour, du.minute, du.second);
				int sect = (clust-1) * 32 + (entry/8); /* TODO Relate these hard-coded values to info-> values */
				int inode = 3 + 8 * sect + (entry%8);
				printf("      <st_ino>%d</st_ino>\n", inode );
				/*
				 * TODO:
				 *      <hashdigest type='md5'></hashdigest>
				 *      <hashdigest type='sha1'></hashdigest>
				 *
				 * */
				/*printf("%5u %c%c%c%c%c%c %10u\n",
				entry,
				(de.attr & 1 ? 'r' : '-'),
				(de.attr & 2 ? 'h' : '-'),
				(de.attr & 4 ? 's' : '-'),
				(de.attr & 8 ? 'v' : '-'),
				(de.attr & 16 ? 'd' : '-'),
				(de.attr & 32 ? 'a' : '-'),
				de.fstart);*/

				/* Build byte runs if possible */
				struct fat_s *brfatptr = build_fat_chain(f, info, de.fstart, de.fsize, de.attr);
				if (fatptr != NULL) {
					printf("      <byte_runs>\n");
					/* fsize_accounted: number of bytes accounted for following fat chain.  If this ends up unequal to de.fsize, there is a data anomaly. */
					uint32_t fsize_accounted = 0;
					uint32_t full_csize = 512 * info->bootinfo.spc;
					uint32_t this_csize = full_csize;
					this_cluster = de.fstart;
					while (brfatptr != NULL) {
						printf("        <byte_run");
						printf(" xtaf:this_cluster='%d'", this_cluster);
						printf(" xtaf:next_cluster='%d'", brfatptr->nextval);
						printf(" file_offset='%d'", fsize_accounted);
						//TODO printf(" fs_offset=''");
						//TODO printf(" img_offset=''");
						if (fsize_accounted + full_csize > de.fsize) {
							this_csize = de.fsize - fsize_accounted;
						}
						printf(" len='%d'", this_csize);
						printf(" />\n");
						fsize_accounted += this_csize;

						this_cluster = brfatptr->nextval;
						brfatptr = brfatptr->next;
					}
					/* Squawk if there's a byte discrepancy, to stderr and to the XML */
					if (fsize_accounted != de.fsize) {
						printf("        <!-- Warning: Fat chain terminated before all bytes in the file were accounted for.  (%d bytes remaining.) -->\n", de.fsize - fsize_accounted);
						fprintf(stderr, "Warning: Fat chain terminated before all bytes in the file were accounted for.  (%d bytes remaining.)\n", de.fsize - fsize_accounted);
					}
					printf("      </byte_runs>\n");
				}

				printf("    </fileobject>\n");
				fflush(stdout);

				/* Recurse into an allocated directory */
				if (de.fnl != 0xe5 && (de.attr & 16)) {
					add_dot_entry(info, dot_table, de.fstart, clust, 1);
					/*Recurse*/
					retval = dfxmlify(f, full_path, info, dot_table);
					rc = fseek(f, (uint64_t)(info->imageoffset + 512 * fatptr->nextval) + (1+entry)*sizeof(struct direntry_s), SEEK_SET); /*Disk image cursor gets tweaked in every dfxmlify call; reset*/
					if (rc < 0) {
						fprintf(stderr, "dfxmlify: fseek error, errno %d\n", errno);
					}
					if (retval) {
						fprintf(stderr, "dfxmlify: Error in recursive call, retval=%d; terminating.\n", retval);
						break;
					}
				} /* End if (allocated directory) */
			} /* End if (successful path concatenation) */
		} /* End for (loop through directory entries) */
	} /* End for (looping over clusters of directory) */
	/*END COPY*/
	info->pwd = prevpwd; /*AJN: Reset pwd after finished with this directory*/
	fflush(stdout);
	return retval;
}

int main(int argc, char *argv[]) {
	struct info_s info;
	struct dot_table_s *dot_table;
	int ret = 0;
	int i;

	if (argc < 2)
		return(usage());

	if (strcmp(argv[1], "attach"))
		read_infofile(&info, &dot_table);

	if (!strcmp(argv[1], "attach") && (argc == 3 || argc == 4)) {
		/* Assign image file name to info struct */
		for (i = 0; i < 255 && i < strlen(argv[2]); i++)
			info.imagename[i] = argv[2][i];
		info.imagename[i] = '\0';
		/* Assign offset of the partition within the image file to info struct */
		if (argc == 4) {
			info.imageoffset = atoll(argv[3]);
		} else {
			/* Default 0 */
			info.imageoffset = 0;
		}
		ret = attach(&info, &dot_table);
	} else if (!strcmp(argv[1], "info") && argc == 2)
		show_info(&info);
	else if (!strcmp(argv[1], "dot") && argc == 2)
		show_dot_table(dot_table);
	else if (!strcmp(argv[1], "ls") && argc == 2)
		ret = ls(&info, &dot_table);
	else if (!strcmp(argv[1], "cat") && argc == 3)
		ret = cat(argv[2], &info, dot_table);
	else if (!strcmp(argv[1], "cd") && argc == 3)
		cd(argv[2], &info, dot_table);
	else if (!strcmp(argv[1], "dfxml") && argc == 2)
		ret = dfxml(&info, dot_table, argc, argv);
	else
		return(usage());

	if (ret != 0)
		fprintf(stderr, "uxtaf: something went wrong, aborting\n");
	else
		write_infofile(&info, &dot_table);
	return(ret);
}
