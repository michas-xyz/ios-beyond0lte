/* SPDX-License-Identifier: BSD-2-Clause */
/* Included by kern_exec.c after EXECUTABLE_KEY/exec_add_user_string.
 * Diagnostic environment only: no loader, trust or entitlement changes. */
#ifndef B2L_EXEC_GRAPHICS_DIAG_H
#define B2L_EXEC_GRAPHICS_DIAG_H
/* Copied env strings only (never user pointers): is KEY= already present? */
static bool
b2l_exec_env_has(struct image_params *imgp, const char *key, size_t key_len)
{
	const char *cursor = imgp->ip_endargv;
	while (cursor < imgp->ip_strendp) {
		size_t available = (size_t)(imgp->ip_strendp - cursor);
		size_t length = strnlen(cursor, available);
		if (length == available) return true;      /* malformed: add nothing */
		if (length >= key_len && memcmp(cursor, key, key_len) == 0) return true;
		cursor += length + 1;
	}
	return false;
}

/* Platform render settings for the CPU-only display path (no Apple GPU):
 * CoreAnimation's asynchronous offscreen nodes trip ogl-context.cpp:1186
 * (found != slist->end()) in backboardd's software renderer (B1L-0013 crash,
 * keyboard/backdrops). Kill switch: boot-arg b2l_ca_env=0. */
static int
b2l_exec_render_env(struct image_params *imgp, int pointer_size)
{
	static const struct { const char *path; const char *setting; } table[] = {
		{ "/usr/libexec/backboardd", "CA_DISABLE_ASYNCHRONOUS=1" },
		{ "/usr/libexec/backboardd", "CA_DISABLE_ASYNC=1" },
		/* Experiment (B1L-0016): cheaper backdrop blurs; parsing not yet verified. */
		{ "/usr/libexec/backboardd", "CA_BACKDROP_SCALE=0.25" },
		/* B1L-0155 (WiFi diagnostic): wifid mirrors its os_log messages to standard error,
		 * which sys_generic.c copies into the persistent log (b2l_wifid_log=0 stops it) */
		{ "/usr/sbin/wifid", "OS_ACTIVITY_DT_MODE=YES" },
	};
	int enabled = 1, backdrop = 0;   /* B1L-0042: off by default; the GPU renders Liquid Glass backdrops at full scale */
	PE_parse_boot_argn("b2l_ca_env", &enabled, sizeof(enabled));
	PE_parse_boot_argn("b2l_ca_backdrop", &backdrop, sizeof(backdrop));
	if (!enabled || (pointer_size != 4 && pointer_size != 8)) return 0;
	const char *path = imgp->ip_strings + strlen(EXECUTABLE_KEY);
	for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (strcmp(path, table[i].path)) continue;
		if (!backdrop && !strncmp(table[i].setting, "CA_BACKDROP_SCALE=", 18)) continue;
		const char *eq = strchr(table[i].setting, '=');
		size_t key_len = (size_t)(eq - table[i].setting) + 1, len = strlen(table[i].setting) + 1;
		if (b2l_exec_env_has(imgp, table[i].setting, key_len)) continue;
		if (imgp->ip_argspace < (int)(len + 3 * pointer_size) || imgp->ip_strspace < (int)len) return 0;
		int error = exec_add_user_string(imgp, CAST_USER_ADDR_T(table[i].setting), UIO_SYSSPACE, TRUE);
		if (error) return error;
		imgp->ip_argspace -= pointer_size;
		imgp->ip_envc++;
		b2l_early_trace("B2L CA ENV: %s for %s\n", table[i].setting, path);
	}
	return 0;
}

/* B1L-0026: b2l-springboard-v5.dylib in SpringBoard (blue placeholder behind
 * the wallpaper windows; userspace/b2l-springboard.m), the way ios-a5xelte
 * herolte inserts libherosb. dyld honours DYLD_INSERT_LIBRARIES only when the
 * AMFI policy answers call 90 for this pid (b2l_amfi_constraint.h).
 * Kill switch: boot-arg b2l_sb_inject=0. */
