// SPDX-License-Identifier: ISC
#ifndef MBT_USEFUL_H
#define MBT_USEFUL_H

/*
 * ASSERT_RETURN: check a precondition at a public API boundary. It stays on in
 * every build and returns r. Use assert(3) for invariants inside the library,
 * and never for data from the network.
 */
#define ASSERT_RETURN(expr, r)                                                 \
	do {                                                                   \
		if (!(expr))                                                   \
			return (r);                                            \
	} while (0)

#endif
