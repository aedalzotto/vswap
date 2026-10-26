// SPDX-License-Identifier: GPL-2.0
/*
 * vswap: a RAM-disk-like block device (/dev/vswap0) backed by VRAM
 * meant for high-priority swap.
 */
#include <linux/blkdev.h>
#include <linux/dma-buf.h>
#include <linux/iosys-map.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/suspend.h>
#include <linux/vmalloc.h>
#include <drm/drm_cache.h>
#include "vswap_uapi.h"

#define MAX_CHUNKS 64

static unsigned int max_readers = 2;
module_param(max_readers, uint, 0444);
MODULE_PARM_DESC(max_readers, "concurrent VRAM readers; BAR reads can lose throughput past 2");

static void vswap_submit_bio(struct bio *bio);
static long vswap_ioctl(struct file *f, unsigned int cmd, unsigned long arg);
static int vswap_pm(struct notifier_block *nb, unsigned long action, void *data);

static const struct block_device_operations vswap_fops = {
	.owner = THIS_MODULE,
	.submit_bio = vswap_submit_bio,
};

static const struct file_operations ctl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = vswap_ioctl,
};

static struct miscdevice ctl_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vswap-ctl",
	.fops = &ctl_fops,
	.mode = 0600,
};

static struct notifier_block pm_nb = {
	.notifier_call = vswap_pm
};

static struct {
	struct dma_buf *buf;
	struct iosys_map pin;	/* dma_buf_vmap, held only to keep the buffer in place */
	struct iosys_map wc;	/* our page-by-page write-combined mapping */
} chunks[MAX_CHUNKS];

static DEFINE_MUTEX(ctl_lock);	/* attach/release/suspend */

static size_t chunk_size;
static unsigned int nchunks;

static struct gendisk *disk;
static struct semaphore readers;

/* Copies one bio segment (at most one page) between its RAM page and the chunk that holds it */
static int vswap_copy_seg(struct bio_vec *bv, sector_t sector, bool write)
{
	u64 pos = (u64)sector << SECTOR_SHIFT;
	size_t off = pos % chunk_size;

	/* the queue limits should make this impossible: never copy out of the chunk */
	if (WARN_ON_ONCE(off + bv->bv_len > chunk_size))
		return -EIO;

	/* VRAM side: the chunk's WC mapping plus the offset; RAM side: the bio's page */
	struct iosys_map src = chunks[pos / chunk_size].wc, dst;
	void *mem = bvec_kmap_local(bv);
	iosys_map_incr(&src, off);

	if (write) {
		memcpy_toio(src.vaddr_iomem, mem, bv->bv_len);
	} else {
		iosys_map_set_vaddr(&dst, mem);
		drm_memcpy_from_wc(&dst, &src, bv->bv_len);	/* plain reads are ~20x slower */
	}

	kunmap_local(mem);

	return 0;
}

/* Runs in the submitter's context and completes the bio before returning (BLK_FEAT_SYNCHRONOUS) */
static void vswap_submit_bio(struct bio *bio)
{
	switch (bio_op(bio)) {
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		break;
	case REQ_OP_DISCARD:	/* nothing to free: the VRAM stays ours */
		bio_endio(bio);
		return;
	default:
		bio_io_error(bio);
		return;
	}

	bool write = op_is_write(bio_op(bio));
	/* reads only: more concurrent BAR readers lose throughput; WC writes don't */
	if (!write)
		down(&readers);

	sector_t sector = bio->bi_iter.bi_sector;

	struct bio_vec bv;
	struct bvec_iter iter;
	bio_for_each_segment(bv, bio, iter) {
		if (vswap_copy_seg(&bv, sector, write)) {
			bio->bi_status = BLK_STS_IOERR;
			break;
		}
		sector += bv.bv_len >> SECTOR_SHIFT;
	}

	if (write)
		wmb();	/* drain the write-combining buffers before we report success */
	else
		up(&readers);

	bio_endio(bio);
}

