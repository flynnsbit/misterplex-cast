// mplex_ddr — write-combine DDR bank window for MiSTerPlex present.
// Userspace PL330 poke is banned (kernel owns dma-pl330; DMAGO faults).
//
// 720p Option-C: banks at 0x30180000 stride 0x180000, doorbell 0x3047F000.
// 4MiB (0x400000) ended at 0x30400000 and clipped bank 1 (frame end 0x30451800).
// 8MiB covers both 720p banks + doorbell + PL330 ABI scratch.
#include <linux/version.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/pgtable.h>
#include <asm/io.h>
#include <linux/gfp.h>
#include <linux/ioctl.h>
#include <linux/mm.h>
#include <linux/types.h>

#define MPLEX_DDR_NAME "mplex_ddr"
#define MPLEX_DDR_PHYS 0x30000000UL
#define MPLEX_DDR_SIZE 0x800000UL
#define MPLEX_CACHED_ORDER 9 /* 2 MiB */
#define MPLEX_CACHED_SLOT (PAGE_SIZE << MPLEX_CACHED_ORDER)
#define MPLEX_CACHED_PGOFF 0x800UL
#define MPLEX_IOC_CACHED_PHYS _IOR('M', 1, struct mplex_cached_phys)
/* K1 (k1-kmod-wc-window-note-1004): R1 host idle FB, IDLE0 0x31600000 + IDLE1 0x31700000 (RGB24 640x480,
 * stride 1920), one 2 MiB WC window keyed by its own pgoff = (phys - MPLEX_DDR_PHYS) >> PAGE_SHIFT = 0x1600.
 * The offset-0 window and the cached slots are unchanged. host/libmisterplex/mplex_ddr_abi.hpp mirrors these
 * (tests/unit/test_mplex_abi_sync.sh compares them). */
#define MPLEX_IDLEFB_PHYS 0x31600000UL
#define MPLEX_IDLEFB_SIZE 0x200000UL
#define MPLEX_IDLEFB_PGOFF ((MPLEX_IDLEFB_PHYS - MPLEX_DDR_PHYS) >> PAGE_SHIFT)
#define MPLEX_CAP_IDLEFB_WC 0x1U
#define MPLEX_IOC_CAPS _IOR('M', 2, u32)

struct mplex_cached_phys {
	u32 phys[2];
	u32 slot_bytes;
};

static dev_t mplex_devt;
static struct class *mplex_class;
static struct cdev mplex_cdev;
static struct page *cached_page[2];
static unsigned long cached_phys[2];

static int mplex_open(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static int mplex_release(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static int mplex_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pfn = MPLEX_DDR_PHYS >> PAGE_SHIFT;

	(void)file;
	if (vma->vm_pgoff == MPLEX_CACHED_PGOFF) {
		if (size != 2 * MPLEX_CACHED_SLOT || !cached_page[0] || !cached_page[1])
			return -EINVAL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
		vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
#else
		vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
#endif
		if (remap_pfn_range(vma, vma->vm_start, page_to_pfn(cached_page[0]),
				    MPLEX_CACHED_SLOT, vma->vm_page_prot))
			return -EAGAIN;
		if (remap_pfn_range(vma, vma->vm_start + MPLEX_CACHED_SLOT,
				    page_to_pfn(cached_page[1]), MPLEX_CACHED_SLOT,
				    vma->vm_page_prot))
			return -EAGAIN;
		return 0;
	}
	/* K1-N2: the pgoff and the size are compared as given, before any shift or address arithmetic; a mapping
	 * must be shared and not executable. */
	if (vma->vm_pgoff == MPLEX_IDLEFB_PGOFF) {
		if (size != MPLEX_IDLEFB_SIZE || !(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
			return -EINVAL;
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
		vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
#else
		vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
#endif
		if (remap_pfn_range(vma, vma->vm_start, MPLEX_IDLEFB_PHYS >> PAGE_SHIFT, MPLEX_IDLEFB_SIZE,
				    vma->vm_page_prot))
			return -EAGAIN;
		return 0;
	}
	if (size > MPLEX_DDR_SIZE)
		return -EINVAL;
	if (vma->vm_pgoff != 0)
		return -EINVAL;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
#endif
	if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot))
		return -EAGAIN;
	return 0;
}

static long mplex_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mplex_cached_phys out;

	(void)file;
	if (cmd == MPLEX_IOC_CAPS) {
		u32 caps = MPLEX_CAP_IDLEFB_WC;

		return copy_to_user((void __user *)arg, &caps, sizeof(caps)) ? -EFAULT : 0;
	}
	if (cmd != MPLEX_IOC_CACHED_PHYS)
		return -ENOTTY;
	if (!cached_page[0] || !cached_page[1])
		return -ENOMEM;
	out.phys[0] = (u32)cached_phys[0];
	out.phys[1] = (u32)cached_phys[1];
	out.slot_bytes = (u32)MPLEX_CACHED_SLOT;
	if (copy_to_user((void __user *)arg, &out, sizeof(out)))
		return -EFAULT;
	return 0;
}

