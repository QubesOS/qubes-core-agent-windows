/* 
    Xen Store Daemon providing simple tree-like database.
    Copyright (C) 2005 Rusty Russell IBM Corporation (interface, Linux impl)
	          (C) 2009 Chris Smowton (Windows impl)
			  Using code by James Harper (http://xenbits.xensource.com/ext/win-pvdrivers.hg)

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

#include "xs.h"
#include "xs_lib.h"
#include "list.h"
#include <setupapi.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* To check: the C90 type "unsigned long" varies with the target machine's register size.
   Does ULONG as well? If so, we'll try to use a 64-bit transaction ID, which isn't right. */
struct xsd_sockmsg
{
    ULONG type;  /* XS_??? */
    ULONG req_id;/* Request identifier, echoed in daemon's response.  */
    ULONG tx_id; /* Transaction id (0 if not related to a transaction). */
    ULONG len;   /* Length of data following this. */

    /* Generally followed by nul-terminated string(s). */
};

struct xs_stored_msg {
	struct list_head list;
	struct xsd_sockmsg hdr;
	char *body;
};

struct xs_event {
	struct list_head list;
	HANDLE ev;
};

struct iovec {
	void* iov_base;
	int iov_len;
};

struct xs_handle {

	/* Communications channel to xenstore daemon. */
	HANDLE fd;
	/*
     * A read thread which pulls messages off the comms channel and
     * signals waiters.
     */
	HANDLE read_thr; // Thread exists when read_thr != INVALID_HANDLE_VALUE

	/*
     * A list of fired watch messages, and a list of events, protected by a mutex. Users should
	 * push an event into the list in order to be notified whenever the watch_list becomes non-empty.
     */
	struct list_head watch_list;
	struct list_head watch_event_list;
	HANDLE watch_mutex;

	/*
     * A list of replies. Currently only one will ever be outstanding
     * because we serialise requests. Again, users should push an event to get a notification
	 * when this list becomes non-empty.
     */
	struct list_head reply_list;
	struct list_head reply_event_list;
	HANDLE reply_mutex;

	HANDLE thread_stop_event;

	/* One request at a time. */
	HANDLE request_mutex;
};

DEFINE_GUID(GUID_DEVINTERFACE_XENBUS, 0x14ce175a, 0x3ee2, 0x4fae, 0x92, 0x52, 0x0, 0xdb, 0xd8, 0x4f, 0x1, 0x8e);

enum xsd_sockmsg_type
{
    XS_DEBUG,
    XS_DIRECTORY,
    XS_READ,
    XS_GET_PERMS,
    XS_WATCH,
    XS_UNWATCH,
    XS_TRANSACTION_START,
    XS_TRANSACTION_END,
    XS_INTRODUCE,
    XS_RELEASE,
    XS_GET_DOMAIN_PATH,
    XS_WRITE,
    XS_MKDIR,
    XS_RM,
    XS_SET_PERMS,
    XS_WATCH_EVENT,
    XS_ERROR,
    XS_IS_DOMAIN_INTRODUCED,
    XS_RESUME,
    XS_SET_TARGET
};

