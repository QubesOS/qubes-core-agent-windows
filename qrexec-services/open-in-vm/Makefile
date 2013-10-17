TARGET = open-in-vm
SRCFILES = qopen-in-vm.c
	
CFLAGS += -mwindows -I../filecopy-common
LDFLAGS += -Wl,--subsystem,windows -L../libs -lwindows-utils -lshlwapi -lfilecopy-common

OBJFILES = $(SRCFILES:.c=.o)

all: $(TARGET).exe

$(TARGET).exe: $(OBJFILES)
	$(CC) -o $@ $(OBJFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe *.coff
