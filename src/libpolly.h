#ifndef LIBUSBY_LIBPOLLY_H
#define LIBUSBY_LIBPOLLY_H

#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libpolly_context libpolly_context;

int libpolly_init(libpolly_context ** ctx);
int libpolly_init_with_worker(libpolly_context ** ctx);
void libpolly_ref_context(libpolly_context * ctx);
void libpolly_unref_context(libpolly_context * ctx);

typedef struct libpolly_event libpolly_event;
int libpolly_create_event(libpolly_context * ctx, libpolly_event ** event);
void libpolly_reset_event(libpolly_event * event);
void libpolly_set_event(libpolly_event * event);
void libpolly_wait_for_event(libpolly_event * event);
void libpolly_destroy_event(libpolly_event * event);

typedef void (* libpolly_task_callback)(void * user_data);
typedef struct libpolly_task libpolly_task;
int libpolly_prepare_task(libpolly_context * ctx, libpolly_task ** task);
void libpolly_cancel_task(libpolly_task * task);
void libpolly_submit_task(libpolly_task * task, libpolly_task_callback callback, void * user_data);
int libpolly_submit_task_direct(libpolly_context * ctx, libpolly_task_callback callback, void * user_data);

typedef enum libpolly_timer_status { libpolly_timer_completed, libpolly_timer_cancelled } libpolly_timer_status;
typedef void (* libpolly_timer_callback)(void * user_data, libpolly_timer_status status);
typedef struct libpolly_timer libpolly_timer;
int libpolly_create_timer(libpolly_context * ctx, libpolly_timer ** timer);
void libpolly_destroy_timer(libpolly_timer * timer);
void libpolly_set_timer(libpolly_timer * timer, int timeout, libpolly_timer_callback callback, void * user_data);
void libpolly_cancel_timer(libpolly_timer * timer);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LIBUSBY_LIBPOLLY_H