static HANDLE
get_xen_interface_handle()
{
  HDEVINFO handle;
  SP_DEVICE_INTERFACE_DATA sdid;
  SP_DEVICE_INTERFACE_DETAIL_DATA *sdidd;
  DWORD buf_len;
  HANDLE h;

  handle = SetupDiGetClassDevs(&GUID_DEVINTERFACE_XENBUS, 0, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (handle == INVALID_HANDLE_VALUE)
  {
    return INVALID_HANDLE_VALUE;
  }
  sdid.cbSize = sizeof(sdid);
  if (!SetupDiEnumDeviceInterfaces(handle, NULL, &GUID_DEVINTERFACE_XENBUS, 0, &sdid))
  {
    return INVALID_HANDLE_VALUE;
  }
  SetupDiGetDeviceInterfaceDetail(handle, &sdid, NULL, 0, &buf_len, NULL);
  sdidd = (SP_DEVICE_INTERFACE_DETAIL_DATA *)malloc(buf_len);
  sdidd->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
  if (!SetupDiGetDeviceInterfaceDetail(handle, &sdid, sdidd, buf_len, NULL, NULL))
  {
    free(sdidd);
    return INVALID_HANDLE_VALUE;
  }
  
  h = CreateFile(
	sdidd->DevicePath, 
	FILE_GENERIC_READ|FILE_GENERIC_WRITE, 
	0, 
	NULL, 
	OPEN_EXISTING, 
	FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL, 
	NULL);

  free(sdidd);
  
  return h;
}


/* Connect to the xs daemon.
 * Returns a handle or NULL.
 */
struct xs_handle *xs_daemon_open(void) {
	
	/* No daemon to find on Windows; Win cannot be dom0 */
	fprintf(stderr, "Xenstore error: Daemon-open is only sensible in dom0\n");
	return (struct xs_handle*)0;

}

struct xs_handle *xs_domain_open(void) {

	HANDLE handle;
	struct xs_handle* ret_handle;

	handle = get_xen_interface_handle();

	if(handle == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Xenstore error: Failed to open xen kernel device due to error %ul\n",
				GetLastError());
		return NULL;
	}

	ret_handle = (struct xs_handle*)malloc(sizeof(struct xs_handle));
	if(!ret_handle) {
		fprintf(stderr, "Xenstore error: couldn't malloc %d bytes for a new xs_handle\n", sizeof(struct xs_handle));
		CloseHandle(handle);
		return NULL;
	}

	ret_handle->fd = handle;
	ret_handle->read_thr = INVALID_HANDLE_VALUE;
	INIT_LIST_HEAD(&ret_handle->watch_list);
	INIT_LIST_HEAD(&ret_handle->watch_event_list);
	INIT_LIST_HEAD(&ret_handle->reply_list);
	INIT_LIST_HEAD(&ret_handle->reply_event_list);
	ret_handle->watch_mutex = CreateMutex(NULL, FALSE, NULL);
	ret_handle->reply_mutex = CreateMutex(NULL, FALSE, NULL);
	ret_handle->request_mutex = CreateMutex(NULL, FALSE, NULL);
	ret_handle->thread_stop_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	if(ret_handle->watch_mutex == INVALID_HANDLE_VALUE
	|| ret_handle->reply_mutex == INVALID_HANDLE_VALUE
	|| ret_handle->request_mutex == INVALID_HANDLE_VALUE
	|| ret_handle->thread_stop_event == INVALID_HANDLE_VALUE) {
		CloseHandle(ret_handle->watch_mutex);
		CloseHandle(ret_handle->reply_mutex);
		CloseHandle(ret_handle->request_mutex);
		CloseHandle(handle);
		free(ret_handle);
		return NULL;
	}

	return ret_handle;

}

/* User should beware the race here -- namely that they may have missed a watch firing whilst
   registering their event. They must now check their key values before sleeping on the event,
   and always be sure to reset their event before checking again. */
BOOL xs_register_watch_event(struct xs_handle* h, HANDLE new_event) {

	struct xs_event* new_event_li = malloc(sizeof(struct xs_event));
	if(!new_event_li)
		return FALSE;
	new_event_li->ev = new_event;

	if(WaitForSingleObject(h->watch_mutex, INFINITE) == WAIT_FAILED) {
		free(new_event_li);
		return FALSE;
	}

	list_add_tail(&new_event_li->list, &h->watch_event_list);

	if(!ReleaseMutex(h->watch_mutex))
		return FALSE;

	return TRUE;

}

BOOL xs_unregister_watch_event(struct xs_handle* h, HANDLE to_remove) {

	struct list_head* iter;
	BOOL found = FALSE;

	if(WaitForSingleObject(h->watch_mutex, INFINITE) == WAIT_FAILED) {
		return FALSE;
	}

	list_for_each(iter, &h->watch_event_list) {
		struct xs_event* this_event = (struct xs_event*)iter;
		if(this_event->ev == to_remove) {
			found = TRUE;
			list_del(iter);
			free(iter);
			break;
		}
	}
	
	if(!ReleaseMutex(h->watch_mutex))
		return FALSE;

	return found;

}

/* Connect to the xs daemon (readonly for non-root clients).
 * Returns a handle or NULL.
 */
struct xs_handle *xs_daemon_open_readonly(void) {

	fprintf(stderr, "Xenstore error: can't open daemon in a guest; use domain_open\n");
	return NULL;

}

