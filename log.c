// SPDX-License-Identifier: ISC
#include <stdio.h>
#include <syslog.h>

#include "mbt/log.h"

static mbt_log_fn log_cb = NULL;
static void *log_arg = NULL;
static uint32_t debug_mask = 0;

void
mbt_log_set(mbt_log_fn fn, void *arg)
{
	log_cb = fn;
	log_arg = arg;
}

void
mbt_log_set_debug(uint32_t component_mask)
{
	debug_mask = component_mask;
}

void
mbt_log_emit(enum mbt_log_level level, enum mbt_log_component comp,
             const char *fmt, ...)
{
	va_list ap;

	if (!log_cb)
		return;

	/* Gate debug messages by component mask */
	if (level == MBT_LOG_DEBUG && !(debug_mask & comp))
		return;

	va_start(ap, fmt);
	log_cb(level, comp, fmt, ap, log_arg);
	va_end(ap);
}

/* --- Built-in: stderr via warnx-style --- */

static const char *
level_str(enum mbt_log_level level)
{
	switch (level) {
	case MBT_LOG_ERROR: return "error";
	case MBT_LOG_WARN:  return "warn";
	case MBT_LOG_INFO:  return "info";
	case MBT_LOG_DEBUG: return "debug";
	}
	return "?";
}

static const char *
comp_str(enum mbt_log_component comp)
{
	switch (comp) {
	case MBT_LOG_SESSION: return "session";
	case MBT_LOG_TORRENT: return "torrent";
	case MBT_LOG_PEER:    return "peer";
	case MBT_LOG_CONN:    return "conn";
	case MBT_LOG_LSD:     return "lsd";
	case MBT_LOG_DHT:     return "dht";
	case MBT_LOG_STORAGE: return "storage";
	case MBT_LOG_PICKER:  return "picker";
	case MBT_LOG_TRACKER: return "tracker";
	case MBT_LOG_TRACKERD: return "trackerd";
	default: break;
	}
	return "mbt";
}

void
mbt_log_stderr(enum mbt_log_level level, enum mbt_log_component comp,
               const char *fmt, va_list ap, void *arg)
{
	(void)arg;
	fprintf(stderr, "%s[%s]: ", comp_str(comp), level_str(level));
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
}

/* --- Built-in: syslog --- */

static int
level_to_syslog(enum mbt_log_level level)
{
	switch (level) {
	case MBT_LOG_ERROR: return LOG_ERR;
	case MBT_LOG_WARN:  return LOG_WARNING;
	case MBT_LOG_INFO:  return LOG_INFO;
	case MBT_LOG_DEBUG: return LOG_DEBUG;
	}
	return LOG_INFO;
}

void
mbt_log_syslog(enum mbt_log_level level, enum mbt_log_component comp,
               const char *fmt, va_list ap, void *arg)
{
	(void)arg;
	(void)comp;
	vsyslog(level_to_syslog(level), fmt, ap);
}

const char *
mbt_log_level_str(enum mbt_log_level level)
{
	switch (level) {
	case MBT_LOG_DEBUG: return "debug";
	case MBT_LOG_INFO:  return "info";
	case MBT_LOG_WARN:  return "warn";
	case MBT_LOG_ERROR: return "error";
	default: return "?";
	}
}

const char *
mbt_log_component_str(enum mbt_log_component comp)
{
	switch (comp) {
	case MBT_LOG_SESSION:  return "session";
	case MBT_LOG_TORRENT:  return "torrent";
	case MBT_LOG_PEER:     return "peer";
	case MBT_LOG_CONN:     return "conn";
	case MBT_LOG_LSD:      return "lsd";
	case MBT_LOG_DHT:      return "dht";
	case MBT_LOG_STORAGE:  return "storage";
	case MBT_LOG_PICKER:   return "picker";
	case MBT_LOG_TRACKER:  return "tracker";
	case MBT_LOG_TRACKERD: return "trackerd";
	default: return "?";
	}
}
