/*-
 * Copyright (c) 2013 Antti Kantee.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <bmk-core/errno.h>
#include <bmk-core/memalloc.h>
#include <bmk-core/printf.h>
#include <bmk-core/sched.h>
#include <bmk-core/string.h>

#include <bmk-rumpuser/core_types.h>
#include <bmk-rumpuser/rumpuser.h>

#define NBLKDEV 10
#define BLKFDOFF 64

#include <solo5.h>

#define XENBLK_MAGIC "XENBLK_"

static int sector_size = 512;

static struct blkdev {
	struct solo5_block_info blk_info;
	int blk_open;
	int blk_vbd;
} blkdevs[NBLKDEV];

/* not really bio-specific, but only touches this file for now */
int
rumprun_platform_rumpuser_init(void)
{
	for (int i = 0; i < NBLKDEV; i++) {
		blkdevs[i].blk_vbd = -1;
	}
	return 0;
}

static int
devopen(int num)
{
	//bmk_assert(num < NBLKDEV);
	struct blkdev *bd = &blkdevs[num];
	char buf[32];

	if (bd->blk_open) {
		bd->blk_open++;
		return 1;
	}
	bmk_snprintf(buf, sizeof(buf), "device/vbd/%d", bd->blk_vbd);

	solo5_block_info(num, &(bd->blk_info));

	bd->blk_open = 1;
	return 0;
	/*
	if (bd->blk_info != NULL) {
		bd->blk_open = 1;
		return 0;
	} else {
		return BMK_EIO; 
	}*/
}

/*
 * Translate block device spec into vbd id.
 * We parse up to "(hd|sd|xvd)[a-z][0-9]?", i.e. max 1 char disk/part.
 * (Feel free to improve)
 */
enum devtype { DEV_SD, DEV_HD, DEV_XVD };
#define XENBLK_MAGIC "XENBLK_"
static int
devname2vbd(const char *name)
{
	const char *dp;
	enum devtype dt;
	int disk, part;
	int vbd;

	/* Do not print anything for clearly incorrect paths */
	if (bmk_strncmp(name, XENBLK_MAGIC, sizeof(XENBLK_MAGIC)-1) != 0)
		return -1;
	name += sizeof(XENBLK_MAGIC)-1;

	/* which type of disk? */
	if (bmk_strncmp(name, "hd", 2) == 0) {
		dp = name+2;
		dt = DEV_HD;
	} else if (bmk_strncmp(name, "sd", 2) == 0) {
		dp = name+2;
		dt = DEV_SD;
	} else if (bmk_strncmp(name, "xvd", 3) == 0) {
		dp = name+3;
		dt = DEV_XVD;
	} else {
		bmk_printf("unsupported devtype %s\n", name);
		return -1;
	}
	if (bmk_strlen(dp) < 1 || bmk_strlen(dp) > 2) {
		bmk_printf("unsupported blkspec %s\n", name);
		return -1;
	}

	/* disk and partition */
	disk = *dp - 'a';
	dp++;
	if (*dp == '\0')
		part = 0;
	else
		part = *dp - '0';
	if (disk < 0 || part < 0 || part > 9) {
		bmk_printf("unsupported disk/partition %d %d\n", disk, part);
		return -1;
	}

	/* construct vbd based on disk type */
	switch (dt) {
	case DEV_HD:
		if (disk < 2) {
			vbd = (3<<8) | (disk<<6) | part;
		} else if (disk < 4) {
			vbd = (22<<8) | ((disk-2)<<6) | part;
		} else {
			goto err;
		}
		break;
	case DEV_SD:
		if (disk > 16 || part > 16)
			goto err;
		vbd = (8<<8) | (disk<<4) | part;
		break;
	case DEV_XVD:
		if (disk < 16)
			vbd = (202<<8) | (disk<<4) | part;
		else if (disk > 'z' - 'a')
			goto err;
		else
			vbd = (1<<28) | (disk<<8) | part;
		break;
	}

	return vbd;
 err:
	bmk_printf("unsupported disk/partition spec %s\n", name);
	return -1;
}


static int
devname2num(const char *name)
{
int vbd;
	int i;

	if ((vbd = devname2vbd(name)) == -1)
		return -1;

	/*
	 * We got a valid vbd.  Check if we know this one already, or
	 * if we need to reserve a new one.
	 */
	for (i = 0; i < NBLKDEV; i++) {
		if (vbd == blkdevs[i].blk_vbd)
			return i;
	}

	/*
	 * No such luck.  Reserve a new one
	 */
	for (i = 0; i < NBLKDEV; i++) {
		if (blkdevs[i].blk_vbd == -1) {
			/* i have you now */
			blkdevs[i].blk_vbd = vbd;
			bmk_printf("\nNew block device registered with vbd=%d as num=%d\n", vbd, i);
			return i;
		}
	}

	bmk_printf("blkdev table full.  Increase NBLKDEV\n");
	return -1;
	
}

int
rumpuser_open(const char *name, int mode, int *fdp)
{
        //struct solo5_block_info bi;
	int num, rv;

	if (bmk_strncmp(name, XENBLK_MAGIC, sizeof(XENBLK_MAGIC)-1) != 0)
		return -1;
	if ((mode & RUMPUSER_OPEN_BIO) == 0 || (num = devname2num(name)) == -1)
		return BMK_ENXIO;
	bmk_printf("rumpuser_open called with dev num=%d\n", num);
	
	if ((rv = devopen(num)) != 0)
		return rv;
	
        //solo5_block_info(num, &bi);
	//blkdevs[num].block_info = bi;
	//sector_size = blkdevs[num].block_size;
	*fdp = BLKFDOFF + num;
	return 0;
}

int
rumpuser_close(int fd)
{
	return 0;
}

int
rumpuser_getfileinfo(const char *name, uint64_t *size, int *type)
{
	struct blkdev *bd;
	int rv, num;

	if (bmk_strncmp(name, XENBLK_MAGIC, sizeof(XENBLK_MAGIC)-1) != 0)
		return -1;
	if ((num = devname2num(name)) == -1)
		return BMK_ENXIO;
	if ((rv = devopen(num)) != 0)
		return rv;

        //solo5_block_info(&bi);
	bd = &blkdevs[num];
	
        *size = bd->blk_info.capacity;
	*type = RUMPUSER_FT_BLK;

	return 0;
}


void
rumpuser_bio(int fd, int op, void *data, size_t dlen, int64_t off,
	rump_biodone_fn biodone, void *donearg)
{
	int len = (int)dlen;
	uint64_t curr_off;
	uint64_t d = (uint64_t)data;
	int num = fd - BLKFDOFF;

	if (len % sector_size != 0 || len == 0) {
		biodone(donearg, 0, BMK_EIO);
		return;
	}

	for (curr_off = off; len > 0; curr_off += sector_size,
					len -= sector_size,
					d += sector_size) {
		int ret;
		if (op & RUMPUSER_BIO_READ)
			ret = solo5_block_read(num, curr_off,
						(void *)d, sector_size);
		else
			ret = solo5_block_write(num, curr_off,
						(void *)d, sector_size);
		if (ret != 0) {
			biodone(donearg, curr_off - off, BMK_EIO);
			return;
		}
	}

	biodone(donearg, dlen, 0);
}
