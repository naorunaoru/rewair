CC     ?= cc
SRCDIR := ../../wiced/apps/rewair/local_bridge
CFLAGS := -std=c99 -Wall -Wextra -Werror -I$(SRCDIR) -DJSMN_PARENT_LINKS

TESTS := test_drops test_tz test_req test_status

all: $(TESTS)

test_drops: test_drops.c $(SRCDIR)/rewair_drops.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

test_tz: test_tz.c $(SRCDIR)/rewair_tz.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

test_req: test_req.c $(SRCDIR)/rewair_json.c $(SRCDIR)/jsmn.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

test_status: test_status.c $(SRCDIR)/rewair_json.c $(SRCDIR)/jsmn.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

clean:
	rm -f $(TESTS)

.PHONY: all clean
