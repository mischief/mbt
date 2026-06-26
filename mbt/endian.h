// SPDX-License-Identifier: ISC
#pragma once

#if defined(__ANDROID__)
#include <sys/endian.h>
#elif defined(__APPLE__)
#include <machine/endian.h>
#elif defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__sun)
#include <sys/endian.h>
#else
#include <endian.h>
#endif
