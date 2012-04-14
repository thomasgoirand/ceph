// -*- mode:C; tab-width:8; c-basic-offset:8; indent-tabs-mode:t -*- 
/*
 * Copyright (c) 1998-2001 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 2.0 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 *
 *	File:	fsx.c
 *	Author:	Avadis Tevanian, Jr.
 *
 *	File system exerciser. 
 *
 *	Rewrite and enhancements 1998-2001 Conrad Minshall -- conrad@mac.com
 *
 *	Various features from Joe Sokol, Pat Dirks, and Clark Warner.
 *
 *	Small changes to work under Linux -- davej@suse.de
 *
 *	Sundry porting patches from Guy Harris 12/2001
 *
 *	Checks for mmap last-page zero fill.
 *
 *	Updated license to APSL 2.0, 2004/7/27 - Jordan Hubbard
 *
 * $FreeBSD: src/tools/regression/fsx/fsx.c,v 1.6 2008/02/19 07:09:18 ru Exp $
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>

#include "include/rados/librados.h"
#include "include/rbd/librbd.h"

#define NUMPRINTCOLUMNS 32	/* # columns of data to print on each line */

/*
 *	A log entry is an operation and a bunch of arguments.
 */

struct log_entry {
	int	operation;
	int	args[3];
};

#define	LOGSIZE	1000

struct log_entry	oplog[LOGSIZE];	/* the log */
int			logptr = 0;	/* current position in log */
int			logcount = 0;	/* total ops */

/*
 *	Define operations
 */

#define	OP_READ		1
#define OP_WRITE	2
#define OP_TRUNCATE	3
#define OP_CLOSEOPEN	4
#define OP_MAPREAD	5
#define OP_MAPWRITE	6
#define OP_SKIPPED	7

int page_size;
int page_mask;

char	*original_buf;			/* a pointer to the original data */
char	*good_buf;			/* a pointer to the correct data */
char	*temp_buf;			/* a pointer to the current data */
char	*pool;				/* name of the pool our test image is in */
char	*iname;				/* name of our test image */
rados_t		cluster;		/* cluster handle */
rados_ioctx_t	ioctx;			/* handle for our test pool */
rbd_image_t	image;			/* handle for our test image */

off_t		file_size = 0;
off_t		biggest = 0;
char		state[256];
unsigned long	testcalls = 0;		/* calls to function "test" */

unsigned long	simulatedopcount = 0;	/* -b flag */
int	closeprob = 0;			/* -c flag */
int	debug = 0;			/* -d flag */
unsigned long	debugstart = 0;		/* -D flag */
unsigned long	maxfilelen = 256 * 1024;	/* -l flag */
int	sizechecks = 1;			/* -n flag disables them */
int	maxoplen = 64 * 1024;		/* -o flag */
int	quiet = 0;			/* -q flag */
unsigned long progressinterval = 0;	/* -p flag */
int	readbdy = 1;			/* -r flag */
int	style = 0;			/* -s flag */
int	truncbdy = 1;			/* -t flag */
int	writebdy = 1;			/* -w flag */
long	monitorstart = -1;		/* -m flag */
long	monitorend = -1;		/* -m flag */
int	lite = 0;			/* -L flag */
long	numops = -1;			/* -N flag */
int	randomoplen = 1;		/* -O flag disables it */
int	seed = 1;			/* -S flag */
int     mapped_writes = 1;	      /* -W flag disables */
int 	mapped_reads = 1;		/* -R flag disables it */
int	fsxgoodfd = 0;
FILE *	fsxlogf = NULL;
int badoff = -1;
int closeopen = 0;


void
vwarnc(code, fmt, ap)
	int code;
	const char *fmt;
	va_list ap;
{
	fprintf(stderr, "fsx: ");
	if (fmt != NULL) {
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, ": ");
	}
	fprintf(stderr, "%s\n", strerror(code));
}

void
warn(const char * fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vwarnc(errno, fmt, ap);
	va_end(ap);
}

void
simple_err(const char *msg, int err)
{
    fprintf(stderr, "%s: %s", msg, strerror(-err));
}

void
prt(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	va_end(args);

	if (fsxlogf) {
		va_start(args, fmt);
		vfprintf(fsxlogf, fmt, args);
		va_end(args);
	}
}

void
prterr(char *prefix)
{
	prt("%s%s%s\n", prefix, prefix ? ": " : "", strerror(errno));
}

