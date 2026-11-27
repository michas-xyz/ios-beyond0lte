/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef B2L_EXEC_BLUETOOTH_H
#define B2L_EXEC_BLUETOOTH_H

#define B2L_BT_TOOL_PATH "/usr/sbin/BlueTool"
#define B2L_BT_DAEMON_PATH "/usr/sbin/bluetoothd"
#define B2L_BT_LIB "/usr/local/lib/b2l-bluetool-v7.dylib"

int b2l_bt_dyld_pid = -1;
int b2l_btd_dyld_pid = -1;

static int
b2l_exec_add_env(struct image_params *imgp, int pointer_size, const char *key, size_t key_len, const char *setting)
{
	if (b2l_exec_env_has(imgp, key, key_len)) return 0;
	size_t len = strlen(setting) + 1;
	if (imgp->ip_argspace < (int)(len + 3 * pointer_size) || imgp->ip_strspace < (int)len) return 0;
	int error = exec_add_user_string(imgp, CAST_USER_ADDR_T(setting), UIO_SYSSPACE, TRUE);
	if (error) return error;
	imgp->ip_argspace -= pointer_size;
	imgp->ip_envc++;
	return 0;
}

static int
b2l_exec_bluetooth(struct image_params *imgp, int pointer_size)
{
	int enabled = 1;
	PE_parse_boot_argn("b2l_bt_ios", &enabled, sizeof(enabled));
	if (!enabled || (pointer_size != 4 && pointer_size != 8)) return 0;

	const char *path = imgp->ip_strings + strlen(EXECUTABLE_KEY);
	bool is_tool = strcmp(path, B2L_BT_TOOL_PATH) == 0;
	bool is_daemon = strcmp(path, B2L_BT_DAEMON_PATH) == 0;
	if (!is_tool && !is_daemon) return 0;

	if (is_daemon) {
		int err = b2l_exec_add_env(imgp, pointer_size, "BT_HCI_TRANSPORT=",
		    sizeof("BT_HCI_TRANSPORT=") - 1,
		    "BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,115200");
		if (err) return err;
		err = b2l_exec_add_env(imgp, pointer_size, "BT_BAUDRATE=",
		    sizeof("BT_BAUDRATE=") - 1,
		    "BT_BAUDRATE=115200");
		if (err) return err;
	}

	vnode_t lib = NULLVP;
	if (vnode_lookup(B2L_BT_LIB, 0, &lib, vfs_context_kernel()) == 0) {
		vnode_put(lib);
		int pid = proc_getpid(vfs_context_proc(imgp->ip_vfs_context));
		if (is_tool) b2l_bt_dyld_pid = pid;
		if (is_daemon) b2l_btd_dyld_pid = pid;
		int err = b2l_exec_add_env(imgp, pointer_size, "DYLD_INSERT_LIBRARIES=",
		    sizeof("DYLD_INSERT_LIBRARIES=") - 1,
		    "DYLD_INSERT_LIBRARIES=" B2L_BT_LIB);
		if (err) return err;
		printf("B2L BLUETOOTH EXEC: %s -> %s (pid %d)\n", path, B2L_BT_LIB, pid);
	}
	return 0;
}

#endif /* B2L_EXEC_BLUETOOTH_H */
