// SPDX-License-Identifier: ISC
#ifndef MBT_BANNED_H
#define MBT_BANNED_H

/*
 * Include this file last, after all system headers. #pragma GCC poison fires
 * on any use of the token, also inside system headers included after it.
 */

/* No bounds: use snprintf and memcpy with a checked length. */
#pragma GCC poison gets strcpy strcat strncpy strncat sprintf vsprintf

/* Not reentrant: use strtok_r. */
#pragma GCC poison strtok

/* No error detection: use parse.h. */
#pragma GCC poison atoi atol atoll

/* Stack smashing hazard. */
#ifdef alloca
#undef alloca
#endif
#pragma GCC poison alloca

#endif
