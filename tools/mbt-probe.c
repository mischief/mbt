/*
 * tools/mbt-probe.c — connect to a peer and dump their bitfield
 * Usage: mbt-probe <infohash_hex> <ip:port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mbt/infohash.h"
#include "mbt/hex.h"

#define HANDSHAKE_SIZE 68

int main(int argc, char *argv[])
{
	if (argc < 3) {
		fprintf(stderr, "usage: mbt-probe <infohash_hex> <ip:port>\n");
		return 1;
	}

	/* Parse infohash */
	const char *hash_hex = argv[1];
	uint8_t hash[20];
	if (strlen(hash_hex) != 40) errx(1, "infohash must be 40 hex chars");
	mbt_hex_decode(hash, 20, hash_hex);

	/* Parse ip:port */
	char ip[64];
	int port;
	if (sscanf(argv[2], "%63[^:]:%d", ip, &port) != 2)
		errx(1, "bad address format (expected ip:port)");

	/* Connect */
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) err(1, "socket");

	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	inet_pton(AF_INET, ip, &sa.sin_addr);

	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		err(1, "connect to %s:%d", ip, port);

	printf("connected to %s:%d\n", ip, port);

	/* Send handshake */
	uint8_t hs[HANDSHAKE_SIZE] = {0};
	hs[0] = 19;
	memcpy(hs + 1, "BitTorrent protocol", 19);
	hs[25] = 0x10; /* extension */
	hs[27] = 0x04; /* fast */
	memcpy(hs + 28, hash, 20);
	/* peer_id: random */
	for (int i = 48; i < 68; i++) hs[i] = rand() & 0xff;

	if (write(fd, hs, HANDSHAKE_SIZE) < 0) err(1, "write");

	/* Read their handshake */
	uint8_t rhs[HANDSHAKE_SIZE];
	int n = 0;
	while (n < HANDSHAKE_SIZE) {
		int r = read(fd, rhs + n, HANDSHAKE_SIZE - n);
		if (r <= 0) errx(1, "handshake read failed at byte %d", n);
		n += r;
	}

	if (rhs[0] != 19 || memcmp(rhs + 1, "BitTorrent protocol", 19) != 0)
		errx(1, "not a BT peer");

	printf("handshake OK, flags: DHT=%d FAST=%d EXT=%d\n",
	       !!(rhs[27] & 0x01), !!(rhs[27] & 0x04), !!(rhs[25] & 0x10));

	/* Read messages until we get bitfield or have_all */
	while (1) {
		uint8_t lenbuf[4];
		n = 0;
		while (n < 4) {
			int r = read(fd, lenbuf + n, 4 - n);
			if (r <= 0) { printf("connection closed\n"); goto done; }
			n += r;
		}
		uint32_t len = (lenbuf[0]<<24)|(lenbuf[1]<<16)|(lenbuf[2]<<8)|lenbuf[3];
		if (len == 0) { printf("keepalive\n"); continue; }

		uint8_t *msg = malloc(len);
		n = 0;
		while (n < (int)len) {
			int r = read(fd, msg + n, len - n);
			if (r <= 0) { printf("read error\n"); free(msg); goto done; }
			n += r;
		}

		uint8_t id = msg[0];
		if (id == 5) {
			/* Bitfield */
			int bits = (len - 1) * 8;
			int set = 0;
			for (uint32_t i = 1; i < len; i++)
				for (int b = 7; b >= 0; b--)
					if (msg[i] & (1 << b)) set++;
			printf("BITFIELD: %d/%d bits set (%.1f%%)\n", set, bits, 100.0*set/bits);
			free(msg);
			goto done;
		} else if (id == 14) {
			printf("HAVE_ALL\n");
			free(msg);
			goto done;
		} else if (id == 15) {
			printf("HAVE_NONE\n");
			free(msg);
			goto done;
		} else if (id == 20) {
			/* Extension — skip */
			printf("extension msg (len=%u)\n", len);
		} else {
			printf("msg id=%d len=%u\n", id, len);
		}
		free(msg);
	}

done:
	close(fd);
	return 0;
}
