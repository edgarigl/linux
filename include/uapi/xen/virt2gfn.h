/******************************************************************************
 * virt2gfn.h
 *
 * Interface to /dev/xen/xv2g.
 *
 * Author: Edgar E. Iglesias <edgar.iglesias@amd.com>
 */

#ifndef __LINUX_PUBLIC_VIRT2GFN_H__
#define __LINUX_PUBLIC_VIRT2GFN_H__

#include <linux/types.h>
#include <linux/ioctl.h>

#define IOCTL_VIRT2GFN \
_IOC(_IOC_READ|_IOC_WRITE, 'G', 5, sizeof(struct ioctl_xen_virt2gfn))
struct ioctl_xen_virt2gfn {
	/* Number of pages to map */
	__u32 count;
    /* padding.  */
	__u32 padding;

	/* Variable array with virt address to convert to gfns.  */
	union {
		__u64 addr[1];
		__DECLARE_FLEX_ARRAY(__u64, addr_flex);
	};
};

#endif /* __LINUX_PUBLIC_VIRT2GFN_H__ */
