#ifndef CONVERT_H
#define CONVERT_H CONVERT_H

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
};

void pixfmt_init(struct pixfmt *fmt, int bpp, int msb,
		unsigned long rmask, unsigned long gmask, unsigned long bmask);

/*
 * Convert the 32bpp BGRX buffer scale() produces into the server's format.
 * src and dst may not overlap.
 */
void pack_rows(const struct pixfmt *fmt, const unsigned char *src, char *dst,
		unsigned int width, unsigned int height,
		unsigned int srcstride, unsigned int dststride);

#endif
