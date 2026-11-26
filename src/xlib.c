
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "meh.h"
#include "scale.h"
#include "convert.h"

static void maskbits(unsigned long mask, int *shift, int *bits){
	int sh = 0, b = 0;
	if(mask){
		while(!(mask & 1)){
			mask >>= 1;
			sh++;
		}
		while(mask & 1){
			mask >>= 1;
			b++;
		}
	}
	*shift = sh;
	*bits = b;
}

struct data_t{
	XImage *ximg;
	XShmSegmentInfo *shminfo;
	Pixmap shmpix;
};

/* Globals */
static Display *display;
static int screen;
static Window window;
static GC gc;

/* Chosen visual, which is not necessarily the default one. */
static Visual *visual;
static int visualdepth;
static Colormap colormap = None;
static int palettised = 0;
static unsigned long blackpixel = 0;
static unsigned char palettelut[LUTSIZE];
static int dither = -1;		/* -1 = decide from the format */

static int xfd;

static int xshm = 0;


/*
 * Score a visual by how well it can show a photograph.
 *
 * A TrueColor visual with at least 5 bits in every channel is ideal. One
 * with a starved channel is not: vga16fb advertises TrueColor at depth 4
 * with masks 0x3/0x4/0x8, which is two bits of red and one each of green
 * and blue. A PseudoColor visual of the same depth has sixteen freely
 * settable entries, which measures about two to three times better, so
 * prefer it. StaticColor comes next (fixed entries, but still chosen by
 * the server rather than by a bit split), then the grey classes.
 */
static int visualscore(XVisualInfo *vi){
	int rb, gb, bb, sh;

	switch(vi->class){
	case TrueColor:
	case DirectColor:
		maskbits(vi->red_mask, &sh, &rb);
		maskbits(vi->green_mask, &sh, &gb);
		maskbits(vi->blue_mask, &sh, &bb);
		if(rb >= 5 && gb >= 5 && bb >= 5)
			return 1000 + rb + gb + bb;
		return 100 + rb + gb + bb;
	case PseudoColor:
		return 500 + vi->colormap_size;
	case StaticColor:
		return 400 + vi->colormap_size;
	case GrayScale:
		return 300 + vi->colormap_size;
	case StaticGray:
		return 200 + vi->colormap_size;
	}
	return 0;
}

static void choosevisual(){
	XVisualInfo tmpl, *vi;
	int n, i, best = -1, bestscore = -1;

	visual = DefaultVisual(display, screen);
	visualdepth = DefaultDepth(display, screen);

	tmpl.screen = screen;
	tmpl.depth = visualdepth;
	if(!(vi = XGetVisualInfo(display, VisualScreenMask | VisualDepthMask, &tmpl, &n)))
		return;

	for(i = 0; i < n; i++){
		int score = visualscore(&vi[i]);
		if(score > bestscore){
			bestscore = score;
			best = i;
		}
	}

	if(best >= 0){
		visual = vi[best].visual;
		visualdepth = vi[best].depth;
		palettised = vi[best].class == PseudoColor || vi[best].class == StaticColor ||
			vi[best].class == GrayScale || vi[best].class == StaticGray;
	}

	XFree(vi);
}

/*
 * Sixteen entries: the eight corners of the RGB cube plus a grey ramp.
 * Measured against a fixed VGA palette and a few cube variants over four
 * test images, this had the lowest mean error.
 */
/*
 * Build an n entry palette: the largest r=g=b colour cube that fits in
 * about 85% of the entries, with the remainder spent on interior greys.
 *
 * That single rule lands on both known-good points. Sixteen entries give
 * a 2x2x2 cube plus eight greys, which measured best of five candidate
 * palettes over four test images; 256 entries give the classic 6x6x6
 * cube plus forty greys. Greys matter more than hues when entries are
 * scarce: a 3x3x2 cube with no grey ramp measured far worse at sixteen.
 */