/* Close the connection to the xs daemon. */
void xs_daemon_close(struct xs_handle * h) {

	struct list_head *msg, *tmsg;

	WaitForSingleObject(h->request_mutex, INFINITE);
	WaitForSingleObject(h->reply_mutex, INFINITE);
	WaitForSingleObject(h->watch_mutex, INFINITE);

	if (h->read_thr != INVALID_HANDLE_VALUE) {
		/* XXX FIXME: May leak an unpublished message buffer. */
		SetEvent(h->thread_stop_event);
		if(WaitForSingleObject(h->read_thr, 5000) == WAIT_TIMEOUT) {
			fprintf(stderr, "Xenstore: timed out waiting for thread to stop; leaking some handles\n");
			return;
		}
		CloseHandle(h->thread_stop_event);
		CloseHandle(h->read_thr);
	}

	list_for_each_safe(msg, tmsg, &h->reply_list)
	{
		struct xs_stored_msg* this_msg = (struct xs_stored_msg*)msg;
		free(this_msg->body);
		free(this_msg);
	}

	list_for_each_safe(msg, tmsg, &h->watch_list)
	{
		struct xs_stored_msg* this_msg = (struct xs_stored_msg*)msg;
		free(this_msg->body);
		free(this_msg);
	}

	ReleaseMutex(h->request_mutex);
	ReleaseMutex(h->reply_mutex);
	ReleaseMutex(h->watch_mutex);

	CloseHandle(h->request_mutex);
	CloseHandle(h->reply_mutex);
	CloseHandle(h->watch_mutex);
	CloseHandle(h->fd);

	free(h);

}

static BOOL read_all(HANDLE fd, void *data, unsigned int len)
{

	char* data_chars = (char*)data;
	OVERLAPPED ol;
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	while (len) {
		DWORD done;
		memset(&ol, 0, sizeof(ol));

		if(!ReadFile(fd, data_chars, len, 0, &ol)) {
			if(GetLastError() != ERROR_IO_PENDING) {
				CloseHandle(ol.hEvent);
				return FALSE;
			}
		}
		if(!GetOverlappedResult(fd, &ol, &done, TRUE)) {
			CloseHandle(ol.hEvent);
			return FALSE;
		}
		data_chars += done;
		len -= done;
	}

	CloseHandle(ol.hEvent);
	return TRUE;
}
/*
static int get_error(const char *errorstring)
{
	unsigned int i;

	for (i = 0; !streq(errorstring, xsd_errors[i].errstring); i++)
		if (i == ARRAY_SIZE(xsd_errors) - 1)
			return EINVAL;
	return xsd_errors[i].errnum;
}
*/

static int read_message(struct xs_handle *h);

/* Adds extra nul terminator, because we generally (always?) hold strings. */
static void *read_reply(
	struct xs_handle *h, enum xsd_sockmsg_type *type, unsigned int *len)
{
	struct xs_stored_msg *msg = NULL;
	char *body;
	HANDLE my_event	= INVALID_HANDLE_VALUE;
	struct xs_event my_event_li;

	/* Read from comms channel ourselves if there is no reader thread. */
	if ((h->read_thr == INVALID_HANDLE_VALUE) && (read_message(h) == -1))
		return NULL;

	while(!msg) {

		// Defer making an event if we find something the first time
		if(my_event != INVALID_HANDLE_VALUE) {
			if(WaitForSingleObject(my_event, INFINITE) == WAIT_FAILED) {
				CloseHandle(my_event);
				return 0;
			}
		}

		if(WaitForSingleObject(h->reply_mutex, INFINITE) == WAIT_FAILED)
			return 0;

		if(!list_empty(&h->reply_list)) {
			struct list_head* head = &h->reply_list;
			msg = (struct xs_stored_msg*)list_top_head(head);
			list_del(&msg->list);
			if(my_event != INVALID_HANDLE_VALUE) {
				list_del((struct list_head*)&my_event_li);
				CloseHandle(my_event);
			}
		}
		else if(my_event == INVALID_HANDLE_VALUE) {
			my_event = CreateEvent(NULL, FALSE, FALSE, NULL);
			my_event_li.ev = my_event;
			list_add(&my_event_li.list, &h->reply_event_list);
		}

		ReleaseMutex(h->reply_mutex);

	}

