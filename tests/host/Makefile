CC     ?= cc
SRCDIR := ../../wiced/apps/rewair/local_bridge
PLATFORMDIR := ../../wiced/platforms/AWAIR
CFLAGS := -std=c99 -Wall -Wextra -Werror -I$(SRCDIR) -I$(PLATFORMDIR) -DJSMN_PARENT_LINKS

TESTS := test_drops test_tz test_req test_status test_uifs test_walltime test_score test_ota test_mqtt_packet

RWFS_FIXTURE := /tmp/test_uifs_fixture.rwfs

all: $(TESTS)

test_drops: test_drops.c $(SRCDIR)/rewair_drops.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

test_tz: test_tz.c $(SRCDIR)/rewair_tz.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

test_req: test_req.c $(SRCDIR)/rewair_json.c $(SRCDIR)/jsmn.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

test_status: test_status.c $(SRCDIR)/rewair_json.c $(SRCDIR)/jsmn.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

test_uifs: test_uifs.c $(SRCDIR)/rewair_uifs.c
	node ../../webui/scripts/pack-rwfs.mjs $(RWFS_FIXTURE) \
		fixtures/index.html:/ fixtures/app.js:/app.js fixtures/rewair.css:/rewair.css
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

test_walltime: test_walltime.c $(SRCDIR)/rewair_walltime.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

test_score: test_score.c $(SRCDIR)/rewair_score.c $(SRCDIR)/rewair_fmt.c
	$(CC) $(CFLAGS) -DREWAIR_HOST_BUILD -o $@ $^ && ./$@

test_ota: test_ota.c $(PLATFORMDIR)/rewair_ota_common.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

test_mqtt_packet: test_mqtt_packet.c $(SRCDIR)/rewair_mqtt_packet.c
	$(CC) $(CFLAGS) -o $@ $^ && ./$@

clean:
	rm -f $(TESTS) $(RWFS_FIXTURE)

.PHONY: all clean
