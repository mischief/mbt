// SPDX-License-Identifier: ISC
#pragma once

/* MIN/MAX: provided by <sys/param.h> on Linux/BSDs, not on Solaris */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
