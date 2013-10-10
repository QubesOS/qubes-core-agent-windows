TARGET = clipboard-paste
SRCFILES = clipboard-paste.c

CFLAGS += -mwindows
LDFLAGS += -Wl,--subsystem,windows -L../libs -lshlwapi -lwindows-utils

OBJFILES = $(SRCFILES:.c=.o)

all: $(TARGET).exe

$(TARGET).exe: $(OBJFILES)
	$(CC) -o $@ $(OBJFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe *.coff