static int vswap_create_disk(void)
{
	int err = 0;

	struct queue_limits lim = {
		.logical_block_size = PAGE_SIZE,
		.physical_block_size = PAGE_SIZE,
		.io_min = PAGE_SIZE,
		.dma_alignment = PAGE_SIZE - 1,	/* O_DIRECT buffers must be page-aligned too */
		.max_hw_discard_sectors = UINT_MAX,
		.features = BLK_FEAT_SYNCHRONOUS	/* I/O completes inside submit_bio; swapon then sets SWP_SYNCHRONOUS_IO and swap-in skips the swap cache */
	};

	disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(disk)) {
		err = PTR_ERR(disk);
		disk = NULL;
		return err;
	}

	disk->flags |= GENHD_FL_NO_PART;
	disk->fops = &vswap_fops;
	strscpy(disk->disk_name, "vswap0");
	err = add_disk(disk);
	if (err) {
		put_disk(disk);
		disk = NULL;
	}
	return err;
}

static void __iomem *vswap_map_user_frames(struct dma_buf *buf, unsigned long addr)
{
	int err = 0;

	unsigned int n = (buf->size >> PAGE_SHIFT);
	unsigned long *pfns = kvmalloc_array(n, sizeof(*pfns), GFP_KERNEL);
	if (pfns == NULL)
		return ERR_PTR(-ENOMEM);

	/* Lock the page tables */
	struct mm_struct *mm = current->mm;
	mmap_read_lock(mm);

	/* Look up the VMA */
	struct vm_area_struct *vma = vma_lookup(mm, addr);
	if (
		   (vma == NULL)
		|| (vma->vm_file != buf->file) 		/* not an mmap of this dma-buf */
		|| (vma->vm_start != addr)	   		/* does not start at the buffer's start */
		|| (vma->vm_end - addr < buf->size) /* does not cover the whole buffer */
	) {
		pr_err("vswap: addr is not an mmap of the whole dma-buf\n");
		err = -EINVAL;
		goto out;
	}

	/* For each page in the buffer */
	for (unsigned int i = 0; i < n; i++) {
		struct follow_pfnmap_args args = {
			.vma = vma,
			.address = addr + ((unsigned long)i << PAGE_SHIFT)
		};

		/* Try to follow the PFN map */
		if (follow_pfnmap_start(&args)) {
			/* not faulted in yet, force a page fault and try again */
			err = fixup_user_fault(mm, args.address, 0, NULL);
			if (err == 0)
				err = follow_pfnmap_start(&args);
			if (err != 0)
				goto out;
		}

		/* Record the frame and drop the page table lock */
		pfns[i] = args.pfn;
		follow_pfnmap_end(&args);

		if (pfn_valid(pfns[i])) {
			/* has a struct page: system RAM, not the BAR */
			/* is_iomem check should cover this, but let's be sure */
			pr_err("vswap: page %u of the dma-buf is in RAM, refusing\n", i);
			err = -EMEDIUMTYPE;
			goto out;
		}
	}

	/* Map the PFNs into a write-combined virtual address */
	void *vaddr = vmap_pfn(pfns, n, pgprot_writecombine(PAGE_KERNEL));
	if (vaddr == NULL)
		err = -ENOMEM;

out:
	mmap_read_unlock(mm);
	kvfree(pfns);
	return err ? ERR_PTR(err) : (void __force __iomem *)vaddr;
}

