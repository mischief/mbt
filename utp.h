#include <stdint.h>

/* utp_state states */
enum {
	UTP_CS_NONE = 0,
	UTP_CS_SYN_SENT = 1,
	UTP_CS_SYN_RECV = 2,
	UTP_CS_CONNECTED = 3,
};

struct utp_state {
	int state;
};

/* utp_frame types */
enum {
	UTP_ST_DATA = 0,
	UTP_ST_FIN = 1,
	UTP_ST_STATE = 2,
	UTP_ST_RESET = 3,
	UTP_ST_SYN = 4,
};

struct __attribute__((packed)) utp_frame {
	uint8_t type:4;
	uint8_t ver:4;
	uint8_t extension;
	uint16_t connectionid;
	uint32_t timestamp_microseconds;
	uint32_t timestamp_difference_microseconds;
	uint32_t wnd_size;
	uint16_t seq_nr;
	uint16_t ack_nr;
};

