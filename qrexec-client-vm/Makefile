TARGET = qrexec-client-vm
SRCFILES = qrexec-client-vm.c log.c

CFLAGS = -DUNICODE -D_UNICODE -Wall -I../include
LDFLAGS = -municode -L../libs -lqubes-utils

CC = gcc

OBJFILES = $(SRCFILES:.c=.o)

all: $(TARGET).exe

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(TARGET).exe: $(OBJFILES)
	$(CC) -o $@ $(OBJFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe
