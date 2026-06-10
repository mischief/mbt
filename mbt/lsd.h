// SPDX-License-Identifier: ISC
#include "export.h"
#include <net/ethernet.h>
#include <arpa/inet.h>

struct mbt_lsd_frame_info;

struct mbt_lsd
{
	int fd;   /* IPv4 multicast */
	int fd6;  /* IPv6 multicast (-1 if unavailable) */

	int (*on_frame)(struct mbt_lsd_frame_info *lfi, void *v);
	void *on_frame_arg;
};

struct mbt_lsd_frame
{
	struct sockaddr_storage sa;
	char remote[INET6_ADDRSTRLEN];
	char frame[ETH_DATA_LEN];
	int len;
};

struct mbt_lsd_frame_info
{
	struct sockaddr_storage sa;
	char remote[INET6_ADDRSTRLEN];
	uint16_t port;
	char *cookie;
	struct mbt_infohash_head infohashes;
};

MBT_API void mbt_lsd_freep(struct mbt_lsd **v);
MBT_API void mbt_lsd_frame_freep(struct mbt_lsd_frame **v);
MBT_API void mbt_lsd_frame_info_freep(struct mbt_lsd_frame_info **v);
MBT_API int mbt_lsd_init(struct mbt_lsd **out, int setfl);
MBT_API int mbt_lsd_recv(struct mbt_lsd *lsd, struct mbt_lsd_frame **out);
MBT_API int mbt_lsd_frame_parse(struct mbt_lsd_frame *frame, struct mbt_lsd_frame_info **out);
MBT_API int mbt_lsd_announce(struct mbt_lsd *lsd, const char *infohash_hex, uint16_t port);

