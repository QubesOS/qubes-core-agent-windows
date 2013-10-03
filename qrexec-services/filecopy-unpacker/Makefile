TARGET = qfile-unpacker
SRCFILES = qfile-unpacker.c gui-fatal.c unpack.c
RCFILES =

CFLAGS = -DUNICODE -D_UNICODE -DWINVER=0x0600 -D_WIN32_WINNT=0x0600
CFLAGS += -Wall -mwindows -I../../include -I../filecopy-common -I$(QUBES_INCLUDES)
LDFLAGS = -Wl,--subsystem,windows -municode 
LDFLAGS += -L../../libs -L../libs -L$(QUBES_LIBS) -lshlwapi -lntdll -lole32 -lshfolder -lqubes-utils -lfilecopy-common

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