	*type = msg->hdr.type;
	if (len)
		*len = msg->hdr.len;
	body = msg->body;

	free(msg);

	return body;
}

#define XENSTORE_PAYLOAD_MAX 4096

/* Send message to xs, get malloc'ed reply.  NULL on error. */
static void *xs_talkv(struct xs_handle *h, xs_transaction_t t,
		      enum xsd_sockmsg_type type,
		      const struct iovec *iovec,
		      unsigned int num_vecs,
		      unsigned int *len)
{
	struct xsd_sockmsg msg;
	void *ret = NULL;
	unsigned int i;

	msg.tx_id = t;
	msg.req_id = 0;
	msg.type = type;
	msg.len = 0;
	for (i = 0; i < num_vecs; i++)
		msg.len += iovec[i].iov_len;

	if (msg.len > XENSTORE_PAYLOAD_MAX) {
		//errno = E2BIG;
		return 0;
	}

	if(WaitForSingleObject(h->request_mutex, INFINITE) == WAIT_FAILED)
		return 0;

	if (!xs_write_all(h->fd, &msg, sizeof(msg)))
		goto fail;

	for (i = 0; i < num_vecs; i++)
		if (!xs_write_all(h->fd, iovec[i].iov_base, iovec[i].iov_len))
			goto fail;

	ret = read_reply(h, (enum xsd_sockmsg_type*)&msg.type, len);
	if (!ret)
		goto fail;

	if(!ReleaseMutex(h->request_mutex))
		return 0;

	if (msg.type == XS_ERROR) {
		//saved_errno = get_error(ret);
		free(ret);
		//errno = saved_errno;
		return NULL;
	}

	if (msg.type != type) {
		free(ret);
		//saved_errno = EBADF;
		goto close_fd;
	}
	return ret;

fail:
	/* We're in a bad state, so close fd. */
	//saved_errno = errno;
	ReleaseMutex(h->request_mutex);
close_fd:
	CloseHandle(h->fd);
	h->fd = INVALID_HANDLE_VALUE;
	//errno = saved_errno;
	return NULL;
}

/* free(), but don't change errno. */
static void free_no_errno(void *p)
{
//	int saved_errno = errno;
	free(p);
//	errno = saved_errno;
}

/* Simplified version of xs_talkv: single message. */
static void *xs_single(struct xs_handle *h, xs_transaction_t t,
		       enum xsd_sockmsg_type type,
		       const char *string,
		       unsigned int *len)
{
	struct iovec iovec;

	iovec.iov_base = (void *)string;
	iovec.iov_len = strlen(string) + 1;
	return xs_talkv(h, t, type, &iovec, 1, len);
}

static BOOL xs_bool(char *reply)
{
	if (!reply)
		return FALSE;
	free(reply);
	return TRUE;
}

char **xs_directory(struct xs_handle *h, xs_transaction_t t,
		    const char *path, unsigned int *num)
{
	char *strings, *p, **ret;
	unsigned int len;

	strings = xs_single(h, t, XS_DIRECTORY, path, &len);
	if (!strings)
		return NULL;

	/* Count the strings. */
	*num = xs_count_strings(strings, len);

	/* Transfer to one big alloc for easy freeing. */
	ret = malloc(*num * sizeof(char *) + len);
	if (!ret) {
		free_no_errno(strings);
		return NULL;
	}
	memcpy(&ret[*num], strings, len);
	free_no_errno(strings);

	strings = (char *)&ret[*num];
	for (p = strings, *num = 0; p < strings + len; p += strlen(p) + 1)
		ret[(*num)++] = p;
	return ret;
}

/* Get the value of a single file, nul terminated.
 * Returns a malloced value: call free() on it after use.
 * len indicates length in bytes, not including the nul.
 */
void *xs_read(struct xs_handle *h, xs_transaction_t t,
	      const char *path, unsigned int *len)
{
	return xs_single(h, t, XS_READ, path, len);
}

/* Write the value of a single file.
 * Returns false on failure.
 */
BOOL xs_write(struct xs_handle *h, xs_transaction_t t,
	      const char *path, const void *data, unsigned int len)
{
	struct iovec iovec[2];

	iovec[0].iov_base = (void *)path;
	iovec[0].iov_len = strlen(path) + 1;
	iovec[1].iov_base = (void *)data;
	iovec[1].iov_len = len;

	return xs_bool(xs_talkv(h, t, XS_WRITE, iovec,
				2, NULL));
}

