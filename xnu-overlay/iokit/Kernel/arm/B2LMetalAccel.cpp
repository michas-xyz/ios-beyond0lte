/*
 * Copyright (c) 2026 the B2L port authors. Licensed under the Apple Public Source License 2.0 as the XNU it is part of.
 *
 * B1L-0044 (Metal M1): what Metal.framework looks for. -[MTLIOAccelServiceGlobalContext init] (Metal 23G90) matches
 * IOServiceMatching("IOAcceleratorES"), and getMetalPluginClassForService() reads two properties of each service found:
 * MetalPluginName, the bundle /System/Library/Extensions/<name>.bundle it loads, and MetalPluginClassName, the class in it
 * (a subclass of _MTLDevice) it sends -initWithAcceleratorPort:. As HakiOS's IOAcceleratorES (MiniIOMobileFramebuffer.cpp,
 * metal=1). The bundle (B2LMetalV3, userspace/metal/B2LMetal.m) shows the device only to the processes it chooses.
 * Kill switch: boot-arg b2l_metal=0.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
};
OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

extern "C" void b2l_metal_accel_start(void);
extern "C" void b2l_metal_ui_fallback(void);

/*
 * B1L-0080 (M6b): with b2l_metal_ui on (the default of this build), the plug-in is B2LMetalv95 -- Scion's Mali driver,
 * shown to backboardd only (userspace/metal/b2l_metal_plugin.m) -- so the window server composes the screen on the GPU.
 * b2l_metal_ui=0: B2LMetalV3, the plug-in that shows nobody a device (the CPU renderer, as before).
 * The kernel goes back to B2LMetalV3 by itself when backboardd has been started three times since boot
 * (b2l_metal_ui_fallback, from b2l_exec_graphics_diag.h): the next backboardd finds the CPU renderer again.
 */
static IOAcceleratorES *b2l_accel;

void
b2l_metal_accel_start(void)
{
	int on = 1;
	IOService *platform = IOService::getPlatform();
	IOAcceleratorES *accel;

	(void)PE_parse_boot_argn("b2l_metal", &on, sizeof(on));
	if (!on || platform == NULL) {
		IOLog("B2L METAL: no IOAcceleratorES (%s)\n", on ? "no platform" : "b2l_metal=0");
		return;
	}
	accel = OSTypeAlloc(IOAcceleratorES);
	if (accel == NULL || !accel->init()) {
		OSSafeReleaseNULL(accel);
		IOLog("B2L METAL: IOAcceleratorES not made\n");
		return;
	}
	int ui = 1;
	(void)PE_parse_boot_argn("b2l_metal_ui", &ui, sizeof(ui));
	accel->setName("B2LMetal");
	accel->setProperty("MetalPluginName", ui ? "B2LMetalv96" : "B2LMetalV3");
	accel->setProperty("MetalPluginClassName", ui ? "B2LMetalUIDevice" : "HakiosMetalDevice");
	accel->setProperty("IOClass", "IOAcceleratorES");
	if (!accel->attach(platform) || !accel->start(platform)) {
		IOLog("B2L METAL: IOAcceleratorES did not start\n");
		accel->release();
		return;
	}
	accel->registerService();
	b2l_accel = accel;      /* the attach keeps it alive; kept for b2l_metal_ui_fallback */
	accel->release();
	IOLog("B2L METAL: IOAcceleratorES published: MetalPluginName %s (b2l_metal_ui=%d)\n", ui ? "B2LMetalv96" : "B2LMetalV3", ui);
}

void
b2l_metal_ui_fallback(void)
{
	if (b2l_accel == NULL) {
		return;
	}
	b2l_accel->setProperty("MetalPluginName", "B2LMetalV3");
	b2l_accel->setProperty("MetalPluginClassName", "HakiosMetalDevice");
	IOLog("B2L METAL: backboardd started three times: back to B2LMetalV3 (the CPU renderer) for the next one\n");
}
