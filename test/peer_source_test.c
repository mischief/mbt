// SPDX-License-Identifier: ISC
#include <assert.h>
#include <string.h>

#include "mbt/peer.h"
#include "mbt/peer_source.h"

static void
test_source_string(void)
{
	assert(strcmp(mbt_peer_source_str(MBT_SOURCE_TRACKER), "tracker") == 0);
	assert(strcmp(mbt_peer_source_str(MBT_SOURCE_DHT), "dht") == 0);
	assert(strcmp(mbt_peer_source_str(MBT_SOURCE_PEX), "pex") == 0);
	assert(strcmp(mbt_peer_source_str(MBT_SOURCE_LSD), "lsd") == 0);
	assert(strcmp(mbt_peer_source_str(MBT_SOURCE_INCOMING), "incoming") == 0);
	assert(strcmp(mbt_peer_source_str(MBT_SOURCE_MANUAL), "manual") == 0);
	assert(strcmp(mbt_peer_source_str(MBT_SOURCE_MAGNET), "magnet") == 0);
	assert(strcmp(mbt_peer_source_str(0xff), "unknown") == 0);
}

int
main(void)
{
	test_source_string();
	return 0;
}
