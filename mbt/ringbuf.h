// SPDX-License-Identifier: ISC
#include "mbt/export.h"
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

struct mbt_ringbuf;

MBT_API int mbt_ringbuf_new(struct mbt_ringbuf **out, uint32_t max_cap);
MBT_API void mbt_ringbuf_free(struct mbt_ringbuf *r);

MBT_API uint32_t mbt_ringbuf_available(const struct mbt_ringbuf *r);
MBT_API uint32_t mbt_ringbuf_space(const struct mbt_ringbuf *r);
MBT_API uint32_t mbt_ringbuf_cap(const struct mbt_ringbuf *r);
MBT_API uint32_t mbt_ringbuf_rd(const struct mbt_ringbuf *r);

/* Produce len bytes from src. Returns 0 on success.
 * Returns -ENOBUFS if at max capacity and data still won't fit.
 * Returns -ENOMEM on allocation failure. */
MBT_API int mbt_ringbuf_produce(struct mbt_ringbuf *r, const void *src, uint32_t len);

/* Consume up to len bytes into dst. Returns bytes consumed, or 0 if empty. */
MBT_API ssize_t mbt_ringbuf_consume(struct mbt_ringbuf *r, void *dst, uint32_t len);

/* Discard len bytes (advance read pointer without copying). */
MBT_API void mbt_ringbuf_discard(struct mbt_ringbuf *r, uint32_t len);

/* Grow to hold at least want more bytes. Compacts live data to offset 0.
 * Callers with external offsets must adjust them by the returned delta (old rd).
 * Returns -ENOBUFS if at max capacity, -ENOMEM on allocation failure, 0 on success. */
MBT_API int mbt_ringbuf_grow(struct mbt_ringbuf *r, uint32_t want, uint32_t *out_delta);

/* Shrink to min(max(min_cap, peak), current cap). Returns 0 on success.
 * Non-fatal: returns 0 if shrink is not beneficial or would exceed peak. */
MBT_API int mbt_ringbuf_shrink(struct mbt_ringbuf *r, uint32_t min_cap, uint32_t *out_delta);

/* Update high-water mark tracking. Call after produce/consume cycles. */
MBT_API void mbt_ringbuf_update_peak(struct mbt_ringbuf *r);
MBT_API uint32_t mbt_ringbuf_peak(const struct mbt_ringbuf *r);

/* Zero-copy write region. Returns pointer and contiguous available length.
 * Caller must finalize with mbt_ringbuf_write_done. */
MBT_API uint8_t *mbt_ringbuf_write_ptr(struct mbt_ringbuf *r, uint32_t *out_len);
MBT_API void mbt_ringbuf_write_done(struct mbt_ringbuf *r, uint32_t len);

/* Peek at data at offset relative to the start of live data (rd).
 * Does not advance rd. Returns pointer to up to *out_len contiguous bytes.
 * Returns NULL if buffer is not yet allocated or offset is beyond wr. */
MBT_API const uint8_t *mbt_ringbuf_peek_at(const struct mbt_ringbuf *r, uint32_t offset, uint32_t *out_len);