static void defaultpalette(unsigned char *rgb, int n){
	int k, i, r, g, b, greys, j = 0;

	if(n > 256)
		n = 256;

	for(k = 2; (k + 1) * (k + 1) * (k + 1) <= n * 85 / 100; k++)
		;
	if(k * k * k > n)
		k = 0;

	for(r = 0; r < k; r++)
		for(g = 0; g < k; g++)
			for(b = 0; b < k; b++){
				rgb[j * 3 + 0] = r * 255 / (k - 1);
				rgb[j * 3 + 1] = g * 255 / (k - 1);
				rgb[j * 3 + 2] = b * 255 / (k - 1);
				j++;
			}

	greys = n - j;
	for(i = 0; i < greys; i++){
		const int v = k ? (i + 1) * 255 / (greys + 1) : i * 255 / (greys > 1 ? greys - 1 : 1);
		rgb[j * 3 + 0] = rgb[j * 3 + 1] = rgb[j * 3 + 2] = v;
		j++;
	}
}

/*
 * Install a colormap for the chosen visual and build the reverse lookup
 * the packer needs. Writable classes get our own palette, read-only ones
 * are read back as they are.
 */
static void greypalette(unsigned char *rgb, int n){
	int i;

	if(n > 256)
		n = 256;
	for(i = 0; i < n; i++){
		const int v = n > 1 ? i * 255 / (n - 1) : 0;
		rgb[i * 3 + 0] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v;
	}
}

static void setuppalette(int mapsize){
	unsigned char rgb[256 * 3];
	XColor cells[256];
	int i;
	int writable = 0, isgrey = 0;
	XVisualInfo tmpl, *vi;
	int n;

	if(mapsize > 256)
		mapsize = 256;
	if(mapsize < 2)
		mapsize = 2;

	tmpl.visualid = XVisualIDFromVisual(visual);
	if((vi = XGetVisualInfo(display, VisualIDMask, &tmpl, &n)) && n > 0){
		writable = vi[0].class == PseudoColor || vi[0].class == GrayScale;
		isgrey = vi[0].class == GrayScale || vi[0].class == StaticGray;
	}
	if(vi)
		XFree(vi);

	colormap = XCreateColormap(display, DefaultRootWindow(display), visual,
			writable ? AllocAll : AllocNone);

	if(writable){
		if(isgrey)
			greypalette(rgb, mapsize);
		else
			defaultpalette(rgb, mapsize);
		for(i = 0; i < mapsize; i++){
			cells[i].pixel = i;
			cells[i].red   = rgb[i * 3 + 0] * 257;
			cells[i].green = rgb[i * 3 + 1] * 257;
			cells[i].blue  = rgb[i * 3 + 2] * 257;
			cells[i].flags = DoRed | DoGreen | DoBlue;
		}
		XStoreColors(display, colormap, cells, mapsize);
	}else{
		for(i = 0; i < mapsize; i++)
			cells[i].pixel = i;
		XQueryColors(display, colormap, cells, mapsize);
		for(i = 0; i < mapsize; i++){
			rgb[i * 3 + 0] = cells[i].red >> 8;
			rgb[i * 3 + 1] = cells[i].green >> 8;
			rgb[i * 3 + 2] = cells[i].blue >> 8;
		}
	}

	palette_lut(palettelut, rgb, mapsize);
	blackpixel = palettelut[0];	/* r5g5b5 index 0 is black */
}

/*
 * Grow-only scratch buffer holding the 32bpp BGRX image scale() produces
 * when the server's format needs a conversion pass. Never shrinks, so a
 * resize storm does not thrash the allocator.
 */
static char *scratch = NULL;
static size_t scratchsize = 0;

static char *getscratch(size_t size){
	if(size > scratchsize){
		free(scratch);
		if(!(scratch = malloc(size))){
			fprintf(stderr, "meh: out of memory\n");
			exit(1);
		}
		scratchsize = size;
	}
	return scratch;
}

