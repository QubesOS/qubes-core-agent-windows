/******************************************************************************
 * evtchn.h
 * 
 * Interface to /dev/xen/evtchn.
 * 
 * Copyright (c) 2003-2005, K A Fraser
 * WDF port copyright (c) 2009 C Smowton
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

#ifndef __WINDOWS_PUBLIC_EVTCHN_H__
#define __WINDOWS_PUBLIC_EVTCHN_H__

typedef UINT16 domid_t;

/*
 * Bind a fresh port to VIRQ @virq.
 * Return allocated port.
 */
#define IOCTL_EVTCHN_BIND_VIRQ 			\
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
struct ioctl_evtchn_bind_virq {
	unsigned int virq;
};

/*
 * Bind a fresh port to remote <@remote_domain, @remote_port>.
 * Return allocated port.
 */
#define IOCTL_EVTCHN_BIND_INTERDOMAIN			\
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
struct ioctl_evtchn_bind_interdomain {
	unsigned int remote_domain, remote_port;
};

/*
 * Allocate a fresh port for binding to @remote_domain.
 * Return allocated port.
 */
#define IOCTL_EVTCHN_BIND_UNBOUND_PORT			\
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
struct ioctl_evtchn_bind_unbound_port {
	unsigned int remote_domain;
};

/*
 * Unbind previously allocated @port.
 */
#define IOCTL_EVTCHN_UNBIND				\
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
struct ioctl_evtchn_unbind {
	unsigned int port;
};

/*
 * Notify previously allocated @port.
 */
#define IOCTL_EVTCHN_NOTIFY				\
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x804, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
struct ioctl_evtchn_notify {
	unsigned int port;
};

/* Clear and reinitialise the event buffer. Clear error condition. */
#define IOCTL_EVTCHN_RESET				\
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x805, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

/* Restruct this file descriptor so that it can only be applied to a
 * nominated domain.  Once a file descriptor has been restricted it
 * cannot be de-restricted, and must be closed and re-openned.  Event
 * channels which were bound before restricting remain bound
 * afterwards, and can be notified as usual.
 */
#define IOCTL_EVTCHN_RESTRICT_DOMID			\
	CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x806, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
struct ioctl_evtchn_restrict_domid {
	domid_t domid;
};


#endif /* __WINDOWS_PUBLIC_EVTCHN_H__ */
