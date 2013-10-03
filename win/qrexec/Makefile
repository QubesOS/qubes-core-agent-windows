TARGET = qrexec-agent
SRCFILES = qrexec-agent.c txrx-vchan.c service.c getopt.c log.c exec.c pipe-server.c
RCFILES = errors.rc
# need to manually compile .mc file with mc from win sdk for now...
CFLAGS = -Wall -DUNICODE -D_UNICODE -I$(QUBES_INCLUDES) -I../include -DWINVER=0x0600 -DDBG
CFLAGS += -DBACKEND_VMM_$(BACKEND_VMM)
LDFLAGS = -municode -L../libs -L$(QUBES_LIBS) -lshlwapi -lwtsapi32 -luserenv -lqubes-utils -lvchan-$(BACKEND_VMM)

CC = gcc
RC = windres

OBJFILES = $(SRCFILES:.c=.o)
RESFILES = $(RCFILES:.rc=.coff)

all: $(TARGET).exe

%.o: %.c
	$(CC) $(CFLAGS) -c $<

%.coff: %.rc
	$(RC) -i $< -o $@

$(TARGET).exe: $(OBJFILES) $(RESFILES)
	$(CC) -o $@ $(OBJFILES) $(RESFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe *.coff