#define B2L_SB_PATH "/System/Library/CoreServices/SpringBoard.app/SpringBoard"
#define B2L_SB_LIB  "/usr/local/lib/b2l-springboard-v7.dylib"
int b2l_sb_dyld_pid = -1;
int b2l_bb_dyld_pid = -1;      /* B1L-0031: backboardd given the EAGL bridge */
#define B2L_BB_PATH "/usr/libexec/backboardd"
#define B2L_BB_LIB  "/usr/local/lib/b2l-gl-v9/libb2l-gl.dylib"
/* B1L-0044: Metal M1, the test library (userspace/metal/b2l_metal_test.m) when the GLES bridge is off */
#define B2L_MTEST_HOST "/weatherd"   /* B1L-0054: an ordinary daemon, not a boot task */
#define B2L_BB_MTEST "/usr/local/lib/b2l-metal-v95/libb2l-metal-test.dylib"
static int
b2l_exec_bb_inject(struct image_params *imgp, int pointer_size)
{
	/* GPU G6: the HakiOS EAGL bridge (OpenGL ES = Mesa panfrost on the Mali) in the window server.
	 * Without a Metal device CoreAnimation's renderer() takes [EAGLContext initWithAPI:3]; the bridge
	 * answers it. B1L-0043: off by default (the owner's choice after B1L-0042: slower and more broken than the CPU
	 * renderer; the work goes to Metal). boot-arg b2l_gl=1 turns it back on for tests. */
	static const char gl_setting[] = "DYLD_INSERT_LIBRARIES=" B2L_BB_LIB;
	static const char mt_setting[] = "DYLD_INSERT_LIBRARIES=" B2L_BB_MTEST;
	int enabled = 0, mtest = 1;
	PE_parse_boot_argn("b2l_gl", &enabled, sizeof(enabled));
	PE_parse_boot_argn("b2l_metal_test", &mtest, sizeof(mtest));
	if ((!enabled && !mtest) || (pointer_size != 4 && pointer_size != 8)) return 0;
	const char *setting = enabled ? gl_setting : mt_setting;
	const char *libpath = enabled ? B2L_BB_LIB : B2L_BB_MTEST;
	const char *path = imgp->ip_strings + strlen(EXECUTABLE_KEY);
	/* B1L-0049: the Metal test runs in our own bootstrap daemon, not in backboardd: QuartzCore in backboardd takes any
	 * Metal device it is shown (B1L-0048: it loaded its default.metallib on ours and aborted), and Metal makes its device
	 * list once per process, so the test and the window server cannot share one */
	if (enabled) {
		if (strcmp(path, B2L_BB_PATH)) return 0;
	} else {
		/* B1L-0050: the bootstrap daemon is no longer launched (apps are laid out by the kernel since B1L-0025);
		 * tzinit (the time zone's set-up, root, once per boot, nothing to do with drawing) hosts the test, matched by
		 * the end of its path */
		size_t pl = strlen(path), hl = strlen(B2L_MTEST_HOST);
		if (pl < hl || strcmp(path + pl - hl, B2L_MTEST_HOST)) return 0;
	}
	vnode_t lib = NULLVP;
	if (vnode_lookup(libpath, 0, &lib, vfs_context_kernel())) return 0;
	vnode_put(lib);
	b2l_bb_dyld_pid = proc_getpid(vfs_context_proc(imgp->ip_vfs_context));
	if (b2l_exec_env_has(imgp, "DYLD_INSERT_LIBRARIES=", sizeof("DYLD_INSERT_LIBRARIES=") - 1)) return 0;
	size_t len = strlen(setting) + 1;
	if (imgp->ip_argspace < (int)(len + 3 * pointer_size) || imgp->ip_strspace < (int)len) return 0;
	int error = exec_add_user_string(imgp, CAST_USER_ADDR_T(setting), UIO_SYSSPACE, TRUE);
	if (error) return error;
	imgp->ip_argspace -= pointer_size;
	imgp->ip_envc++;
	printf("B2L GL: %s inserted in backboardd (pid %d)\n", libpath, b2l_bb_dyld_pid);
	return 0;
}
/* B1L-0079, Metal M6a: libb2l-metal-app.dylib in the Clock app (userspace/metal/b2l_metal_app.m): a full-screen
 * CAMetalLayer drawn by the Mali with QuartzCore's shaders, on top of the app, presented through CoreAnimation to
 * backboardd's (CPU) compositor. Kill switch: boot-arg b2l_metal_app=0. */
