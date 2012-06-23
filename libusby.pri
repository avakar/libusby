SOURCES += $$PWD/src/libusby.c
HEADERS += $$PWD/src/libusb.h \
    $$PWD/src/libusby.h \
    $$PWD/src/libusbyi.h \
    $$PWD/src/libusbyi_fwd.h
INCLUDEPATH += $$PWD/src

win32 {
    SOURCES += $$PWD/src/os/libusb0_win32.c
    HEADERS += $$PWD/src/os/libusb0_win32.h \
        $$PWD/src/os/libusb0_win32_intf.h \
        $$PWD/src/os/win32.h
}

linux-g++ {
    SOURCES += $$PWD/src/os/linux_usbfs.c
    HEADERS += $$PWD/src/os/linux_usbfs.h
}
