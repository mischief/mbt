// SPDX-License-Identifier: ISC
/*
 * test/peerdb_test.c — exercise mbt_peerdb
 */
#include <assert.h>
#include <stdio.h>

#include "mbt/peerdb.h"
#include "mbt/peer_source.h"

int main(void)
{
	struct mbt_peerdb db;
	mbt_peerdb_init(&db);

	/* Insert peers */
	mbt_peerdb_insert(&db, "192.168.1.1", 6881, MBT_SOURCE_TRACKER);
	mbt_peerdb_insert(&db, "192.168.1.2", 6882, MBT_SOURCE_DHT);
	mbt_peerdb_insert(&db, "192.168.1.3", 6883, MBT_SOURCE_PEX);
	mbt_peerdb_insert(&db, "10.0.0.1", 51413, MBT_SOURCE_LSD);

	/* Count */
	assert(mbt_peerdb_count(&db, MBT_PEERDB_KNOWN) == 4);
	assert(mbt_peerdb_count(&db, MBT_PEERDB_CONNECTING) == 0);
	assert(mbt_peerdb_count(&db, MBT_PEERDB_ACTIVE) == 0);

	/* Dedup — inserting same addr:port again shouldn't increase count */
	mbt_peerdb_insert(&db, "192.168.1.1", 6881, MBT_SOURCE_PEX);
	assert(mbt_peerdb_count(&db, MBT_PEERDB_KNOWN) == 4);

	/* Select returns a peer */
	struct mbt_peer_entry *pe = mbt_peerdb_select(&db);
	assert(pe != NULL);
	assert(pe->port > 0);

	/* Mark it connecting */
	pe->state = MBT_PEERDB_CONNECTING;
	assert(mbt_peerdb_count(&db, MBT_PEERDB_KNOWN) == 3);
	assert(mbt_peerdb_count(&db, MBT_PEERDB_CONNECTING) == 1);

	/* Select again — shouldn't return the connecting one */
	struct mbt_peer_entry *pe2 = mbt_peerdb_select(&db);
	assert(pe2 != NULL);
	assert(pe2 != pe);

	/* Mark failed with backoff */
	pe->state = MBT_PEERDB_FAILED;
	pe->failures++;
	assert(mbt_peerdb_count(&db, MBT_PEERDB_FAILED) == 1);

	fprintf(stderr, "peerdb_test: PASS\n");

	mbt_peerdb_free(&db);
	return 0;
}
