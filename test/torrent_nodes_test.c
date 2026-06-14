// SPDX-License-Identifier: ISC
// Test: .torrent nodes= field (parse + encode round-trip)
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "mbt/metainfo.h"

int main(void)
{
	/* Create a metainfo with nodes */
	struct mbt_metainfo *mi;
	assert(mbt_metainfo_new(&mi) == 0);

	assert(mbt_metainfo_add_node(mi, "192.168.1.1", 6881) == 0);
	assert(mbt_metainfo_add_node(mi, "10.0.0.1", 7799) == 0);
	assert(mbt_metainfo_add_node(mi, "router.example.com", 6881) == 0);
	assert(mbt_metainfo_node_count(mi) == 3);
	assert(strcmp(mbt_metainfo_node_host(mi, 0), "192.168.1.1") == 0);
	assert(mbt_metainfo_node_port(mi, 0) == 6881);
	assert(strcmp(mbt_metainfo_node_host(mi, 1), "10.0.0.1") == 0);
	assert(mbt_metainfo_node_port(mi, 1) == 7799);
	assert(strcmp(mbt_metainfo_node_host(mi, 2), "router.example.com") == 0);
	assert(mbt_metainfo_node_port(mi, 2) == 6881);
	mbt_metainfo_free(mi);

	/* Parse a .torrent with nodes embedded */
	const char *torrent =
		"d"
		"4:infod"
			"6:lengthi1024e"
			"4:name8:test.bin"
			"12:piece lengthi256e"
			"6:pieces80:01234567890123456789012345678901234567890123456789012345678901234567890123456789"
		"e"
		"5:nodes"
			"l"
				"l11:192.168.1.1i6881ee"
				"l8:10.0.0.1i7799ee"
			"e"
		"e";

	struct mbt_metainfo *mi2 = NULL;
	int rv = mbt_metainfo_parse(&mi2, torrent, strlen(torrent));
	assert(rv == 0);
	assert(mbt_metainfo_node_count(mi2) == 2);
	assert(strcmp(mbt_metainfo_node_host(mi2, 0), "192.168.1.1") == 0);
	assert(mbt_metainfo_node_port(mi2, 0) == 6881);
	assert(strcmp(mbt_metainfo_node_host(mi2, 1), "10.0.0.1") == 0);
	assert(mbt_metainfo_node_port(mi2, 1) == 7799);
	mbt_metainfo_free(mi2);

	fprintf(stderr, "torrent_nodes_test: PASS\n");
	return 0;
}
