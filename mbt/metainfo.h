// SPDX-License-Identifier: ISC
#ifndef MBT_METAINFO_H
#define MBT_METAINFO_H

#include "export.h"

#include <stdint.h>
#include <stddef.h>

struct mbt_metainfo;
struct mbt_infohash;

/* --- Lifecycle --- */

/* Create empty metainfo (for building a new .torrent) */
MBT_API int mbt_metainfo_new(struct mbt_metainfo **out);

/* Parse from bencoded buffer (e.g. .torrent file contents) */
MBT_API int mbt_metainfo_parse(struct mbt_metainfo **out, const void *data, size_t len);

/* Free */
MBT_API void mbt_metainfo_free(struct mbt_metainfo *mi);

/* --- Builder (for creating .torrents) --- */

/* Set content from file or directory. Reads files, computes piece hashes. */
MBT_API int mbt_metainfo_set_content(struct mbt_metainfo *mi, const char *path, uint32_t piece_length);

/* Add a tracker URL */
MBT_API int mbt_metainfo_add_tracker(struct mbt_metainfo *mi, const char *url);

/* Set comment */
MBT_API void mbt_metainfo_set_comment(struct mbt_metainfo *mi, const char *comment);

/* Set private flag (disables DHT/PEX/LSD) */
MBT_API void mbt_metainfo_set_private(struct mbt_metainfo *mi, int private);

/* --- Accessors --- */

MBT_API const char *mbt_metainfo_name(struct mbt_metainfo *mi);
MBT_API struct mbt_infohash *mbt_metainfo_infohash(struct mbt_metainfo *mi);
MBT_API uint32_t mbt_metainfo_piece_count(struct mbt_metainfo *mi);
MBT_API uint32_t mbt_metainfo_piece_length(struct mbt_metainfo *mi);
MBT_API uint64_t mbt_metainfo_total_length(struct mbt_metainfo *mi);
MBT_API int mbt_metainfo_file_count(struct mbt_metainfo *mi);
MBT_API const char *mbt_metainfo_file_path(struct mbt_metainfo *mi, int idx);
MBT_API uint64_t mbt_metainfo_file_length(struct mbt_metainfo *mi, int idx);
MBT_API int mbt_metainfo_tracker_count(struct mbt_metainfo *mi);
MBT_API const char *mbt_metainfo_tracker(struct mbt_metainfo *mi, int idx);
MBT_API int mbt_metainfo_is_private(struct mbt_metainfo *mi);
MBT_API const char *mbt_metainfo_comment(struct mbt_metainfo *mi);

/* DHT bootstrap nodes */
MBT_API int mbt_metainfo_add_node(struct mbt_metainfo *mi, const char *host, uint16_t port);
MBT_API int mbt_metainfo_node_count(struct mbt_metainfo *mi);
MBT_API const char *mbt_metainfo_node_host(struct mbt_metainfo *mi, int idx);
MBT_API uint16_t mbt_metainfo_node_port(struct mbt_metainfo *mi, int idx);

/* Raw info dict bytes (for infohash computation, BEP 9 serving) */
MBT_API const void *mbt_metainfo_raw_info(struct mbt_metainfo *mi, size_t *len);

/* Infohash as hex string */
MBT_API const char *mbt_metainfo_infohash_hex(struct mbt_metainfo *mi);

/* --- Serialization --- */

/* Encode to bencoded .torrent buffer (caller frees *out) */
MBT_API int mbt_metainfo_encode(struct mbt_metainfo *mi, unsigned char **out, size_t *len);

#endif
