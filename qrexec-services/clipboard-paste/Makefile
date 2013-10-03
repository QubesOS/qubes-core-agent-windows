TARGET = clipboard-paste
SRCFILES = clipboard-paste.c

CFLAGS = -DUNICODE -D_UNICODE -Wall -mwindows -I../../include -I$(QUBES_INCLUDES)
LDFLAGS = -Wl,--subsystem,windows -municode -L../../libs -L../libs -L$(QUBES_LIBS) -lshlwapi -lqubes-utils

CC = gcc

OBJFILES = $(SRCFILES:.c=.o)

all: $(TARGET).exe

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(TARGET).exe: $(OBJFILES)
	$(CC) -o $@ $(OBJFILES) $(LDFLAGS)

clean:
	rm -f *.o *.exe *.coff
