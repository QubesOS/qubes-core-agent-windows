TARGET = libfilecopy_common
SRCFILES = copy_file.c crc32.c gui-fatal.c ioall.c
OUT_DIR = ../libs

CFLAGS = -DUNICODE -D_UNICODE -DWINVER=0x0600 -D_WIN32_WINNT=0x600 -Wall -I$(QUBES_INCLUDES) -I../../include -I.

CC = gcc
AR = ar

all: $(OUT_DIR)/$(TARGET).a

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(OUT_DIR)/$(TARGET).a: copy_file.o crc32.o gui-fatal.o ioall.o
	$(AR) rcs $(OUT_DIR)/$(TARGET).a copy_file.o crc32.o gui-fatal.o ioall.o

clean:
	rm -f *.o *.exe
