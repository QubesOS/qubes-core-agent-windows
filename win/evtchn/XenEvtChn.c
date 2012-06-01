
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <winioctl.h>
#include <stdlib.h>

#include <xen_public.h>
#include <evtchn_ioctl.h>
#include <xenctrl.h>

static HANDLE
get_xen_interface_handle()
{
  HDEVINFO handle;
  SP_DEVICE_INTERFACE_DATA sdid;
  SP_DEVICE_INTERFACE_DETAIL_DATA *sdidd;
  DWORD buf_len;
  HANDLE h;

  handle = SetupDiGetClassDevs(&GUID_DEVINTERFACE_EVTCHN, 0, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (handle == INVALID_HANDLE_VALUE)
  {
    return INVALID_HANDLE_VALUE;
  }
  sdid.cbSize = sizeof(sdid);
  if (!SetupDiEnumDeviceInterfaces(handle, NULL, &GUID_DEVINTERFACE_EVTCHN, 0, &sdid))
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

/*
static void report_error() {

	DWORD err = GetLastError();
	LPWSTR errbuf;
	if(!FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err, 0, (LPWSTR)&errbuf, 0, NULL)) {
			printf("Error: %x. Failed to get description (error %x)\n", err, GetLastError());
	}
	else {
		printf("Error: %x (%ws)\n", err, errbuf);
		LocalFree(errbuf);
	}

}
*/
/*
 * Return a handle to the event channel driver, or an invalid handle on failure, in which case
 * see GetLastError for details.
 */
HANDLE xc_evtchn_open(void) {

	return get_xen_interface_handle();
}

/*
 * Close a handle previously allocated with xc_evtchn_open().
 */
int xc_evtchn_close(HANDLE xce_handle) {

	CloseHandle(xce_handle);
	return 0;

}

/*
 * Return a handle that can be used in various Win32 asynchronous I/O
 * techniques in lieu of calls to xc_evtchn_pending and _unmask.
 * The handle is opened with FLAG_OVERLAPPED. Reading chunks of size evtchn_port_t will yield live ports,
 * whilst writing will unmask those ports.
 */
HANDLE xc_evtchn_fd(HANDLE xce_handle) {

	return xce_handle;

}

/*
 * Notify the given event channel. Returns -1 on failure, in which case
 * call GetLastError for more details.
 */
int xc_evtchn_notify(HANDLE xce_handle, evtchn_port_t port) {

	struct ioctl_evtchn_notify params;
	DWORD bytes_written;
	OVERLAPPED ol;


	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	params.port = port;

	if(!DeviceIoControl(xce_handle, IOCTL_EVTCHN_NOTIFY, (LPVOID)&params, sizeof(params), NULL, 0, NULL, &ol)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(ol.hEvent);
			return -1;
		}
	}
	if(!GetOverlappedResult(xce_handle, &ol, &bytes_written, TRUE)) { /* Wait */
		CloseHandle(ol.hEvent);
		return -1;
	}
	else {
		CloseHandle(ol.hEvent);
		return 0;
	}

}

/*
 * Returns a new event port awaiting interdomain connection from the given
 * domain ID, or -1 on failure, in which case call GetLastError for more information.
 */
evtchn_port_or_error_t
xc_evtchn_bind_unbound_port(HANDLE xce_handle, int domid) {

	struct ioctl_evtchn_bind_unbound_port params;
	unsigned int port_out;
	DWORD bytes_written; 
	OVERLAPPED ol;


	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	params.remote_domain = domid;

	if(!DeviceIoControl(xce_handle, IOCTL_EVTCHN_BIND_UNBOUND_PORT, (LPVOID)&params, sizeof(params), &port_out, sizeof(port_out), NULL, &ol)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(ol.hEvent);
			return -1;
		}
	}

	if(!GetOverlappedResult(xce_handle, &ol, &bytes_written, TRUE)) /* Wait */ {
		CloseHandle(ol.hEvent);
		return -1;
	}
	else {
		CloseHandle(ol.hEvent);
		return (int)port_out;
	}

}

/*
 * Returns a new event port bound to the remote port for the given domain ID,
 * or -1 on failure, in which case call GetLastError for more information.
 */
