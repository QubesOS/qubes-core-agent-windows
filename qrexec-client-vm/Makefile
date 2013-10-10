TARGET = qrexec-client-vm
SRCFILES = qrexec-client-vm.c

CFLAGS += -I../include
LDFLAGS += -lwindows-utils

OBJFILES = $(SRCFILES:.c=.o)

all: $(TARGET).exe

$(TARGET).exe: $(OBJFILES)
	$(CC) -o $@ $(OBJFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe
