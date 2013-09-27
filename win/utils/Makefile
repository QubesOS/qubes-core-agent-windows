TARGET = libqubes_utils
SRCFILES = utf8_conv.c
OUT_DIR = ../libs

CFLAGS = -DUNICODE -D_UNICODE -DWINVER=0x0600 -D_WIN32_WINNT=0x600 -Wall -I../include

CC = gcc
AR = ar

all: $(OUT_DIR)/$(TARGET).a

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(OUT_DIR)/$(TARGET).a: utf8_conv.o
	$(AR) rcs $(OUT_DIR)/$(TARGET).a utf8_conv.o

clean:
	rm -f *.o *.exe
