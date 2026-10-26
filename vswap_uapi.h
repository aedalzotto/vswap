/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* ioctls on /dev/vswap-ctl, shared by vswap.ko and the vswap tool */
#ifndef VSWAP_UAPI_H
#define VSWAP_UAPI_H
#include <linux/ioctl.h>
#include <linux/types.h>

struct vswap_attach {
	__s32 fd;	/* dma-buf */
	__u32 pad;
	__u64 addr;	/* caller's MAP_SHARED mmap of the whole dma-buf, at offset 0 */
};

/**
 * Appends one dma-buf to /dev/vswap0, in call order.
 * The module keeps its own reference and reads the page frames out of the
 * caller's mapping, so the caller may unmap, close and exit afterwards.
 */
#define VSWAP_ATTACH  _IOW('k', 1, struct vswap_attach)

/* Drops /dev/vswap0 and every dma-buf; -EBUSY while swap is on. */
#define VSWAP_RELEASE _IO('k', 2)

#endif
