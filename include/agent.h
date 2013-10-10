#pragma once

#ifdef BACKEND_VMM_wni
#define	TRIGGER_PIPE_NAME	TEXT("\\\\.\\pipe\\%s\\qrexec-trigger")
#else
#define	TRIGGER_PIPE_NAME	TEXT("\\\\.\\pipe\\qrexec-trigger")
#endif
