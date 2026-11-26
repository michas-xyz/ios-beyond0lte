/*
 * CPU scanout bridge for the S10+ firmware-provided framebuffer. Physical
 * range, width, height and stride come from the loader/PE console, not A5.
 * AppleCLCD matches the samsung,beyond2lte-display arm-io nub.
 *
 * iOS 26.6.2 IOMobileFramebuffer client contract inspected locally:
 *   3 default IOSurface port; 4 swap id; 5 swap record (0x588 bytes)
 *   record +0x98 swap id, +0x9c surface IDs, +0xac source rects,
 *   +0x10c destination rects, +0x14c updated-layer mask, +0x150 disabled-layer mask (four layers).
 *   6 wait; 7 display id; 8 dimensions; 18 main; 28 dot pitch;
 *   72 notification control. Unsupported selectors return Unsupported.
 *
 * BGRA layers are copied without resizing. A 60 Hz software timer supplies
 * notification type 5, not hardware scanout synchronization. After the first
 * accepted copy only console drawing is suppressed; persistent logging and
 * panic drawing remain active. GPU acceleration is not provided.
 */
#include "B2LSurface.h"
#include "B2LPerf.h"
#include "B2LBlit.h"

#if defined(B2L)
#include <IOKit/IOService.h>
#include <arm64/b2l_diag.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>
#include <arm64/b2l_early.h>
extern "C" void b2l_prof_start(void);   /* B1L-0113: osfmk/arm64/b2l_prof.c */

enum {
	kB2LFBGetLayerDefaultSurface  = 3,
	kB2LFBSwapBegin               = 4,
	kB2LFBSwapEnd                 = 5,
	kB2LFBSwapWait                = 6,
	kB2LFBGetID                   = 7,
	kB2LFBGetDisplaySize          = 8,
	kB2LFBIsMainDisplay           = 18,
	kB2LFBGetDotPitch             = 28,
	kB2LFBNotifications           = 72,
	kB2LFBFrameInfoDescriptor     = 74,
	kB2LFBSetContrast             = 84,
	kB2LFBSelectDisplay           = 88,
};

