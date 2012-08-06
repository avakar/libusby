SOURCES += \
    $$PWD/src/libspy.c
HEADERS += \
    $$PWD/src/libspy.h \
    $$PWD/src/libspy.hpp \
    $$PWD/src/os/spy.h

INCLUDEPATH += $$PWD/src

win32 {
    SOURCES += \
        $$PWD/src/os/spy_win32.c
}
