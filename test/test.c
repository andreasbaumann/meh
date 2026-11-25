#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/time.h"

#include "../src/meh.h"
#include "../src/scale.h"
#include "../src/convert.h"

#define TESTRUNS 20
#define STARTTEST(name) int test_##name(){ testname = #name; testsrun++; do
#define ENDTEST while(0); testspassed++; return 0;}
#define STARTTIME struct timeval tvs[2];int _i, _j; for(_i = 0; _i < 2; gettimeofday(&tvs[_i], NULL), _i++){for(_j = 0; _j < (_i ? TESTRUNS : 3); _j++)
#define ENDTIME }long int mselapsed = (tvs[1].tv_sec - tvs[0].tv_sec) * 1000000L + (tvs[1].tv_usec - tvs[0].tv_usec); printf("%s: average time: %li.%.3li ms\n", testname, mselapsed/1000/TESTRUNS, (mselapsed/TESTRUNS)%1000);
#define assert(x) if(!(x)){fprintf(stderr, "test \"%s\" failed\n   assert(%s) was false\n   at %s:%i\n\n", testname, #x, __FILE__ ,__LINE__);return -1;}

char *testname = NULL;
int testsrun = 0, testspassed = 0;

typedef int (*test_t)();

STARTTEST(scale){
	struct image img;
	img.bufwidth = 1280*2;
	img.bufheight = 1024*2;
	img.buf = malloc(img.bufwidth*img.bufheight*4);
	memset(img.buf, 0x80, img.bufwidth*img.bufheight*3);
	char *to = malloc(1280*1024*4);
	STARTTIME{
		scale(&img, 1280, 1024, 1280*4, to);
	}ENDTIME
	free(to);
	free(img.buf);
}ENDTEST

STARTTEST(nearestscale){
	struct image img;
	img.bufwidth = 1280*2;
	img.bufheight = 1024*2;
	img.buf = malloc(img.bufwidth*img.bufheight*4);
	memset(img.buf, 0x80, img.bufwidth*img.bufheight*3);
	char *to = malloc(1280*1024*4);
	STARTTIME{
		nearestscale(&img, 1280, 1024, 1280*4, to);
	}ENDTIME
	free(to);
	free(img.buf);
}ENDTEST


STARTTEST(pixfmt_masks){
	struct pixfmt fmt;

	/* RGB565 */
	pixfmt_init(&fmt, 16, 0, 0xf800, 0x07e0, 0x001f);
	assert(fmt.rshift == 11 && fmt.rbits == 5);
	assert(fmt.gshift ==  5 && fmt.gbits == 6);
	assert(fmt.bshift ==  0 && fmt.bbits == 5);
	assert(!fmt.direct);

	/* RGB555 */
	pixfmt_init(&fmt, 16, 0, 0x7c00, 0x03e0, 0x001f);
	assert(fmt.rshift == 10 && fmt.rbits == 5);
	assert(fmt.gshift ==  5 && fmt.gbits == 5);
	assert(fmt.bshift ==  0 && fmt.bbits == 5);

	/* 10 bit deep colour */
	pixfmt_init(&fmt, 32, 0, 0x3ff00000, 0x000ffc00, 0x000003ff);
	assert(fmt.rshift == 20 && fmt.rbits == 10);
	assert(fmt.gshift == 10 && fmt.gbits == 10);
	assert(fmt.bshift ==  0 && fmt.bbits == 10);
	assert(!fmt.direct);

	/* what scale() writes natively */
	pixfmt_init(&fmt, 32, 0, 0xff0000, 0x00ff00, 0x0000ff);
	assert(fmt.direct);

	/* same masks but MSBFirst must not take the direct path */
	pixfmt_init(&fmt, 32, 1, 0xff0000, 0x00ff00, 0x0000ff);
	assert(!fmt.direct);
}ENDTEST

STARTTEST(pack_rgb565){
	struct pixfmt fmt;
	/* source is BGRX, as scale() writes it */
	unsigned char red[4]   = {0x00, 0x00, 0xff, 0x00};
	unsigned char blue[4]  = {0xff, 0x00, 0x00, 0x00};
	unsigned char white[4] = {0xff, 0xff, 0xff, 0x00};
	unsigned char black[4] = {0x00, 0x00, 0x00, 0x00};
	unsigned char dst[2];

	pixfmt_init(&fmt, 16, 0, 0xf800, 0x07e0, 0x001f);

	pack_rows(&fmt, red, (char *)dst, 1, 1, 4, 2);
	assert(dst[0] == 0x00 && dst[1] == 0xf8);	/* 0xf800 LSBFirst */

	pack_rows(&fmt, blue, (char *)dst, 1, 1, 4, 2);
	assert(dst[0] == 0x1f && dst[1] == 0x00);	/* 0x001f */

	pack_rows(&fmt, white, (char *)dst, 1, 1, 4, 2);
	assert(dst[0] == 0xff && dst[1] == 0xff);	/* saturates to all ones */

	pack_rows(&fmt, black, (char *)dst, 1, 1, 4, 2);
	assert(dst[0] == 0x00 && dst[1] == 0x00);
}ENDTEST

