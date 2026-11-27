/* SPDX-License-Identifier: BSD-2-Clause
 * B1L-0080, Metal M6b: the plug-in Metal loads for IOAcceleratorES when the kernel's switch b2l_metal_ui is on
 * (MetalPluginName B2LMetalV4, MetalPluginClassName B2LMetalUIDevice; B2LMetalAccel.cpp). It is a thin bundle over
 * Scion's Mali driver, libhakiosmetal (linked by its install name, so every process shares the one image the tests and
 * M6a also load): B2LMetalUIDevice is HakiosMetalDevice, shown ONLY to the window server, backboardd -- QuartzCore
 * there takes it instead of the CPU renderer. Every other process gets no device from Metal's discovery, as before.
 * Log prefix "b2l-plugin:" (kern.hakios.say).
 */
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

@interface HakiosMetalDevice : NSObject
- (instancetype)initWithAcceleratorPort:(io_service_t)port;
@end

@interface B2LMetalUIDevice : HakiosMetalDevice
@end

static void
psay(const char *format, ...)
{
	char line[400], out[440];
	va_list ap;
	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	snprintf(out, sizeof(out), "b2l-plugin: %s", line);
	(void)sysctlbyname("kern.hakios.say", NULL, NULL, out, strlen(out));
}

@implementation B2LMetalUIDevice
- (instancetype)initWithAcceleratorPort:(io_service_t)port
{
	/* B2L (B1L-0269 + Step 3): expose Mali GPU to backboardd, SpringBoard, chronod, and
	 * iconservicesagent so Clock, Calendar, widgets, Control Center glyphs, and QuartzCore
	 * Glass render on the Mali GPU (boot-arg b2l_gpu_icons=0 restricts to backboardd alone;
	 * b2l_gpu_all=1 exposes to all processes). */
	static const char *const icon_daemons[] = { "SpringBoard", "chronod", "iconservicesagent" };
	const char *me = getprogname();
	bool is_icon_daemon = false;
	for (unsigned i = 0; me != NULL && i < sizeof(icon_daemons) / sizeof(icon_daemons[0]); i++)
		is_icon_daemon |= strcmp(me, icon_daemons[i]) == 0;
	bool ok = me != NULL && (strcmp(me, "backboardd") == 0 || is_icon_daemon);
	if (me != NULL) {
		char bootargs[512] = "";
		size_t blen = sizeof(bootargs) - 1;
		if (sysctlbyname("kern.bootargs", bootargs, &blen, NULL, 0) == 0) {
			bootargs[blen] = '\0';
			if (strstr(bootargs, "b2l_gpu_all=1") != NULL) {
				ok = true;
			} else if (strstr(bootargs, "b2l_gpu_icons=0") != NULL && is_icon_daemon) {
				ok = false;
			}
		}
	}
	if (!ok) {
		/* not enabled for GPU: no device, as the hidden plug-in of B1L-0049. NOT released: -[_MTLDevice dealloc]
		 * on an object _MTLDevice never initialised crashed SpringBoard four times a boot (B1L-0085..0087, EXC_BAD_ACCESS
		 * in its dealloc, via -[HakiosMetalDevice dealloc]); one unused object a process is the price. */
		return nil;
	}
	psay("%s (pid %d) asks for a device: the Mali's", me, getpid());
	self = [super initWithAcceleratorPort:port];
	psay("device %s", self != nil ? "made" : "NOT made");
	return self;
}
@end