#define B2L_FB_SWAP_REC_SIZE          0x588
#define B2L_FB_LAYERS                 4
#define B2L_FB_NOTIFY_TYPES           8
#define B2L_FB_NOTIFY_VSYNC           5
#define B2L_FB_VSYNC_US               16667
#if defined(B2L_BOARD_BEYOND0LTE) || defined(TARGET_BEYOND0LTE)
#define B2L_FB_PANEL_WIDTH            1080
#define B2L_FB_PANEL_HEIGHT           2280
#define B2L_FB_DOT_PITCH              438     /* Galaxy S10e (beyond0lte) */
#elif defined(B2L_BOARD_BEYOND1LTE)
#define B2L_FB_PANEL_WIDTH            1440
#define B2L_FB_PANEL_HEIGHT           3040
#define B2L_FB_DOT_PITCH              550     /* Galaxy S10 (beyond1lte) */
#else
#define B2L_FB_PANEL_WIDTH            1440
#define B2L_FB_PANEL_HEIGHT           3040
#define B2L_FB_DOT_PITCH              522     /* Galaxy S10+ (beyond2lte) */
#endif
#define B2L_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	/* Size reported to userland. b2l_fb_lw/b2l_fb_lh select a smaller CPU
	 * canvas that swap() scales to the panel; otherwise it is the panel. */
	uint32_t      fLogicalW, fLogicalH;
	bool          fScaled;
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	B2LSurface *fSurface;         /* the panel memory as an IOSurface */
	B2LSurface *fClientSurface;   /* B1L-0097: the default surface handed out: ordinary memory, never the panel's */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[B2L_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[B2L_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

/* B1L-0096: the zero-copy display, below */
static void b2l_zc_init(uint64_t panel);
static bool b2l_zc_show(uint64_t phys, uint32_t bytesPerRow);
static bool b2l_zc_active(void);
bool HakiosMaliDrawn(B2LSurface *surface, uint64_t milliseconds);

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
	uint64_t mapped_bytes=0;
    if(!b2l_early_range(&fPhys,&mapped_bytes))return false;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (fWidth == 0 || fHeight == 0) {
		fWidth = B2L_FB_PANEL_WIDTH;
		fHeight = B2L_FB_PANEL_HEIGHT;
		if (fRowBytes < fWidth * 4) {
			fRowBytes = fWidth * 4;
		}
	}
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fWidth>8192 || fHeight>8192 || fRowBytes < fWidth * 4 || (uint64_t)fRowBytes*fHeight>mapped_bytes) {
		IOLog("B2L FB: no boot framebuffer\n");
		return false;
	}
	fLogicalW = fWidth;
	fLogicalH = fHeight;
	fScaled = false;
	uint32_t lw = 0, lh = 0;
	if (PE_parse_boot_argn("b2l_fb_lw", &lw, sizeof(lw)) &&
	    PE_parse_boot_argn("b2l_fb_lh", &lh, sizeof(lh))) {
		/* At most 2x downscaling, and never larger than the panel. */
		if (lw >= fWidth / 2 && lw <= fWidth && lh >= fHeight / 2 && lh <= fHeight) {
			fLogicalW = lw;
			fLogicalH = lh;
			fScaled = lw != fWidth || lh != fHeight;
		} else {
			IOLog("B2L FB: ignoring logical size %ux%u for %ux%u panel\n", lw, lh, fWidth, fHeight);
		}
	}
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	/* The default surface must have the presented size: a mismatch leaves
	 * every swap layer empty. Scaled, it is the panel's top-left part. */
	fSurface = b2l_surface_create_physical(fPhys, fLogicalW, fLogicalH, fRowBytes, B2L_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	if (fSurface != NULL && !fScaled) {   /* B1L-0096 */
		b2l_zc_init(fPhys);
		b2l_surface_set_scanout_shape(fWidth, fHeight, fRowBytes, b2l_zc_active());
	}
	/* B1L-0097: backboardd drew into the panel memory itself when given it as the default surface -- every element seen
	 * appearing one after the other. It gets ordinary memory of the same shape, shown like any other surface.
	 * Mali/Panfrost requires 64-byte row alignment to import an IOSurface directly as a render target; on 1080px-wide
	 * panels (4320 bytes/row), unaligned rows fall back to a read-only live copy so the scanout surface stays black. */
	uint32_t clientRowBytes = !fScaled ? ((fRowBytes + 63u) & ~63u) : fRowBytes;
	fClientSurface = fSurface != NULL ? b2l_surface_create_memory(fLogicalW, fLogicalH, clientRowBytes, B2L_FB_FORMAT_BGRA,
	    b2l_zc_active()) : NULL;
	IOLog("B2L FB: default surface for clients: %s (id %u, scan-out %#llx)\n",
	    fClientSurface != NULL ? "its own memory" : "the panel memory",
	    fClientSurface != NULL ? fClientSurface->id : (fSurface != NULL ? fSurface->id : 0),
	    fClientSurface != NULL ? (unsigned long long)fClientSurface->scanoutPhys : 0ULL);
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("B2L FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("B2L FB: no vsync timer\n");
		return false;
	}
	fVsync->setTimeoutUS(B2L_FB_VSYNC_US);

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	registerService();
	b2l_prof_start();   /* B1L-0113: backboardd sampler */
	IOLog("B2L FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, logical %ux%u%s\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH,
	    fScaled ? " (scaled)" : "");
	return true;
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(B2L_FB_NOTIFY_VSYNC, values, 6);
	}
	IOLockUnlock(fb->fLock);
	sender->setTimeoutUS(B2L_FB_VSYNC_US);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t b2l_fb_swaps_logged;

/* Copy each enabled layer's surface into the panel memory, scaling the
 * logical canvas to the panel when a smaller logical size is selected. */
void
AppleCLCD::swap(const uint8_t *rec)
{
	/* kern_SwapSetLayer stores client+0x164/+0x168; kern_SwapEnd sends
	 * client+0x18. client+0x194 (wire+0x17c) is unrelated and cleared by
	 * SwapBegin. Reading it silently discarded every native CPU frame. */
	uint32_t mask = get32(rec, 0x14c);
	uint32_t disabled = get32(rec, 0x150);
	static unsigned records;
	if (__atomic_fetch_add(&records, 1, __ATOMIC_RELAXED) < 32) {
		b2l_diag_record("B2L FB RECORD: swap=%u updated=0x%x disabled=0x%x ids=%u,%u,%u,%u\n",
		    get32(rec, 0x98), mask, disabled, get32(rec, 0x9c),
		    get32(rec, 0xa0), get32(rec, 0xa4), get32(rec, 0xa8));
	}

	for (uint32_t layer = 0; layer < B2L_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x9c + 4 * layer);
		const uint8_t *a = rec + 0xac + 16 * layer;
		const uint8_t *b = rec + 0x10c + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || (disabled & (1u << layer)) || id == 0) {
			continue;
		}
		B2LSurface *s = b2l_surface_copy(id);
		if (b2l_fb_swaps_logged < 16) {
			b2l_fb_swaps_logged++;
			IOLog("B2L FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x98), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL) {
			continue;
		}
		/* B1L-0096: what the GPU draws into it, finished before it is shown (nothing waited for it until now:
		 * frames were shown half drawn) */
		(void)HakiosMaliDrawn(s, 100);
		{
			uint64_t phys = s == fSurface ? fPhys : s->scanoutPhys;
			bool whole = mask == (1u << layer) && sx == 0 && sy == 0 && dx == 0 && dy == 0 &&
			    (w <= 0 || (uint32_t)w == fWidth) && (h <= 0 || (uint32_t)h == fHeight) &&
			    s->width == fWidth && s->height == fHeight && s->bytesPerRow >= fRowBytes &&
			    (s->bytesPerRow & 3) == 0;
			if (phys != 0 && whole && !fScaled && b2l_zc_show(phys, s->bytesPerRow)) {
				if (!fConsolePaused) {
					b2l_early_gui_present();
					fConsolePaused = true;
					IOLog("B2L FB: first frame shown by the display itself (zero-copy)\n");
				}
				s->release();
				continue;
			}
		}
		if (s == fSurface) {
			OSSafeReleaseNULL(s);
			continue;
		}
		(void)b2l_zc_show(fPhys, fRowBytes);   /* a copy goes to the panel memory: the display reads that again */
		uintptr_t src = s->kernelAddress();
        if(src && s->memory && s->bytesPerElement==4 && s->elementWidth==1 && s->elementHeight==1 &&
            s->pixelFormat==B2L_FB_FORMAT_BGRA) {
            if(w<=0||h<=0){w=(int32_t)s->width;h=(int32_t)s->height;}
            IOLockLock(fLock);
            uint64_t b2l_t0 = mach_absolute_time();   /* B1L-0095: what the copy to the panel costs */
            int shown=fScaled?
                b2l_blit_scaled((void*)fPixels,(uint64_t)fRowBytes*fHeight,fWidth,fHeight,fRowBytes,
                (const void*)src,s->memory->getLength(),s->width,s->height,s->bytesPerRow,s->offset,
                fLogicalW,fLogicalH,sx,sy,dx,dy,w,h):
                b2l_blit((void*)fPixels,(uint64_t)fRowBytes*fHeight,fWidth,fHeight,fRowBytes,
                (const void*)src,s->memory->getLength(),s->width,s->height,s->bytesPerRow,s->offset,
                sx,sy,dx,dy,w,h);
            if(shown&&!fConsolePaused){b2l_early_gui_present();fConsolePaused=true;IOLog("B2L FB: first IOSurface frame copied to physical scanout\n");}
            __asm__ volatile("dsb sy":::"memory");
            b2l_perf_note(B2L_PERF_BLIT, mach_absolute_time() - b2l_t0, (uint64_t)fRowBytes * (uint64_t)fHeight);
            IOLockUnlock(fLock);
        }

		s->release();
	}
}

