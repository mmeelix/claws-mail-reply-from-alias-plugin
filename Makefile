# Build against the Claws Mail tree you configured, not a random checkout.
#   make CLAWS_SRC=/path/to/claws-mail-4.3.1
#
# Quit Claws before install. A running process keeps the old .so mapped.
#   make install CLAWS_SRC=/path/to/claws-mail-4.3.1
CLAWS_SRC ?= ../claws-mail-4.3.1
PLUGINDIR ?= /usr/local/lib/claws-mail/plugins
TARGET    := claws-replyfromalias.so

CFLAGS := -shared -fPIC -DHAVE_CONFIG_H \
	-I$(CLAWS_SRC) -I$(CLAWS_SRC)/src \
	-I$(CLAWS_SRC)/src/common -I$(CLAWS_SRC)/src/gtk \
	$(shell pkg-config --cflags gtk+-3.0 glib-2.0)

.PHONY: all install clean
all: $(TARGET)

$(TARGET): claws-replyfromalias.c
	$(CC) $(CFLAGS) -o $@ $<

install: $(TARGET)
	cp $(TARGET) $(PLUGINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
