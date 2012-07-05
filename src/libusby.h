#ifndef LIBUSBY_LIBUSBY_H
#define LIBUSBY_LIBUSBY_H

#include <stdint.h>

/* A workaround (`<windows.h>` defines `interface`). */
#ifdef interface
#undef interface
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libusby_context libusby_context;
typedef struct libusby_device libusby_device;
typedef struct libusby_device_handle libusby_device_handle;

typedef uint8_t libusby_endpoint_t;
typedef unsigned int libusby_timeout_t;

typedef enum libusby_transfer_status
{
	LIBUSBY_TRANSFER_COMPLETED,
	LIBUSBY_TRANSFER_ERROR,
	LIBUSBY_TRANSFER_TIMED_OUT,
	LIBUSBY_TRANSFER_CANCELLED,
	LIBUSBY_TRANSFER_STALL,
	LIBUSBY_TRANSFER_NO_DEVICE,
	LIBUSBY_TRANSFER_OVERFLOW,
} libusby_transfer_status;

typedef enum libusby_error
{
	LIBUSBY_SUCCESS = 0,
	LIBUSBY_ERROR_IO = -1,
	LIBUSBY_ERROR_INVALID_PARAM = -2,
	LIBUSBY_ERROR_ACCESS = -3,
	LIBUSBY_ERROR_NO_DEVICE = -4,
	LIBUSBY_ERROR_NOT_FOUND = -5,
	LIBUSBY_ERROR_BUSY = -6,
	LIBUSBY_ERROR_TIMEOUT = -7,
	LIBUSBY_ERROR_OVERFLOW = -8,
	LIBUSBY_ERROR_PIPE = -9,
	LIBUSBY_ERROR_INTERRUPTED = -10,
	LIBUSBY_ERROR_NO_MEM = -11,
	LIBUSBY_ERROR_NOT_SUPPORTED = -12,
	LIBUSBY_ERROR_OTHER = -99,
} libusby_error;

typedef enum libusby_transfer_type
{
	LIBUSBY_TRANSFER_TYPE_CONTROL = 0,
	LIBUSBY_TRANSFER_TYPE_ISOCHRONOUS = 1,
	LIBUSBY_TRANSFER_TYPE_BULK = 2,
	LIBUSBY_TRANSFER_TYPE_INTERRUPT = 3,
} libusby_transfer_type;

#define LIBUSBY_ENDPOINT_DIR_MASK 0x80
#define LIBUSBY_ENDPOINT_IN 0x80
#define LIBUSBY_ENDPOINT_OUT 0x00

typedef struct libusby_iso_packet_descriptor {
	int length;
	int actual_length;
	libusby_transfer_status status;
} libusby_iso_packet_descriptor;

typedef struct libusby_transfer libusby_transfer;

typedef void (* libusby_transfer_cb_fn)(libusby_transfer * transfer);

struct libusby_transfer
{
	libusby_device_handle * dev_handle;
	uint8_t flags;
	libusby_endpoint_t endpoint;
	uint8_t type;
	libusby_timeout_t timeout;
	libusby_transfer_status status;
	int length;
	int actual_length;
	libusby_transfer_cb_fn callback;
	void * user_data;
	uint8_t * buffer;
	int num_iso_packets;
	libusby_iso_packet_descriptor iso_packet_desc[1];
};

typedef struct libusby_device_descriptor
{
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint16_t bcdUSB;
	uint8_t  bDeviceClass;
	uint8_t  bDeviceSubClass;
	uint8_t  bDeviceProtocol;
	uint8_t  bMaxPacketSize0;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t  iManufacturer;
	uint8_t  iProduct;
	uint8_t  iSerialNumber;
	uint8_t  bNumConfigurations;
} libusby_device_descriptor;

typedef struct libusby_endpoint_descriptor
{
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint8_t  bEndpointAddress;
	uint8_t  bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t  bInterval;
	uint8_t  bRefresh;
	uint8_t  bSynchAddress;
} libusby_endpoint_descriptor;

typedef struct libusby_interface_descriptor
{
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bInterfaceNumber;
	uint8_t bAlternateSetting;
	uint8_t bNumEndpoints;
	uint8_t bInterfaceClass;
	uint8_t bInterfaceSubClass;
	uint8_t bInterfaceProtocol;
	uint8_t iInterface;
	libusby_endpoint_descriptor * endpoint;
} libusby_interface_descriptor;

typedef struct libusby_interface
{
	libusby_interface_descriptor * altsetting;
	int num_altsetting;
} libusby_interface;

typedef struct libusby_config_descriptor
{
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint16_t wTotalLength;
	uint8_t  bNumInterfaces;
	uint8_t  bConfigurationValue;
	uint8_t  iConfiguration;
	uint8_t  bmAttributes;
	uint8_t  MaxPower;
	libusby_interface * interface;
} libusby_config_descriptor;

/*typedef enum libusby_transfer_flags
{
	LIBUSBY_TRANSFER_SHORT_NOT_OK    = (1<<0),
	LIBUSBY_TRANSFER_FREE_BUFFER     = (1<<1),
	LIBUSBY_TRANSFER_FREE_TRANSFER   = (1<<2),
	LIBUSBY_TRANSFER_ADD_ZERO_PACKET = (1<<3),
} libusby_transfer_flags;*/

