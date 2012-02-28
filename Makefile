# Basic DIRes
DESTDIR ?=
PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man

# Build with pcre by default
ENABLE  ?= libpcreposix

# GNU make executes the commands here and sets the variables to the outputs
EXTRA_LIBS  = $(shell $(EXTRA_LIBS!))
EXTRA_FLAGS = $(shell $(EXTRA_FLAGS!))

# pmake executes the commands, GNU make creates variables with a ! suffix
EXTRA_LIBS!= pkg-config --libs $(ENABLE) 2>/dev/null || echo
EXTRA_FLAGS!= pkg-config --cflags $(ENABLE) 2>/dev/null || echo

#Default flags
CFLAGS ?= -Wall -O2 -g

# Overwrites for the linker
MYLDLIBS = $(EXTRA_LIBS)
MYCFLAGS = -DHAVE_CONFIG_H $(EXTRA_FLAGS)

# Linker and compiler commands
MYLD = $(CC) $(LDFLAGS) $(TARGET_ARCH)
MYCC = $(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH)

# Features to test for when creating configure.h
FEATURES := GETOPT_LONG POSIX_FADVISE $(ENABLE)

all: hardlink

config.log config.h:
	@for feat in $(FEATURES); do \
		printf "Checking for %s..." "$$feat" | tee -a config.log; \
		if $(MYCC) $(LDFLAGS) $(LDLIBS) $(MYLDLIBS) $(MYCFLAGS) -o /dev/null \
			-DTEST_$$feat configure.c 2>>config.log; then \
			printf "\tOK\n" | tee -a config.log; \
			echo "#define HAVE_$$feat 1" >&3; \
		else \
			printf "\tFAIL\n" | tee -a config.log ; \
			echo "#undef HAVE_$$feat" >&3; \
		fi; \
	done 3> config.h

hardlink.o: hardlink.c config.h
	$(MYCC) $(MYCFLAGS) -o $@ -c hardlink.c

hardlink: hardlink.o
	$(MYLD) $(LDLIBS) $(MYLDLIBS) -o $@ hardlink.o

install: hardlink
	install -d  $(DESTDIR)$(BINDIR)
	install -d  $(DESTDIR)$(MANDIR)/man1
	install -m 755 hardlink $(DESTDIR)$(BINDIR)/hardlink
	install -m 644 hardlink.1  $(DESTDIR)$(MANDIR)/man1/hardlink.1

clean:
	rm -f hardlink hardlink.o config.h config.log
