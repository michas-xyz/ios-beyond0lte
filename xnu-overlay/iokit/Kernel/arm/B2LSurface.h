/*
 * Built-in IOSurface for B2L (see B2LSurface.cpp).
 *
 * Shared with B2LFramebuffer.cpp: the framebuffer looks surfaces up by
 * id when a swap names them and publishes the panel memory itself as the
 * default framebuffer surface.
 */
#ifndef _B2L_SURFACE_H
#define _B2L_SURFACE_H

#include <pexpert/arm64/board_config.h>

#if defined(B2L)
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSSerialize.h>
#include <mach/mach_types.h>

#define B2L_SURFACE_MAX_PLANES        20

struct b2l_surface_plane {
	uint32_t width, height, bytesPerRow, offset, size;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
};

class B2LSurface : public OSObject {
	OSDeclareDefaultStructors(B2LSurface);

public:
	uint32_t id;
	uint32_t width, height, bytesPerRow, offset, pixelFormat, allocSize;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
	uint32_t cacheMode, planeCount, parentID;
	struct b2l_surface_plane planes[B2L_SURFACE_MAX_PLANES];

	IOMemoryDescriptor       *memory;       /* pixels (buffer or physical) */
	IOOptionBits              mapCache;     /* cache mode for user mappings */
	IOBufferMemoryDescriptor *state;        /* shared state page, read-only for users */
	OSDictionary             *properties;   /* protected by the surface lock */
	OSSerialize              *serializedProperties; /* immutable readback snapshot */
	IOMemoryMap              *kernelMap;    /* lazily created for kernel copies */
	volatile uint64_t         maliSeq;      /* B1L-0029: last Mali job drawing into it (0 none, ~0 pending) */
	uint64_t                  scanoutPhys;  /* B1L-0096: physical address when the display can scan it out directly (0 not) */
	bool                      flushOnce;    /* B1L-0097: a GPU render target of backboardd's: cleaned before the GPU once */
	bool                      flushedOnce;

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;

	/* Kernel address of the pixels (mapped on first use), or 0. */
	uintptr_t kernelAddress();
};

/* Retained surface for an id, or NULL. */
B2LSurface *b2l_surface_copy(uint32_t id);

/*
 * Publish physical memory (the panel framebuffer) as a surface. Returns a
 * retained surface that stays in the id table until released by the caller
 * and every user.
 */
B2LSurface *b2l_surface_create_physical(uint64_t phys, uint32_t width,
    uint32_t height, uint32_t bytesPerRow, uint32_t pixelFormat);

/* B1L-0096: the panel's size and format, set by the display (B2LFramebuffer.cpp): a surface of exactly that, made by
 * backboardd, is given physically contiguous write-combined memory the display can scan out (at most a few). */
void b2l_surface_set_scanout_shape(uint32_t width, uint32_t height, uint32_t bytesPerRow, bool contiguous);
void b2l_surface_prealloc_scanout(uint32_t width, uint32_t height, uint32_t bytesPerRow, unsigned count);

/* B1L-0097: an ordinary (cached) surface of the given shape, published in the table: the display's default surface,
 * so that nothing draws into the memory the panel is reading */
B2LSurface *b2l_surface_create_memory(uint32_t width, uint32_t height, uint32_t bytesPerRow, uint32_t pixelFormat,
    bool contiguous);

/* Send right for the surface in task (IOSurfaceLookupFromMachPort). */
mach_port_name_t b2l_surface_port_in_task(B2LSurface *surface, task_t task);

#endif /* B2L */
#endif /* _B2L_SURFACE_H */