static const struct file_operations mplex_fops = {
	.owner = THIS_MODULE,
	.open = mplex_open,
	.release = mplex_release,
	.mmap = mplex_mmap,
	.unlocked_ioctl = mplex_ioctl,
};

static int __init mplex_ddr_init(void)
{
	int ret;
	int i;

	/* K1: the idle-FB key is disjoint from the offset-0 window (pgoff 0..0x7FF) and the cached slots
	 * (0x800..0xBFF), page aligned, and maps exactly IDLE0 + IDLE1. */
	BUILD_BUG_ON(MPLEX_IDLEFB_PHYS != MPLEX_DDR_PHYS + (MPLEX_IDLEFB_PGOFF << PAGE_SHIFT));
	BUILD_BUG_ON(MPLEX_IDLEFB_PGOFF < (MPLEX_DDR_SIZE >> PAGE_SHIFT));
	BUILD_BUG_ON(MPLEX_IDLEFB_PGOFF < MPLEX_CACHED_PGOFF + ((2 * MPLEX_CACHED_SLOT) >> PAGE_SHIFT));
	BUILD_BUG_ON(MPLEX_IDLEFB_PHYS & (PAGE_SIZE - 1));
	BUILD_BUG_ON(MPLEX_IDLEFB_SIZE & (PAGE_SIZE - 1));
	BUILD_BUG_ON(MPLEX_IDLEFB_SIZE != 2 * 0x100000UL);
	BUILD_BUG_ON(PAGE_SHIFT == 12 && MPLEX_IDLEFB_PGOFF != 0x1600UL);

	for (i = 0; i < 2; i++) {
		cached_page[i] = alloc_pages(GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN,
					     MPLEX_CACHED_ORDER);
		if (!cached_page[i]) {
			pr_warn("mplex_ddr: cached slot %d alloc_pages order=%d failed\n",
				i, MPLEX_CACHED_ORDER);
			break;
		}
		cached_phys[i] = page_to_phys(cached_page[i]);
	}
	if (i != 2) {
		while (--i >= 0) {
			__free_pages(cached_page[i], MPLEX_CACHED_ORDER);
			cached_page[i] = NULL;
			cached_phys[i] = 0;
		}
	}

	ret = alloc_chrdev_region(&mplex_devt, 0, 1, MPLEX_DDR_NAME);
	if (ret)
		return ret;
	cdev_init(&mplex_cdev, &mplex_fops);
	ret = cdev_add(&mplex_cdev, mplex_devt, 1);
	if (ret)
		goto err_cdev;
	mplex_class = class_create(THIS_MODULE, MPLEX_DDR_NAME);
	if (IS_ERR(mplex_class)) {
		ret = PTR_ERR(mplex_class);
		goto err_class;
	}
	if (IS_ERR(device_create(mplex_class, NULL, mplex_devt, NULL, MPLEX_DDR_NAME))) {
		ret = -EINVAL;
		goto err_dev;
	}
	pr_info("mplex_ddr: WC mmap phys=0x%lx size=0x%lx cached0=0x%lx cached1=0x%lx idlefb=0x%lx+0x%lx\n",
		MPLEX_DDR_PHYS, MPLEX_DDR_SIZE, cached_phys[0], cached_phys[1], MPLEX_IDLEFB_PHYS,
		MPLEX_IDLEFB_SIZE);
	return 0;
err_dev:
	class_destroy(mplex_class);
err_class:
	cdev_del(&mplex_cdev);
err_cdev:
	unregister_chrdev_region(mplex_devt, 1);
	return ret;
}

static void __exit mplex_ddr_exit(void)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (cached_page[i]) {
			__free_pages(cached_page[i], MPLEX_CACHED_ORDER);
			cached_page[i] = NULL;
			cached_phys[i] = 0;
		}
	}
	device_destroy(mplex_class, mplex_devt);
	class_destroy(mplex_class);
	cdev_del(&mplex_cdev);
	unregister_chrdev_region(mplex_devt, 1);
}

module_init(mplex_ddr_init);
module_exit(mplex_ddr_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("MiSTerPlex");
MODULE_DESCRIPTION("Write-combine DDR bank window for MiSTerPlex present");
MODULE_VERSION("1.1");
