TARGET = ask_vm_and_run
SRCFILES = ask_vm_and_run.c
RCFILES = inputbox.rc

CFLAGS = -DUNICODE -D_UNICODE -Wall -mwindows
LDFLAGS = -Wl,--subsystem,windows -municode -lshlwapi

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