/* Library initialization/exit */
int libusby_init(libusby_context ** ctx);
void libusby_exit(libusby_context * ctx);

/* Device handling and enumeration */
int libusby_get_device_list(libusby_context * ctx, libusby_device *** list);
void libusby_free_device_list(libusby_device ** list, int unref_devices);

libusby_device * libusby_ref_device(libusby_device * dev);
void libusby_unref_device(libusby_device * dev);

int libusby_open(libusby_device * dev, libusby_device_handle ** dev_handle);
libusby_device_handle * libusby_open_device_with_vid_pid(libusby_context * ctx, uint16_t vendor_id, uint16_t product_id);
void libusby_close(libusby_device_handle * dev_handle);
libusby_device * libusby_get_device(libusby_device_handle * dev_handle);

int libusby_get_configuration(libusby_device_handle * dev_handle, int * config_value);
int libusby_get_configuration_cached(libusby_device_handle * dev_handle, int * config_value);
int libusby_set_configuration(libusby_device_handle * dev_handle, int config_value);
int libusby_set_interface_alt_setting(libusby_device_handle * dev_handle, int interface_number, int alternate_setting);

int libusby_claim_interface(libusby_device_handle * dev_handle, int interface_number);
int libusby_release_interface(libusby_device_handle * dev_handle, int interface_number);

int libusby_clear_halt(libusby_device_handle * dev_handle, libusby_endpoint_t endpoint);
int libusby_reset_device(libusby_device_handle * dev_handle);

/* USB descriptors */
int libusby_get_device_descriptor_cached(libusby_device * dev, libusby_device_descriptor * desc);
int libusby_get_config_descriptor_cached(libusby_device * dev, uint8_t config_index, libusby_config_descriptor ** config);

int libusby_get_device_descriptor(libusby_device_handle * dev_handle, libusby_device_descriptor * desc);
int libusby_get_active_config_descriptor(libusby_device_handle * dev_handle, libusby_config_descriptor ** config);
int libusby_get_config_descriptor(libusby_device_handle * dev_handle, uint8_t config_index, libusby_config_descriptor ** config);
int libusby_get_config_descriptor_by_value(libusby_device_handle * dev_handle, uint8_t config_value, libusby_config_descriptor ** config);
void libusby_free_config_descriptor(libusby_config_descriptor * config);
int libusby_get_string_descriptor_ascii(libusby_device_handle * dev_handle, uint8_t desc_index, unsigned char * data, int length);
int libusby_get_descriptor(libusby_device_handle * dev_handle, uint8_t desc_type, uint8_t desc_index, unsigned char * data, int length);
int libusby_get_string_descriptor(libusby_device_handle * dev_handle, uint8_t desc_index, uint16_t langid, unsigned char * data, int length);
int libusby_get_string_descriptor_utf8(libusby_device_handle * dev_handle, uint8_t desc_index, uint16_t langid, char * data, int length);

/* Asynchronous device I/O */
libusby_transfer * libusby_alloc_transfer(libusby_context * ctx, int iso_packets);
void libusby_free_transfer(libusby_transfer * transfer);
int libusby_submit_transfer(libusby_transfer * transfer);
int libusby_wait_for_transfer(libusby_transfer * transfer); // XXX: perhaps this shouldn't return error?
int libusby_perform_transfer(libusby_transfer * transfer);
int libusby_cancel_transfer(libusby_transfer * transfer);
uint8_t * libusby_control_transfer_get_data(libusby_transfer * transfer);
void libusby_fill_control_setup(uint8_t * buffer, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength);
void libusby_fill_control_transfer(libusby_transfer * transfer, libusby_device_handle * dev_handle, uint8_t * buffer, libusby_transfer_cb_fn callback, void * user_data, libusby_timeout_t timeout);
void libusby_fill_bulk_transfer(libusby_transfer * transfer, libusby_device_handle * dev_handle, libusby_endpoint_t endpoint, uint8_t * buffer, int length,
	libusby_transfer_cb_fn callback, void * user_data, libusby_timeout_t timeout);
void libusby_fill_interrupt_transfer(libusby_transfer * transfer, libusby_device_handle * dev_handle, libusby_endpoint_t endpoint,
	uint8_t * buffer, int length, libusby_transfer_cb_fn callback, void * user_data, libusby_timeout_t timeout);

/* Synchronous device I/O */
int libusby_control_transfer(libusby_device_handle * dev_handle, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint8_t * data, uint16_t wLength, libusby_timeout_t timeout);
int libusby_bulk_transfer(libusby_device_handle * dev_handle, libusby_endpoint_t endpoint, uint8_t * data, int length, int * transferred, libusby_timeout_t timeout);
int libusby_interrupt_transfer(libusby_device_handle * dev_handle, libusby_endpoint_t endpoint, uint8_t * data, int length, int * transferred, libusby_timeout_t timeout);

/* Event loop */
int libusby_run_event_loop(libusby_context * ctx);
void libusby_stop_event_loop(libusby_context * ctx);
void libusby_reset_event_loop(libusby_context * ctx);

#ifdef __cplusplus
}
#endif

#endif // LIBUSBY_LIBUSBY_H
