# Basic pathes
PREFIX  ?= usr
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man

# Complete the path names
BINDIR  := $(DESTDIR)/$(BINDIR)
MANDIR  := $(DESTDIR)/$(MANDIR)

all:

install:
	install -D -o root -g root -m 755 hardlink.py $(BINDIR)/hardlink
	install -D -o root -g root -m 644 hardlink.1  $(MANDIR)/man1/hardlink.1

clean:
	rm -f *.pyc *.pyo *~
