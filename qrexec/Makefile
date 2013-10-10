TARGET = qrexec-agent
SRCFILES = qrexec-agent.c txrx-vchan.c service.c pipe-server.c
RCFILES = errors.rc
# need to manually compile .mc file with mc from win sdk for now...
CFLAGS = -Wall -DUNICODE -D_UNICODE -I../include -I$(QUBES_INCLUDES) -DWINVER=0x0600 -D_WIN32_WINNT=0x0600
CFLAGS += -DBACKEND_VMM_$(BACKEND_VMM)
LDFLAGS = -municode -L$(QUBES_LIBS) -lwindows-utils -lvchan-$(BACKEND_VMM) -lshlwapi -lwtsapi32 -luserenv

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
