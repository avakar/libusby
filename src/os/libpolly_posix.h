#ifndef LIBUSBY_OS_LIBPOLLY_POSIX_H
#define LIBUSBY_OS_LIBPOLLY_POSIX_H

#include "../libpolly.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*libpolly_posix_callback)(int fd, short revents, void * user_data);

int libpolly_posix_prepare_add(libpolly_context * ctx);
void libpolly_posix_cancel_add(libpolly_context * ctx);
void libpolly_posix_add(libpolly_context * ctx, int fd, short events, libpolly_posix_callback callback, void * user_data);

pthread_mutex_t * libpolly_posix_get_loop_mutex(libpolly_context * ctx);

// loop mutex must be locked
int libpolly_posix_acquire_loop(libpolly_context * ctx);
void libpolly_posix_release_loop(libpolly_context * ctx);

typedef struct libpolly_posix_loop_release_registration
{
	pthread_cond_t * cond;
	libpolly_context * ctx;
	struct libpolly_posix_loop_release_registration * next;
	struct libpolly_posix_loop_release_registration * prev;
} libpolly_posix_loop_release_registration;

void libpolly_posix_register_loop_release_notification(libpolly_context * ctx, libpolly_posix_loop_release_registration * registration);
void libpolly_posix_unregister_loop_release_notification(libpolly_posix_loop_release_registration * registration);

// loop tokem must be acquired
typedef int (*libpolly_posix_run_callback)(int fd, short revents, libpolly_posix_callback callback, void * callback_data, void * run_data);
int libpolly_posix_run(libpolly_context * ctx, libpolly_posix_run_callback run_callback, void * run_data);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LIBUSBY_OS_LIBPOLLY_POSIX_H