void
prterrcode(char *prefix, int code)
{
	prt("%s%s%s\n", prefix, prefix ? ": " : "", strerror(-code));
}

void
log4(int operation, int arg0, int arg1, int arg2)
{
	struct log_entry *le;

	le = &oplog[logptr];
	le->operation = operation;
	if (closeopen)
		le->operation = ~ le->operation;
	le->args[0] = arg0;
	le->args[1] = arg1;
	le->args[2] = arg2;
	logptr++;
	logcount++;
	if (logptr >= LOGSIZE)
		logptr = 0;
}


void
logdump(void)
{
	int	i, count, down;
	struct log_entry	*lp;

	prt("LOG DUMP (%d total operations):\n", logcount);
	if (logcount < LOGSIZE) {
		i = 0;
		count = logcount;
	} else {
		i = logptr;
		count = LOGSIZE;
	}
	for ( ; count > 0; count--) {
		int opnum;

		opnum = i+1 + (logcount/LOGSIZE)*LOGSIZE;
		prt("%d(%d mod 256): ", opnum, opnum%256);
		lp = &oplog[i];
		if ((closeopen = lp->operation < 0))
			lp->operation = ~ lp->operation;
			
		switch (lp->operation) {
		case OP_MAPREAD:
			prt("MAPREAD\t0x%x thru 0x%x\t(0x%x bytes)",
			    lp->args[0], lp->args[0] + lp->args[1] - 1,
			    lp->args[1]);
			if (badoff >= lp->args[0] && badoff <
						     lp->args[0] + lp->args[1])
				prt("\t***RRRR***");
			break;
		case OP_MAPWRITE:
			prt("MAPWRITE 0x%x thru 0x%x\t(0x%x bytes)",
			    lp->args[0], lp->args[0] + lp->args[1] - 1,
			    lp->args[1]);
			if (badoff >= lp->args[0] && badoff <
						     lp->args[0] + lp->args[1])
				prt("\t******WWWW");
			break;
		case OP_READ:
			prt("READ\t0x%x thru 0x%x\t(0x%x bytes)",
			    lp->args[0], lp->args[0] + lp->args[1] - 1,
			    lp->args[1]);
			if (badoff >= lp->args[0] &&
			    badoff < lp->args[0] + lp->args[1])
				prt("\t***RRRR***");
			break;
		case OP_WRITE:
			prt("WRITE\t0x%x thru 0x%x\t(0x%x bytes)",
			    lp->args[0], lp->args[0] + lp->args[1] - 1,
			    lp->args[1]);
			if (lp->args[0] > lp->args[2])
				prt(" HOLE");
			else if (lp->args[0] + lp->args[1] > lp->args[2])
				prt(" EXTEND");
			if ((badoff >= lp->args[0] || badoff >=lp->args[2]) &&
			    badoff < lp->args[0] + lp->args[1])
				prt("\t***WWWW");
			break;
		case OP_TRUNCATE:
			down = lp->args[0] < lp->args[1];
			prt("TRUNCATE %s\tfrom 0x%x to 0x%x",
			    down ? "DOWN" : "UP", lp->args[1], lp->args[0]);
			if (badoff >= lp->args[!down] &&
			    badoff < lp->args[!!down])
				prt("\t******WWWW");
			break;
		case OP_SKIPPED:
			prt("SKIPPED (no operation)");
			break;
		default:
			prt("BOGUS LOG ENTRY (operation code = %d)!",
			    lp->operation);
		}
		if (closeopen)
			prt("\n\t\tCLOSE/OPEN");
		prt("\n");
		i++;
		if (i == LOGSIZE)
			i = 0;
	}
}


void
save_buffer(char *buffer, off_t bufferlength, int image)
{
	off_t ret;
	ssize_t byteswritten;

	if (image <= 0 || bufferlength == 0)
		return;

	if (bufferlength > SSIZE_MAX) {
		prt("fsx flaw: overflow in save_buffer\n");
		exit(67);
	}
	if (lite) {
		off_t size_by_seek = lseek(image, (off_t)0, SEEK_END);
		if (size_by_seek == (off_t)-1)
			prterr("save_buffer: lseek eof");
		else if (bufferlength > size_by_seek) {
			warn("save_buffer: .fsxgood file too short... will save 0x%llx bytes instead of 0x%llx\n", (unsigned long long)size_by_seek,
			     (unsigned long long)bufferlength);
			bufferlength = size_by_seek;
		}
	}

	ret = lseek(image, (off_t)0, SEEK_SET);
	if (ret == (off_t)-1)
		prterr("save_buffer: lseek 0");
	
	byteswritten = write(image, buffer, (size_t)bufferlength);
	if (byteswritten != bufferlength) {
		if (byteswritten == -1)
			prterr("save_buffer write");
		else
			warn("save_buffer: short write, 0x%x bytes instead of 0x%llx\n",
			     (unsigned)byteswritten,
			     (unsigned long long)bufferlength);
	}
}


