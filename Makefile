TARGET  = ppmd
SRC     = ppmd.c $(wildcard lib/Ppmd7*.c lib/Ppmd8*.c)
OBJ     = $(SRC:.c=.o)
# ppmd picks decompress / cat mode from its own name
LINKS   = unppmd ppmdcat

CFLAGS  ?= -O2 -flto
CFLAGS  += -Wall -Wextra -DNDEBUG
CPPFLAGS += -Ilib -MMD -MP

# Opt-in: faster, but the binary only runs on CPUs like this one (`make NATIVE=1`).
ifdef NATIVE
CFLAGS  += -march=native
endif

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	for l in $(LINKS); do ln -sf $(TARGET) $(DESTDIR)$(BINDIR)/$$l; done

test: $(TARGET)
	@[ "$$(echo test | ./$(TARGET) -c | ./$(TARGET) -d)" = test ] && echo OK || { echo FAILED; exit 1; }

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) $(addprefix $(DESTDIR)$(BINDIR)/,$(LINKS))

clean:
	rm -f $(TARGET) $(OBJ) $(OBJ:.o=.d)

-include $(OBJ:.o=.d)