/* B1L-0095: B2LPerf.h */
static struct { uint64_t count, abs, bytes; } b2l_perf[B2L_PERF_KINDS];
static struct { uint64_t count, abs; } b2l_calls[B2L_CALL_KINDS][128];   /* B1L-0102 */

extern "C" void
b2l_perf_call(int kind, uint32_t selector, uint64_t absolute_time)
{
	if (kind < 0 || kind >= B2L_CALL_KINDS) {
		return;
	}
	selector &= 127;
	__atomic_fetch_add(&b2l_calls[kind][selector].count, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&b2l_calls[kind][selector].abs, absolute_time, __ATOMIC_RELAXED);
}

static void
b2l_perf_calls_say(void)
{
	static const char *const name[B2L_CALL_KINDS] = { "mali", "surf", "fb" };
	struct { int k; unsigned s; uint64_t n, ms; } top[8];
	memset(top, 0, sizeof(top));
	for (int k = 0; k < B2L_CALL_KINDS; k++) {
		for (unsigned sel = 0; sel < 128; sel++) {
			uint64_t n = __atomic_exchange_n(&b2l_calls[k][sel].count, 0, __ATOMIC_RELAXED);
			uint64_t abs = __atomic_exchange_n(&b2l_calls[k][sel].abs, 0, __ATOMIC_RELAXED), ns;
			if (n == 0) {
				continue;
			}
			absolutetime_to_nanoseconds(abs, &ns);
			unsigned at = 8;
			while (at > 0 && top[at - 1].ms < ns / 1000000) at--;
			if (at >= 8) continue;
			memmove(&top[at + 1], &top[at], (7 - at) * sizeof(top[0]));
			top[at].k = k; top[at].s = sel; top[at].n = n; top[at].ms = ns / 1000000;
		}
	}
	char line[400];
	int at = snprintf(line, sizeof(line), "B2L PERF CALLS:");
	for (int i = 0; i < 8 && top[i].n && at < (int)sizeof(line) - 48; i++) {
		at += snprintf(line + at, sizeof(line) - at, "%s %s %u: %llu x %llu ms", i ? " |" : "", name[top[i].k],
		    top[i].s, top[i].n, top[i].ms);
	}
	IOLog("%s\n", line);
}
static uint64_t b2l_perf_since;
static IOSimpleLock *b2l_perf_lock;

