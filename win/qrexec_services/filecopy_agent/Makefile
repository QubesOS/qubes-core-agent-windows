TARGET = qfile_agent
SRCFILES = qfile-agent.c gui-progress.c
RCFILES = qfile-agent.rc

CFLAGS = -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 -DUNICODE -D_UNICODE -Wall -mwindows
CFLAGS += -I../../include -I../filecopy_common -I$(QUBES_INCLUDES)
LDFLAGS = -Wl,--subsystem,windows -municode -L../../libs -L../libs -L$(QUBES_LIBS) -lshlwapi -lcomctl32 -lqubes_utils -lfilecopy_common

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
