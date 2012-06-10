#ifndef LIBUSBY_OS_LIBUSB0_WIN32_INTF_H
#define LIBUSBY_OS_LIBUSB0_WIN32_INTF_H

#include <stdint.h>

#define LIBUSB_MAX_NUMBER_OF_DEVICES 256

#define LIBUSB_IOCTL_SET_CONFIGURATION        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_GET_CONFIGURATION        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_GET_DESCRIPTOR           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_INTERRUPT_OR_BULK_WRITE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80A, METHOD_IN_DIRECT,  FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_INTERRUPT_OR_BULK_READ   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80B, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_ABORT_ENDPOINT           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80F, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_RESET_DEVICE             CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_CLAIM_INTERFACE          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x815, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_RELEASE_INTERFACE        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x816, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_GET_CACHED_CONFIGURATION CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_CONTROL_WRITE            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x90A, METHOD_IN_DIRECT,  FILE_ANY_ACCESS)
#define LIBUSB_IOCTL_CONTROL_READ             CTL_CODE(FILE_DEVICE_UNKNOWN, 0x90B, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

typedef struct libusb0_win32_request
{
	uint32_t timeout;

	union {
		uint8_t _req_size[20];

		struct {
			uint32_t configuration;
		} configuration;

		struct {
			uint32_t interface_number;
			uint32_t altsetting_number;
		} intf;

		struct {
			uint32_t endpoint;
			uint32_t packet_size;

			uint32_t max_transfer_size;
			uint32_t transfer_flags;
			uint32_t iso_start_frame_latency;
		} endpoint;

		struct {
			uint32_t type;
			uint32_t index;
			uint32_t language_id;
			uint32_t recipient;
		} descriptor;

		struct {
			uint8_t  bmRequestType;
			uint8_t  bRequest;
			uint16_t wValue;
			uint16_t wIndex;
			uint16_t wLength;
		} control;
	};
} libusb0_win32_request;

#endif // LIBUSBY_OS_LIBUSB0_WIN32_INTF_H