/* Create a new directory.
 * Returns false on failure, or success if it already exists.
 */
BOOL xs_mkdir(struct xs_handle *h, xs_transaction_t t,
	      const char *path)
{
	return xs_bool(xs_single(h, t, XS_MKDIR, path, NULL));
}

/* Destroy a file or directory (directories must be empty).
 * Returns false on failure, or success if it doesn't exist.
 */
BOOL xs_rm(struct xs_handle *h, xs_transaction_t t,
	   const char *path)
{
	return xs_bool(xs_single(h, t, XS_RM, path, NULL));
}

/* Get permissions of node (first element is owner).
 * Returns malloced array, or NULL: call free() after use.
 */
struct xs_permissions *xs_get_permissions(struct xs_handle *h,
					  xs_transaction_t t,
					  const char *path, unsigned int *num)
{
	char *strings;
	unsigned int len;
	struct xs_permissions *ret;

	strings = xs_single(h, t, XS_GET_PERMS, path, &len);
	if (!strings)
		return NULL;

	/* Count the strings: each one perms then domid. */
	*num = xs_count_strings(strings, len);

	/* Transfer to one big alloc for easy freeing. */
	ret = malloc(*num * sizeof(struct xs_permissions));
	if (!ret) {
		free_no_errno(strings);
		return NULL;
	}

	if (!xs_strings_to_perms(ret, *num, strings)) {
		free_no_errno(ret);
		ret = NULL;
	}

	free(strings);
	return ret;
}

/* Set permissions of node (must be owner).
 * Returns false on failure.
 */
BOOL xs_set_permissions(struct xs_handle *h,
			xs_transaction_t t,
			const char *path,
			struct xs_permissions *perms,
			unsigned int num_perms)
{
	unsigned int i;
	struct iovec* iov = malloc(sizeof(struct iovec) * (1+num_perms));

	iov[0].iov_base = (void *)path;
	iov[0].iov_len = strlen(path) + 1;
	
	for (i = 0; i < num_perms; i++) {
		char buffer[MAX_STRLEN(unsigned int)+1];

		if (!xs_perm_to_string(&perms[i], buffer, sizeof(buffer)))
			goto unwind;

		iov[i+1].iov_base = _strdup(buffer);
		iov[i+1].iov_len = strlen(buffer) + 1;
		if (!iov[i+1].iov_base)
			goto unwind;
	}

	if (!xs_bool(xs_talkv(h, t, XS_SET_PERMS, iov, 1+num_perms, NULL)))
		goto unwind;
	for (i = 0; i < num_perms; i++)
		free(iov[i+1].iov_base);
	free(iov);
	return TRUE;

unwind:
	num_perms = i;
	for (i = 0; i < num_perms; i++)
		free_no_errno(iov[i+1].iov_base);
	free(iov);
	return FALSE;
}

static DWORD WINAPI read_thread(void *arg);

/* Watch a node for changes (register an event to detect, or call read_watch()).
 * When the node (or any child) changes, fd will become readable.
 * Token is returned when watch is read, to allow matching.
 * Returns false on failure.
 */
BOOL xs_watch(struct xs_handle *h, const char *path, const char *token)
{
	struct iovec iov[2];

	/* We dynamically create a reader thread on demand. */
	if(WaitForSingleObject(h->request_mutex, INFINITE) == WAIT_FAILED)
		return FALSE;
	if (h->read_thr == INVALID_HANDLE_VALUE) {
		h->read_thr = CreateThread(NULL, 0, read_thread, h, 0, NULL);
		if(h->read_thr == INVALID_HANDLE_VALUE) {
			ReleaseMutex(h->request_mutex);
			return FALSE;
		}
	}
	ReleaseMutex(h->request_mutex);

	iov[0].iov_base = (void *)path;
	iov[0].iov_len = strlen(path) + 1;
	iov[1].iov_base = (void *)token;
	iov[1].iov_len = strlen(token) + 1;

	return xs_bool(xs_talkv(h, XBT_NULL, XS_WATCH, iov,
				2, NULL));
}