#define B2L_APP_HOST "/MobileTimer.app/MobileTimer"
#define B2L_APP_LIB "/usr/local/lib/b2l-metal-v95/libb2l-metal-app.dylib"
int b2l_app_dyld_pid = -1;
static int
b2l_exec_app_inject(struct image_params *imgp, int pointer_size)
{
	static const char setting[] = "DYLD_INSERT_LIBRARIES=" B2L_APP_LIB;
	int enabled = 1;
	PE_parse_boot_argn("b2l_metal_app", &enabled, sizeof(enabled));
	if (!enabled || (pointer_size != 4 && pointer_size != 8)) return 0;
	const char *path = imgp->ip_strings + strlen(EXECUTABLE_KEY);
	size_t pl = strlen(path), hl = strlen(B2L_APP_HOST);
	if (pl < hl || strcmp(path + pl - hl, B2L_APP_HOST)) return 0;
	vnode_t lib = NULLVP;
	if (vnode_lookup(B2L_APP_LIB, 0, &lib, vfs_context_kernel())) return 0;
	vnode_put(lib);
	b2l_app_dyld_pid = proc_getpid(vfs_context_proc(imgp->ip_vfs_context));
	if (b2l_exec_env_has(imgp, "DYLD_INSERT_LIBRARIES=", sizeof("DYLD_INSERT_LIBRARIES=") - 1)) return 0;
	size_t len = sizeof(setting);
	if (imgp->ip_argspace < (int)(len + 3 * pointer_size) || imgp->ip_strspace < (int)len) return 0;
	int error = exec_add_user_string(imgp, CAST_USER_ADDR_T(setting), UIO_SYSSPACE, TRUE);
	if (error) return error;
	imgp->ip_argspace -= pointer_size;
	imgp->ip_envc++;
	printf("B2L METAL APP: %s inserted (pid %d)\n", B2L_APP_LIB, b2l_app_dyld_pid);
	return 0;
}
static int
b2l_exec_sb_inject(struct image_params *imgp, int pointer_size)
{
	static const char setting[] = "DYLD_INSERT_LIBRARIES=" B2L_SB_LIB;
	int enabled = 1;
	PE_parse_boot_argn("b2l_sb_inject", &enabled, sizeof(enabled));
	if (!enabled || (pointer_size != 4 && pointer_size != 8)) return 0;
	const char *path = imgp->ip_strings + strlen(EXECUTABLE_KEY);
	if (strcmp(path, B2L_SB_PATH)) return 0;
	vnode_t lib = NULLVP;
	if (vnode_lookup(B2L_SB_LIB, 0, &lib, vfs_context_kernel())) return 0;
	vnode_put(lib);
	b2l_sb_dyld_pid = proc_getpid(vfs_context_proc(imgp->ip_vfs_context));
	if (b2l_exec_env_has(imgp, "DYLD_INSERT_LIBRARIES=", sizeof("DYLD_INSERT_LIBRARIES=") - 1)) return 0;
	size_t len = sizeof(setting);
	if (imgp->ip_argspace < (int)(len + 3 * pointer_size) || imgp->ip_strspace < (int)len) return 0;
	int error = exec_add_user_string(imgp, CAST_USER_ADDR_T(setting), UIO_SYSSPACE, TRUE);
	if (error) return error;
	imgp->ip_argspace -= pointer_size;
	imgp->ip_envc++;
	printf("B2L SB: %s inserted (pid %d)\n", B2L_SB_LIB, b2l_sb_dyld_pid);
	return 0;
}

/*
 * B1L-0052: four speech synthesisers (Audio Unit extensions) abort at once on this phone and are started again and again
 * -- 60 times each in nine minutes in B1L-0051, a full dyld start and its PAC traps every time. Their exec is refused
 * (EPERM) before anything is loaded. Kill switch: boot-arg b2l_block_tts=0.
 */
