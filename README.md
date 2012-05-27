This is a complete rewrite of libusb-1.0 which addresses some
of its design issues, while maintaining a nearly identical API.
The changes to the API were made carefully so as not to silently
change the behavior of existing programs, should they decide to use
`libusby`.

# What is libusby?

It is a library that provides access to USB devices connected to
the system. The client application can perform transfers directly
to and from the USB endpoints of the device.

# Where can I find the documentation?

There will soon be an auto-genereated documentation available,
for now, browse the [libusb-1.0 documentation][6], but remember that
some functions are missing and some were added in libusby.
Comments will be slowly appearing in the `libusby.h` file, so take
a look in there too.

# What are the advantages of libusby over libusb-1.0?

Advantages:

 * Support for `libusb0.sys`.
 * Design that focuses more on the ability to cancel rather than
   on timeouts. This is why `libusb_handle_events()` was removed.
 * Higher performance. In my test setup, I've decreased the ping latency
   on a full-speed device from 300us to 140us.
 * Less overhead for synchronous transfers.
 * No spurious wake-ups for reaper threads.
 * Permissive license (yes, each and every line
   was written from scratch).

Disadvantages:

 * No support for OS X and OpenBSD (yet).
 * No support for isochronous transfers (yet).
 * No support for WinUSB (yet).
 * Less tested (currently).

# I don't understand how this works at the low level.

On Linux, USB devices are made available to the user via device files
by udev (e.g. on my system they're in `/dev/bus/usb`). Access to devices
is controled by file permissions. Reading the file will get you
the device and config descriptors of the corresponding
USB device. USB transfers are done via `ioctl` and require write
permissions.

On Windows, the situation is a bit more complicated. By default,
the Windows kernel will not expose the device to user-space at all.

When the device is plugged in, the USB enumerator will read out the
device descriptor and determine a set of hardware and compatible IDs.
For instance, my USB mouse has the following hardware IDs

    USB\VID_046D&PID_C051&REV_3000
    USB\VID_046D&PID_C051

and the following compatible IDs

    USB\Class_03&SubClass_01&Prot_02
    USB\Class_03&SubClass_01
    USB\Class_03

Based on these IDs, the kernel will search for a driver to associate
with the device. In the case of my mouse, there is an association made
by `input.inf` between `USB\Class_03&SubClass_01` and `hidusb.sys`.

If you want to control your custom USB device that is neither a HID
device nor a mass storage device, no hardware and compatible ID will
likely be matched and no driver for your device loaded. You will have
to provide a driver and an .inf file that associates your device's
hardware ID with your driver.

Instead of writing a USB kernel-mode driver yourself, you can use
a USB driver that exposes the device to user mode. There's a bunch
of commercial solutions out there, but if you don't want to pay, you
have basically three choices:

  * [libusb0.sys][2],
  * [libusbK.sys][3], and
  * [winusb.sys][4].

The latter is distributed with Windows Vista and later, but it is
slightly more difficult to get it installed via an .inf. For testing,
however, you can get either of these drivers installed for your device
using the [zadig utility][5].

Both libusb0 and libusbK expose the USB devices as files named
`\\.\libusb0-0001`, where the four-digit number is between 1 and 255.
All transfers are done via `DeviceIoControl` or through `libusb0.dll`.
Note that `libusbK` provides a strict superset of `libusb0`
functionality.

WinUSB also exposes the device as a file, but the name of the file
is not straightforward to deduce. The .inf file that is used to
install the device provides the driver with a GUID, the device
interface class. When loaded, the driver adds an interface with this
GUID to the device node and stores the name of the file as a property
of this interface. The user-mode program must then search
the device database for devices exposing this inteface and get
the name of the file from there.

The transfers are then performed via `DeviceIoControl`, or through
`WinUSB.dll`.

# How does libusb-1.0, libusbx and libusby fit into this?

These libraries provide a platform- and driver-independent API.
On Windows, libusb-1.0 only supports WinUSB. libusbx was forked
from libusb-1.0 when the author of `libusb0.sys` became agitated
by the speed with which the original project accepted patches and
made releases, and from what I gather also by the attitute of
libusb-1.0 maintainers.

libusbx promises to release often. They also promise to add
`libusb0.sys` supports within a few months.

libusby is not a fork, it is written from scratch and attempts
to fix the mistakes I believe the developers of libusb-1.0 made
in the library's design. While forking would probably be easier,
given the amount of changes I would be forced to make, it seems
to me that a complete rewrite will not be as hard
and, more importantly, will allow me to license the code under
a more permissive license.

Regretably, the library will lose support for OS X and OpenBSD
by the rewrite (I'm no expert in OpenBSD and I can't even run OS X),
at least until someone generously contributes the code.

# How did you come up with the name libusby?

I lack imagination, so I simply grabbed a name that follows libusbx.

  [1]: http://www.libusb.org/wiki/libusb-1.0
  [2]: http://sourceforge.net/apps/trac/libusb-win32/wiki
  [3]: http://code.google.com/p/usb-travis/
  [4]: http://msdn.microsoft.com/en-us/library/windows/hardware/ff540196%28v=vs.85%29.aspx
  [5]: http://sourceforge.net/apps/mediawiki/libwdi/index.php?title=Main_Page
  [6]: http://libusb.sourceforge.net/api-1.0/
