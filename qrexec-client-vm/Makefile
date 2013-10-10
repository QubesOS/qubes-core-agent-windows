TARGET = qrexec-client-vm
SRCFILES = qrexec-client-vm.c

CFLAGS = -DUNICODE -D_UNICODE -Wall -I../include -I$(QUBES_INCLUDES) 
LDFLAGS = -municode -L$(QUBES_LIBS) -lwindows-utils

CC = gcc

OBJFILES = $(SRCFILES:.c=.o)

all: $(TARGET).exe

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(TARGET).exe: $(OBJFILES)
	$(CC) -o $@ $(OBJFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe
