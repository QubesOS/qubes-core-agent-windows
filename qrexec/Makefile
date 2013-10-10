TARGET = qrexec-agent
SRCFILES = qrexec-agent.c txrx-vchan.c service.c pipe-server.c
RCFILES = errors.rc
# need to manually compile .mc file with mc from win sdk for now...
CFLAGS += -I../include
LDFLAGS += -lwindows-utils -lvchan-$(BACKEND_VMM) -lshlwapi -lwtsapi32 -luserenv

OBJFILES = $(SRCFILES:.c=.o)
RESFILES = $(RCFILES:.rc=.coff)

all: $(TARGET).exe

%.coff: %.rc
	$(RC) -i $< -o $@

$(TARGET).exe: $(OBJFILES) $(RESFILES)
	$(CC) -o $@ $(OBJFILES) $(RESFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe *.coff
