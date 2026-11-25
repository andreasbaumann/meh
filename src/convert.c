
#include <stddef.h>

#include "convert.h"

/*
 * Amplitude of the ordered dither in palette mode, in 8 bit channel
 * steps. A 16 entry palette has a grey spacing of about 28, so this is
 * deliberately a little under one step.
 */
#define DITHERAMP 24

/* Ordered dither, indexed by ((y & 3) << 2) | (x & 3). */
static const int bayer[16] = {
	 0,  8,  2, 10,
	12,  4, 14,  6,
	 3, 11,  1,  9,
	15,  7, 13,  5
};

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

static int clamp8(int v){
	if(v < 0)
		return 0;
	if(v > 255)
		return 255;
	return v;
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
	fmt->lut = NULL;
	fmt->dither = 0;

	/* The format scale() already writes: 32bpp LSBFirst BGRX. */
	fmt->direct = bpp == 32 && !fmt->msb &&
		rmask == 0xff0000UL && gmask == 0xff00UL && bmask == 0xffUL;
}

void pixfmt_setpalette(struct pixfmt *fmt, const unsigned char *lut){
	fmt->lut = lut;
	fmt->direct = 0;
}

void pixfmt_setdither(struct pixfmt *fmt, int on){
	fmt->dither = !!on;
	if(on)
		fmt->direct = 0;
}

int pixfmt_iscoarse(const struct pixfmt *fmt){
	if(fmt->lut)
		return 1;
	return fmt->rbits < 5 || fmt->gbits < 5 || fmt->bbits < 5;
}

void palette_lut(unsigned char *lut, const unsigned char *rgb, int n){
	unsigned int r, g, b;
	int i;

	if(n < 1)
		n = 1;
	if(n > 256)
		n = 256;

	for(r = 0; r < (1 << LUTBITS); r++){
		const int r8 = (r << 3) | (r >> 2);
		for(g = 0; g < (1 << LUTBITS); g++){
			const int g8 = (g << 3) | (g >> 2);
			for(b = 0; b < (1 << LUTBITS); b++){
				const int b8 = (b << 3) | (b >> 2);
				long bestd = -1;
				int best = 0;
				for(i = 0; i < n; i++){
					const int dr = rgb[i * 3 + 0] - r8;
					const int dg = rgb[i * 3 + 1] - g8;
					const int db = rgb[i * 3 + 2] - b8;
					const long d = (long)dr * dr + (long)dg * dg + (long)db * db;
					if(bestd < 0 || d < bestd){
						bestd = d;
						best = i;
					}
				}
				lut[(r << (2 * LUTBITS)) | (g << LUTBITS) | b] = best;
			}
		}
	}
}

/*
 * One pass per output format, chosen outside the pixel loop.
 *
 * The three channel tables turn mask arithmetic into a lookup, so odd
 * layouts (RGB555, RGB565, 2-1-1, 10 bit deep colour) all cost the same.
 * The dither offsets are zero when dithering is off, which costs an add
 * and a clamp per channel rather than doubling the number of loops; the
 * conversion pass is far cheaper than scale() either way, and the 24/32
 * bit case skips it entirely through fmt->direct.
 */
#define PACKLOOP(BODY) \
	for(y = 0; y < height; y++){ \
		const unsigned char * __restrict__ s = src + (size_t)y * srcstride; \
		unsigned char * __restrict__ d = (unsigned char *)dst + (size_t)y * dststride; \
		const int * __restrict__ bay = &bayer[(y & 3) << 2]; \
		for(x = 0; x < width; x++){ \
			const int bo = bay[x & 3] - 8; \
			const unsigned int r = clamp8(s[2] + ((bo * ar) >> 4)); \
			const unsigned int g = clamp8(s[1] + ((bo * ag) >> 4)); \
			const unsigned int b = clamp8(s[0] + ((bo * ab) >> 4)); \
			BODY \
			s += 4; \
		} \
	}

#define TRUECOLOUR(STORE) PACKLOOP( \
		const unsigned long px = rtab[r] | gtab[g] | btab[b]; \
		STORE \
	)

void pack_rows(const struct pixfmt *fmt, const unsigned char *src, char *dst,
		unsigned int width, unsigned int height,
		unsigned int srcstride, unsigned int dststride){
	unsigned long rtab[256], gtab[256], btab[256];
	const unsigned char *lut = fmt->lut;
	unsigned int x, y, i;
	int ar = 0, ag = 0, ab = 0;

	if(fmt->dither){
		if(lut){
			ar = ag = ab = DITHERAMP;
		}else{
			/* one quantisation step per channel */
			ar = fmt->rbits > 0 ? 255 / ((1 << fmt->rbits) - 1) : 0;
			ag = fmt->gbits > 0 ? 255 / ((1 << fmt->gbits) - 1) : 0;
			ab = fmt->bbits > 0 ? 255 / ((1 << fmt->bbits) - 1) : 0;
		}
	}

	if(lut){
		/* Palettised: one byte per pixel, nearest entry via the table. */
		PACKLOOP(
			*d++ = lut[((r >> 3) << (2 * LUTBITS)) | ((g >> 3) << LUTBITS) | (b >> 3)];
		)
		return;
	}

	for(i = 0; i < 256; i++){
		rtab[i] = widen(i, fmt->rbits) << fmt->rshift;
		gtab[i] = widen(i, fmt->gbits) << fmt->gshift;
		btab[i] = widen(i, fmt->bbits) << fmt->bshift;
	}

	switch(fmt->bpp){
	case 32:
		if(fmt->msb){
			TRUECOLOUR(
				*d++ = px >> 24;
				*d++ = px >> 16;
				*d++ = px >> 8;
				*d++ = px;
			)
		}else{
			TRUECOLOUR(
				*d++ = px;
				*d++ = px >> 8;
				*d++ = px >> 16;
				*d++ = px >> 24;
			)
		}
		break;
	case 24:
		if(fmt->msb){
			TRUECOLOUR(
				*d++ = px >> 16;
				*d++ = px >> 8;
				*d++ = px;
			)
		}else{
			TRUECOLOUR(
				*d++ = px;
				*d++ = px >> 8;
				*d++ = px >> 16;
			)
		}
		break;
	case 16:
		if(fmt->msb){
			TRUECOLOUR(
				*d++ = px >> 8;
				*d++ = px;
			)
		}else{
			TRUECOLOUR(
				*d++ = px;
				*d++ = px >> 8;
			)
		}
		break;
	case 8:
		TRUECOLOUR(
			*d++ = px;
		)
		break;
	}
}
