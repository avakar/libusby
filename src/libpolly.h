#ifndef LIBUSBY_LIBPOLLY_H
#define LIBUSBY_LIBPOLLY_H

#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libpolly_context libpolly_context;

int libpolly_init(libpolly_context ** ctx);
void libpolly_ref_context(libpolly_context * ctx);
void libpolly_unref_context(libpolly_context * ctx);

typedef void (* libpolly_task_callback)(void * user_data);
typedef struct libpolly_task libpolly_task;
int libpolly_prepare_task(libpolly_context * ctx, libpolly_task ** task);
void libpolly_cancel_task(libpolly_task * task);
void libpolly_submit_task(libpolly_task * task, libpolly_task_callback callback, void * user_data);
int libpolly_submit_task_direct(libpolly_context * ctx, libpolly_task_callback callback, void * user_data);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LIBUSBY_LIBPOLLY_H