/* Find out what node change was on (will block if nothing pending if 'wait' is true).
 * Returns array of two pointers: path and token, or NULL.
 * Call free() after use.
 */
char **xs_read_watch(struct xs_handle *h, unsigned int *num, BOOL wait)
{
	struct xs_stored_msg *msg = NULL;
	char **ret, *strings, c = 0;
	unsigned int num_strings, i;
	HANDLE my_event = INVALID_HANDLE_VALUE;
	struct xs_event my_event_li;

	do {

		// Defer making an event if we find something the first time
		if(my_event != INVALID_HANDLE_VALUE) {
			if(WaitForSingleObject(my_event, INFINITE) == WAIT_FAILED) {
				CloseHandle(my_event);
				return 0;
			}
		}

		if(WaitForSingleObject(h->watch_mutex, INFINITE) == WAIT_FAILED)
			return 0;

		if(!list_empty(&h->watch_list)) {
			struct list_head* head = &h->watch_list;
			msg = (struct xs_stored_msg*)list_top_head(head);
			list_del(&msg->list);
			if(my_event != INVALID_HANDLE_VALUE) {
				list_del((struct list_head*)&my_event_li);
				CloseHandle(my_event);
			}
		}
		else if(wait && (my_event == INVALID_HANDLE_VALUE)) {
			my_event = CreateEvent(NULL, FALSE, FALSE, NULL);
			my_event_li.ev = my_event;
			list_add(&my_event_li.list, &h->watch_event_list);
		}

		ReleaseMutex(h->watch_mutex);

	} while((wait) && !msg);

	if((!wait) && (!msg))
		return 0;

	assert(msg->hdr.type == XS_WATCH_EVENT);

	strings     = msg->body;
	num_strings = xs_count_strings(strings, msg->hdr.len);

	ret = malloc(sizeof(char*) * num_strings + msg->hdr.len);
	if (!ret) {
		free_no_errno(strings);
		free_no_errno(msg);
		return NULL;
	}

	ret[0] = (char *)(ret + num_strings);
	memcpy(ret[0], strings, msg->hdr.len);

	free(strings);
	free(msg);

	for (i = 1; i < num_strings; i++)
		ret[i] = ret[i - 1] + strlen(ret[i - 1]) + 1;

	*num = num_strings;

	return ret;
}

/* Remove a watch on a node.
 * Returns false on failure (no watch on that node).
 */
BOOL xs_unwatch(struct xs_handle *h, const char *path, const char *token)
{
	struct iovec iov[2];

	iov[0].iov_base = (char *)path;
	iov[0].iov_len = strlen(path) + 1;
	iov[1].iov_base = (char *)token;
	iov[1].iov_len = strlen(token) + 1;

	return xs_bool(xs_talkv(h, XBT_NULL, XS_UNWATCH, iov,
				2, NULL));
}

/* Start a transaction: changes by others will not be seen during this
 * transaction, and changes will not be visible to others until end.
 * Returns XBT_NULL on failure.
 */
xs_transaction_t xs_transaction_start(struct xs_handle *h)
{
	char *id_str;
	xs_transaction_t id;

	id_str = xs_single(h, XBT_NULL, XS_TRANSACTION_START, "", NULL);
	if (id_str == NULL)
		return XBT_NULL;

	id = strtoul(id_str, NULL, 0);
	free(id_str);

	return id;
}

/* End a transaction.
 * If abandon is true, transaction is discarded instead of committed.
 * Returns false on failure, which indicates an error: transactions will
 * not fail spuriously.
 */
BOOL xs_transaction_end(struct xs_handle *h, xs_transaction_t t,
			BOOL abort)
{
	char abortstr[2];

	if (abort)
		strcpy(abortstr, "F");
	else
		strcpy(abortstr, "T");
	
	return xs_bool(xs_single(h, t, XS_TRANSACTION_END, abortstr, NULL));
}

/* Introduce a new domain.
 * This tells the store daemon about a shared memory page, event channel and
 * store path associated with a domain: the domain uses these to communicate.
 */
BOOL xs_introduce_domain(struct xs_handle *h,
			 unsigned int domid,
			 unsigned long mfn,
			 unsigned int eventchn) {

				fprintf(stderr, "introduce domain not sensible in a guest\n");
				return FALSE;

}

