#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "qrexec.h"
#include "libvchan.h"
#include "glue.h"





static HANDLE Execute(PUCHAR pszCommand, HANDLE hPipeStdin, HANDLE hPipeStdout)
{
	PROCESS_INFORMATION	pi;
	STARTUPINFO	si;
	HANDLE	hProcess = INVALID_HANDLE_VALUE;


	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

//	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;

	si.hStdInput = hPipeStdin;
	si.hStdOutput = hPipeStdout;

	DuplicateHandle(
		GetCurrentProcess(),
		hPipeStdout, 
		GetCurrentProcess(), 
		&si.hStdError, 
		DUPLICATE_SAME_ACCESS, 
		TRUE, 
		0);

	if (CreateProcess(NULL, pszCommand, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		hProcess = pi.hProcess;
		CloseHandle(pi.hThread);
	} else 
		fprintf(stderr, "Execute(): Failed to run \"%s\", error = %d\n", pszCommand, GetLastError());


	return hProcess;
}


void handle_exec(int client_id, int len)
{
	char *buf;
	int pid, stdin_fd, stdout_fd, stderr_fd;
	HANDLE	hProcess;

	buf = malloc(len + 1);
	if (!buf)
		return;
	buf[len] = 0;


	read_all_vchan_ext(buf, len);

//	do_fork_exec(buf, &pid, &stdin_fd, &stdout_fd, &stderr_fd);
 
//	create_info_about_client(client_id, pid, stdin_fd, stdout_fd,
//				 stderr_fd);

	hProcess = Execute(buf, NULL, NULL);
	if (hProcess != INVALID_HANDLE_VALUE) {
		fprintf(stderr, "executed %s\n", buf);
		CloseHandle(hProcess);
	}

	free(buf);
}

void handle_just_exec(int client_id, int len)
{
	char *buf;
	int fdn, pid;
	HANDLE	hProcess;

	buf = malloc(len + 1);
	if (!buf)
		return;
	buf[len] = 0;

	read_all_vchan_ext(buf, len);
//	switch (pid = fork()) {
//	case -1:
//		perror("fork");
//		exit(1);
//	case 0:
//		fdn = open("/dev/null", O_RDWR);
//		fix_fds(fdn, fdn, fdn);
//		do_exec(buf);
//		perror("execl");
//		exit(1);
//	default:;
//	}
	hProcess = Execute(buf, NULL, NULL);
	if (hProcess != INVALID_HANDLE_VALUE) {
		fprintf(stderr, "executed (nowait) %s\n", buf);
		CloseHandle(hProcess);
	}
	free(buf);
}


void handle_input(int client_id, int len)
{
	char *buf;


	buf = malloc(len + 1);
	if (!buf)
		return;
	buf[len] = 0;

	read_all_vchan_ext(buf, len);
//	if (!client_info[client_id].pid)
//		return;
/*
	if (len == 0) {
		if (client_info[client_id].is_blocked)
			client_info[client_id].is_close_after_flush_needed
			 = 1;
		else {
			close(client_info[client_id].stdin_fd);
			client_info[client_id].stdin_fd = -1;
		}
		return;
	}

	switch (write_stdin
		(client_info[client_id].stdin_fd, client_id, buf, len,
		 &client_info[client_id].buffer)) {
	case WRITE_STDIN_OK:
		break;
	case WRITE_STDIN_BUFFERED:
		client_info[client_id].is_blocked = 1;
		break;
	case WRITE_STDIN_ERROR:
		remove_process(client_id, 128);
		break;
	default:
		fprintf(stderr, "unknown write_stdin?\n");
		exit(1);
	}
*/
	fprintf(stderr, "%s\n", buf);
	free(buf);
}




void handle_server_data()
{
	struct server_header s_hdr;


	read_all_vchan_ext(&s_hdr, sizeof s_hdr);
	fprintf(stderr, "got %x %x %x\n", s_hdr.type, s_hdr.client_id, s_hdr.len);

	switch (s_hdr.type) {
	case MSG_XON:
		fprintf(stderr, "MSG_XON\n");
//		set_blocked_outerr(s_hdr.client_id, 0);
		break;
	case MSG_XOFF:
		fprintf(stderr, "MSG_XOFF\n");
//		set_blocked_outerr(s_hdr.client_id, 1);
		break;
	case MSG_SERVER_TO_AGENT_CONNECT_EXISTING:
		fprintf(stderr, "MSG_SERVER_TO_AGENT_CONNECT_EXISTING\n");
//		handle_connect_existing(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_EXEC_CMDLINE:
		fprintf(stderr, "MSG_SERVER_TO_AGENT_EXEC_CMDLINE\n");
//		wake_meminfo_writer();
		handle_exec(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_JUST_EXEC:
		fprintf(stderr, "MSG_SERVER_TO_AGENT_JUST_EXEC\n");
//		wake_meminfo_writer();
		handle_just_exec(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_INPUT:
		fprintf(stderr, "MSG_SERVER_TO_AGENT_INPUT\n");
		handle_input(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_CLIENT_END:
		fprintf(stderr, "MSG_SERVER_TO_AGENT_CLIENT_END\n");
//		remove_process(s_hdr.client_id, -1);
		break;
	default:
		fprintf(stderr, "msg type from daemon is %d ?\n",
			s_hdr.type);
		exit(1);
	}
}


VOID __cdecl main()
{
	EVTCHN	evtchn;
	OVERLAPPED	ol;
	DWORD bytes_read;
	unsigned int fired_port;


	peer_server_init(REXEC_PORT);
	evtchn = libvchan_fd_for_select(ctrl);

	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);



	for (;;) {

		libvchan_prepare_to_select(ctrl);

		if (!ReadFile(evtchn, &fired_port, sizeof(fired_port), NULL, &ol)) {
			if (GetLastError() != ERROR_IO_PENDING) {
				printf("Async read failed, last error: %d\n", GetLastError());
//				CloseHandle(ol.hEvent);
//				return -1;
			}
		}


		WaitForSingleObject(ol.hEvent, INFINITE);


		if (libvchan_is_eof(ctrl))
			break;


		while (read_ready_vchan_ext())
			handle_server_data();

	}

	libvchan_close(ctrl);
	CloseHandle(ol.hEvent);


	return;
}