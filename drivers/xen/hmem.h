/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _XEN_HMEM_H
#define _XEN_HMEM_H

#include <xen/privcmd.h>

struct hmem *hmem_init(domid_t domid);

void hmem_destroy(struct hmem *hmem);

int hmem_handle_op(struct hmem *hmem, struct privcmd_hmem_op op);

#endif	/* _XEN_HMEM_H */
