TARGET = wait-for-logon
SRCFILES = wait-for-logon.c
	
CFLAGS += -mwindows -I../filecopy-common
LDFLAGS += -Wl,--subsystem,windows -L../libs
LDFLAGS += -lshlwapi -lwtsapi32 -lwindows-utils -lfilecopy-common

OBJFILES = $(SRCFILES:.c=.o)

all: $(TARGET).exe

$(TARGET).exe: $(OBJFILES)
	$(CC) -o $@ $(OBJFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe *.coff
