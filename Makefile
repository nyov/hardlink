
all:

install:
	install -D -o root -g root -m 755 hardlink.py $(DESTDIR)/usr/bin/hardlink

clean:
	rm -f *.pyc *.pyo *~
