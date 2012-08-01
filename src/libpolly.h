#ifndef LIBUSBY_LIBPOLLY_H
#define LIBUSBY_LIBPOLLY_H

#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libpolly_context libpolly_context;
typedef struct libpolly_event_loop libpolly_event_loop;

int libpolly_init(libpolly_context ** ctx);
void libpolly_ref_context(libpolly_context * ctx);
void libpolly_unref_context(libpolly_context * ctx);

int libpolly_start_event_loop(libpolly_context * ctx, libpolly_event_loop ** loop);
void libpolly_join_event_loop(libpolly_event_loop * loop);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LIBUSBY_LIBPOLLY_H