extern "C" void
b2l_perf_note(int kind, uint64_t absolute_time, uint64_t bytes)
{
	if (kind < 0 || kind >= B2L_PERF_KINDS) {
		return;
	}
	if (b2l_perf_lock == NULL) {
		IOSimpleLock *l = IOSimpleLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, l, (void * volatile *)&b2l_perf_lock)) {
			IOSimpleLockFree(l);
		}
	}
	uint64_t now = mach_absolute_time(), ns, o[B2L_PERF_KINDS][3];
	bool say = false;
	IOSimpleLockLock(b2l_perf_lock);
	b2l_perf[kind].count++;
	b2l_perf[kind].abs += absolute_time;
	b2l_perf[kind].bytes += bytes;
	if (b2l_perf_since == 0) {
		b2l_perf_since = now;
	}
	absolutetime_to_nanoseconds(now - b2l_perf_since, &ns);
	static int b2l_perf_on = -1;   /* B1L-0110: said only with boot-arg b2l_perf=1 */
	if (b2l_perf_on < 0) {
		uint32_t v = 0;
		b2l_perf_on = PE_parse_boot_argn("b2l_perf", &v, sizeof(v)) && v == 1;
	}
	if (ns >= 10ull * NSEC_PER_SEC && b2l_perf_on) {
		for (int k = 0; k < B2L_PERF_KINDS; k++) {
			absolutetime_to_nanoseconds(b2l_perf[k].abs, &o[k][1]);
			o[k][0] = b2l_perf[k].count;
			o[k][2] = b2l_perf[k].bytes;
		}
		bzero(b2l_perf, sizeof(b2l_perf));
		b2l_perf_since = now;
		say = true;
	}
	IOSimpleLockUnlock(b2l_perf_lock);
	if (say) {
		IOLog("B2L PERF: %llu s: blit %llu x, %llu ms, %llu MB | flush %llu x, %llu ms, %llu MB | submit %llu x, %llu ms"
		    " | flip %llu x, %llu ms\n",
		    ns / NSEC_PER_SEC, o[0][0], o[0][1] / 1000000, o[0][2] >> 20, o[1][0], o[1][1] / 1000000, o[1][2] >> 20,
		    o[2][0], o[2][1] / 1000000, o[3][0], o[3][1] / 1000000);
		b2l_perf_calls_say();   /* B1L-0102 */
	}
}

/*
 * B1L-0096: THE DISPLAY SHOWS THE GPU'S SURFACE ITSELF -- no copy by the processor. The Exynos 9820's display (DECON-F
 * at 0x19030000, command mode with the panel's TE as hardware trigger, left running by the bootloader) reads its
 * picture through one of six DMA channels (IDMA blocks at 0x19071000 + 0x1000 * n), whose IDMA_IN_BASE_ADDR_Y (+0x40)
 * holds the picture's physical address: 0xCA000000, the panel memory, at boot. A swap of a surface the display can
 * read (the panel memory itself, or one of backboardd's contiguous surfaces, B2LSurface.cpp) writes that address and
 * asks DECON to take it at the next frame (SHADOW_REG_UPDATE_REQ +0x60: global and window bits, which clear once
 * taken), and waits for that -- double buffering, at the panel's pace. Register layout from Samsung's dpu20/cal_9820
 * (LineageOS android_kernel_samsung_exynos9820, regs-dpp.h and regs-decon.h). Nothing is assumed about which channel:
 * the one holding the panel memory's address is found, and every register is said in the log once. A request DECON
 * does not take within 50 ms turns this off for good and the copy is used again. Boot-arg b2l_fb_zc=0: off.
 */
