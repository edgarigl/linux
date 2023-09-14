/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_IVSHMEM_H
#define _LINUX_IVSHMEM_H

#include <linux/types.h>

struct ivshm_regs {
	u32 int_mask;
	u32 int_status;
	u32 ivposition;
	u32 doorbell;
};

#define IVSHM_INT_ENABLE		BIT(0)

#endif /* _LINUX_IVSHMEM_H */