static void ximage(struct data_t *data, struct image *img, unsigned int width, unsigned int height, int fast){
	int depth;
	XImage *ximg = NULL;
	XShmSegmentInfo *shminfo = NULL;
	struct pixfmt fmt;

	depth = visualdepth;

	if(xshm){
		shminfo = malloc(sizeof(XShmSegmentInfo));
		if(!(ximg = XShmCreateImage(display,
			visual, depth,
			ZPixmap,
			NULL,
			shminfo,
			width, height
		))){
			fprintf(stderr, "XShm problems\n");
			exit(1);
		}
		if((shminfo->shmid = shmget(IPC_PRIVATE, ximg->bytes_per_line * ximg->height, IPC_CREAT|0777)) == -1){
			fprintf(stderr, "XShm problems\n");
			exit(1);
		}
		if((shminfo->shmaddr = ximg->data = shmat(shminfo->shmid, 0, 0)) == (void *)-1){
			fprintf(stderr, "XShm problems\n");
			exit(1);
		}
		shminfo->readOnly = False;
		if(!XShmAttach(display, shminfo)){
			fprintf(stderr, "XShm problems, falling back to to XImage\n");
			xshm = 0;
		}
		if(xshm & (1 << 1)){
			data->shmpix =
				XShmCreatePixmap(display, window,
						 ximg->data, shminfo,
						 width, height, depth);
		}
	}else{
		ximg = XCreateImage(display, 
			visual, depth, 
			ZPixmap, 0, 
			NULL,
			width, height,
			32, 0
		);
		ximg->data = malloc(ximg->bytes_per_line * ximg->height);
		XInitImage(ximg);
	}

	pixfmt_init(&fmt, ximg->bits_per_pixel, ximg->byte_order == MSBFirst,
			ximg->red_mask, ximg->green_mask, ximg->blue_mask);

	if(palettised){
		if(ximg->bits_per_pixel != 8){
			fprintf(stderr, "meh: palettised visual at %d bits per pixel is not supported.\n",
					ximg->bits_per_pixel);
			exit(1);
		}
		pixfmt_setpalette(&fmt, palettelut);
	}
	/*
	 * Only ever dither a format coarse enough to band. Forcing it on a
	 * 24 bit display would cost the direct path for a plus or minus one
	 * perturbation, which is worse than useless.
	 */
	pixfmt_setdither(&fmt, (dither != 0) && pixfmt_iscoarse(&fmt));

	if(fmt.direct){
		/* The server wants exactly what scale() writes. */
		(fast ? nearestscale : scale)(img, ximg->width, ximg->height, ximg->bytes_per_line, ximg->data);
	}else if(fmt.lut || (fmt.rmask | fmt.gmask | fmt.bmask)){
		/* Scale into a 32bpp buffer, then convert it a row at a time. */
		unsigned int stride = ximg->width * 4;
		char *buf = getscratch((size_t)stride * ximg->height);

		(fast ? nearestscale : scale)(img, ximg->width, ximg->height, stride, buf);
		pack_rows(&fmt, (const unsigned char *)buf, ximg->data,
				ximg->width, ximg->height, stride, ximg->bytes_per_line);
	}else{
		fprintf(stderr, "meh: unsupported visual: depth %d, %d bits per pixel, no RGB masks.\n",
				depth, ximg->bits_per_pixel);
		fprintf(stderr, "meh: no usable visual was found for this screen.\n");
		exit(1);
	}

	data->ximg = ximg;
	data->shminfo = shminfo;
}

void backend_prepare(struct image *img, unsigned int width, unsigned int height, int fast){
	struct data_t *data = img->backend = malloc(sizeof (struct data_t));
	if(width * img->bufheight > height * img->bufwidth){
		ximage(data, img, img->bufwidth * height / img->bufheight, height, fast);
	}else{
		ximage(data, img, width, img->bufheight * width / img->bufwidth, fast);
	}

	img->state |= SCALED;
	if(!fast)
		img->state |= SLOWSCALED;
}

void backend_draw(struct image *img, unsigned int width, unsigned int height){
	struct data_t *data = img->backend;
	assert(data);
	assert(data->ximg);

	XImage *ximg = data->ximg;

	XRectangle rects[2];
	int yoffset, xoffset;
	xoffset = (width - ximg->width) / 2;
	yoffset = (height - ximg->height) / 2;
	if(xoffset || yoffset){
		rects[0].x = rects[0].y = 0;
		if(xoffset){
			rects[0].width = rects[1].width = xoffset;
			rects[0].height = rects[1].height = height;
			rects[1].x = xoffset + ximg->width;
			rects[1].y = 0;
			rects[1].width = width - rects[1].x;
		}else if(yoffset){
			rects[0].width = rects[1].width = width;
			rects[0].height = yoffset;
			rects[1].x = 0;
			rects[1].y = yoffset + ximg->height;
			rects[1].height = height - rects[1].y;
		}
		XFillRectangles(display, window, gc, rects, 2);
	}
	if(data->shmpix){
		XCopyArea(display, data->shmpix, window, gc,
			  0, 0,
			  ximg->width, ximg->height,
			  xoffset, yoffset);
	} else if(xshm){
		XShmPutImage(display, window, gc, ximg, 0, 0, xoffset, yoffset, ximg->width, ximg->height, False);
	}else{
		XPutImage(display, window, gc, ximg, 0, 0, xoffset, yoffset, ximg->width, ximg->height);
	}
	XFlush(display);
}

