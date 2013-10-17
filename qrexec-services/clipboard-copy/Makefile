TARGET = clipboard-copy
SRCFILES = clipboard-copy.c

CFLAGS += -mwindows
LDFLAGS += -Wl,--subsystem,windows -L../libs -lwindows-utils -lshlwapi

OBJFILES = $(SRCFILES:.c=.o)

all: $(TARGET).exe

$(TARGET).exe: $(OBJFILES)
	$(CC) -o $@ $(OBJFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe *.coff
