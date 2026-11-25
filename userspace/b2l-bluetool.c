/* SPDX-License-Identifier: BSD-2-Clause */
/* b2l-bluetool-v1.dylib:
 * 1) Stand-in for Apple's BlueTool (/usr/sbin/BlueTool) on com.apple.BlueTool XPC.
 * 2) Platform identity shim for /usr/sbin/bluetoothd 23G90 (DA7ADF00-...):
 *    answers true to MGIsDeviceOneOfType at the platform 0x16 creation site
 *    so bluetoothd does not crash on Unknown platform 0xffffffff at +0x31c.
 */
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <xpc/xpc.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BT_CTL_NAME   "com.apple.uart.sk.bluetooth"
#define BT_CTL_LOGOPT 0xb2
#define BT_READY_WAIT_MS 60000

/* <sys/kern_control.h> and <sys/sys_domain.h> are macOS-only in the SDK; same ABI on iOS. */
#define SYSPROTO_CONTROL 2
#define AF_SYS_CONTROL   2
struct ctl_info { uint32_t ctl_id; char ctl_name[96]; };
#define CTLIOCGINFO _IOWR('N', 3, struct ctl_info)
struct sockaddr_ctl {
	u_char sc_len, sc_family;
	uint16_t ss_sysaddr;
	uint32_t sc_id, sc_unit, sc_reserved[5];
};
_Static_assert(CTLIOCGINFO == 0xc0644e03, "CTLIOCGINFO as used by bluetoothd 0x100193058");

/* Not marked available on iOS in the SDK headers; present in libxpc. */
extern xpc_connection_t b2l_xpc_mach_service(const char *, dispatch_queue_t, uint64_t)
    __asm__("_xpc_connection_create_mach_service");

extern bool MGIsDeviceOneOfType(const int *types);

static int log_fd = -1;

/* 0: control found (fd connected for logging), -1: not registered yet, 1: socket refused */
static int ctl_open(void)
{
	if (log_fd >= 0) return 0;
	int fd = socket(PF_SYSTEM, SOCK_STREAM, SYSPROTO_CONTROL);
	if (fd < 0) return 1;
	struct ctl_info info;
	memset(&info, 0, sizeof(info));
	strlcpy(info.ctl_name, BT_CTL_NAME, sizeof(info.ctl_name));
	if (ioctl(fd, CTLIOCGINFO, &info)) { close(fd); return -1; }
	struct sockaddr_ctl sc;
	memset(&sc, 0, sizeof(sc));
	sc.sc_len = sizeof(sc);
	sc.sc_family = AF_SYSTEM;
	sc.ss_sysaddr = AF_SYS_CONTROL;
	sc.sc_id = info.ctl_id;
	if (connect(fd, (struct sockaddr *)&sc, sizeof(sc))) { close(fd); return 1; }
	log_fd = fd;
	return 0;
}

static void tool_log(const char *fmt, ...)
{
	char line[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (ctl_open() == 0) setsockopt(log_fd, SYSPROTO_CONTROL, BT_CTL_LOGOPT, line, (socklen_t)strlen(line));
}

/* Returns the wait in ms, or -1 when the chip never came up. */
static long wait_ready(void)
{
	for (long ms = 0; ms < BT_READY_WAIT_MS; ms += 250) {
		int r = ctl_open();
		if (r == 0) return ms;
		if (r > 0) return ms;   /* sandbox refused the socket: let bluetoothd try it itself */
		usleep(250 * 1000);
	}
	return -1;
}

static void handle(xpc_object_t msg)
{
	if (xpc_get_type(msg) != XPC_TYPE_DICTIONARY) return;
	const char *id = xpc_dictionary_get_string(msg, "kMsgId");
	xpc_object_t args = xpc_dictionary_get_value(msg, "kMsgArgs");
	const char *script = (args && xpc_get_type(args) == XPC_TYPE_DICTIONARY) ?
	    xpc_dictionary_get_string(args, "script") : NULL;
	int64_t result = 0;
	long waited = 0;
	if (id && !strcmp(id, "RunBuiltinScript") && script && (!strcmp(script, "boot") || !strcmp(script, "init"))) {
		waited = wait_ready();
		if (waited < 0) result = 19;   /* ENODEV: kernel never brought the chip up */
	}
	xpc_object_t reply = xpc_dictionary_create_reply(msg);
	if (reply) {
		xpc_dictionary_set_int64(reply, "result", result);
		xpc_dictionary_set_string(reply, "firmware", "BCM4375B1 b2l");
		xpc_dictionary_set_int64(reply, "manufacturer", 15);   /* Broadcom */
		xpc_connection_send_message(xpc_dictionary_get_remote_connection(msg), reply);
		xpc_release(reply);
	}
	tool_log("%s %s -> %lld (waited %ld ms)%s", id ? id : "?", script ? script : "-",
	    (long long)result, waited, reply ? "" : " no reply wanted");
}

static void __attribute__((noreturn)) tool_main(void)
{
	dispatch_queue_t q = dispatch_queue_create("b2l.bluetool", DISPATCH_QUEUE_SERIAL);
	xpc_connection_t listener = b2l_xpc_mach_service("com.apple.BlueTool", q,
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
		xpc_connection_set_target_queue(peer, q);
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) { handle(m); });
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	tool_log("stand-in listening on com.apple.BlueTool (pid %d)", getpid());
	dispatch_main();
}

static int b2l_getopt(int argc, char *const argv[], const char *optstring)
{
	if (optstring && !strcmp(optstring, "c:hf:F:R")) tool_main();
	return getopt(argc, argv, optstring);
}

/* Check LC_UUID of image 0 matches bluetoothd 23G90 (DA7ADF00-04FA-3EE0-8782-E336E0982712). */
static uintptr_t bluetoothd_base_if_matched(void)
{
	static int checked;
	static uintptr_t base;
	if (checked) return base;
	checked = 1;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(0);
	if (!mh || mh->magic != MH_MAGIC_64) return 0;
	static const uint8_t want[16] = {
		0xda, 0x7a, 0xdf, 0x00, 0x04, 0xfa, 0x3e, 0xe0,
		0x87, 0x82, 0xe3, 0x36, 0xe0, 0x98, 0x27, 0x12
	};
	const uint8_t *p = (const uint8_t *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		const struct load_command *lc = (const struct load_command *)p;
		if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
			const struct uuid_command *uc = (const struct uuid_command *)lc;
			if (!memcmp(uc->uuid, want, 16)) {
				base = (uintptr_t)mh;
			}
			break;
		}
		p += lc->cmdsize;
	}
	return base;
}

static bool b2l_MGIsDeviceOneOfType(const int *types)
{
	uintptr_t mh = bluetoothd_base_if_matched();
	if (mh) {
		uintptr_t ret = (uintptr_t)__builtin_return_address(0) - mh;
		/* Platform 0x16 call site in bluetoothd 23G90 (DA7ADF00-...) */
		if (ret == 0x432160) {
			return true;
		}
	}
	return MGIsDeviceOneOfType(types);
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *replacee; } b2l_interpose[] = {
	{ (const void *)b2l_getopt, (const void *)getopt },
	{ (const void *)b2l_MGIsDeviceOneOfType, (const void *)MGIsDeviceOneOfType },
};
