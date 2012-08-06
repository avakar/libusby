#ifndef LIBSPY_OS_SPYI_H
#define LIBSPY_OS_SPYI_H

#include "../libpolly.h"

typedef struct spyb_context spyb_context;
typedef struct spyb_device spyb_device;
typedef struct spyb_transfer spyb_transfer;

typedef struct spyi_list
{
	struct spyi_list * prev;
	struct spyi_list * next;
} spyi_list;

void spyi_init_list(spyi_list * list);

struct libspy_context
{
	libpolly_context * polly;
};

typedef enum spyi_transfer_flags
{
	spyi_tf_resubmit = 1
} spyi_transfer_flags;

struct libspy_transfer
{
	spyi_list transfer_list;

	spyb_context * ctx;
	void * user_data;

	uint8_t * buf;
	int length;
	int actual_length;

	int flags;

	libspy_device_handle * handle;
	void (* callback)(libspy_transfer * transfer, libspy_transfer_status status);
};

extern int const spyb_context_size;
extern int const spyb_transfer_size;

int spyb_init(spyb_context * ctx);
int spyb_init(spyb_context * ctx);
void spyb_exit(spyb_context * ctx);

int spyb_init_transfer(spyb_transfer * transfer);
void spyb_destroy_transfer(spyb_transfer * transfer);
void spyb_cancel_transfer(spyb_transfer * transfer);

void spyb_wait_for_transfer(spyb_transfer * transfer);
int spyb_submit_read(spyb_transfer * transfer);

#endif // LIBSPY_OS_SPYI_H
