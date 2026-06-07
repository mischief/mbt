#include <net/ethernet.h>
#include <arpa/inet.h>

struct lsd_frame_info;

struct lsd
{
	int fd;

	int (*on_frame)(struct lsd_frame_info *lfi, void *v);
	void *on_frame_arg;
};

struct lsd_frame
{
	struct sockaddr_storage sa;
	char remote[INET6_ADDRSTRLEN];
	char frame[ETH_DATA_LEN];
	int len;
};

struct lsd_frame_info
{
	struct sockaddr_storage sa;
	char remote[INET6_ADDRSTRLEN];
	uint16_t port;
	char *cookie;
	struct mbt_infohash_head infohashes;
};

void lsd_freep(struct lsd **v);
void lsd_frame_freep(struct lsd_frame **v);
void lsd_frame_info_freep(struct lsd_frame_info **v);
int lsd_init(struct lsd **out, int setfl);
int lsd_recv(struct lsd *lsd, struct lsd_frame **out);
int lsd_frame_parse(struct lsd_frame *frame, struct lsd_frame_info **out);
int lsd_announce(struct lsd *lsd, const char *infohash_hex, uint16_t port);