STARTTEST(pack_byteorder){
	struct pixfmt fmt;
	unsigned char red[4] = {0x00, 0x00, 0xff, 0x00};
	unsigned char dst[2];

	pixfmt_init(&fmt, 16, 1, 0xf800, 0x07e0, 0x001f);
	pack_rows(&fmt, red, (char *)dst, 1, 1, 4, 2);
	assert(dst[0] == 0xf8 && dst[1] == 0x00);	/* 0xf800 MSBFirst */
}ENDTEST

STARTTEST(pack_deepcolour){
	struct pixfmt fmt;
	unsigned char white[4] = {0xff, 0xff, 0xff, 0x00};
	unsigned char green[4] = {0x00, 0xff, 0x00, 0x00};
	unsigned char dst[4];

	pixfmt_init(&fmt, 32, 0, 0x3ff00000, 0x000ffc00, 0x000003ff);

	/* 0xff must widen to all ten bits, not to 0x0ff */
	pack_rows(&fmt, white, (char *)dst, 1, 1, 4, 4);
	assert(dst[0] == 0xff && dst[1] == 0xff && dst[2] == 0xff && dst[3] == 0x3f);

	pack_rows(&fmt, green, (char *)dst, 1, 1, 4, 4);
	assert(dst[0] == 0x00 && dst[1] == 0xfc && dst[2] == 0x0f && dst[3] == 0x00);
}ENDTEST

STARTTEST(pack_stride){
	struct pixfmt fmt;
	/* two rows of one pixel, source stride 8, dest stride 4 */
	unsigned char src[8] = {0x00,0x00,0xff,0x00, 0xff,0x00,0x00,0x00};
	unsigned char dst[4];

	pixfmt_init(&fmt, 16, 0, 0xf800, 0x07e0, 0x001f);
	dst[2] = dst[3] = 0xaa;
	pack_rows(&fmt, src, (char *)dst, 1, 2, 4, 2);
	assert(dst[0] == 0x00 && dst[1] == 0xf8);	/* row 0 red */
	assert(dst[2] == 0x1f && dst[3] == 0x00);	/* row 1 blue */
}ENDTEST


/*
 * The vga16fb i486 target: default visual is TrueColor at depth 4 with a
 * 2-1-1 bit layout, and depth 4 uses one byte per pixel. The server
 * advertises "preallocated pixels: black 0, white 15", which pins both
 * ends of our mapping.
 */
STARTTEST(pack_vga16){
	struct pixfmt fmt;
	unsigned char black[4] = {0x00, 0x00, 0x00, 0x00};
	unsigned char white[4] = {0xff, 0xff, 0xff, 0x00};
	unsigned char red[4]   = {0x00, 0x00, 0xff, 0x00};
	unsigned char green[4] = {0x00, 0xff, 0x00, 0x00};
	unsigned char blue[4]  = {0xff, 0x00, 0x00, 0x00};
	unsigned char out;

	pixfmt_init(&fmt, 8, 0, 0x3, 0x4, 0x8);
	assert(fmt.rshift == 0 && fmt.rbits == 2);
	assert(fmt.gshift == 2 && fmt.gbits == 1);
	assert(fmt.bshift == 3 && fmt.bbits == 1);
	assert(!fmt.direct);
	/* must take the mask path, not the unsupported-visual exit */
	assert(fmt.rmask | fmt.gmask | fmt.bmask);

	pack_rows(&fmt, black, (char *)&out, 1, 1, 4, 1);
	assert(out == 0);
	pack_rows(&fmt, white, (char *)&out, 1, 1, 4, 1);
	assert(out == 15);
	pack_rows(&fmt, red, (char *)&out, 1, 1, 4, 1);
	assert(out == 3);
	pack_rows(&fmt, green, (char *)&out, 1, 1, 4, 1);
	assert(out == 4);
	pack_rows(&fmt, blue, (char *)&out, 1, 1, 4, 1);
	assert(out == 8);

	/* nothing may escape the four planes */
	for(unsigned int i = 0; i < 256; i++){
		unsigned char px[4];
		px[0] = px[1] = px[2] = i; px[3] = 0;
		pack_rows(&fmt, px, (char *)&out, 1, 1, 4, 1);
		assert(out < 16);
	}
}ENDTEST

test_t tests[] = {
	test_scale,
	test_nearestscale,
	test_pixfmt_masks,
	test_pack_rgb565,
	test_pack_byteorder,
	test_pack_deepcolour,
	test_pack_stride,
	test_pack_vga16,
	NULL
};

void summary(){
	printf("%i tests run: %i passed  %i failed\n", testsrun, testspassed, testsrun - testspassed);
}

int main(){
	test_t *test = tests;
	for(; *test; test++)
		(*test)();
	summary();
	return 0;
}

