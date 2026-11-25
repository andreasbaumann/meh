
#include <stddef.h>

#include "convert.h"

static void maskinfo(unsigned long mask, int *shift, int *bits){
	int s = 0, b = 0;
	if(mask){
		while(!(mask & 1)){
			mask >>= 1;
			s++;
		}
		while(mask & 1){
			mask >>= 1;
			b++;
		}
	}
	*shift = s;
	*bits = b;
}

/*
 * Widen an 8 bit channel value to n bits. Narrowing truncates, widening
 * replicates the high bits so that 0xff maps to all ones.
 */
static unsigned long widen(unsigned int v, int bits){
	if(bits <= 0)
		return 0;
	if(bits >= 8)
		return (v << (bits - 8)) | (v >> (16 - bits));
	return v >> (8 - bits);
}

void pixfmt_init(struct pixfmt *fmt, int bpp, int msb,
		unsigned long rmask, unsigned long gmask, unsigned long bmask){
	fmt->bpp = bpp;
	fmt->msb = !!msb;
	fmt->rmask = rmask;
	fmt->gmask = gmask;
	fmt->bmask = bmask;
	maskinfo(rmask, &fmt->rshift, &fmt->rbits);
	maskinfo(gmask, &fmt->gshift, &fmt->gbits);
	maskinfo(bmask, &fmt->bshift, &fmt->bbits);

	/* The format scale() already writes: 32bpp LSBFirst BGRX. */
	fmt->direct = bpp == 32 && !fmt->msb &&
		rmask == 0xff0000UL && gmask == 0xff00UL && bmask == 0xffUL;
}

/*
 * One pass per output format, chosen outside the pixel loop. The three
 * channel tables turn mask arithmetic into a lookup, so odd layouts
 * (RGB555, RGB565, 10 bit deep colour, 3-3-2) all cost the same.
 */
#define PACKLOOP(BODY) \
	for(y = 0; y < height; y++){ \
		const unsigned char * __restrict__ s = src + (size_t)y * srcstride; \
		unsigned char * __restrict__ d = (unsigned char *)dst + (size_t)y * dststride; \
		for(x = 0; x < width; x++){ \
			const unsigned long px = rtab[s[2]] | gtab[s[1]] | btab[s[0]]; \
			BODY \
			s += 4; \
		} \
	}

void pack_rows(const struct pixfmt *fmt, const unsigned char *src, char *dst,
		unsigned int width, unsigned int height,
		unsigned int srcstride, unsigned int dststride){
	unsigned long rtab[256], gtab[256], btab[256];
	unsigned int x, y, i;

	for(i = 0; i < 256; i++){
		rtab[i] = widen(i, fmt->rbits) << fmt->rshift;
		gtab[i] = widen(i, fmt->gbits) << fmt->gshift;
		btab[i] = widen(i, fmt->bbits) << fmt->bshift;
	}

	switch(fmt->bpp){
	case 32:
		if(fmt->msb){
			PACKLOOP(
				*d++ = px >> 24;
				*d++ = px >> 16;
				*d++ = px >> 8;
				*d++ = px;
			)
		}else{
			PACKLOOP(
				*d++ = px;
				*d++ = px >> 8;
				*d++ = px >> 16;
				*d++ = px >> 24;
			)
		}
		break;
	case 24:
		if(fmt->msb){
			PACKLOOP(
				*d++ = px >> 16;
				*d++ = px >> 8;
				*d++ = px;
			)
		}else{
			PACKLOOP(
				*d++ = px;
				*d++ = px >> 8;
				*d++ = px >> 16;
			)
		}
		break;
	case 16:
		if(fmt->msb){
			PACKLOOP(
				*d++ = px >> 8;
				*d++ = px;
			)
		}else{
			PACKLOOP(
				*d++ = px;
				*d++ = px >> 8;
			)
		}
		break;
	case 8:
		PACKLOOP(
			*d++ = px;
		)
		break;
	}
}
