#ifndef CONVERT_H
#define CONVERT_H CONVERT_H

/* r5g5b5 index into a palette lookup table */
#define LUTBITS 5
#define LUTSIZE (1 << (3 * LUTBITS))

/*
 * Description of the server's output pixel format.
 *
 * Deliberately free of Xlib types: xlib.c pulls the numbers out of the
 * XImage and the visual, so the packing code below can be unit tested
 * without a display (test/test links convert.o but not xlib.o).
 */
struct pixfmt{
	int bpp;			/* bits per pixel: 8, 16, 24 or 32 */
	int msb;			/* true if the server wants MSBFirst */
	unsigned long rmask, gmask, bmask;
	int rshift, gshift, bshift;	/* position of each channel */
	int rbits, gbits, bbits;	/* width of each channel */
	int direct;			/* scale() may write to the XImage itself */

	const unsigned char *lut;	/* palette lookup, NULL for TrueColor */
	int dither;			/* apply an ordered dither */
};

void pixfmt_init(struct pixfmt *fmt, int bpp, int msb,
		unsigned long rmask, unsigned long gmask, unsigned long bmask);

/* Switch fmt to palette mode. lut must hold LUTSIZE entries. */
void pixfmt_setpalette(struct pixfmt *fmt, const unsigned char *lut);

void pixfmt_setdither(struct pixfmt *fmt, int on);

/* True if fmt is coarse enough that an undithered image will band badly. */
int pixfmt_iscoarse(const struct pixfmt *fmt);

/*
 * Fill lut with the nearest of n palette entries for every r5g5b5 colour.
 * rgb holds n * 3 bytes. Costs LUTSIZE * n distance comparisons, which is
 * fine for the 16 entries of a depth 4 visual and slow for 256.
 */
void palette_lut(unsigned char *lut, const unsigned char *rgb, int n);

/*
 * Convert the 32bpp BGRX buffer scale() produces into the server's format.
 * src and dst may not overlap.
 */
void pack_rows(const struct pixfmt *fmt, const unsigned char *src, char *dst,
		unsigned int width, unsigned int height,
		unsigned int srcstride, unsigned int dststride);

#endif
