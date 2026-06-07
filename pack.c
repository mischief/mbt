#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <endian.h>
#include <stdint.h>

#include "pack.h"

int
mbt_pack(unsigned char *out, char *v, ...)
{
	int s;
	size_t l;
	va_list ap;
	uint32_t be;

	s = 0;

	va_start(ap, v);

	while(*v){
		switch(*v++){
		case 'b':
			*out++ = (char) va_arg(ap, int);
			s++;
			break;
		case 'l':
			be = htobe32(va_arg(ap, uint32_t));
			memcpy(out, &be, sizeof(uint32_t));
			out += 4;
			s += 4;
			break;
		case '*':
			l = va_arg(ap, size_t);
			memmove(out, va_arg(ap, void*), l);
			out += l;
			s += l;
			break;
		default:
			abort();
		}
	}

	va_end(ap);

	return s;
}

int
mbt_unpack(unsigned char *in, char *v, ...)
{
	int s, l;
	va_list ap;
	uint32_t be;

	s = 0;

	va_start(ap, v);

	while(*v){
		switch(*v){
		case 'b':
			*va_arg(ap, unsigned char*) = *in;
			in++;
			s++;
			break;
		case 'l':
			memmove(&be, in, sizeof(uint32_t));
			be = be32toh(be);
			*va_arg(ap, uint32_t*) = be;
			in += 4;
			s += 4;
			break;
		case '*':
			l = va_arg(ap, int);
			memmove(va_arg(ap, unsigned char*), in, l);
			in += l;
			s += l;
			break;
		}

		v++;
	}

	va_end(ap);

	return s;
}
