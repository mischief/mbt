#include <assert.h>
#include <string.h>

#include "magnet.h"

static void
test_basic(void)
{
	struct mbt_magnet m;
	int rv;

	rv = mbt_magnet_parse("magnet:?xt=urn:btih:6f4370df4304609a8793ce2b59178dcc8febf5e2&dn=debian-12.11.0-amd64-netinst.iso", &m);
	assert(rv == 0);
	assert(strcmp(m.infohash_hex, "6F4370DF4304609A8793CE2B59178DCC8FEBF5E2") == 0);
	assert(m.infohash_type == INFOHASH_SHA1);
	assert(strcmp(m.display_name, "debian-12.11.0-amd64-netinst.iso") == 0);
	assert(m.tracker_count == 0);
	assert(m.peer_count == 0);

	mbt_magnet_free(&m);
}

static void
test_trackers(void)
{
	struct mbt_magnet m;
	int rv;

	rv = mbt_magnet_parse(
		"magnet:?xt=urn:btih:C8295CE630F2064F08440DB1534E4992CFE4862A"
		"&tr=udp%3A%2F%2Ftracker.opentrackr.org%3A1337%2Fannounce"
		"&tr=http%3A%2F%2Ftracker.example.com%2Fannounce", &m);
	assert(rv == 0);
	assert(strcmp(m.infohash_hex, "C8295CE630F2064F08440DB1534E4992CFE4862A") == 0);
	assert(m.tracker_count == 2);
	assert(strcmp(m.trackers[0], "udp://tracker.opentrackr.org:1337/announce") == 0);
	assert(strcmp(m.trackers[1], "http://tracker.example.com/announce") == 0);

	mbt_magnet_free(&m);
}

static void
test_peers(void)
{
	struct mbt_magnet m;
	int rv;

	rv = mbt_magnet_parse(
		"magnet:?xt=urn:btih:6F4370DF4304609A8793CE2B59178DCC8FEBF5E2"
		"&x.pe=192.168.0.10:28888"
		"&x.pe=10.0.0.1:6881", &m);
	assert(rv == 0);
	assert(m.peer_count == 2);
	assert(strcmp(m.peers[0].addr, "192.168.0.10") == 0);
	assert(m.peers[0].port == 28888);
	assert(strcmp(m.peers[1].addr, "10.0.0.1") == 0);
	assert(m.peers[1].port == 6881);

	mbt_magnet_free(&m);
}

static void
test_ipv6_peer(void)
{
	struct mbt_magnet m;
	int rv;

	rv = mbt_magnet_parse(
		"magnet:?xt=urn:btih:6F4370DF4304609A8793CE2B59178DCC8FEBF5E2"
		"&x.pe=[::1]:6881", &m);
	assert(rv == 0);
	assert(m.peer_count == 1);
	assert(strcmp(m.peers[0].addr, "::1") == 0);
	assert(m.peers[0].port == 6881);

	mbt_magnet_free(&m);
}

static void
test_errors(void)
{
	struct mbt_magnet m;

	/* Not a magnet */
	assert(mbt_magnet_parse("http://example.com", &m) < 0);

	/* No infohash */
	assert(mbt_magnet_parse("magnet:?dn=test", &m) < 0);

	/* Bad hash length */
	assert(mbt_magnet_parse("magnet:?xt=urn:btih:tooshort", &m) < 0);
}

static void
test_full(void)
{
	struct mbt_magnet m;
	int rv;

	rv = mbt_magnet_parse(
		"magnet:?xt=urn:btih:C8295CE630F2064F08440DB1534E4992CFE4862A"
		"&dn=ubuntu-25.10-desktop-amd64.iso"
		"&tr=udp%3A%2F%2Ftracker.opentrackr.org%3A1337%2Fannounce"
		"&x.pe=192.168.0.10:28888"
		"&x.pe=192.168.0.189:51413", &m);
	assert(rv == 0);
	assert(m.infohash_type == INFOHASH_SHA1);
	assert(strcmp(m.display_name, "ubuntu-25.10-desktop-amd64.iso") == 0);
	assert(m.tracker_count == 1);
	assert(m.peer_count == 2);

	mbt_magnet_free(&m);
}

int
main(void)
{
	test_basic();
	test_trackers();
	test_peers();
	test_ipv6_peer();
	test_errors();
	test_full();
	return 0;
}