void
report_failure(int status)
{
	logdump();
	
	if (fsxgoodfd) {
		if (good_buf) {
			save_buffer(good_buf, file_size, fsxgoodfd);
			prt("Correct content saved for comparison\n");
			prt("(maybe hexdump \"%s\" vs \"%s.fsxgood\")\n",
			    iname, iname);
		}
		close(fsxgoodfd);
	}
	exit(status);
}


#define short_at(cp) ((unsigned short)((*((unsigned char *)(cp)) << 8) | \
					*(((unsigned char *)(cp)) + 1)))

void
check_buffers(unsigned offset, unsigned size)
{
	unsigned char c, t;
	unsigned i = 0;
	unsigned n = 0;
	unsigned op = 0;
	unsigned bad = 0;

	if (memcmp(good_buf + offset, temp_buf, size) != 0) {
		prt("READ BAD DATA: offset = 0x%x, size = 0x%x\n",
		    offset, size);
		prt("OFFSET\tGOOD\tBAD\tRANGE\n");
		while (size > 0) {
			c = good_buf[offset];
			t = temp_buf[i];
			if (c != t) {
				if (n == 0) {
					bad = short_at(&temp_buf[i]);
					prt("0x%5x\t0x%04x\t0x%04x", offset,
					    short_at(&good_buf[offset]), bad);
					op = temp_buf[offset & 1 ? i+1 : i];
				}
				n++;
				badoff = offset;
			}
			offset++;
			i++;
			size--;
		}
		if (n) {
			prt("\t0x%5x\n", n);
			if (bad)
				prt("operation# (mod 256) for the bad data may be %u\n", ((unsigned)op & 0xff));
			else
				prt("operation# (mod 256) for the bad data unknown, check HOLE and EXTEND ops\n");
		} else
			prt("????????????????\n");
		report_failure(110);
	}
}


void
check_size(void)
{
	rbd_image_info_t statbuf;
	int ret;

	if ((ret = rbd_stat(image, &statbuf, sizeof(statbuf))) < 0) {
		prterrcode("check_size: fstat", ret);
	}
	if (file_size != statbuf.size) {
		prt("Size error: expected 0x%llx stat 0x%llx\n",
		    (unsigned long long)file_size,
		    (unsigned long long)statbuf.size);
		report_failure(120);
	}
}


void
check_trunc_hack(void)
{
	rbd_image_info_t statbuf;

	rbd_resize(image, (off_t)0);
	rbd_resize(image, (off_t)100000);
	rbd_stat(image, &statbuf, sizeof(statbuf));
	if (statbuf.size != (off_t)100000) {
		prt("no extend on truncate! not posix!\n");
		exit(130);
	}
	rbd_resize(image, (off_t)0);
}


int
create_image()
{
	int r;
	int order = 0;
	r = rados_create(&cluster, NULL);
	if (r < 0) {
		simple_err("Could not create cluster handle", r);
		return r;
	}
	rados_conf_parse_env(cluster, NULL);
	r = rados_conf_read_file(cluster, NULL);
	if (r < 0) {
		simple_err("Error reading ceph config file", r);
		goto failed_shutdown;
	}
	r = rados_connect(cluster);
	if (r < 0) {
		simple_err("Error connecting to cluster", r);
		goto failed_shutdown;
	}
	r = rados_pool_create(cluster, pool);
	if (r < 0) {
		simple_err("Error creating pool", r);
		goto failed_shutdown;
	}
	r = rados_ioctx_create(cluster, pool, &ioctx);
	if (r < 0) {
		simple_err("Error creating ioctx", r);
		goto failed_pool;
	}
	r = rbd_create(ioctx, iname, 0, &order);
	if (r < 0) {
		simple_err("Error creating image", r);
		goto failed_open;
	}

	return 0;

 failed_open:
	rados_ioctx_destroy(ioctx);
 failed_pool:
	rados_pool_delete(cluster, pool);
 failed_shutdown:
	rados_shutdown(cluster);
	return r;
}

