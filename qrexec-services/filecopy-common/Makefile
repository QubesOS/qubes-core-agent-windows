TARGET = libfilecopy-common
SRCFILES = copy-file.c crc32.c gui-fatal.c ioall.c
OUT_DIR = ../libs

CFLAGS += -I.

all: $(OUT_DIR)/$(TARGET).a

$(OUT_DIR)/$(TARGET).a: copy-file.o crc32.o gui-fatal.o ioall.o
	$(AR) rcs $(OUT_DIR)/$(TARGET).a copy-file.o crc32.o gui-fatal.o ioall.o

clean:
	rm -f *.o *.exe
