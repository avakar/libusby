#ifndef LIBUSBY_OS_LIBPOLLY_WIN32_H
#define LIBUSBY_OS_LIBPOLLY_WIN32_H

#include "../libpolly.h"
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (* libpolly_win32_callback)(HANDLE handle, void * user_data);
int libpolly_win32_add_handle(libpolly_context * ctx, HANDLE handle, libpolly_win32_callback callback, void * user_data);
void libpolly_win32_remove_handles(libpolly_context * ctx, HANDLE * handles, int handle_count);
int libpolly_win32_wait_until(libpolly_context * ctx, HANDLE handle);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LIBUSBY_OS_LIBPOLLY_WIN32_H
