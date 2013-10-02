TARGET = open_in_vm
SRCFILES = qopen-in-vm.c
RCFILES =
	
CFLAGS = -DUNICODE -D_UNICODE -Wall -mwindows -I../../include -I../filecopy_common -I$(QUBES_INCLUDES)
LDFLAGS = -Wl,--subsystem,windows -municode -L../../libs -L../libs -L$(QUBES_LIBS) -lshlwapi -lqubes_utils -lfilecopy_common

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
