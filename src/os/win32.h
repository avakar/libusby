#ifndef LIBUSBY_OS_WIN32_H
#define LIBUSBY_OS_WIN32_H

#include "../libusby.h"
#include "../libusbyi_fwd.h"
#include <windows.h>
#ifdef interface
#undef interface
#endif

struct usbyi_os_transfer
{
	HANDLE hCompletionEvent;
	OVERLAPPED overlapped;
	int submitted;
};

#endif