void delete_image()
{
	rbd_remove(ioctx, iname);
	rados_ioctx_destroy(ioctx);
	rados_pool_delete(cluster, pool);
	rados_shutdown(cluster);
}

void cleanup_image()
{
	rbd_close(image);
	delete_image();
}

void
doread(unsigned offset, unsigned size)
{
	int ret;

	offset -= offset % readbdy;
	if (size == 0) {
		if (!quiet && testcalls > simulatedopcount)
			prt("skipping zero size read\n");
		log4(OP_SKIPPED, OP_READ, offset, size);
		return;
	}
	if (size + offset > file_size) {
		if (!quiet && testcalls > simulatedopcount)
			prt("skipping seek/read past end of file\n");
		log4(OP_SKIPPED, OP_READ, offset, size);
		return;
	}

	log4(OP_READ, offset, size, 0);

	if (testcalls <= simulatedopcount)
		return;

	if (!quiet && ((progressinterval &&
			testcalls % progressinterval == 0) ||
		       (debug &&
			(monitorstart == -1 ||
			 (offset + size > monitorstart &&
			  (monitorend == -1 || offset <= monitorend))))))
		prt("%lu read\t0x%x thru\t0x%x\t(0x%x bytes)\n", testcalls,
		    offset, offset + size - 1, size);
	ret = rbd_read(image, offset, size, temp_buf);
	if (ret != size) {
		if (ret < 0)
			prterrcode("doread: read", ret);
		else
			prt("short read: 0x%x bytes instead of 0x%x\n",
			    ret, size);
		report_failure(141);
	}
	check_buffers(offset, size);
}

void
check_eofpage(char *s, unsigned offset, char *p, int size)
{
	uintptr_t last_page, should_be_zero;

	if (offset + size <= (file_size & ~page_mask))
		return;
	/*
	 * we landed in the last page of the file
	 * test to make sure the VM system provided 0's 
	 * beyond the true end of the file mapping
	 * (as required by mmap def in 1996 posix 1003.1)
	 */
	last_page = ((uintptr_t)p + (offset & page_mask) + size) & ~page_mask;

	for (should_be_zero = last_page + (file_size & page_mask);
	     should_be_zero < last_page + page_size;
	     should_be_zero++)
		if (*(char *)should_be_zero) {
			prt("Mapped %s: non-zero data past EOF (0x%llx) page offset 0x%x is 0x%04x\n",
			    s, file_size - 1, should_be_zero & page_mask,
			    short_at(should_be_zero));
			report_failure(205);
		}
}


void
gendata(char *original_buf, char *good_buf, unsigned offset, unsigned size)
{
	while (size--) {
		good_buf[offset] = testcalls % 256; 
		if (offset % 2)
			good_buf[offset] += original_buf[offset];
		offset++;
	}
}


void
dowrite(unsigned offset, unsigned size)
{
	ssize_t ret;
	off_t newsize;

	offset -= offset % writebdy;
	if (size == 0) {
		if (!quiet && testcalls > simulatedopcount)
			prt("skipping zero size write\n");
		log4(OP_SKIPPED, OP_WRITE, offset, size);
		return;
	}

	log4(OP_WRITE, offset, size, file_size);

	gendata(original_buf, good_buf, offset, size);
	if (file_size < offset + size) {
		newsize = ceil(((double)offset + size) / truncbdy) * truncbdy;
		if (file_size < newsize)
			memset(good_buf + file_size, '\0', newsize - file_size);
		file_size = newsize;
		if (lite) {
			warn("Lite file size bug in fsx!");
			report_failure(149);
		}
		ret = rbd_resize(image, newsize);
		if (ret < 0) {
			prterrcode("dowrite: resize", ret);
			report_failure(150);
		} else {
			prt("resized to 0x%x ", newsize);
		}
	}

	if (testcalls <= simulatedopcount)
		return;

	if (!quiet && ((progressinterval &&
			testcalls % progressinterval == 0) ||
		       (debug &&
			(monitorstart == -1 ||
			 (offset + size > monitorstart &&
			  (monitorend == -1 || offset <= monitorend))))))
		prt("%lu write\t0x%x thru\t0x%x\t(0x%x bytes)\n", testcalls,
		    offset, offset + size - 1, size);

	ret = rbd_write(image, offset, size, good_buf + offset);
	if (ret != size) {
		if (ret < 0)
			prterrcode("dowrite: rbd_write", ret);
		else
			prt("short write: 0x%x bytes instead of 0x%x\n",
			    ret, size);
		report_failure(151);
	}
}


