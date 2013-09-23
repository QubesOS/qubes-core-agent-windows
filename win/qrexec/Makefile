TARGET = qrexec_agent
SRCFILES = qrexec_agent.c txrx-vchan.c service.c getopt.c log.c exec.c pipe_server.c
RCFILES = errors.rc
# need to manually compile .mc file with mc from win sdk for now...
CFLAGS = -Wall -DUNICODE -D_UNICODE -I../include -DWINVER=0x0600 -DDBG
LDFLAGS = -municode -L../libs -lshlwapi -lwtsapi32 -luserenv -lqubes_utils -lvchan-wni

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
