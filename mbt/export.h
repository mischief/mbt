// SPDX-License-Identifier: ISC
#ifndef MBT_EXPORT_H
#define MBT_EXPORT_H

#ifdef MBT_BUILDING
#define MBT_API __attribute__((visibility("default")))
#else
#define MBT_API
#endif

#endif
