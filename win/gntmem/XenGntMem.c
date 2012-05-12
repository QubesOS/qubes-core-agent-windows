/******************************************************************************
 * xengntmem.cpp
 *
 * A library for granting pages on the local machine to foreign domains
 *
 * Copyright (c) 2009 C Smowton <chris.smowton@cl.cam.ac.uk>
 */

#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>

#include <xen_public.h>
#include <gntmem_ioctl.h>
#include <xen_gntmem.h>

static HANDLE
get_xen_interface_handle()
{
  HDEVINFO handle;
  SP_DEVICE_INTERFACE_DATA sdid;
  SP_DEVICE_INTERFACE_DETAIL_DATA *sdidd;
  DWORD buf_len;
  HANDLE h;

  handle = SetupDiGetClassDevs(&GUID_DEVINTERFACE_GNTMEM, 0, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (handle == INVALID_HANDLE_VALUE)
  {
    return INVALID_HANDLE_VALUE;
  }
  sdid.cbSize = sizeof(sdid);
  if (!SetupDiEnumDeviceInterfaces(handle, NULL, &GUID_DEVINTERFACE_GNTMEM, 0, &sdid))
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

struct grant_handle {

	struct grant_handle* next;
	struct ioctl_gntmem_grant_pages grant;
	OVERLAPPED overlapped;
	INT32 uid;

};

struct gntmem_handle {

	HANDLE h;
	struct grant_handle* first_grant;
	INT32 next_uid;

};

struct gntmem_handle* gntmem_open() {

	HANDLE h;
	struct gntmem_handle* new_handle;


	h = get_xen_interface_handle();
	if(h == INVALID_HANDLE_VALUE) {
		return NULL;
	}

	new_handle = malloc(sizeof(*new_handle));
	if(!new_handle) {
		CloseHandle(h);
		return NULL;
	}

	new_handle->h = h;
	new_handle->first_grant = NULL;
	new_handle->next_uid = 1;

	return new_handle;

}

void gntmem_close(struct gntmem_handle* h) {

	struct grant_handle* next_grant = h->first_grant;
	DWORD transferred;

	if(!CancelIo(h->h)) {
		fprintf(stderr, "Gntmem: Unable to cancel outstanding grants\n");
		return;
	}

	while(next_grant) {
		
		struct grant_handle* next = next_grant->next;
		// Wait for I/O to finish before freeing buffers
		if(WaitForSingleObject(next_grant->overlapped.hEvent, 5000) == WAIT_OBJECT_0) {
			// Event signalled; I/O should have finished
			if(!GetOverlappedResult(h->h, &next_grant->overlapped, &transferred, FALSE)) {
				// Should have died with STATUS_CANCELLED, which is an error
				if(GetLastError() != ERROR_IO_INCOMPLETE) {
					// i.e. it really is finished, and we can free
					CloseHandle(next_grant->overlapped.hEvent);
					free(next_grant);
				}
				else {
					fprintf(stderr, "Gntmem: grant %d incomplete after event had fired; leaking\n", next_grant->uid);
				}
			}
			else {
				fprintf(stderr, "Gntmem: grant %d completed successfully, which should never happen; leaking\n", next_grant->uid);
			}
		}
		else {
			fprintf(stderr, "Gntmem: timed out waiting for grant %d to be released; leaking\n", next_grant->uid);
		}

		next_grant = next;

	}

	CloseHandle(h->h);
	free(h); // Might leave some dangling grant_handles though, dependent on previous results

}

int gntmem_set_local_quota(struct gntmem_handle* h, int new_limit) {

	struct ioctl_gntmem_set_limit setlim;
	DWORD bytes_written;
	OVERLAPPED ol;

	setlim.new_limit = new_limit;
	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	if(!DeviceIoControl(h->h, IOCTL_GNTMEM_SET_LOCAL_LIMIT, &setlim, sizeof(setlim), NULL, 0, NULL, &ol)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(ol.hEvent);
			return -1;
		}
	}

	if(!GetOverlappedResult(h->h, &ol, &bytes_written, TRUE)) {
		CloseHandle(ol.hEvent);
		return -1;
	}

	CloseHandle(ol.hEvent);
	return 0;

}

int gntmem_set_global_quota(struct gntmem_handle* h, int new_limit) {

	struct ioctl_gntmem_set_limit setlim;
	DWORD bytes_written;
	OVERLAPPED ol;

	setlim.new_limit = new_limit;
	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	if(!DeviceIoControl(h->h, IOCTL_GNTMEM_SET_GLOBAL_LIMIT, &setlim, sizeof(setlim), NULL, 0, NULL, &ol)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(ol.hEvent);
			return -1;
		}
	}
	if(!GetOverlappedResult(h->h, &ol, &bytes_written, TRUE)) {
		CloseHandle(ol.hEvent);
		return -1;
	}

	CloseHandle(ol.hEvent);
	return 0;

}