void
dotruncate(unsigned size)
{
	int oldsize = file_size;
	int ret;

	size -= size % truncbdy;
	if (size > biggest) {
		biggest = size;
		if (!quiet && testcalls > simulatedopcount)
			prt("truncating to largest ever: 0x%x\n", size);
	}

	log4(OP_TRUNCATE, size, (unsigned)file_size, 0);

	if (size > file_size)
		memset(good_buf + file_size, '\0', size - file_size);
	else if (size < file_size)
		memset(good_buf + size, '\0', file_size - size);
	file_size = size;

	if (testcalls <= simulatedopcount)
		return;
	
	if ((progressinterval && testcalls % progressinterval == 0) ||
	    (debug && (monitorstart == -1 || monitorend == -1 ||
		       size <= monitorend)))
		prt("%lu trunc\tfrom 0x%x to 0x%x\n", testcalls, oldsize, size);
	if ((ret = rbd_resize(image, size)) < 0) {
		prt("rbd_resize: %x\n", size);
		prterrcode("dotruncate: ftruncate", ret);
		report_failure(160);
	}
}


void
writefileimage()
{
	ssize_t ret;

	ret = rbd_write(image, 0, file_size, good_buf);
	if (ret != file_size) {
		if (ret < 0)
			prterrcode("writefileimage: write", ret);
		else
			prt("short write: 0x%x bytes instead of 0x%llx\n",
			    ret, (unsigned long long)file_size);
		report_failure(172);
	}
	if (lite ? 0 : (ret = rbd_resize(image, file_size)) < 0) {
		prt("rbd_resize: %llx\n", (unsigned long long)file_size);
		prterrcode("writefileimage: rbd_resize", ret);
		report_failure(173);
	}
}


void
docloseopen(void)
{
	int ret;
	if (testcalls <= simulatedopcount)
		return;

	if (debug)
		prt("%lu close/open\n", testcalls);
	if ((ret = rbd_close(image)) < 0) {
		prterrcode("docloseopen: close", ret);
		report_failure(180);
	}
	ret = rbd_open(ioctx, iname, &image, NULL);
	if (ret < 0) {
		prterrcode("docloseopen: open", ret);
		report_failure(181);
	}
}


void
test(void)
{
	unsigned long	offset;
	unsigned long	size = maxoplen;
	unsigned long	rv = random();
	unsigned long	op = rv % (3 + !lite + mapped_writes);

	/* turn off the map read if necessary */

	if (op == 2 && !mapped_reads)
	    op = 0;

	if (simulatedopcount > 0 && testcalls == simulatedopcount)
		writefileimage();

	testcalls++;

	if (closeprob)
		closeopen = (rv >> 3) < (1 << 28) / closeprob;

	if (debugstart > 0 && testcalls >= debugstart)
		debug = 1;

	if (!quiet && testcalls < simulatedopcount && testcalls % 100000 == 0)
		prt("%lu...\n", testcalls);

	/*
	 * READ:	op = 0
	 * WRITE:	op = 1
	 * MAPREAD:     op = 2
	 * TRUNCATE:	op = 3
	 * MAPWRITE:    op = 3 or 4
	 */
	if (lite ? 0 : op == 3 && style == 0) /* vanilla truncate? */
		dotruncate(random() % maxfilelen);
	else {
		if (randomoplen)
			size = random() % (maxoplen+1);
		if (lite ? 0 : op == 3)
			dotruncate(size);
		else {
			offset = random();
			if (op == 1 || op == (lite ? 3 : 4)) {
				offset %= maxfilelen;
				if (offset + size > maxfilelen)
					size = maxfilelen - offset;
				if (op != 1)
					exit(182);
				else
					dowrite(offset, size);
			} else {
				if (file_size)
					offset %= file_size;
				else
					offset = 0;
				if (offset + size > file_size)
					size = file_size - offset;
				if (op != 0)
					exit(183);
				else
					doread(offset, size);
			}
		}
	}
	if (sizechecks && testcalls > simulatedopcount)
		check_size();
	if (closeopen)
		docloseopen();
}


