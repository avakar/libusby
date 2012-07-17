SOURCES += $$PWD/src/libusby.c
HEADERS += $$PWD/src/libusb.h \
    $$PWD/src/libusby.h \
    $$PWD/src/libusby.hpp \
    $$PWD/src/libusbyi.h \
    $$PWD/src/libusbyi_fwd.h \
    $$PWD/src/os/os.h
INCLUDEPATH += $$PWD/src

win32 {
    SOURCES += $$PWD/src/os/libusb0_win32.c
    HEADERS += $$PWD/src/os/libusb0_win32_intf.h
}

unix:!macx:!symbian {
    SOURCES += $$PWD/src/os/linux_usbfs.c
}