void backend_free(struct image *img){
	assert(img);
	if(img->backend){
		struct data_t *data = (struct data_t *)img->backend;
		XImage *ximg = data->ximg;
		if (data->shmpix)
			XFreePixmap(display, data->shmpix);
		if(ximg){
			if(xshm && data->shminfo){
				XShmDetach(display, data->shminfo);
				XDestroyImage(ximg);
				shmdt(data->shminfo->shmaddr);
				shmctl(data->shminfo->shmid, IPC_RMID, 0);
				free(data->shminfo);
			}else{
				XDestroyImage(ximg);
			}
		}
		img->backend = NULL;
	}
}


void backend_setaspect(unsigned int w, unsigned int h){
	XSizeHints *hints = XAllocSizeHints();
	//hints->flags = PAspect;
	hints->flags = 0;
	hints->min_aspect.x = hints->max_aspect.x = w;
	hints->min_aspect.y = hints->max_aspect.y = h;
	XSetWMNormalHints(display, window, hints);
	XFlush(display);
	XFree(hints);
}

/* Alt-F4 silent. Keeps people happy */
static int xquit(Display *d){
	(void)d;
	exit(EXIT_SUCCESS);
	return 0;
}

static int _x_error_occurred;

static int
_check_error_handler(Display     *display,
		     XErrorEvent *event)
{
    _x_error_occurred = 1;
    return False; /* ignored */
}

static int
can_use_shm(Display *dpy)
{
    XShmSegmentInfo info;
    int (*old_handler)(Display *display, XErrorEvent *event);
    Status success;
    int major, minor, has_pixmap;

    if (!XShmQueryExtension(dpy))
	return 0;

    XShmQueryVersion(dpy, &major, &minor, &has_pixmap);

    info.shmid = shmget(IPC_PRIVATE, 0x1000, IPC_CREAT | 0600);
    if (info.shmid == -1)
	return 0;

    info.readOnly = 0;
    info.shmaddr = shmat(info.shmid, NULL, 0);
    if (info.shmaddr == (char *) -1) {
	shmctl(info.shmid, IPC_RMID, NULL);
	return 0;
    }

    XLockDisplay (dpy);
    XSync(dpy, False);
    old_handler = XSetErrorHandler (_check_error_handler);

    _x_error_occurred = 0;
    success = XShmAttach(dpy, &info);
    if (success)
	XShmDetach(dpy, &info);

    XSync(dpy, False);
    XSetErrorHandler(old_handler);
    XUnlockDisplay(dpy);

    shmctl(info.shmid, IPC_RMID, NULL);
    shmdt(info.shmaddr);

    if (!success || _x_error_occurred)
	    return 0;

    return !!has_pixmap << 1 | 1;
}

