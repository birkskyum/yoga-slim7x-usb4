/* SPDX-License-Identifier: GPL-2.0-only */
/* Userspace mock shim only; never supplied to a kernel build. */
#ifdef __linux__
#include_next <linux/errno.h>
#else
#include <errno.h>
#endif
