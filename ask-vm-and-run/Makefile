TARGET = ask-vm-and-run
SRCFILES = ask-vm-and-run.c
RCFILES = inputbox.rc

CFLAGS += -mwindows
LDFLAGS += -Wl,--subsystem,windows -lshlwapi

OBJFILES = $(SRCFILES:.c=.o)
RESFILES = $(RCFILES:.rc=.coff)

all: $(TARGET).exe

%.coff: %.rc
	$(RC) -i $< -o $@

$(TARGET).exe: $(OBJFILES) $(RESFILES)
	$(CC) -o $@ $(OBJFILES) $(RESFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe *.coff
