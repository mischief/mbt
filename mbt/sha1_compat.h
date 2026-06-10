// SPDX-License-Identifier: ISC
#ifndef MBT_SHA1_COMPAT_H
#define MBT_SHA1_COMPAT_H

#if defined(__FreeBSD__)
#include <sha.h>
#define SHA1_CTX	SHA_CTX
#define SHA1Init	SHA_Init
#define SHA1Update	SHA_Update
#define SHA1Final	SHA_Final
#else
#include <sha1.h>
#endif

#endif