static int vswap_attach(struct vswap_attach *a)
{
	int err = 0;

	if (nchunks == MAX_CHUNKS)
		return -ENOSPC;

	/* Getting the reference preserves the buffer. Tool can exit. */
	struct dma_buf *buf = dma_buf_get(a->fd);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (
		   (buf->size == 0)
		|| (buf->size % PAGE_SIZE != 0) /* not a whole number of pages */
		|| ((nchunks != 0) && (buf->size != chunk_size)) /* every chunk must match the first */
	) {
		err = -EINVAL;
		goto put;
	}

	/* Pin the buffer where it is. This mapping is never used for I/O (TTM maps it as one contiguous, cached range); see vswap_map_user_frames() */
	struct iosys_map map;
	err = dma_buf_vmap_unlocked(buf, &map);
	if (err)
		goto put;

	if (!map.is_iomem) {	/* the driver put it in RAM */
		pr_err("vswap: dma-buf from %s is not in VRAM, refusing\n", buf->exp_name);
		err = -EMEDIUMTYPE;
		goto unmap;
	}

	/* Get contiguous write-combined mapping for the buffer */
	void __iomem *wc = vswap_map_user_frames(buf, a->addr);
	if (IS_ERR(wc)) {
		err = PTR_ERR(wc);
		goto unmap;
	}

	/* First chunk since load or since the last release: create /dev/vswap0 */
	if (disk == NULL) {
		err = vswap_create_disk();
		if (err)
			goto unmap_wc;
	}

	/* Record the chunk: size (set by the first), dma-buf reference, pin and WC mapping */
	chunk_size = buf->size;
	chunks[nchunks].buf = buf;
	chunks[nchunks].pin = map;
	iosys_map_set_vaddr_iomem(&chunks[nchunks].wc, wc);
	nchunks++;

	/* Grow the disk only now: the block layer rejects I/O past the capacity, so no bio can reach a chunk before it is recorded */
	set_capacity_and_notify(disk, (u64)nchunks * chunk_size >> SECTOR_SHIFT);
	pr_info("vswap: %u MiB of VRAM from %s\n", (unsigned int)((u64)nchunks * chunk_size >> 20), buf->exp_name);

	return 0;

unmap_wc:
	vunmap((void __force *)wc);
unmap:
	dma_buf_vunmap_unlocked(buf, &map);
put:
	dma_buf_put(buf);
	return err;
}

/* Drops /dev/vswap0 and gives every chunk back to its exporter; -EBUSY while swap is on */
static int vswap_release(void)
{
	if (disk == NULL)
		return 0;

	mutex_lock(&disk->open_mutex);
	if (disk_openers(disk)) {	/* swapon holds it open */
		mutex_unlock(&disk->open_mutex);
		return -EBUSY;
	}
	mutex_unlock(&disk->open_mutex);

	/* del_gendisk drains in-flight bios and rejects new ones, so unmapping below is safe */
	del_gendisk(disk);
	put_disk(disk);
	disk = NULL;

	while (nchunks--) {
		vunmap((void __force *)chunks[nchunks].wc.vaddr_iomem);
		dma_buf_vunmap_unlocked(chunks[nchunks].buf, &chunks[nchunks].pin);
		dma_buf_put(chunks[nchunks].buf);
	}
	nchunks = 0;

	pr_info("vswap: VRAM released\n");
	return 0;
}

static long vswap_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int err;
	struct vswap_attach a;

	switch (cmd) {
	case VSWAP_ATTACH:
		if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
			return -EFAULT;
		mutex_lock(&ctl_lock);
		err = vswap_attach(&a);
		mutex_unlock(&ctl_lock);
		return err;
	case VSWAP_RELEASE:
		mutex_lock(&ctl_lock);
		err = vswap_release();
		mutex_unlock(&ctl_lock);
		return err;
	default:
		break;
	}
	return -ENOTTY;
}

/* VRAM loses power in S3 and hibernation doesn't save it: refuse while we hold any */
static int vswap_pm(struct notifier_block *nb, unsigned long action, void *data)
{
	if (action != PM_SUSPEND_PREPARE && action != PM_HIBERNATION_PREPARE)
		return NOTIFY_DONE;

	mutex_lock(&ctl_lock);
	bool busy = (nchunks > 0);
	mutex_unlock(&ctl_lock);

	if (busy)
		pr_warn("vswap: refusing suspend, VRAM is mapped (swapoff + vswap release first)\n");

	return busy ? NOTIFY_BAD : NOTIFY_DONE;
}

static int __init vswap_init(void)
{
	sema_init(&readers, max(max_readers, 1u));

	int err = register_pm_notifier(&pm_nb);

	if (err)
		return err;

	err = misc_register(&ctl_dev);
	if (err)
		unregister_pm_notifier(&pm_nb);

	return err;
}

static void __exit vswap_exit(void)
{
	misc_deregister(&ctl_dev);
	/* cannot be busy: an open /dev/vswap0 or /dev/vswap-ctl holds a module reference */
	vswap_release();
	unregister_pm_notifier(&pm_nb);
}

module_init(vswap_init);
module_exit(vswap_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");
MODULE_DESCRIPTION("Swap on VRAM: a block device for swapon backed by GPU dma-bufs");
MODULE_IMPORT_NS("DMA_BUF");