void backend_init(){
	XGCValues gcv;

	display = XOpenDisplay(NULL);
	if(!display){
		fprintf(stderr, "Can't open X display.\n");
		exit(EXIT_FAILURE);
	}

	xfd = ConnectionNumber(display);
	screen = DefaultScreen(display);

	choosevisual();

	{
		const char *e = getenv("MEH_DITHER");
		if(e)
			dither = *e != '0';
	}

	if(palettised || visual != DefaultVisual(display, screen)){
		XSetWindowAttributes attr;
		XVisualInfo tmpl, *vi;
		int n, mapsize = 1 << visualdepth;

		tmpl.visualid = XVisualIDFromVisual(visual);
		if((vi = XGetVisualInfo(display, VisualIDMask, &tmpl, &n)) && n > 0)
			mapsize = vi[0].colormap_size;
		if(vi)
			XFree(vi);

		setuppalette(mapsize);

		/*
		 * A window whose visual differs from its parent's must be
		 * given its own colormap and an explicit border pixel, or
		 * XCreateWindow fails with BadMatch.
		 */
		attr.colormap = colormap;
		attr.background_pixel = blackpixel;
		attr.border_pixel = blackpixel;
		window = XCreateWindow(display, DefaultRootWindow(display), 0, 0, 640, 480, 0,
				visualdepth, InputOutput, visual,
				CWColormap | CWBackPixel | CWBorderPixel, &attr);
	}else{
		window = XCreateWindow(display, DefaultRootWindow(display), 0, 0, 640, 480, 0,
				visualdepth, InputOutput, visual, 0, NULL);
	}
	backend_setaspect(1, 1);

	gcv.graphics_exposures = False;
	gc = XCreateGC(display, window, GCGraphicsExposures, &gcv);
	/* the letterbox bars, which must be black in the chosen colormap */
	XSetForeground(display, gc, blackpixel);

	XStoreName(display, window, "meh");

	XSelectInput(display, window, StructureNotifyMask | ExposureMask | KeyPressMask | ButtonPressMask);
	XMapRaised(display, window);
	if(colormap != None){
		XSetWindowColormap(display, window, colormap);
		/*
		 * Servers of this vintage often run without a window manager
		 * to install the colormap for us, and this one allows only a
		 * single installed map anyway.
		 */
		XInstallColormap(display, colormap);
	}
	XFlush(display);
	XSetIOErrorHandler(xquit);
	XFlush(display);

	xshm = can_use_shm(display);
}

void handlekeypress(XEvent *event){
	KeySym key = XLookupKeysym(&event->xkey, 0);
	switch(key){
		case XK_Escape:
		case XK_q:
			meh_quit();
			break;
		case XK_Return:
			key_action();
			break;
		case XK_j:
		case XK_l:
		case XK_Right:
		case XK_Down:
			key_next();
			break;
		case XK_k:
		case XK_h:
		case XK_Left:
		case XK_Up:
			key_prev();
			break;
		case XK_r:
			key_reload();
			break;
		case XK_0:
			if(curimg)
				XResizeWindow(display, window, curimg->bufwidth, curimg->bufheight);
			break;
		default:
			key_default(XKeysymToString(key));
	}
}

void handlebuttonpress(XEvent *event){
	XButtonPressedEvent *ev = &event->xbutton;
	switch(ev->button){
		case Button4:
			key_prev();
			break;
		case Button5:
			key_next();
			break;
	}
}

void handleevent(XEvent *event){
	Atom closed = XInternAtom(display, "WM_DELETE_WINDOW", True);
	XSetWMProtocols(display, window, &closed, 1);

	switch(event->type){
		/* Might not get ConfigureNotify, for example if there's no window manager */
		case ClientMessage:
			meh_quit();
			break;
		case MapNotify:
			if (!width || !height)
			{
				XWindowAttributes attr;
				XGetWindowAttributes(event->xmap.display, event->xmap.window, &attr);
				width = attr.width;
				height = attr.height;
			}
			break;
		case ConfigureNotify:
			if(width != event->xconfigure.width || height != event->xconfigure.height){
				width = event->xconfigure.width;
				height = event->xconfigure.height;
				if(curimg){
					backend_free(curimg);
					curimg->state &= ~(DRAWN | SCALED | SLOWSCALED);

					/* Some window managers need reminding */
					backend_setaspect(curimg->bufwidth, curimg->bufheight);
				}
			}
			break;
		case Expose:
			if(curimg){
				curimg->state &= ~DRAWN;
			}
			break;
		case KeyPress:
			handlekeypress(event);
			break;
		case ButtonPress:
			handlebuttonpress(event);
			break;
	}
}

void backend_run(){
	fd_set fds;
	struct timeval tv0 = {0, 0};
	struct timeval *tv = &tv0;

	for(;;){
		int maxfd = setup_fds(&fds);
		FD_SET(xfd, &fds);
		if(xfd > maxfd)
			maxfd = xfd;
		int ret = select(maxfd+1, &fds, NULL, NULL, tv);
		if(ret == -1){
			perror("select failed\n");
		}
		if(process_fds(&fds)){
			tv = &tv0;
		}
		if(XPending(display)){
			tv = &tv0;
			XEvent event;
			do{
				XNextEvent(display, &event);
				handleevent(&event);
			}while(XPending(display));
		}else if(ret == 0){
			if(!process_idle()){
				/* If we get here everything has been drawn in full or we need more input to continue */
				tv = NULL;
			}
		}
	}
}


