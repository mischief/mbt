// SPDX-License-Identifier: ISC
/*
 * mbt-dht-check: ask one DHT node about a list of infohashes, and count them.
 * The tool talks to that node alone. It joins no network.
 * Exit status: 0 if the node has peers for all, 1 if not, 2 for a usage error.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbt/be.h"
#include "mbt/dht.h"
#include "mbt/event.h"
#include "mbt/hex.h"
#include "tools/args.h"
#include "banned.h"

#define HASH_LEN 20
#define HEX_LEN 40
#define WINDOW_DEFAULT 16
#define NODE_INFO_V4 26
#define STEP_MS 10

enum state {
	ST_LOOKUP,               /* get_peers, for the count or for a token */
	ST_ANNOUNCE,             /* announce_peer */
	ST_VERIFY,               /* get_peers after the announce */
	ST_DONE,
};

struct item {
	uint8_t hash[HASH_LEN];
	char hex[HEX_LEN + 1];
	enum state state;
	int busy;                /* a query is in flight */
	int replied;
	int announced;
	int peers;
	int nodes;
	uint8_t token[64];
	size_t token_len;
};

static struct item *items;
static size_t count, cap;
static struct mbt_ev *ev;
static struct mbt_dht *dht;
static const char *host;
static uint16_t node_port;
static uint16_t announce_port;
static uint8_t my_id[HASH_LEN];
static int verbose;
static size_t finished;
static long long stored = -1;
static int stored_done;

static void
usage(void)
{
	fprintf(stderr, "usage: mbt-dht-check [-a port] [-f file] [-j window] [-v] addr:port "
	        "[infohash ...]\n"
	        "  -a port    announce this peer port, then check that it took\n"
	        "  -f file    read infohashes from file, one per line, - for stdin\n"
	        "  -j window  queries in flight (default %d)\n"
	        "  -v         print one line per infohash\n", WINDOW_DEFAULT);
	exit(2);
}

static void
add_hash(const char *s)
{
	if (count == cap) {
		cap = cap ? cap * 2 : 64;
		if ((items = reallocarray(items, cap, sizeof(*items))) == NULL)
			err(2, "reallocarray");
	}
	struct item *it = &items[count];

	memset(it, 0, sizeof(*it));
	if (strlen(s) != HEX_LEN || mbt_hex_decode(it->hash, HASH_LEN, s) < 0)
		errx(2, "bad infohash: %s", s);
	memcpy(it->hex, s, HEX_LEN);
	count++;
}

static void
read_list(const char *path)
{
	FILE *fp = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
	char *line = NULL;
	size_t len = 0;
	ssize_t n;

	if (!fp)
		err(2, "%s", path);
	while ((n = getline(&line, &len, fp)) >= 0) {
		while (n > 0 && strchr(" \t\r\n", line[n - 1]))
			line[--n] = '\0';
		if (n > 0 && line[0] != '#')
			add_hash(line);
	}
	free(line);
	if (fp != stdin)
		fclose(fp);
}

static struct mbt_be_value *
args_new(void)
{
	struct mbt_be_value *a;

	if (mbt_be_dict_new(&a) < 0 || mbt_be_dict_append_string(a, "id", my_id, HASH_LEN) < 0)
		errx(2, "out of memory");
	return a;
}

static void
item_finish(struct item *it)
{
	it->state = ST_DONE;
	finished++;
}

static void
lookup_reply(struct item *it, struct mbt_be_value *r)
{
	struct mbt_be_value *values = mbt_be_find(r, "values");
	struct mbt_be_value *nodes = mbt_be_find(r, "nodes");
	struct mbt_be_value *token = mbt_be_find(r, "token");
	struct mbt_be_value *v;

	it->replied = 1;
	it->peers = 0;
	if (values && values->type == BE_LIST)
		TAILQ_FOREACH(v, &values->listhead, next)
			it->peers++;
	it->nodes = nodes && nodes->type == BE_STRING ? (int)(nodes->string.len / NODE_INFO_V4) : 0;
	if (token && token->type == BE_STRING && token->string.len <= sizeof(it->token)) {
		memcpy(it->token, token->string.buf, token->string.len);
		it->token_len = token->string.len;
	}
}

static void
query_done(struct mbt_be_value *reply, int rv, void *arg)
{
	struct item *it = arg;
	struct mbt_be_value *r = rv == 0 ? mbt_be_find(reply, "r") : NULL;

	it->busy = 0;
	switch (it->state) {
	case ST_LOOKUP:
		if (r)
			lookup_reply(it, r);
		if (r && announce_port && it->token_len)
			it->state = ST_ANNOUNCE;
		else
			item_finish(it);
		break;
	case ST_ANNOUNCE:
		it->announced = r != NULL;
		if (r)
			it->state = ST_VERIFY;
		else
			item_finish(it);
		break;
	case ST_VERIFY:
		if (r)
			lookup_reply(it, r);
		item_finish(it);
		break;
	case ST_DONE:
		break;
	}
}

