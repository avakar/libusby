HEADERS += \
    $$PWD/src/libpolly.h

INCLUDEPATH += $$PWD/src

win32 {
    SOURCES += \
        $$PWD/src/os/polly_win32.c

    HEADERS += \
        $$PWD/src/os/libpolly_win32.h
}

unix {
    SOURCES += \
        $$PWD/src/os/polly_posix.c

    HEADERS += \
        $$PWD/src/os/libpolly_posix.h
}
