#ifndef LIBUSBY_OS_OS_H
#define LIBUSBY_OS_OS_H

#include "../libusby.h"

typedef struct usbyb_context usbyb_context;
typedef struct usbyb_device usbyb_device;
typedef struct usbyb_device_handle usbyb_device_handle;
typedef struct usbyb_transfer usbyb_transfer;

extern int const usbyb_context_size;
extern int const usbyb_device_size;
extern int const usbyb_device_handle_size;
extern int const usbyb_transfer_size;
extern int const usbyb_transfer_pub_offset;

int usbyb_init(usbyb_context * ctx);
void usbyb_exit(usbyb_context * ctx);

int usbyb_get_device_list(usbyb_context * ctx, libusby_device *** list);
void usbyb_finalize_device(usbyb_device * dev);

int usbyb_open(usbyb_device_handle *dev_handle); // opt
void usbyb_close(usbyb_device_handle *dev_handle); // opt

int usbyb_get_descriptor(usbyb_device_handle * dev_handle, uint8_t desc_type, uint8_t desc_index, uint16_t langid, unsigned char * data, int length); // opt
int usbyb_get_descriptor_cached(usbyb_device * dev, uint8_t desc_type, uint8_t desc_index, uint16_t langid, unsigned char * data, int length); // opt

int usbyb_get_configuration(usbyb_device_handle * dev_handle, int * config_value, int cached_only); // opt
int usbyb_set_configuration(usbyb_device_handle * dev_handle, int config_value); // opt

int usbyb_claim_interface(usbyb_device_handle * dev_handle, int interface_number); // opt
int usbyb_release_interface(usbyb_device_handle * dev_handle, int interface_number); // opt

int usbyb_perform_transfer(usbyb_transfer * tran); // opt
int usbyb_submit_transfer(usbyb_transfer * tran);
int usbyb_cancel_transfer(usbyb_transfer * tran);

int usbyb_wait_for_transfer(usbyb_transfer * transfer);

int usbyb_init_transfer(usbyb_transfer * tran);
void usbyb_clear_transfer(usbyb_transfer * tran);

#endif
