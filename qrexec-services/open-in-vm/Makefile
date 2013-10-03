TARGET = open-in-vm
SRCFILES = qopen-in-vm.c
RCFILES =
	
CFLAGS = -DUNICODE -D_UNICODE -Wall -mwindows -I../../include -I../filecopy-common -I$(QUBES_INCLUDES)
LDFLAGS = -Wl,--subsystem,windows -municode -L../../libs -L../libs -L$(QUBES_LIBS) -lshlwapi -lqubes-utils -lfilecopy-common

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
