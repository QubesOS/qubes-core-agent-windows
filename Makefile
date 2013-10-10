VERSION := $(shell cat version)

# common settings for all projects
CFLAGS += -DUNICODE -D_UNICODE
CFLAGS += -DWINVER=0x0600 -D_WIN32_WINNT=0x0600
CFLAGS += -I$(QUBES_INCLUDES)
CFLAGS += -DBACKEND_VMM_$(BACKEND_VMM)
CFLAGS += -Wall

LDFLAGS += -municode -L$(QUBES_LIBS)

CC := gcc
RC := windres
AR := ar

export

help:
	@echo "make all      --- build all"
	@echo "make clean    --- clean all"

all:
	$(MAKE) all -C qrexec
	$(MAKE) all -C qrexec-client-vm
	$(MAKE) all -C ask-vm-and-run
	$(MAKE) all -C qrexec-services

clean:
	$(MAKE) clean -C qrexec
	$(MAKE) clean -C qrexec-client-vm
	$(MAKE) clean -C ask-vm-and-run
	$(MAKE) clean -C qrexec-services
	rm -f libs/*.a libs/*.o

msi:
	candle -arch x64 -dversion=$(VERSION) installer.wxs
	light -o core-agent-windows.msm installer.wixobj
