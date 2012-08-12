SOURCES += \
    $$PWD/src/libspy.c
HEADERS += \
    $$PWD/src/libspy.h \
    $$PWD/src/libspy.hpp

INCLUDEPATH += $$PWD/src

win32 {
    SOURCES += \
        $$PWD/src/os/spy_win32.c
}

unix {
    SOURCES += \
        $$PWD/src/os/spy_posix.c
}

linux-* {
    SOURCES += \
        $$PWD/src/os/spy_enum_linux.c
}