static volatile uint32_t *b2l_zc_decon, *b2l_zc_dma;
static IOMemoryMap *b2l_zc_decon_map, *b2l_zc_dma_map;
static volatile uint32_t *b2l_zc_base;   /* B1L-0097: the base register found holding the panel memory */
static uint64_t b2l_zc_base_pa;
static bool b2l_zc_on;
static uint64_t b2l_zc_panel, b2l_zc_shown;
static uint32_t b2l_zc_src_size, b2l_zc_shown_bpr, b2l_zc_panel_bpr;   /* B1L-0098: IDMA_SRC_SIZE (width = row length in pixels) */
static unsigned b2l_zc_flips;
/* B1L-0113: read by the backboardd sampler (osfmk/arm64/b2l_prof.c) */
static unsigned b2l_fb_swaps;
extern "C" unsigned b2l_fb_swap_count(void) { return __atomic_load_n(&b2l_fb_swaps, __ATOMIC_RELAXED); }
extern "C" unsigned b2l_fb_zc_flip_count(void) { return __atomic_load_n(&b2l_zc_flips, __ATOMIC_RELAXED); }

#define B2L_DECON_PA            0x19030000ULL
#define B2L_DMA_PA              0x19070000ULL
#define B2L_DECON_UPDATE_REQ    (0x0060 / 4)
#define B2L_DECON_UPDATE_BITS   ((1u << 31) | 0x3fu)
#define B2L_IDMA(ch, off)       (b2l_zc_dma[(0x1000 * ((ch) + 1) + (off)) / 4])

static volatile uint32_t *
b2l_zc_map(uint64_t pa, uint32_t size, IOMemoryMap **out)
{
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(pa, size, kIODirectionInOut);
	if (md == NULL) {
		return NULL;
	}
	*out = md->map(kIOMapInhibitCache);
	md->release();
	return *out != NULL ? (volatile uint32_t *)(*out)->getVirtualAddress() : NULL;
}

