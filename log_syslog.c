// SPDX-License-Identifier: ISC
#include <syslog.h>
#include <stdarg.h>
#include "mbt/log.h"

static int
level_to_syslog(enum mbt_log_level level)
{
	switch (level) {
	case MBT_LOG_ERROR: return LOG_ERR;
	case MBT_LOG_WARN:  return LOG_WARNING;
	case MBT_LOG_INFO:  return LOG_INFO;
	case MBT_LOG_DEBUG: return LOG_DEBUG;
	case MBT_LOG_TRACE: return LOG_DEBUG;
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