/* Set the target of a domain
 * This tells the store daemon that a domain is targetting another one, so
 * it should let it tinker with it.
 */
BOOL xs_set_target(struct xs_handle *h,
		   unsigned int domid,
		   unsigned int target) {

	fprintf(stderr, "set_target not sensible in a guest\n");
	return FALSE;

}

/* Resume a domain.
 * Clear the shutdown flag for this domain in the store.
 */
BOOL xs_resume_domain(struct xs_handle *h, unsigned int domid) {

	fprintf(stderr, "resume-domain not sensible in a guest\n");
	return FALSE;

}

/* Release a domain.
 * Tells the store domain to release the memory page to the domain.
 */
BOOL xs_release_domain(struct xs_handle *h, unsigned int domid) {

	fprintf(stderr, "release-domain not sensible in a guest\n");
	return FALSE;

}

char *xs_get_domain_path(struct xs_handle *h, unsigned int domid)
{
	char domid_str[MAX_STRLEN(domid)];

	_snprintf(domid_str, sizeof(domid_str), "%u", domid);

	return xs_single(h, XBT_NULL, XS_GET_DOMAIN_PATH, domid_str, NULL);
}

static void * single_with_domid(struct xs_handle *h,
				enum xsd_sockmsg_type type,
				unsigned int domid)
{
	char domid_str[MAX_STRLEN(domid)];

	_snprintf(domid_str, sizeof(domid_str), "%u", domid);

	return xs_single(h, XBT_NULL, type, domid_str, NULL);
}

BOOL xs_is_domain_introduced(struct xs_handle *h, unsigned int domid)
{
	return (BOOL)strcmp("F",
		      single_with_domid(h, XS_IS_DOMAIN_INTRODUCED, domid));
}

/* Only useful for DEBUG versions */
char *xs_debug_command(struct xs_handle *h, const char *cmd,
					   void *data, unsigned int len) {

	fprintf(stderr, "xs-debug-command not implemented\n");
	return NULL;

}

static int dispatch_message(struct xs_handle* h, struct xs_stored_msg* msg) {

	struct list_head* next_entry;

	if (msg->hdr.type == XS_WATCH_EVENT) {
		if(WaitForSingleObject(h->watch_mutex, INFINITE) == WAIT_FAILED)
			return 0;

		list_add_tail(&msg->list, &h->watch_list);
		/* Wake both people in xs_read_watch and users who've registered wait events */
		list_for_each(next_entry, &h->watch_event_list) {
			struct xs_event* next_event = (struct xs_event*)next_entry;
			SetEvent(next_event->ev);
		}

		ReleaseMutex(h->watch_mutex);
	} else {
		if(WaitForSingleObject(h->reply_mutex, INFINITE) == WAIT_FAILED)
			return 0;

		/* There should only ever be one response pending! */
		if (!list_empty(&h->reply_list)) {
			ReleaseMutex(h->reply_mutex);
			return 0;
		}

		list_add_tail(&msg->list, &h->reply_list);
		/* Wake people in xs_read_reply */
		list_for_each(next_entry, &h->reply_event_list) {
			struct xs_event* next_event = (struct xs_event*)next_entry;
			SetEvent(next_event->ev);
		}
		ReleaseMutex(h->reply_mutex);
	}

	return 1; // Success

}

static int read_message(struct xs_handle *h)
{
	struct xs_stored_msg *msg = NULL;
	char *body = NULL;

	/* Allocate message structure and read the message header. */
	msg = malloc(sizeof(*msg));
	if (msg == NULL)
		goto error;
	if (!read_all(h->fd, &msg->hdr, sizeof(msg->hdr)))
		goto error;

	/* Allocate and read the message body. */
	body = msg->body = malloc(msg->hdr.len + 1);
	if (body == NULL)
		goto error;
	if (!read_all(h->fd, body, msg->hdr.len))
		goto error;
	body[msg->hdr.len] = '\0';

	if(!dispatch_message(h, msg))
		goto error;

	return 0;

 error:
	//saved_errno = errno;
	free(msg);
	free(body);
	//errno = saved_errno;
	return -1;
}

/* Modified to use Win32 asynchronous I/O, and therefore correctly wait on the
   thread-is-ending event. */