static void
b2l_zc_init(uint64_t panel)
{
	uint32_t want = 1;
	PE_parse_boot_argn("b2l_fb_zc", &want, sizeof(want));
	b2l_zc_panel = panel;
	b2l_zc_decon = b2l_zc_map(B2L_DECON_PA, 0x10000, &b2l_zc_decon_map);
	b2l_zc_dma = b2l_zc_map(B2L_DMA_PA, 0x8000, &b2l_zc_dma_map);
	if (b2l_zc_decon == NULL || b2l_zc_dma == NULL) {
		IOLog("B2L ZC: the display's registers could not be mapped: copy\n");
		return;
	}
	IOLog("B2L ZC: DECON 0x00 %08x %08x %08x %08x | 0x60 %08x | 0x70 %08x | 0x214 %08x 0x218 %08x 0x230 %08x\n",
	    b2l_zc_decon[0], b2l_zc_decon[1], b2l_zc_decon[2], b2l_zc_decon[3], b2l_zc_decon[0x60 / 4],
	    b2l_zc_decon[0x70 / 4], b2l_zc_decon[0x214 / 4], b2l_zc_decon[0x218 / 4], b2l_zc_decon[0x230 / 4]);
	for (int w = 0; w < 6; w++) {
		IOLog("B2L ZC: window %d control %08x %08x start %08x end %08x\n", w,
		    b2l_zc_decon[(0x1000 + 0x30 * w) / 4], b2l_zc_decon[(0x1004 + 0x30 * w) / 4],
		    b2l_zc_decon[(0x1008 + 0x30 * w) / 4], b2l_zc_decon[(0x100c + 0x30 * w) / 4]);
	}
	/* B1L-0097: the IDMA blocks at 0x19071000.. read 0 at B1L-0096 -- they are not the ones scanning out here. The
	 * panel memory's address is looked for in the whole of the display's register space instead: DPP 0x19020000,
	 * DECON 0x19030000, DMA 0x19070000 (each 32 KiB), and every place holding it is said. */
	{
		static const struct { uint64_t pa; const char *name; } area[] = {
			{ 0x19020000ULL, "DPP" }, { 0x19030000ULL, "DECON" }, { 0x19070000ULL, "DMA" },
		};
		int hits = 0;
		for (unsigned a = 0; a < sizeof(area) / sizeof(area[0]); a++) {
			IOMemoryMap *m = NULL;
			volatile uint32_t *r = b2l_zc_map(area[a].pa, 0x8000, &m);
			if (r == NULL) {
				continue;
			}
			unsigned live = 0;
			for (uint32_t off = 0; off < 0x8000; off += 4) {
				uint32_t v = r[off / 4];
				live += v != 0;
				if (v == (uint32_t)panel && hits < 16) {
					hits++;
					IOLog("B2L ZC: the panel memory's address %#x is at %s+%#x (%#llx)\n", v, area[a].name, off,
					    (unsigned long long)(area[a].pa + off));
					if (b2l_zc_base == NULL && (off & 0xfff) == 0x40 && area[a].pa != 0x19030000ULL) {
						b2l_zc_base = &r[off / 4];
						b2l_zc_base_pa = area[a].pa + off;
						m->retain();
					}
				}
			}
			IOLog("B2L ZC: %s %#llx: %u of 8192 words not zero\n", area[a].name, (unsigned long long)area[a].pa, live);
			m->release();
		}
		if (b2l_zc_base == NULL) {
			IOLog("B2L ZC: no IDMA base register holds the panel memory %#llx (%d places hold it): copy\n",
			    (unsigned long long)panel, hits);
			return;
		}
	}
	b2l_zc_shown = panel;
	b2l_zc_src_size = b2l_zc_base[(0x10 - 0x40) / 4];   /* IDMA_SRC_SIZE: height << 16 | width */
	b2l_zc_panel_bpr = (b2l_zc_src_size & 0xffff) * 4;
	b2l_zc_shown_bpr = b2l_zc_panel_bpr;
	IOLog("B2L ZC: IDMA src size %08x offset %08x img %08x in_con %08x enable %08x\n", b2l_zc_src_size,
	    b2l_zc_base[(0x14 - 0x40) / 4], b2l_zc_base[(0x18 - 0x40) / 4], b2l_zc_base[(0x08 - 0x40) / 4],
	    b2l_zc_base[(0x00 - 0x40) / 4]);
	b2l_zc_on = want != 0 && (b2l_zc_src_size & 0xffff) != 0;
	IOLog("B2L ZC: the panel memory is read through the base register at %#llx; zero-copy display %s\n",
	    (unsigned long long)b2l_zc_base_pa, b2l_zc_on ? "ON" : "off (b2l_fb_zc=0)");
	b2l_diag_record("B2L ZC: init base=%#llx src_size=%08x %s\n",
	    (unsigned long long)b2l_zc_base_pa, b2l_zc_src_size, b2l_zc_on ? "ON" : "OFF");
}

static bool
b2l_zc_active(void)
{
	return b2l_zc_on;
}