evtchn_port_or_error_t
xc_evtchn_bind_interdomain(HANDLE xce_handle, int domid, evtchn_port_t remote_port) {

	struct ioctl_evtchn_bind_interdomain params;
	unsigned int port_out;
	DWORD bytes_written;
	OVERLAPPED ol;


	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	params.remote_domain = domid;
	params.remote_port = remote_port;

	if(!DeviceIoControl(xce_handle, IOCTL_EVTCHN_BIND_INTERDOMAIN, (LPVOID)&params, sizeof(params), &port_out, sizeof(port_out), NULL, &ol)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(ol.hEvent);
			return -1;
		}
	}

	if(!GetOverlappedResult(xce_handle, &ol, &bytes_written, TRUE)) {
		CloseHandle(ol.hEvent);
		return -1;
	} else {
		CloseHandle(ol.hEvent);
		return (int)port_out;
	}

}

/*
 * Bind an event channel to the given VIRQ. Returns the event channel bound to
 * the VIRQ, or -1 on failure, in which call GetLastError for more information.
 */
evtchn_port_or_error_t
xc_evtchn_bind_virq(HANDLE xce_handle, unsigned int virq) {

	struct ioctl_evtchn_bind_virq params;
	DWORD bytes_written;
	OVERLAPPED ol;
	unsigned int port_out;


	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	params.virq = virq;

	if(!DeviceIoControl(xce_handle, IOCTL_EVTCHN_BIND_VIRQ, (LPVOID)&params, sizeof(params), &port_out, sizeof(port_out), NULL, &ol)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(ol.hEvent);
			return -1;
		}
	}

	if(!GetOverlappedResult(xce_handle, &ol, &bytes_written, TRUE)) {
		CloseHandle(ol.hEvent);
		return -1;
	}
	else {
		CloseHandle(ol.hEvent);
		return (int)port_out;
	}

}

/*
 * Unbind the given event channel. Returns -1 on failure, in which case call 
 * GetLastError for more information.
 */
int xc_evtchn_unbind(HANDLE xce_handle, evtchn_port_t port) {

	struct ioctl_evtchn_unbind params;
	DWORD bytes_written;
	OVERLAPPED ol;


	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	params.port = port;

	if(!DeviceIoControl(xce_handle, IOCTL_EVTCHN_UNBIND, (LPVOID)&params, sizeof(params), NULL, 0, &bytes_written, NULL)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(ol.hEvent);
			return -1;
		}
	}
	
	if(!GetOverlappedResult(xce_handle, &ol, &bytes_written, TRUE)) {
		CloseHandle(ol.hEvent);
		return -1;
	}
	else {
		CloseHandle(ol.hEvent);
		return 0;
	}

}

/*
 * Return the next event channel to become pending, or -1 on failure, in which
 * case call GetLastError for more information.
 */
evtchn_port_or_error_t
xc_evtchn_pending(HANDLE xce_handle) {

	DWORD bytes_read;
	OVERLAPPED ol;
	unsigned int fired_port;


	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);


	if(!ReadFile(xce_handle, &fired_port, sizeof(fired_port), NULL, &ol)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(ol.hEvent);
			return -1;
		}
	}
	if(!GetOverlappedResult(xce_handle, &ol, &bytes_read, TRUE)) {
		CloseHandle(ol.hEvent);
		return -1;
	}
	else {
		CloseHandle(ol.hEvent);
		return fired_port;
	}

}

/*
 * Unmask the given event channel. Returns -1 on failure, in which case call 
 * GetLastError for more information.
 */
int xc_evtchn_unmask(HANDLE xce_handle, evtchn_port_t port) {

	DWORD bytes_written;
	OVERLAPPED ol;


	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	if(!WriteFile(xce_handle, &port, sizeof(port), NULL, &ol)) {
		if(GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(ol.hEvent);
			return -1;
		}
	}

	if(!GetOverlappedResult(xce_handle, &ol, &bytes_written, TRUE)) {
		CloseHandle(ol.hEvent);
		return -1;
	}
	else {
		CloseHandle(ol.hEvent);
		return 0;
	}

}
