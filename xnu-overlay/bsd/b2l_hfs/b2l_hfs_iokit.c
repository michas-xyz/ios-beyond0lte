/* SPDX-License-Identifier: BSD-2-Clause
 * Builtin registration follows the OSS HFS/HFSEncodings start routines.
 * Native UFS has no IOMedia or external-journal enumeration service.
 */
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include "hfs.h"
#include "hfs_cnode.h"
#include "hfs_dbg.h"
#include "hfs_iokit.h"
#include "upstream/hfs_encodings/hfs_encodings_internal.h"

#ifndef panic_on_assert
bool panic_on_assert;
#endif
#if DEBUG
bool hfs_corruption_panics = true;
#endif
extern struct vnodeopv_desc hfs_vnodeop_opv_desc;
extern struct vnodeopv_desc hfs_specop_opv_desc;
extern struct vnodeopv_desc hfs_fifoop_opv_desc;
extern const struct vfsops hfs_vfsops;
extern int b2l_ufs_root_device(dev_t *);
extern void b2l_early_trace(const char *fmt, ...);
static vfstable_t b2l_hfs_vfs_handle;
static struct vfsops b2l_hfs_vfsops;

static int
b2l_hfs_mount(mount_t mp, vnode_t devvp, user_addr_t data, vfs_context_t context)
{
	dev_t selected;
	int rw = 0;
	int is_root = (vfs_flags(mp) & MNT_ROOTFS) && devvp &&
	    !b2l_ufs_root_device(&selected) && vnode_specrdev(devvp) == selected;
	b2l_early_trace("B2L HFS: mount begin dev=0x%x flags=0x%llx is_root=%d\n",
	    devvp ? (unsigned)vnode_specrdev(devvp) : 0, (unsigned long long)vfs_flags(mp), is_root);
	if (is_root && PE_parse_boot_argn("b2l_root_rw", &rw, sizeof(rw)) && rw == 1) {
		vfs_clearflags(mp, MNT_RDONLY);
	}
	b2l_early_trace("B2L HFS: calling upstream vfs_mount (rdonly=%d)...\n", vfs_isrdonly(mp));
	int error = hfs_vfsops.vfs_mount(mp, devvp, data, context);
	b2l_early_trace("B2L HFS: upstream vfs_mount result=%d\n", error);
	if (error && is_root && !vfs_isrdonly(mp)) {
		b2l_early_trace("B2L HFS: RW mount failed (%d); fallback to read-only root...\n", error);
		vfs_setflags(mp, MNT_RDONLY);
		error = hfs_vfsops.vfs_mount(mp, devvp, data, context);
		b2l_early_trace("B2L HFS: read-only fallback result=%d\n", error);
	}
	if (!error && (vfs_flags(mp) & MNT_ROOTFS)) {
		b2l_early_trace("B2L HFS: native root mounted %s\n",
		    vfs_isrdonly(mp) ? "read-only" : "read-write");
		printf("B2L HFS: native root mounted %s (unencrypted)\n",
		    vfs_isrdonly(mp) ? "read-only" : "read-write");
	}
	return error;
}

void
b2l_hfs_register(void)
{
#ifndef panic_on_assert
	panic_on_assert = PE_i_can_has_kernel_configuration() & kPEICanHasAssertions;
#endif
#if DEBUG
	PE_parse_boot_argn("hfs_corruption_panics", &hfs_corruption_panics, sizeof(hfs_corruption_panics));
#endif
#if !TARGET_OS_IPHONE
	/* HFS Standard converters are intentionally absent from iPhoneOS HFS+. */
	hfs_converterinit();
#endif
	struct vnodeopv_desc *operations[] = {
		&hfs_vnodeop_opv_desc, &hfs_specop_opv_desc,
#if FIFO
		&hfs_fifoop_opv_desc,
#endif
	};
	b2l_hfs_vfsops = hfs_vfsops;
	b2l_hfs_vfsops.vfs_mount = b2l_hfs_mount;
	struct vfs_fsentry entry = {
		.vfe_vfsops = &b2l_hfs_vfsops,
		.vfe_vopcnt = sizeof(operations) / sizeof(operations[0]),
		.vfe_opvdescs = operations,
		.vfe_fsname = "hfs",
		.vfe_flags = VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBLREADDIR_EXTENDED |
		    VFS_TBL64BITREADY | VFS_TBLVNOP_PAGEOUTV2 | VFS_TBLVNOP_PAGEINV2 |
		    VFS_TBLTHREADSAFE | VFS_TBLCANMOUNTROOT | VFS_TBLVNOP_SECLUDE_RENAME |
		    VFS_TBLNATIVEXATTR,
	};
	int error = vfs_fsadd(&entry, &b2l_hfs_vfs_handle);
	if (error) panic("B2L HFS: vfs_fsadd failed: %d", error);
	hfs_init_zones();
	hfs_sysctl_register();
	uint32_t cpus;
	size_t size = sizeof(cpus);
	if (!sysctlbyname("hw.physicalcpu", &cpus, &size, NULL, 0)) {
		if (2 * cpus > MAX_CACHED_ORIGINS_DEFAULT) {
			_hfs_max_origins = _hfs_max_file_origins = 2 * cpus;
		} else if (2 * cpus > MAX_CACHED_FILE_ORIGINS_DEFAULT) {
			_hfs_max_file_origins = 2 * cpus;
		}
	}
	printf("B2L HFS: hfs-715.120.4 registered (no content protection)\n");
}

OSKextLoadTag b2l_hfs_current_load_tag(void) { return kOSKextInvalidLoadTag; }
int hfs_is_ejectable(__unused const char *name) { return 0; }
void hfs_iterate_media_with_content(__unused const char *content,
    __unused int (*callback)(const char *, const char *, void *), __unused void *arg) {}
kern_return_t hfs_get_platform_serial_number(char *out, uint32_t size)
{
	if (out && size) out[0] = '\0';
	return KERN_NOT_SUPPORTED;
}
