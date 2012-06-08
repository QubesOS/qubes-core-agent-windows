#pragma once


#ifndef _PREFAST_
#pragma warning(disable:4068)
#endif

#define BUILD_AS_SERVICE

#define SERVICE_NAME	TEXT("qrexec_agent")
#define DEFAULT_USER_PASSWORD_UNICODE	L"userpass"

#ifdef DBG
#define LOG_FILE		TEXT("c:\\qrexec_agent.log")
#endif

//#define DISPLAY_CONSOLE_OUTPUT

