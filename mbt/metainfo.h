#ifndef MBT_METAINFO_H
#define MBT_METAINFO_H

#include <stdint.h>
#include <stddef.h>

struct mbt_metainfo;
struct mbt_infohash;

/* --- Lifecycle --- */

/* Create empty metainfo (for building a new .torrent) */
int mbt_metainfo_new(struct mbt_metainfo **out);

/* Parse from bencoded buffer (e.g. .torrent file contents) */
int mbt_metainfo_parse(struct mbt_metainfo **out, const void *data, size_t len);

/* Free */
void mbt_metainfo_free(struct mbt_metainfo *mi);

/* --- Builder (for creating .torrents) --- */

/* Set content from file or directory. Reads files, computes piece hashes. */
int mbt_metainfo_set_content(struct mbt_metainfo *mi, const char *path, uint32_t piece_length);

/* Add a tracker URL */
int mbt_metainfo_add_tracker(struct mbt_metainfo *mi, const char *url);

/* Set comment */
void mbt_metainfo_set_comment(struct mbt_metainfo *mi, const char *comment);

/* Set private flag (disables DHT/PEX/LSD) */
void mbt_metainfo_set_private(struct mbt_metainfo *mi, int private);

/* --- Accessors --- */

const char *mbt_metainfo_name(struct mbt_metainfo *mi);
struct mbt_infohash *mbt_metainfo_infohash(struct mbt_metainfo *mi);
uint32_t mbt_metainfo_piece_count(struct mbt_metainfo *mi);
uint32_t mbt_metainfo_piece_length(struct mbt_metainfo *mi);
uint64_t mbt_metainfo_total_length(struct mbt_metainfo *mi);
int mbt_metainfo_file_count(struct mbt_metainfo *mi);
const char *mbt_metainfo_file_path(struct mbt_metainfo *mi, int idx);
uint64_t mbt_metainfo_file_length(struct mbt_metainfo *mi, int idx);
int mbt_metainfo_tracker_count(struct mbt_metainfo *mi);
const char *mbt_metainfo_tracker(struct mbt_metainfo *mi, int idx);
int mbt_metainfo_is_private(struct mbt_metainfo *mi);
const char *mbt_metainfo_comment(struct mbt_metainfo *mi);

/* Raw info dict bytes (for infohash computation, BEP 9 serving) */
const void *mbt_metainfo_raw_info(struct mbt_metainfo *mi, size_t *len);

/* Infohash as hex string */
const char *mbt_metainfo_infohash_hex(struct mbt_metainfo *mi);

/* --- Serialization --- */

/* Encode to bencoded .torrent buffer (caller frees *out) */
int mbt_metainfo_encode(struct mbt_metainfo *mi, unsigned char **out, size_t *len);

#endif
