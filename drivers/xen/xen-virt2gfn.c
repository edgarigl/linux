/*
 * xen-virt2gfn.c
 */

#define pr_fmt(fmt) "xen:" KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/highmem.h>

#include <xen/xen.h>
#include <xen/page.h>
#include <xen/virt2gfn.h>

struct xv2g_file_private_data {
    int dummy;
};

static int xv2g_open(struct inode *inode, struct file *filp)
{
	struct xv2g_file_private_data *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		goto out_nomem;

	filp->private_data = priv;

	pr_debug("%s: priv %p\n", __func__, priv);

	return 0;

out_nomem:
	return -ENOMEM;
}

static int xv2g_release(struct inode *inode, struct file *filp)
{
	struct xv2g_file_private_data *priv = filp->private_data;

	pr_debug("%s: priv %p\n", __func__, priv);

	kfree(priv);

	return 0;
}

struct xv2g_info {
    struct page **pages;
    uint64_t pfn;
};

static long xen_virt2gfn(struct xv2g_file_private_data *priv,
        struct ioctl_xen_virt2gfn __user *arg)
{
    struct ioctl_xen_virt2gfn op;
    struct page *page;
    uint64_t pfn;
    void *virt;
    int rc = 0;
    struct xv2g_info r;
    struct vm_area_struct *vma;

    if (copy_from_user(&op, arg, sizeof(op))) {
        rc = -EFAULT;
        goto out;
    }

    if (op.count != 1) {
        rc = -EINVAL;
        goto out;
    }


    int     res;
    virt = (void *) op.addr[0];

    mmap_read_lock(current->mm);
    vma = find_vma(current->mm, op.addr[0]);
    if (IS_ERR(vma)) {
        mmap_read_unlock(current->mm);
        rc = -EFAULT;
        goto out;
    }

    r.pages = vma->vm_private_data;
    if (0) {
        res = get_user_pages_fast(op.addr[0], 1, 0, &page);
        printk("res=%d\n", res);
        if (res == 1) {
            pfn = page_to_pfn(page) << PAGE_SHIFT;
            put_page(page);
            printk("%s: virt %p page %p pfn %llx\n", __func__, virt, page, pfn);
            op.addr[0] = pfn;
        } else {
            rc = -EFAULT;
        }
    } else {
        uint64_t offset = op.addr[0] - vma->vm_start;

        offset >>= PAGE_SHIFT;
        page = r.pages[offset];
        if (page) {
            pfn = page_to_pfn(page) << PAGE_SHIFT;
            //printk("%s: virt %p page %p pfn %llx\n", __func__, virt, page, pfn);
            op.addr[0] = pfn;
        } else {
            rc = -EFAULT;
        }
    }
    mmap_read_unlock(current->mm);

    if (copy_to_user(arg, &op, sizeof(op))) {
        rc = -EFAULT;
        goto out;
    }

out:
    return rc;
}


static long xv2g_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	struct xv2g_file_private_data *priv = filp->private_data;

	switch (cmd) {
    case IOCTL_VIRT2GFN:
        return xen_virt2gfn(priv, (void __user *) arg);
	default:
		return -ENOIOCTLCMD;
	}

	return 0;
}

static const struct file_operations xv2g_fops = {
	.owner = THIS_MODULE,
	.open = xv2g_open,
	.release = xv2g_release,
	.unlocked_ioctl = xv2g_ioctl,
};

static struct miscdevice xv2g_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "xen/xv2g",
	.fops	= &xv2g_fops,
};

static int __init xv2g_init(void)
{
	int err;

	if (!xen_domain())
		return -ENODEV;

	err = misc_register(&xv2g_miscdev);
	if (err != 0) {
		pr_err("Could not register misc xv2g device\n");
		return err;
	}

	pr_debug("Created grant allocation device at %d,%d\n",
			MISC_MAJOR, xv2g_miscdev.minor);

	return 0;
}

static void __exit xv2g_exit(void)
{
	misc_deregister(&xv2g_miscdev);
}

module_init(xv2g_init);
module_exit(xv2g_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Edgar E. Iglesias <edgar.iglesias@amd.com>");
MODULE_DESCRIPTION("User-space virt to gfn mapping driver");
