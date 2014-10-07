/*
    Xen Store Daemon providing simple tree-like database.
    Copyright (C) 2005 Rusty Russell IBM Corporation

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef _XS_H
#define _XS_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBT_NULL 0

struct xs_handle;
typedef unsigned __int32 uint32_t;
typedef uint32_t xs_transaction_t;

/* Connect to the xs daemon.
 * Returns a handle or NULL.
 */
struct xs_handle *xs_daemon_open(void);
struct xs_handle *xs_domain_open(void);

/* Connect to the xs daemon (readonly for non-root clients).
 * Returns a handle or NULL.
 */
struct xs_handle *xs_daemon_open_readonly(void);

/* Close the connection to the xs daemon. */
void xs_daemon_close(struct xs_handle *);

/* Get contents of a directory.
 * Returns a malloced array: call free() on it after use.
 * Num indicates size.
 */
char **xs_directory(struct xs_handle *h, xs_transaction_t t,
            const char *path, unsigned int *num);

/* Get the value of a single file, nul terminated.
 * Returns a malloced value: call free() on it after use.
 * len indicates length in bytes, not including terminator.
 */
void *xs_read(struct xs_handle *h, xs_transaction_t t,
          const char *path, unsigned int *len);

/* Write the value of a single file.
 * Returns false on failure.
 */
BOOL xs_write(struct xs_handle *h, xs_transaction_t t,
          const char *path, const void *data, unsigned int len);

/* Create a new directory.
 * Returns false on failure, or success if it already exists.
 */
BOOL xs_mkdir(struct xs_handle *h, xs_transaction_t t,
          const char *path);

/* Destroy a file or directory (and children).
 * Returns false on failure, or if it doesn't exist.
 */
BOOL xs_rm(struct xs_handle *h, xs_transaction_t t,
       const char *path);

/* Get permissions of node (first element is owner, first perms is "other").
 * Returns malloced array, or NULL: call free() after use.
 */
struct xs_permissions *xs_get_permissions(struct xs_handle *h,
                      xs_transaction_t t,
                      const char *path, unsigned int *num);

/* Set permissions of node (must be owner).
 * Returns false on failure.
 */
BOOL xs_set_permissions(struct xs_handle *h, xs_transaction_t t,
            const char *path, struct xs_permissions *perms,
            unsigned int num_perms);

/* Watch a node for changes (poll on fd to detect, or call read_watch()).
 * When the node (or any child) changes, fd will become readable.
 * Token is returned when watch is read, to allow matching.
 * Returns false on failure.
 */
BOOL xs_watch(struct xs_handle *h, const char *path, const char *token);

/* Replacement for xs_fileno, which isn't really suited to Win32.
   Windows events are vulnerable to missed wakeup when other threads are waiting on them in an
   unsynchronised fashion. (For example, supposed two threads had called xs_fileno, and were waiting.
   Thread 1 is waiting on Key 1, and Thread 2 on Key 2. Thread 1 checks the store and notes Key 1 is unchanged.
   Before Thread 1 can call WaitForSingleObject, the watch fires; Thread 2 is woken, resets the event, and checks
   the store. However, he finds that Key 1 has changed, which is of no interest, and goes back to sleep.
   Now thread 1 will never wake. The way around this lack of atomic mutex-release-and-wait is to use an event
   per thread. This function allows one to register an event to be signalled whenever a watch fires. */
BOOL xs_register_watch_event(struct xs_handle *h, HANDLE event);

/* Remove an event previously registered for watch events */
BOOL xs_unregister_watch_event(struct xs_handle *h, HANDLE event);

/* Find out what node change was on (will block if nothing pending if 'wait' is true).
 * Returns array containing the path and token. Use XS_WATCH_* to access these
 * elements. Call free() after use.
 */
char **xs_read_watch(struct xs_handle *h, unsigned int *num, BOOL wait);

/* Remove a watch on a node: implicitly acks any outstanding watch.
 * Returns false on failure (no watch on that node).
 */
BOOL xs_unwatch(struct xs_handle *h, const char *path, const char *token);

/* Start a transaction: changes by others will not be seen during this
 * transaction, and changes will not be visible to others until end.
 * Returns NULL on failure.
 */
xs_transaction_t xs_transaction_start(struct xs_handle *h);

/* End a transaction.
 * If abandon is true, transaction is discarded instead of committed.
 * Returns false on failure: if errno == EAGAIN, you have to restart
 * transaction.
 */
BOOL xs_transaction_end(struct xs_handle *h, xs_transaction_t t,
            BOOL abort);

/* Introduce a new domain.
 * This tells the store daemon about a shared memory page, event channel and
 * store path associated with a domain: the domain uses these to communicate.
 */
BOOL xs_introduce_domain(struct xs_handle *h,
             unsigned int domid,
             unsigned long mfn,
                         unsigned int eventchn);

/* Set the target of a domain
 * This tells the store daemon that a domain is targetting another one, so
 * it should let it tinker with it.
 */
BOOL xs_set_target(struct xs_handle *h,
           unsigned int domid,
           unsigned int target);

/* Resume a domain.
 * Clear the shutdown flag for this domain in the store.
 */
BOOL xs_resume_domain(struct xs_handle *h, unsigned int domid);

/* Release a domain.
 * Tells the store domain to release the memory page to the domain.
 */
BOOL xs_release_domain(struct xs_handle *h, unsigned int domid);

/* Query the home path of a domain.  Call free() after use.
 */
char *xs_get_domain_path(struct xs_handle *h, unsigned int domid);

/* Return whether the domain specified has been introduced to xenstored.
 */
BOOL xs_is_domain_introduced(struct xs_handle *h, unsigned int domid);

/* Only useful for DEBUG versions */
char *xs_debug_command(struct xs_handle *h, const char *cmd,
               void *data, unsigned int len);

#ifdef __cplusplus
}
#endif

#endif /* _XS_H */

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
