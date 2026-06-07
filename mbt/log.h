#ifndef MBT_LOG_H
#define MBT_LOG_H

#include <stdarg.h>
#include <stdint.h>

enum mbt_log_level {
	MBT_LOG_ERROR,
	MBT_LOG_WARN,
	MBT_LOG_INFO,
	MBT_LOG_DEBUG,
};

enum mbt_log_component {
	MBT_LOG_SESSION  = 1 << 0,
	MBT_LOG_TORRENT  = 1 << 1,
	MBT_LOG_PEER     = 1 << 2,
	MBT_LOG_CONN     = 1 << 3,
	MBT_LOG_LSD      = 1 << 4,
	MBT_LOG_STORAGE  = 1 << 5,
	MBT_LOG_PICKER   = 1 << 6,
};

typedef void (*mbt_log_fn)(enum mbt_log_level level, enum mbt_log_component comp,
                           const char *fmt, va_list ap, void *arg);

/* Set log callback. NULL = silent (default). */
void mbt_log_set(mbt_log_fn fn, void *arg);

/* Set which components emit DEBUG messages (bitmask). Default: 0 (none). */
void mbt_log_set_debug(uint32_t component_mask);

/* Built-in callbacks */
void mbt_log_stderr(enum mbt_log_level level, enum mbt_log_component comp,
                    const char *fmt, va_list ap, void *arg);
void mbt_log_syslog(enum mbt_log_level level, enum mbt_log_component comp,
                    const char *fmt, va_list ap, void *arg);

/* Internal — used by library code via macros below */
void mbt_log_emit(enum mbt_log_level level, enum mbt_log_component comp,
                  const char *fmt, ...);

#define mbt_log_error(comp, ...) mbt_log_emit(MBT_LOG_ERROR, (comp), __VA_ARGS__)
#define mbt_log_warn(comp, ...)  mbt_log_emit(MBT_LOG_WARN, (comp), __VA_ARGS__)
#define mbt_log_info(comp, ...)  mbt_log_emit(MBT_LOG_INFO, (comp), __VA_ARGS__)
#define mbt_log_debug(comp, ...) mbt_log_emit(MBT_LOG_DEBUG, (comp), __VA_ARGS__)

#endif