static DWORD WINAPI read_thread(PVOID arg)
{

	struct xs_handle *h = arg;
	OVERLAPPED pendingio;
	struct xs_stored_msg *msg = NULL;
	HANDLE wait_handles[2];
	char* current_target;
	int bytes_needed;
	int bytes_got;
	int stage = 0;
	int dying = 0;

	memset(&pendingio, 0, sizeof(pendingio));
	pendingio.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	wait_handles[0] = h->thread_stop_event;
	wait_handles[1] = pendingio.hEvent;

	msg = malloc(sizeof(*msg));
	if(!msg)
		return -1;
	msg->body = 0;
	current_target = (char*)&msg->hdr;
	fprintf(stderr, "Allocated memory at %x\n", (ULONG)current_target);
	bytes_needed = sizeof(msg->hdr);
	bytes_got = 0;

	fprintf(stderr, "Thread: start\n");

	if(!ReadFile(h->fd, current_target, bytes_needed, NULL, &pendingio)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			fprintf(stderr, "Thread: Failed to issue first request, error %x\n", GetLastError());
			free(msg);
			return -1;
		}
		else {
			fprintf(stderr, "Thread: ReadFile returned with STATUS_PENDING\n");
		}
	}

	fprintf(stderr, "Thread: start loop\n");

	while(1) {

		DWORD ret = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
		fprintf(stderr, "Thread: left WFMO\n");
		if(ret == WAIT_FAILED) {
			if(msg->body)
				free(msg->body);
			free(msg);
			return -1;
		}
		else if(ret == WAIT_OBJECT_0) {
			fprintf(stderr, "Thread: Ordered to die, cancelled local I/O\n");
			CancelIo(h->fd);
			dying = 1;
		}
		else {
			// Overlapped I/O has completed
			DWORD bytes_transferred;
			fprintf(stderr, "Thread: Woke to service I/O\n");
			if(!GetOverlappedResult(h->fd, &pendingio, &bytes_transferred, FALSE)) {
				if(GetLastError() == ERROR_IO_INCOMPLETE)
					continue;
				fprintf(stderr, "Thread: I/O error\n");
				if(msg->body) {
					fprintf(stderr, "Freeing body at %x\n", (ULONG)msg->body);
					free(msg->body);
				}
				fprintf(stderr, "Freeing message at %x\n", (ULONG)msg);
				free(msg);
				fprintf(stderr, "Thread ends\n");
				return -1;
			}
			else {
				if(dying) {
					fprintf(stderr, "Thread: Found dying = 1, exiting\n");
					return 0;
				}
				else {
					fprintf(stderr, "Thread: Successful I/O completion, queueing more\n");
					// Successful I/O completion
					bytes_got += bytes_transferred;
					if(bytes_got == bytes_needed) {
						// Whole message received
						if(stage == 0) {
							/* Just got a message header */
							msg->body = malloc(msg->hdr.len + 1);
							if(!msg->body) {
								free(msg);
								return -1;
							}
							current_target = msg->body;
							fprintf(stderr, "Allocated body at %x\n", (ULONG)current_target);
							bytes_needed = msg->hdr.len;
							bytes_got = 0;
							stage = 1; // Get a body
						}
						else if(stage == 1) {
							/* Just got a message body */
							msg->body[msg->hdr.len] = '\0';
							if(!dispatch_message(h, msg)) {
								free(msg->body);
								free(msg);
								return -1;
							}
							msg = malloc(sizeof(*msg));
							if(!msg)
								return -1;
							msg->body = 0;
							current_target = (char*)&msg->hdr;
							fprintf(stderr, "Allocated header at %x\n", (ULONG)current_target);
							bytes_needed = sizeof(msg->hdr);
							bytes_got = 0;
							stage = 0; // Get a header
						}
					}
					// Issue a new I/O
					memset(&pendingio, 0, sizeof(pendingio));
					pendingio.hEvent = wait_handles[1];
					if(!ReadFile(h->fd, current_target + bytes_got, bytes_needed - bytes_got, NULL, &pendingio)) {
						if(GetLastError() != ERROR_IO_PENDING) {
							if(msg->body)
								free(msg->body);
							free(msg);
							return -1;
						}
					}
				}
			}
		}
	}

	return -1;
}

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
