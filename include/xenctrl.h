/******************************************************************************
 * xenctrl.h
 *
 * A library for low-level access to the Xen control interfaces.
 *
 * Copyright (c) 2003-2004, K A Fraser.
 * 
 *
 * xc_gnttab functions:
 * Copyright (c) 2007-2008, D G Murray <Derek.Muray@cl.cam.ac.uk>
 *
 * Windows modifications:
 * Copyright (c) 2009 C Smowton <chris.smowton@cl.cam.ac.uk>
 * 
 * Quoted here solely for the /dev/evtchn-related functions.
 */

#ifndef XENCTRL_EVTCHN_H
#define XENCTRL_EVTCHN_H

#ifdef __cplusplus
extern "C" {
#endif

/* A port identifier is guaranteed to fit in 31 bits. */
typedef int evtchn_port_or_error_t;
typedef unsigned int evtchn_port_t;

/*
 * Return a handle to the event channel driver, or -1 on failure, in which case
 * errno will be set appropriately.
 */
HANDLE xc_evtchn_open(void);

/*
 * Close a handle previously allocated with xc_evtchn_open().
 */
int xc_evtchn_close(HANDLE xce_handle);

/*
 * Return a handle that can be used in various Win32 asynchronous I/O
 * techniques in lieu of calls to xc_evtchn_pending and _unmask.
 */
HANDLE xc_evtchn_fd(HANDLE xce_handle);

/*
 * Notify the given event channel. Returns -1 on failure, in which case
 * call GetLastError for more details.
 */
int xc_evtchn_notify(HANDLE xce_handle, evtchn_port_t port);

/*
 * Returns a new event port awaiting interdomain connection from the given
 * domain ID, or -1 on failure, in which case call GetLastError for more information.
 */
evtchn_port_or_error_t
xc_evtchn_bind_unbound_port(HANDLE xce_handle, int domid);

/*
 * Returns a new event port bound to the remote port for the given domain ID,
 * or -1 on failure, in which case call GetLastError for more information.
 */
evtchn_port_or_error_t
xc_evtchn_bind_interdomain(HANDLE xce_handle, int domid,
                           evtchn_port_t remote_port);

/*
 * Bind an event channel to the given VIRQ. Returns the event channel bound to
 * the VIRQ, or -1 on failure, in which call GetLastError for more information.
 */
evtchn_port_or_error_t
xc_evtchn_bind_virq(HANDLE xce_handle, unsigned int virq);

/*
 * Unbind the given event channel. Returns -1 on failure, in which case call 
 * GetLastError for more information.
 */
int xc_evtchn_unbind(HANDLE xce_handle, evtchn_port_t port);

/*
 * Return the next event channel to become pending, or -1 on failure, in which
 * case call GetLastError for more information.
 */
evtchn_port_or_error_t
xc_evtchn_pending(HANDLE xce_handle);

/*
 * Return the next event channel to become pending, or -1 on failure, in which
 * case call GetLastError for more information.
 * This function flushes remaining pending events from evtchn buffer.
 */
evtchn_port_or_error_t
xc_evtchn_pending_with_flush(HANDLE xce_handle);

/*
 * Unmask the given event channel. Returns -1 on failure, in which case call 
 * GetLastError for more information.
 */
int xc_evtchn_unmask(HANDLE xce_handle, evtchn_port_t port);

/*
 * Reset event channel ring. This clears all pending events and (more important) clears error flag.
 * Returns -1 on failure, in which case call GetLastError for more information.
 */
int xc_evtchn_reset(HANDLE xce_handle);

#ifdef __cplusplus
}
#endif

#endif /* XENCTRL_EVTCHN_H */