/* the display reads `phys` from the next frame on; false (and zero-copy off for good) when DECON did not take it */
static bool
b2l_zc_show(uint64_t phys, uint32_t bytesPerRow)
{
	if (!b2l_zc_on || phys == 0 || phys >= 0x100000000ULL) {
		return false;
	}
	if (phys == b2l_zc_shown && bytesPerRow == b2l_zc_shown_bpr) {
		return true;
	}
	uint64_t t0 = mach_absolute_time();
	uint32_t req;
	/* Wait for any pending DECON shadow update before programming the next scanout address, then wait
	 * until DECON latches `phys` on TE. backboardd's zero-copy swapchain only has 2 buffers (surface 2 & 3),
	 * so returning before DECON switches away from the previous buffer lets Mali overwrite it mid-scanout. */
	for (int i = 0; ((req = b2l_zc_decon[B2L_DECON_UPDATE_REQ]) & B2L_DECON_UPDATE_BITS) != 0 && i < 150; i++) {
		IOSleep(1);
	}
	if ((req & B2L_DECON_UPDATE_BITS) != 0) {
		static unsigned zc_pre_timeouts;
		if (zc_pre_timeouts < 8) {
			zc_pre_timeouts++;
			b2l_diag_record("B2L ZC: pre-flip timeout flip=%u req=%08x\n", b2l_zc_flips, req);
		}
		if (b2l_zc_flips == 0) {
			*b2l_zc_base = (uint32_t)b2l_zc_panel;
			b2l_zc_base[(0x10 - 0x40) / 4] = b2l_zc_src_size;
			b2l_zc_decon[B2L_DECON_UPDATE_REQ] = b2l_zc_decon[B2L_DECON_UPDATE_REQ] | B2L_DECON_UPDATE_BITS;
			b2l_zc_on = false;
			b2l_zc_shown = b2l_zc_panel;
			b2l_diag_record("B2L ZC: DECON initial flip timeout (req=%08x): OFF\n", req);
		}
		return false;
	}
	/* B1L-0098: the row length, in pixels, is the source width of IDMA_SRC_SIZE (backboardd's rows are 5888
	 * bytes where the panel's are 5760); the image (IDMA_IMG_SIZE) stays the panel's */
	b2l_zc_base[(0x10 - 0x40) / 4] = (b2l_zc_src_size & 0xffff0000u) | (bytesPerRow / 4);
	*b2l_zc_base = (uint32_t)phys;
	__asm__ volatile("dsb sy" ::: "memory");
	b2l_zc_decon[B2L_DECON_UPDATE_REQ] = b2l_zc_decon[B2L_DECON_UPDATE_REQ] | B2L_DECON_UPDATE_BITS;
	__asm__ volatile("dsb sy" ::: "memory");
	for (int i = 0; ((req = b2l_zc_decon[B2L_DECON_UPDATE_REQ]) & B2L_DECON_UPDATE_BITS) != 0 && i < 150; i++) {
		IOSleep(1);
	}
	if (b2l_zc_flips < 8 || (req & B2L_DECON_UPDATE_BITS) != 0) {
		IOLog("B2L ZC: flip %u to %#llx (%u bytes a row): update request now %08x, base %08x, shadow base %08x, "
		    "shadow src size %08x\n", b2l_zc_flips, (unsigned long long)phys, bytesPerRow, req, *b2l_zc_base,
		    b2l_zc_base[0x800 / 4], b2l_zc_base[(0x800 + 0x10 - 0x40) / 4]);
	}
	if ((req & B2L_DECON_UPDATE_BITS) != 0) {
		if (b2l_zc_flips == 0) {
			/* not taken on initial flip: back to the panel memory, and copy from now on */
			*b2l_zc_base = (uint32_t)b2l_zc_panel;
			b2l_zc_base[(0x10 - 0x40) / 4] = b2l_zc_src_size;
			b2l_zc_decon[B2L_DECON_UPDATE_REQ] = b2l_zc_decon[B2L_DECON_UPDATE_REQ] | B2L_DECON_UPDATE_BITS;
			b2l_zc_on = false;
			b2l_zc_shown = b2l_zc_panel;
			b2l_diag_record("B2L ZC: DECON initial flip timeout (req=%08x): OFF\n", req);
			return false;
		}
		static unsigned zc_timeouts;
		if (zc_timeouts < 8) {
			zc_timeouts++;
			b2l_diag_record("B2L ZC: transient timeout flip=%u req=%08x\n", b2l_zc_flips, req);
		}
	}
	b2l_zc_flips++;
	b2l_zc_shown = phys;
	b2l_zc_shown_bpr = bytesPerRow;
	b2l_perf_note(B2L_PERF_FLIP, mach_absolute_time() - t0, 0);
	return true;
}

