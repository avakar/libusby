#ifndef LIBUSBY_OS_WIN32_H
#define LIBUSBY_OS_WIN32_H

#include "../libusby.h"
#include "../libusbyi_fwd.h"
#include <windows.h>
#ifdef interface
#undef interface
#endif

typedef struct usbyi_handle_list
{
	int capacity;
	HANDLE * handles;
} usbyi_handle_list;

struct usbyi_os_ctx
{
	CRITICAL_SECTION ctx_mutex;

	usbyi_transfer * trani_first;
	usbyi_transfer * trani_last;
	int tran_count;

	usbyi_handle_list handle_list;

	HANDLE hReaperLock;
	HANDLE hEventLoopStopped;
	HANDLE hTransferListUpdated;
};

struct usbyi_os_transfer
{
	HANDLE hCompletionEvent;
	OVERLAPPED overlapped;
	int submitted;
};

int usbyi_init_os_ctx(libusby_context * ctx);
void usbyi_clear_os_ctx(libusby_context * ctx);

int usbyi_init_os_transfer(usbyi_transfer * trani);
void usbyi_clear_os_transfer(usbyi_transfer * trani);

void usbyi_win32_add_transfer(usbyi_transfer * trani);
void usbyi_win32_remove_transfer(usbyi_transfer * trani);

#endif
