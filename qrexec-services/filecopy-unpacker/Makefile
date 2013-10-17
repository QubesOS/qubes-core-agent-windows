TARGET = qfile-unpacker
SRCFILES = qfile-unpacker.c gui-fatal.c unpack.c

CFLAGS += -mwindows -I../filecopy-common
LDFLAGS += -Wl,--subsystem,windows -L../libs -lntdll -lole32 -lshfolder -lwindows-utils -lfilecopy-common -lshlwapi

OBJFILES = $(SRCFILES:.c=.o)

all: $(TARGET).exe

$(TARGET).exe: $(OBJFILES)
	$(CC) -o $@ $(OBJFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe *.coff