void
cleanup(sig)
	int	sig;
{
	if (sig)
		prt("signal %d\n", sig);
	prt("testcalls = %lu\n", testcalls);
	exit(sig);
}


void
usage(void)
{
	fprintf(stdout, "usage: %s",
		"fsx [-dnqLOW] [-b opnum] [-c Prob] [-l flen] [-m start:end] [-o oplen] [-p progressinterval] [-r readbdy] [-s style] [-t truncbdy] [-w writebdy] [-D startingop] [-N numops] [-P dirpath] [-S seed] iname\n\
	-b opnum: beginning operation number (default 1)\n\
	-c P: 1 in P chance of file close+open at each op (default infinity)\n\
	-d: debug output for all operations\n\
	-l flen: the upper bound on file size (default 262144)\n\
	-m startop:endop: monitor (print debug output) specified byte range (default 0:infinity)\n\
	-n: no verifications of file size\n\
	-o oplen: the upper bound on operation size (default 65536)\n\
	-p progressinterval: debug output at specified operation interval\n\
	-q: quieter operation\n\
	-r readbdy: 4096 would make reads page aligned (default 1)\n\
	-s style: 1 gives smaller truncates (default 0)\n\
	-t truncbdy: 4096 would make truncates page aligned (default 1)\n\
	-w writebdy: 4096 would make writes page aligned (default 1)\n\
	-D startingop: debug output starting at specified operation\n\
	-L: fsxLite - no file creations & no file size changes\n\
	-N numops: total # operations to do (default infinity)\n\
	-O: use oplen (see -o flag) for every op (default random)\n\
	-P dirpath: save .fsxlog and .fsxgood files in dirpath (default ./)\n\
	-S seed: for random # generator (default 1) 0 gets timestamp\n\
	-W: mapped write operations DISabled\n\
	-R: mapped read operations DISabled)\n\
	poolname: this is REQUIRED (no default)\n\
	imagename: this is REQUIRED (no default)\n");
	exit(89);
}


int
getnum(char *s, char **e)
{
	int ret = -1;

	*e = (char *) 0;
	ret = strtol(s, e, 0);
	if (*e)
		switch (**e) {
		case 'b':
		case 'B':
			ret *= 512;
			*e = *e + 1;
			break;
		case 'k':
		case 'K':
			ret *= 1024;
			*e = *e + 1;
			break;
		case 'm':
		case 'M':
			ret *= 1024*1024;
			*e = *e + 1;
			break;
		case 'w':
		case 'W':
			ret *= 4;
			*e = *e + 1;
			break;
		}
	return (ret);
}