void* gntmem_grant_pages_to_domain(struct gntmem_handle* h, domid_t domain, int n_pages, grant_ref_t* grants_out) {

	DWORD bytesWritten;
	int out_buffer_size;
	void* out_buffer;
	struct grant_handle* new_handle;
	struct ioctl_gntmem_get_grants get_grants;
	OVERLAPPED ggol;

	if(domain < 0)
		return NULL;
	if(n_pages <= 0)
		return NULL;

	new_handle = malloc(sizeof(struct grant_handle));
	if(!new_handle)
		return NULL;

	out_buffer_size = ((sizeof(void*)) + (n_pages * sizeof(grant_ref_t)));
	out_buffer = malloc(out_buffer_size);
	if(!out_buffer)
		return NULL;

	new_handle->uid = InterlockedIncrement(&h->next_uid);
	new_handle->grant.domid = domain;
	new_handle->grant.uid = new_handle->uid;
	new_handle->grant.n_pages = n_pages;
	memset(&new_handle->overlapped, 0, sizeof(OVERLAPPED));
	new_handle->overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	if(!DeviceIoControl(h->h, IOCTL_GNTMEM_GRANT_PAGES, &new_handle->grant, sizeof(new_handle->grant), NULL, 0, NULL, &new_handle->overlapped)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			free(out_buffer);
			free(new_handle);
			return NULL;
		}
	}

	get_grants.uid = new_handle->uid;
	ggol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	if(!DeviceIoControl(h->h, IOCTL_GNTMEM_GET_GRANTS, &get_grants, sizeof(get_grants),
		out_buffer, out_buffer_size, NULL, &ggol)) {
		/* Problem here: can't do anything about the pending ioctl above, which may
		   keep some pages granted until thread termination, because CancelIo kills all
		   IO from this thread, and therefore other sections too.

		   We just hope that failure of GET_GRANTS implies the section has gone away. */
			if(GetLastError() != ERROR_IO_PENDING) {
				CloseHandle(ggol.hEvent);
				free(out_buffer);
				fprintf(stderr, "Warning: gntmem library leaked %d bytes and two handles\n", sizeof(new_handle->grant));
				// Must leak new handle, which might be referenced by overlapped I/O.
				return NULL;
			}
	}

	if(!GetOverlappedResult(h->h, &ggol, &bytesWritten, TRUE)) {
		CloseHandle(ggol.hEvent);
		free(out_buffer);
		fprintf(stderr, "Warning: gntmem library leaked %d bytes and two handles\n", sizeof(new_handle->grant));
		// Must leak new handle, which might be referenced by overlapped I/O.
		return NULL;
	}

	memcpy(grants_out, (grant_ref_t*)(&(((void**)out_buffer)[1])), sizeof(grant_ref_t) * n_pages);
	new_handle->next = h->first_grant;
	h->first_grant = new_handle;
	return *((void**)out_buffer);

}

