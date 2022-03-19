QT -= core gui

CONFIG += c++11 console static
CONFIG -= app_bundle

include(D:/lib/ffmpeg-4.3/ffmpeg.pri)
include(D:/lib/fmt-7.1.3/fmt.pri)

QMAKE_CXXFLAGS += /Ox /Qpar
QMAKE_CFLAGS += /Ox/Qpar


# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
        main.cpp \
        screenrecordimpl.cpp

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

HEADERS += \
    screenrecordimpl.h
