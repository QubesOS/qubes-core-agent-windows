TARGET = qfile-agent
SRCFILES = qfile-agent.c gui-progress.c
RCFILES = qfile-agent.rc

CFLAGS += -mwindows
CFLAGS += -I../filecopy-common
LDFLAGS += -Wl,--subsystem,windows -municode -L../libs -lcomctl32 -lwindows-utils -lshlwapi -lfilecopy-common

OBJFILES = $(SRCFILES:.c=.o)
RESFILES = $(RCFILES:.rc=.coff)

all: $(TARGET).exe

%.coff: %.rc
	$(RC) -i $< -o $@

$(TARGET).exe: $(OBJFILES) $(RESFILES)
	$(CC) -o $@ $(OBJFILES) $(RESFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe *.coff