static int
b2l_exec_block_loops(struct image_params *imgp)
{
	static const char *const names[] = { "/KonaSynthesizer", "/MauiAUSP", "/MacinTalkAUSP", "/SiriAUSP" };
	static unsigned said;
	int on = 1;
	PE_parse_boot_argn("b2l_block_tts", &on, sizeof(on));
	const char *path = imgp->ip_strings + strlen(EXECUTABLE_KEY);
	size_t pl = strlen(path);
	{	/* B1L-0111: imagent (iMessage/FaceTime: re-registers with Apple for ever without a SIM or an account) and
		 * dasd (the background-task scheduler: millions of corecrypto PAC traps a boot) -- neither draws anything.
		 * Owner's decision. Kill switch: boot-arg b2l_block_daemons=0. */
		static const char *const daemons[] = { "/imagent", "/usr/libexec/dasd" };
		static unsigned told;
		int block = 1;
		PE_parse_boot_argn("b2l_block_daemons", &block, sizeof(block));
		for (unsigned i = 0; block && i < sizeof(daemons) / sizeof(daemons[0]); i++) {
			size_t nl = strlen(daemons[i]);
			if (pl >= nl && !strcmp(path + pl - nl, daemons[i])) {
				if (told++ < 8) printf("B2L EXEC: %s refused (not needed to draw; b2l_block_daemons=0 allows it)\n", path);
				return EPERM;
			}
		}
	}
	if (!on) return 0;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		size_t nl = strlen(names[i]);
		if (pl >= nl && !strcmp(path + pl - nl, names[i])) {
			if (said++ < 8) printf("B2L EXEC: %s refused (crashes on start in a loop; b2l_block_tts=0 allows it)\n", path);
			return EPERM;
		}
	}
	return 0;
}

/* B1L-0080 (M6b): backboardd's starts since boot; at the third, Metal goes back to the CPU renderer for it */
extern void b2l_metal_ui_fallback(void);
static void
b2l_exec_count_backboardd(struct image_params *imgp)
{
	static int starts;
	const char *path = imgp->ip_strings + strlen(EXECUTABLE_KEY);
	if (strcmp(path, B2L_BB_PATH)) return;
	int n = __atomic_add_fetch(&starts, 1, __ATOMIC_RELAXED);
	printf("B2L METAL: backboardd start %d\n", n);
	if (n == 3) b2l_metal_ui_fallback();
}
#include "b2l_exec_bluetooth.h"
static int
b2l_exec_graphics_diag(struct image_params *imgp, int pointer_size)
{
	b2l_exec_count_backboardd(imgp);
	int render_error = b2l_exec_block_loops(imgp);
	if (render_error) return render_error;
	render_error = b2l_exec_bluetooth(imgp, pointer_size);
	if (render_error) return render_error;
	render_error = b2l_exec_render_env(imgp, pointer_size);
	if (render_error) return render_error;
	render_error = b2l_exec_sb_inject(imgp, pointer_size);
	if (render_error) return render_error;
	render_error = b2l_exec_bb_inject(imgp, pointer_size);
	if (render_error) return render_error;
	render_error = b2l_exec_app_inject(imgp, pointer_size);   /* B1L-0079 */
	if (render_error) return render_error;
	static const char setting[] = "CG_CONTEXT_SHOW_BACKTRACE=1";
	static const char key[] = "CG_CONTEXT_SHOW_BACKTRACE=";
	int enabled = 0;
	if (!PE_parse_boot_argn("b2l_diag", &enabled, sizeof(enabled)) || enabled != 2 ||   /* B1L-0110 */
	    (pointer_size != 4 && pointer_size != 8)) return 0;
	const char *path = imgp->ip_strings + strlen(EXECUTABLE_KEY);
	if (strcmp(path, "/System/Library/CoreServices/SpringBoard.app/SpringBoard") &&
	    strcmp(path, "/usr/libexec/backboardd") &&
	    strcmp(path, "/Applications/Preferences.app/Preferences")) return 0;
	/* Preserve a caller-provided value and inspect only already copied,
	 * NUL-terminated env strings, never original user pointers. */
	const char *cursor = imgp->ip_endargv;
	while (cursor < imgp->ip_strendp) {
		size_t available = (size_t)(imgp->ip_strendp - cursor);
		size_t length = strnlen(cursor, available);
		if (length == available) return 0;
		if (length >= sizeof(key) - 1 &&
		    memcmp(cursor, key, sizeof(key) - 1) == 0) return 0;
		cursor += length + 1;
	}
	/* Leave the normal env terminator and tail alignment ample space. A
	 * diagnostics-only addition must not push an otherwise valid exec over
	 * NCARGS. The regular checked string copier accounts for string bytes. */
	if (imgp->ip_argspace < (int)(sizeof(setting) + 3 * pointer_size) ||
	    imgp->ip_strspace < (int)sizeof(setting)) return 0;
	int error = exec_add_user_string(imgp, CAST_USER_ADDR_T(setting), UIO_SYSSPACE, TRUE);
	if (error) return error;
	imgp->ip_argspace -= pointer_size;
	imgp->ip_envc++;
	b2l_early_trace("B2L CG DIAG: backtrace environment enabled for %s\n", path);
	return 0;
}
#endif