static int
item_send(struct item *it)
{
	struct mbt_be_value *a = args_new();
	const char *q = "get_peers";
	int rv;

	if (mbt_be_dict_append_string(a, "info_hash", it->hash, HASH_LEN) < 0)
		errx(2, "out of memory");
	if (it->state == ST_ANNOUNCE) {
		q = "announce_peer";
		if (mbt_be_dict_append_number(a, "port", announce_port) < 0 ||
		    mbt_be_dict_append_string(a, "token", it->token, it->token_len) < 0)
			errx(2, "out of memory");
	}
	it->busy = 1;
	if ((rv = mbt_dht_query(dht, host, node_port, q, a, query_done, it)) < 0)
		it->busy = 0;
	mbt_be_unref(a);
	return rv;
}

static void
stored_reply(struct mbt_be_value *reply, int rv, void *arg)
{
	struct mbt_be_value *r = rv == 0 ? mbt_be_find(reply, "r") : NULL;
	struct mbt_be_value *n = r ? mbt_be_find(r, "num") : NULL;

	(void)arg;
	if (n && n->type == BE_INTEGER)
		stored = n->integer;
	stored_done = 1;
}

static void
ask_stored(void)
{
	struct mbt_be_value *a = args_new();

	if (mbt_be_dict_append_string(a, "target", my_id, HASH_LEN) < 0)
		errx(2, "out of memory");
	if (mbt_dht_query(dht, host, node_port, "sample_infohashes", a, stored_reply, NULL) < 0)
		stored_done = 1;
	mbt_be_unref(a);
}

static void
run_queries(size_t window)
{
	size_t first = 0;

	while (finished < count) {
		size_t flying = 0;

		while (items[first].state == ST_DONE)
			first++;
		for (size_t i = first; i < count && flying < window; i++) {
			struct item *it = &items[i];

			if (it->state == ST_DONE)
				continue;
			if (!it->busy && item_send(it) < 0)
				continue;
			flying++;
		}
		mbt_ev_step(ev, STEP_MS);
	}
}

static void
parse_node(char *arg)
{
	char *colon = strrchr(arg, ':');

	if (!colon)
		usage();
	*colon = '\0';
	node_port = (uint16_t)mbt_arg_int(colon + 1, 1, UINT16_MAX);
	host = arg;
	if (host[0] == '[') {
		size_t n = strlen(host);

		if (n < 2 || host[n - 1] != ']')
			usage();
		arg[n - 1] = '\0';
		host++;
	}
}

int
main(int argc, char *argv[])
{
	size_t window = WINDOW_DEFAULT, with_peers = 0, nodes_only = 0, silent = 0, told = 0;
	const char *list = NULL;
	int ch, rv;

	while ((ch = getopt(argc, argv, "a:f:j:vh")) != -1) {
		switch (ch) {
		case 'a': announce_port = (uint16_t)mbt_arg_int(optarg, 1, UINT16_MAX); break;
		case 'f': list = optarg; break;
		case 'j': window = (size_t)mbt_arg_int(optarg, 1, 128); break;
		case 'v': verbose = 1; break;
		default: usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1)
		usage();
	parse_node(argv[0]);
	for (int i = 1; i < argc; i++)
		add_hash(argv[i]);
	if (list)
		read_list(list);
	if (count == 0)
		usage();

	signal(SIGPIPE, SIG_IGN);
	arc4random_buf(my_id, sizeof(my_id));
	if ((rv = mbt_ev_new_default(&ev)) < 0)
		errx(2, "event loop: %s", strerror(-rv));
	struct mbt_dht_config cfg = {.ev = ev, .node_id = my_id};

	if ((rv = mbt_dht_new(&dht, &cfg)) < 0)
		errx(2, "dht: %s", strerror(-rv));

	run_queries(window);
	ask_stored();
	while (!stored_done)
		mbt_ev_step(ev, STEP_MS);

	for (size_t i = 0; i < count; i++) {
		const struct item *it = &items[i];

		if (it->peers > 0)
			with_peers++;
		else if (it->replied)
			nodes_only++;
		else
			silent++;
		told += it->announced != 0;
		if (verbose) {
			if (it->replied)
				printf("%s peers %d nodes %d%s\n", it->hex, it->peers, it->nodes,
				       it->announced ? " announced" : "");
			else
				printf("%s no reply\n", it->hex);
		}
	}
	printf("checked %zu: peers %zu, no peers %zu, no reply %zu", count, with_peers,
	       nodes_only, silent);
	if (announce_port)
		printf(", announced %zu", told);
	if (stored >= 0)
		printf(", node stores %lld", stored);
	printf("\n");

	mbt_dht_free(dht);
	mbt_ev_destroy(ev);
	free(items);
	return with_peers == count ? 0 : 1;
}