/* B1L-0096: the GPU's job drawing a surface waited for before the display shows it (B2LMaliService.cpp) */
bool HakiosMaliDrawn(B2LSurface *surface, uint64_t milliseconds);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    b2l_diag_record("B2L FB OPEN: pid=%d type=%u\n",proc_selfpid(),type);
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	setProperty(kIOUserClientDefaultLockingKey,kOSBooleanTrue);
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= B2L_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t b2l_fb_logged;        /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	B2LCallTimer b2l_timer(B2L_CALL_FB, selector);   /* B1L-0102 */
    if (args == NULL) return kIOReturnBadArgument;
    static unsigned samples;
    if(__atomic_fetch_add(&samples,1,__ATOMIC_RELAXED)<64)
        b2l_diag_record("B2L FB CALL: pid=%d selector=%u scalar_in=%u struct_in=%llu struct_out=%llu\n",
            proc_selfpid(),selector,args?args->scalarInputCount:0,
            args?(uint64_t)args->structureInputSize:0,args?(uint64_t)args->structureOutputSize:0);

	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	/* 0058: preserve unknown block/parameter failures while capturing their ABI.
	 * No profile data is fabricated from the requested output length. */
	if (selector == 68 || selector == 79 || selector == kB2LFBSetContrast) {
		static unsigned detailSamples;
		if (__atomic_fetch_add(&detailSamples, 1, __ATOMIC_RELAXED) < 24) {
			uint64_t values[3] = { 0, 0, 0 }, input = 0;
			if (args->scalarInput != NULL) {
				for (unsigned i = 0; i < args->scalarInputCount && i < 3; ++i)
					values[i] = args->scalarInput[i];
			}
			if (args->structureInputDescriptor == NULL && args->structureInput != NULL)
				memcpy(&input, args->structureInput,
				    args->structureInputSize < sizeof(input) ? args->structureInputSize : sizeof(input));
			b2l_diag_record("B2L FB DETAIL: pid=%d selector=%u values=0x%llx,0x%llx,0x%llx input=0x%llx\n",
			    proc_selfpid(), selector, values[0], values[1], values[2], input);
		}
	}

	switch (selector) {
	/* Native wrappers: 0x2294bf384 (74), 0x2294c00d4 (84),
	 * 0x2294c1adc (88), pristine iOS 26.6.2 cache, unslid addresses. */
	case kB2LFBFrameInfoDescriptor: {
		if (args->scalarInputCount != 1 || args->scalarInput == NULL ||
		    args->structureInputSize != 0 || args->structureInputDescriptor != NULL) {
			return kIOReturnBadArgument;
		}
		if (args->scalarInput[0] != 0) return kIOReturnBadArgument;
		if (args->structureOutputDescriptor != NULL) return kIOReturnUnsupported;
		if (args->scalarOutputCount != 2 || args->scalarOutput == NULL ||
		    args->structureOutputSize != 64 || args->structureOutput == NULL) {
			return kIOReturnNoSpace;
		}
		/* Fixed CPU scanout has no frame-info profiles. Native enumeration
		 * at 0x2294bde68 accepts descriptor count zero without reading a name. */
		bzero(args->structureOutput, 64);
		args->structureOutputSize = 64;
		outCount = 2; /* count = 0, selected index = 0 */
		break;
	}
	case kB2LFBSetContrast: {
		uint32_t contrastBits;
		if (args->scalarInputCount != 0 || args->structureInputDescriptor != NULL ||
		    args->structureInput == NULL || args->structureInputSize != sizeof(contrastBits) ||
		    args->scalarOutputCount != 0 || args->structureOutputSize != 0 ||
		    args->structureOutputDescriptor != NULL) {
			return kIOReturnBadArgument;
		}
		memcpy(&contrastBits, args->structureInput, sizeof(contrastBits));
		/* Identity contrast needs no pixel transform; other values are unsupported. */
		if (contrastBits != 0x3f800000U) ret = kIOReturnUnsupported;
		break;
	}
	case kB2LFBSelectDisplay:
		if (args->scalarInputCount != 1 || args->scalarInput == NULL ||
		    args->structureInputSize != 0 || args->structureInputDescriptor != NULL ||
		    args->scalarOutputCount != 0 || args->structureOutputSize != 0 ||
		    args->structureOutputDescriptor != NULL) {
			return kIOReturnBadArgument;
		}
		/* This service exposes only logical display type 0 (primary). */
		if (args->scalarInput[0] != 0) ret = kIOReturnUnsupported;
		break;
	case kB2LFBGetLayerDefaultSurface:
		out[0] = b2l_surface_port_in_task(fFB->fClientSurface != NULL ? fFB->fClientSurface : fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kB2LFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kB2LFBSwapEnd: {
		uint8_t rec[B2L_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInput == NULL || args->structureInputSize != B2L_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		__atomic_add_fetch(&b2l_fb_swaps, 1, __ATOMIC_RELAXED);   /* B1L-0113 */
		fFB->swap(rec);
		break;
	}
	case kB2LFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kB2LFBGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kB2LFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kB2LFBGetDotPitch:
		out[0] = B2L_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kB2LFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput && args->scalarInput[2] < B2L_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("B2L FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kB2LFBSwapWait:
		break;
    default: ret=kIOReturnUnsupported;break;
	}

    if(outCount && (args->scalarOutputCount<outCount || !args->scalarOutput))return kIOReturnNoSpace;
    for(uint32_t i=0;i<outCount;++i)args->scalarOutput[i]=out[i];
    args->scalarOutputCount=outCount;

	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(b2l_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			b2l_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("B2L FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* B2L */