int
main(int argc, char **argv)
{
	int	i, ch, ret;
	char	*endp;
	char goodfile[1024];
	char logfile[1024];

	goodfile[0] = 0;
	logfile[0] = 0;

	page_size = getpagesize();
	page_mask = page_size - 1;

	setvbuf(stdout, (char *)0, _IOLBF, 0); /* line buffered stdout */

	while ((ch = getopt(argc, argv, "b:c:dl:m:no:p:qr:s:t:w:D:LN:OP:RS:W"))
	       != -1)
		switch (ch) {
		case 'b':
			simulatedopcount = getnum(optarg, &endp);
			if (!quiet)
				fprintf(stdout, "Will begin at operation %ld\n",
					simulatedopcount);
			if (simulatedopcount == 0)
				usage();
			simulatedopcount -= 1;
			break;
		case 'c':
			closeprob = getnum(optarg, &endp);
			if (!quiet)
				fprintf(stdout,
					"Chance of close/open is 1 in %d\n",
					closeprob);
			if (closeprob <= 0)
				usage();
			break;
		case 'd':
			debug = 1;
			break;
		case 'l':
			maxfilelen = getnum(optarg, &endp);
			if (maxfilelen <= 0)
				usage();
			break;
		case 'm':
			monitorstart = getnum(optarg, &endp);
			if (monitorstart < 0)
				usage();
			if (!endp || *endp++ != ':')
				usage();
			monitorend = getnum(endp, &endp);
			if (monitorend < 0)
				usage();
			if (monitorend == 0)
				monitorend = -1; /* aka infinity */
			debug = 1;
		case 'n':
			sizechecks = 0;
			break;
		case 'o':
			maxoplen = getnum(optarg, &endp);
			if (maxoplen <= 0)
				usage();
			break;
		case 'p':
			progressinterval = getnum(optarg, &endp);
			break;
		case 'q':
			quiet = 1;
			break;
		case 'r':
			readbdy = getnum(optarg, &endp);
			if (readbdy <= 0)
				usage();
			break;
		case 's':
			style = getnum(optarg, &endp);
			if (style < 0 || style > 1)
				usage();
			break;
		case 't':
			truncbdy = getnum(optarg, &endp);
			if (truncbdy <= 0)
				usage();
			break;
		case 'w':
			writebdy = getnum(optarg, &endp);
			if (writebdy <= 0)
				usage();
			break;
		case 'D':
			debugstart = getnum(optarg, &endp);
			if (debugstart < 1)
				usage();
			break;
		case 'L':
			lite = 1;
			prt("lite mode not supported for rbd");
			exit(1);
			break;
		case 'N':
			numops = getnum(optarg, &endp);
			if (numops < 0)
				usage();
			break;
		case 'O':
			randomoplen = 0;
			break;
		case 'P':
			strncpy(goodfile, optarg, sizeof(goodfile));
			strcat(goodfile, "/");
			strncpy(logfile, optarg, sizeof(logfile));
			strcat(logfile, "/");
			break;
		case 'R':
			mapped_reads = 0;
			break;
		case 'S':
			seed = getnum(optarg, &endp);
			if (seed == 0)
				seed = time(0) % 10000;
			if (!quiet)
				fprintf(stdout, "Seed set to %d\n", seed);
			if (seed < 0)
				usage();
			break;
		case 'W':
			mapped_writes = 0;
			if (!quiet)
				fprintf(stdout, "mapped writes DISABLED\n");
			break;

		default:
			usage();
			/* NOTREACHED */
		}
	argc -= optind;
	argv += optind;
	if (argc != 2)
		usage();
	pool = argv[0];
	iname = argv[1];

	signal(SIGHUP,	cleanup);
	signal(SIGINT,	cleanup);
	signal(SIGPIPE,	cleanup);
	signal(SIGALRM,	cleanup);
	signal(SIGTERM,	cleanup);
	signal(SIGXCPU,	cleanup);
	signal(SIGXFSZ,	cleanup);
	signal(SIGVTALRM,	cleanup);
	signal(SIGUSR1,	cleanup);
	signal(SIGUSR2,	cleanup);

	initstate(seed, state, 256);
	setstate(state);

	ret = create_image();
	if (ret < 0) {
		prterrcode(iname, ret);
		exit(90);
	}
	ret = rbd_open(ioctx, iname, &image, NULL);
	if (ret < 0) {
		simple_err("Error opening image", ret);
		delete_image();
		exit(91);
	}
	atexit(cleanup_image);

	strncat(goodfile, iname, 256);
	strcat (goodfile, ".fsxgood");
	fsxgoodfd = open(goodfile, O_RDWR|O_CREAT|O_TRUNC, 0666);
	if (fsxgoodfd < 0) {
		prterr(goodfile);
		exit(92);
	}
	strncat(logfile, iname, 256);
	strcat (logfile, ".fsxlog");
	fsxlogf = fopen(logfile, "w");
	if (fsxlogf == NULL) {
		prterr(logfile);
		exit(93);
	}
	original_buf = (char *) malloc(maxfilelen);
	for (i = 0; i < maxfilelen; i++)
		original_buf[i] = random() % 256;
	good_buf = (char *) malloc(maxfilelen);
	memset(good_buf, '\0', maxfilelen);
	temp_buf = (char *) malloc(maxoplen);
	memset(temp_buf, '\0', maxoplen);
	if (lite) {	/* zero entire existing file */
		ssize_t written;

		written = rbd_write(image, 0, (size_t)maxfilelen, good_buf);
		if (written != maxfilelen) {
			if (written < 0) {
				prterrcode(iname, written);
				warn("main: error on write");
			} else
				warn("main: short write, 0x%x bytes instead of 0x%x\n",
				     (unsigned)written, maxfilelen);
			exit(98);
		}
	} else 
		check_trunc_hack();

	while (numops == -1 || numops--)
		test();

	prt("All operations completed A-OK!\n");

	exit(0);
	return 0;
}
