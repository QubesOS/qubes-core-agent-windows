/******************************************************************************
 * gntmem_ioctl.h
 *
 * Interface to Windows memory-granting driver
 *
 * Copyright (c) 2009 Chris Smowton <chris.smowton@cl.cam.ac.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef __WINDOWS_PUBLIC_GNTMEM_H__
#define __WINDOWS_PUBLIC_GNTMEM_H__

typedef UINT32 grant_ref_t;
typedef UINT16 domid_t;

/*
 * Set a limit on how many grants this device may issue
 */
#define IOCTL_GNTMEM_SET_LOCAL_LIMIT 			\
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
struct ioctl_gntmem_set_limit {
	unsigned int new_limit;
};

/*
 * Set a global limit on how many grants all gntmem devices taken together may issue
 */
#define IOCTL_GNTMEM_SET_GLOBAL_LIMIT			\
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
/* Uses same in-struct as above */

/*
 * Given a page-aligned virtual address, and region length in number of pages,
 * issue a grant against each of those pages. Output buffer should hold the appropriate number of grant_ref_ts.
 */
#define IOCTL_GNTMEM_GRANT_PAGES			\
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
struct ioctl_gntmem_grant_pages {
	int n_pages;
	domid_t domid;
	INT32 uid; // Device-local identifier for later use in GET_GRANTS.
};

#define IOCTL_GNTMEM_GET_GRANTS \
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
struct ioctl_gntmem_get_grants {
	INT32 uid;
};
// Output is a void* followed by (n_pages) grants.

#endif /* __WINDOWS_PUBLIC_GNTMEM_H__ */
