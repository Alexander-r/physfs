TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt
CONFIG += c++11

DEFINES += PHYSFS_NO_CDROM_SUPPORT=1
DEFINES += PHYSFS_SUPPORTS_ZIP=1
DEFINES += PHYSFS_SUPPORTS_7Z=1
DEFINES += PHYSFS_SUPPORTS_GRP=1
DEFINES += PHYSFS_SUPPORTS_WAD=1
DEFINES += PHYSFS_SUPPORTS_HOG=1
DEFINES += PHYSFS_SUPPORTS_MVL=1
DEFINES += PHYSFS_SUPPORTS_QPAK=1
DEFINES += PHYSFS_SUPPORTS_SLB=1
DEFINES += PHYSFS_SUPPORTS_ISO9660=1

INCLUDEPATH += $$PWD/lzma938/C
INCLUDEPATH += $$PWD/src 
INCLUDEPATH += /usr/include

SOURCES += \
    test/test_physfs.c \
    lzma938/C/7zArcIn.c \
    lzma938/C/7zCrc.c \
    lzma938/C/7zCrcOpt.c \
    lzma938/C/7zBuf.c \
    lzma938/C/LzmaDec.c \
    lzma938/C/Lzma2Dec.c \
    lzma938/C/7zDec.c \
    lzma938/C/7zStream.c \
    lzma938/C/CpuArch.c \
    lzma938/C/Bra.c \
    lzma938/C/Bra86.c \
    lzma938/C/Bcj2.c \
    src/archiver_7z.c \
    src/platform_beos.cpp \
    src/platform_winrt.cpp \
    src/archiver_dir.c \
    src/archiver_grp.c \
    src/archiver_hog.c \
    src/archiver_iso9660.c \
    src/archiver_mvl.c \
    src/archiver_qpak.c \
    src/archiver_slb.c \
    src/archiver_unpacked.c \
    src/archiver_wad.c \
    src/archiver_zip.c \
    src/physfs.c \
    src/physfs_byteorder.c \
    src/physfs_unicode.c \
    src/platform_macosx.c \
    src/platform_posix.c \
    src/platform_unix.c \
    src/platform_windows.c

HEADERS += \
    src/physfs.h \
    src/physfs_casefolding.h \
    src/physfs_internal.h \
    src/physfs_miniz.h \
    src/physfs_platforms.h
