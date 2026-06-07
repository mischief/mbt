#include <ctype.h>
#include <stdlib.h>
#include <errno.h>

#include <hex.h>

static int
mchrtobin(char h, unsigned char *out)
{
	if(!isxdigit(h))
		return -EINVAL;

	switch(h){
	case '0': case '1': case '2': case '3': 
	case '4': case '5': case '6': case '7': 
	case '8': case '9':
		*out = h - '0';
		break;
	case 'A': case 'B': case 'C': case 'D': 
	case 'E': case 'F':
		*out = h - 'A' + 10;
		break;
	case 'a': case 'b': case 'c': case 'd': 
	case 'e': case 'f':
		*out = h - 'a' + 10;
		break;
	default:
		abort();
	}

	return 0;
}

int
mbt_hex_decode(unsigned char *buf, size_t len, const char *str)
{
	size_t i;
	int rv;
	unsigned char h, l;

	if(!buf)
		return len;

	for(i = 0; i < len; i++){
		if((rv = mchrtobin(str[i*2], &h)) < 0)
			return rv;

		if((rv = mchrtobin(str[i*2+1], &l)) < 0)
			return rv;

		buf[i] = (h << 4) | l;
	}

	return len;
}

int
mbt_hex_encode(unsigned char *buf, size_t blen, char *str, size_t slen)
{
	size_t i;
	const char hex[] = "0123456789ABCDEF";

	for(i = 0; i < blen; i++){
		if(i*2 >= slen)
			return -ENOBUFS;

		str[i*2] = hex[buf[i] >> 4];
		str[i*2+1] = hex[buf[i] & 0xF];
	}

	str[i*2] = '\0';

	return 0;
}

