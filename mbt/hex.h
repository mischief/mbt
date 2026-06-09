// SPDX-License-Identifier: ISC
#include "export.h"
#include <stdlib.h>

MBT_API int mbt_hex_decode(unsigned char *buf, size_t len, const char *str);
MBT_API int mbt_hex_encode(unsigned char *buf, size_t blen, char *str, size_t slen);

