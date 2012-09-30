#ifndef LIBSPY_LIBSPY_H
#define LIBSPY_LIBSPY_H

#include <stdint.h>
#include "libpolly.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum libspy_error
{
	LIBSPY_SUCCESS = 0,
	LIBSPY_ERROR_NO_MEM = -11
} libspy_error;

typedef enum libspy_transfer_status
{
	libspy_transfer_completed,
	libspy_transfer_canceled,
	libspy_transfer_error
} libspy_transfer_status;

typedef struct libspy_context libspy_context;
typedef struct libspy_device_handle libspy_device_handle;
typedef struct libspy_transfer libspy_transfer;
typedef struct libspy_open_future libspy_open_future;

typedef struct libspy_device
{
	char const * name;
	char const * friendly_name;
	char const * path;
} libspy_device;

typedef enum libspy_parity
{
	libspy_parity_none,
	libspy_parity_odd,
	libspy_parity_even,
	libspy_parity_mark,
	libspy_parity_space
} libspy_parity;

typedef enum libspy_stopbits
{
	libspy_stopbits_1,
	libspy_stopbits_1_5,
	libspy_stopbits_2
} libspy_stopbits;

typedef struct libspy_device_settings
{
	int timeout;
	int baud_rate;
	libspy_parity parity;
	int bits;
	libspy_stopbits stopbits;
} libspy_device_settings;

int libspy_init(libspy_context ** ctx);
int libspy_init_with_polly(libspy_context ** ctx, libpolly_context * polly);
void libspy_exit(libspy_context * ctx);

libpolly_context * libspy_get_polly(libspy_context * ctx);

int libspy_get_device_list(libspy_context * ctx, libspy_device const ** device_list);
void libspy_free_device_list(libspy_device const * device_list);

typedef void (* libspy_open_callback)(libspy_open_future * future, void * user_data, libspy_device_handle * handle);
int libspy_begin_open(libspy_context * ctx, char const * path, libspy_device_settings const * settings, libspy_open_callback callback, void * user_data, libspy_open_future ** future);
int libspy_is_open_cancelable(libspy_context * ctx);
void libspy_cancel_open(libspy_open_future * future);
void libspy_wait_for_open(libspy_open_future * future);
void libspy_free_open_future(libspy_open_future * future);

int libspy_open(libspy_context * ctx, char const * path, libspy_device_settings const * settings, libspy_device_handle ** handle);
void libspy_close(libspy_device_handle * handle);
libspy_context * libspy_get_context(libspy_device_handle * handle);

int libspy_read(libspy_device_handle * handle, uint8_t * buf, int len);
int libspy_write(libspy_device_handle * handle, uint8_t const * buf, int len);

libspy_transfer * libspy_alloc_transfer(libspy_context * ctx);
void libspy_free_transfer(libspy_transfer * transfer);
void libspy_cancel_transfer(libspy_transfer * transfer);
void libspy_wait_for_transfer(libspy_transfer * transfer);

void libspy_set_user_data(libspy_transfer * transfer, void * user_data);
void * libspy_get_user_data(libspy_transfer * transfer);

int libspy_get_transfer_length(libspy_transfer * transfer);

int libspy_submit_read(libspy_transfer * transfer,
	libspy_device_handle * handle, uint8_t * buf, int len,
	void (* callback)(libspy_transfer * transfer, libspy_transfer_status status));

int libspy_submit_write(libspy_transfer * transfer,
	libspy_device_handle * handle, uint8_t const * buf, int len,
	void (* callback)(libspy_transfer * transfer, libspy_transfer_status status));

#ifdef __cplusplus
}
#endif

#endif // LIBSPY_LIBSPY_H
