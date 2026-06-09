// SPDX-License-Identifier: ISC
#ifndef MBT_PCP_H
#define MBT_PCP_H

#include <stdint.h>

struct mbt_ev;
struct mbt_pcp;

/* Callback: called when mapping is established or renewed */
typedef void (*mbt_pcp_cb)(const char *external_addr, uint16_t external_port,
                           uint32_t lifetime, void *arg);

/* Create PCP client. gateway=NULL auto-detects default gateway. */
int mbt_pcp_new(struct mbt_pcp **out, struct mbt_ev *ev, const char *gateway);
void mbt_pcp_free(struct mbt_pcp *p);

/* Request a port mapping. Handles retransmit + renewal automatically. */
int mbt_pcp_map(struct mbt_pcp *p, uint8_t protocol, uint16_t internal_port,
                uint32_t lifetime, mbt_pcp_cb cb, void *arg);

/* Delete mapping */
int mbt_pcp_unmap(struct mbt_pcp *p, uint8_t protocol, uint16_t internal_port);

/* Get assigned external address (NULL if not yet mapped) */
const char *mbt_pcp_external_addr(struct mbt_pcp *p);
uint16_t mbt_pcp_external_port(struct mbt_pcp *p);

#endif
