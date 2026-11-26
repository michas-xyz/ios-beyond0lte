/* SPDX-License-Identifier: BSD-2-Clause
 * Opt-in publication of the owned native migration launcher. Existing files
 * must match exactly; a synced executable is published before its launch job.
 */
#include <pexpert/pexpert.h>
#include <kern/kalloc.h>   /* B1L-0149: WiFi firmware buffers */
#include <libkern/zlib.h>
#include "b2l_userspace_payload.h"
#include "b2l_springboard_payload.h"
#include "b2l_bluetool_payload.h"
#include "b2l_metal_payload.h"
#include "b2l_apps_payload_compare.h"
static int b2l_apps_cache_ready;
int b2l_apps_cache_is_ready(void){return b2l_apps_cache_ready;}
static int b2l_apps_directory(const char *path,vfs_context_t ctx)
{
    struct nameidata nd;struct vnode_attr va;vnode_t vp=NULL;
    NDINIT(&nd,CREATE,OP_MKDIR,LOCKPARENT,UIO_SYSSPACE,CAST_USER_ADDR_T(path),ctx);
    nd.ni_flag|=NAMEI_NOFOLLOW_ANY;nd.ni_cnd.cn_flags|=WILLBEDIR;
    int error=namei(&nd);if(error)return error;
    if(!root_type(nd.ni_dvp,VDIR)){error=EXDEV;goto out;}
    if(nd.ni_vp){
        VATTR_INIT(&va);VATTR_WANTED(&va,va_uid);VATTR_WANTED(&va,va_mode);
        error=root_type(nd.ni_vp,VDIR)?vnode_getattr(nd.ni_vp,&va,ctx):ENOTDIR;
        if(!error&&(!VATTR_IS_SUPPORTED(&va,va_uid)||!VATTR_IS_SUPPORTED(&va,va_mode)||va.va_uid||(va.va_mode&0022)))error=EPERM;
        goto out;
    }
    VATTR_INIT(&va);VATTR_SET(&va,va_type,VDIR);VATTR_SET(&va,va_mode,0755);
    VATTR_SET(&va,va_uid,0);VATTR_SET(&va,va_gid,0);
    error=vn_mkdir(nd.ni_dvp,&vp,&nd,&va,ctx);
    if(!error){
        vnode_update_identity(vp,nd.ni_dvp,nd.ni_cnd.cn_nameptr,nd.ni_cnd.cn_namelen,nd.ni_cnd.cn_hash,VNODE_UPDATE_NAME|VNODE_UPDATE_PARENT);
        error=VNOP_FSYNC(vp,MNT_WAIT,ctx);if(!error)error=VNOP_FSYNC(nd.ni_dvp,MNT_WAIT,ctx);
    }
    if(vp)vnode_put(vp);
out:lookup_done(&nd);return error;
}
/* B1L-0086: a directory anyone may write into (01777, like /tmp), for the Metal driver's shader dumps on SYSTEM */
static int b2l_open_directory(const char *path,vfs_context_t ctx)
{
    struct nameidata nd;struct vnode_attr va;vnode_t vp=NULL;
    NDINIT(&nd,CREATE,OP_MKDIR,LOCKPARENT,UIO_SYSSPACE,CAST_USER_ADDR_T(path),ctx);
    nd.ni_flag|=NAMEI_NOFOLLOW_ANY;nd.ni_cnd.cn_flags|=WILLBEDIR;
    int error=namei(&nd);if(error)return error;
    if(!root_type(nd.ni_dvp,VDIR)){error=EXDEV;goto out;}
    if(nd.ni_vp){error=root_type(nd.ni_vp,VDIR)?0:ENOTDIR;goto out;}
    VATTR_INIT(&va);VATTR_SET(&va,va_type,VDIR);VATTR_SET(&va,va_mode,01777);
    VATTR_SET(&va,va_uid,0);VATTR_SET(&va,va_gid,0);
    error=vn_mkdir(nd.ni_dvp,&vp,&nd,&va,ctx);
    if(!error){
        vnode_update_identity(vp,nd.ni_dvp,nd.ni_cnd.cn_nameptr,nd.ni_cnd.cn_namelen,nd.ni_cnd.cn_hash,VNODE_UPDATE_NAME|VNODE_UPDATE_PARENT);
        error=VNOP_FSYNC(vp,MNT_WAIT,ctx);if(!error)error=VNOP_FSYNC(nd.ni_dvp,MNT_WAIT,ctx);
    }
    if(vp)vnode_put(vp);
out:lookup_done(&nd);return error;
}
static int b2l_apps_sync_payload(vnode_t vp,const unsigned char *data,size_t bytes,
    unsigned mode,bool writing,vfs_context_t ctx)
{
    if(!bytes||bytes>4*1024*1024||!root_type(vp,VREG))return EINVAL;
    struct vnode_attr va;VATTR_INIT(&va);
    VATTR_WANTED(&va,va_uid);VATTR_WANTED(&va,va_mode);VATTR_WANTED(&va,va_nlink);VATTR_WANTED(&va,va_data_size);
    int error=vnode_getattr(vp,&va,ctx);if(error)return error;
    if(!VATTR_IS_SUPPORTED(&va,va_uid)||!VATTR_IS_SUPPORTED(&va,va_mode)||!VATTR_IS_SUPPORTED(&va,va_nlink)||!VATTR_IS_SUPPORTED(&va,va_data_size)||
       va.va_uid||(va.va_mode&0777)!=mode||va.va_nlink<1||va.va_nlink>(writing?1:2)||va.va_data_size<0||
       (uint64_t)va.va_data_size>bytes||(!writing&&(uint64_t)va.va_data_size!=bytes))return EINVAL;
    unsigned char check[256];size_t size=(size_t)va.va_data_size;
    for(size_t offset=0;offset<size;){
        size_t n=size-offset;if(n>sizeof(check))n=sizeof(check);int resid=0;
        error=vn_rdwr(UIO_READ,vp,(caddr_t)check,(int)n,(off_t)offset,UIO_SYSSPACE,0,kauth_cred_get(),&resid,current_proc());
        if(error||resid||!b2l_apps_payload_equal(data,bytes,offset,check,n,
            !writing&&mode==0755&&data==b2l_apps_executable))return error?error:EIO;
        offset+=n;
    }
    if(!writing)return 0;
    for(size_t offset=size;offset<bytes;){
        size_t n=bytes-offset;if(n>4096)n=4096;int resid=0;
        error=vn_rdwr(UIO_WRITE,vp,(caddr_t)data+offset,(int)n,(off_t)offset,UIO_SYSSPACE,IO_SYNC,kauth_cred_get(),&resid,current_proc());
        if(error||resid)return error?error:EIO;offset+=n;
    }
    error=VNOP_FSYNC(vp,MNT_WAIT,ctx);if(error)return error;
    return b2l_apps_sync_payload(vp,data,bytes,mode,false,ctx);
}
static int b2l_apps_publish(const char *final,const char *staging,const unsigned char *data,size_t bytes,unsigned mode,vfs_context_t ctx)
{
    vnode_t vp=NULL;int error=vnode_lookup(final,VNODE_LOOKUP_NOFOLLOW_ANY,&vp,ctx);
    if(!error){error=b2l_apps_sync_payload(vp,data,bytes,mode,false,ctx);vnode_put(vp);return error;}
    if(error!=ENOENT)return error;
    error=vnode_open(staging,FREAD|FWRITE|O_CREAT,mode,VNODE_LOOKUP_NOFOLLOW_ANY,&vp,ctx);
    if(error)return error;
    error=b2l_apps_sync_payload(vp,data,bytes,mode,true,ctx);
    if(!error){
        struct nameidata nd;NDINIT(&nd,CREATE,OP_LINK,LOCKPARENT,UIO_SYSSPACE,CAST_USER_ADDR_T(final),ctx);nd.ni_flag|=NAMEI_NOFOLLOW_ANY;
        error=namei(&nd);
        if(!error){
            if(nd.ni_vp)error=EEXIST;
            else if(!root_type(nd.ni_dvp,VDIR))error=EXDEV;
            else {error=VNOP_LINK(vp,nd.ni_dvp,&nd.ni_cnd,ctx);if(!error)error=VNOP_FSYNC(nd.ni_dvp,MNT_WAIT,ctx);}
            lookup_done(&nd);
        }
    }
    int close_error=vnode_close(vp,FREAD|FWRITE,ctx);return error?error:close_error;
}
#include "b2l_profile_cache.h"
#include "b2l_kbd_dictation.h"   /* B1L-0164 */
#include "b2l_apps_layout.h"
/* B1L-0031 (GPU G6): Mesa and the EAGL bridge, from SYSTEM:/b2l-gl-v9/ to /usr/local/lib/b2l-gl-v9/, once.
 * A file already there is never replaced (a new version takes a new directory). Kill switch: b2l_gl=0. */
extern int b2l_system_file_copy(const char *, int (*)(void *, uint64_t, const void *, size_t), void *, uint64_t *);
extern void b2l_bt_fetch(void);   /* B1L-0212: B2LBluetooth.cpp, Bluetooth patch RAM (bt.hcd) */
static int b2l_gl_sink(void *ctx, uint64_t off, const void *buf, size_t n)
{
    int resid = 0;
    int e = vn_rdwr(UIO_WRITE, (vnode_t)ctx, (caddr_t)(uintptr_t)buf, (int)n, (off_t)off, UIO_SYSSPACE, IO_NOAUTH, kauth_cred_get(), &resid, current_proc());
    return e ? e : (resid ? EIO : 0);
}
static int b2l_gl_publish_one(const char *src, const char *final, const char *staging, vfs_context_t ctx)
{
    vnode_t vp = NULL;
    uint64_t size = 0;
    if (b2l_layout_exists(final, ctx) == 1) { printf("B2L GL: %s present\n", final); return 0; }
    int error = vnode_open(staging, FREAD | FWRITE | O_CREAT | O_TRUNC, 0755, VNODE_LOOKUP_NOFOLLOW_ANY, &vp, ctx);
    if (error) return error;
    error = b2l_system_file_copy(src, b2l_gl_sink, vp, &size);
    if (!error) error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
    if (!error) {
        struct nameidata nd; NDINIT(&nd, CREATE, OP_LINK, LOCKPARENT, UIO_SYSSPACE, CAST_USER_ADDR_T(final), ctx); nd.ni_flag |= NAMEI_NOFOLLOW_ANY;
        error = namei(&nd);
        if (!error) {
            if (nd.ni_vp) error = EEXIST;
            else { error = VNOP_LINK(vp, nd.ni_dvp, &nd.ni_cnd, ctx); if (!error) error = VNOP_FSYNC(nd.ni_dvp, MNT_WAIT, ctx); }
            lookup_done(&nd);
        }
    }
    int ce = vnode_close(vp, FREAD | FWRITE, ctx);
    printf("B2L GL: %s -> %s (%llu bytes) error=%d\n", src, final, (unsigned long long)size, error ? error : ce);
    return error ? error : ce;
}
static void b2l_gl_publish(vfs_context_t ctx)
{
    int on = 0; PE_parse_boot_argn("b2l_gl", &on, sizeof(on));
    if (!on) { return; }
    int e = b2l_apps_directory("/usr/local", ctx);
    if (!e) e = b2l_apps_directory("/usr/local/lib", ctx);
    if (!e) e = b2l_apps_directory("/usr/local/lib/b2l-gl-v9", ctx);
    if (!e) e = b2l_gl_publish_one("b2l-gl-v9/libOSMesa.8.dylib", "/usr/local/lib/b2l-gl-v9/libOSMesa.8.dylib", "/usr/local/lib/b2l-gl-v9/.libOSMesa.8.dylib", ctx);
    if (!e) e = b2l_gl_publish_one("b2l-gl-v9/libb2l-gl.dylib", "/usr/local/lib/b2l-gl-v9/libb2l-gl.dylib", "/usr/local/lib/b2l-gl-v9/.libb2l-gl.dylib", ctx);
    printf("B2L GL: publication error=%d\n", e);
}
static void *b2l_seed_za(void *o __unused, unsigned n, unsigned s)
{
    if (n && s > (SIZE_MAX - 16) / n) return NULL;
    size_t len = (size_t)n * s + 16;
    uint8_t *p = kalloc_data(len, Z_WAITOK | Z_ZERO);
    if (!p) return NULL;
    *(size_t *)(void *)p = len;
    return p + 16;
}
static void b2l_seed_zf(void *o __unused, void *q)
{
    if (!q) return;
    uint8_t *p = (uint8_t *)q - 16;
    kfree_data(p, *(size_t *)(void *)p);
}
#define B2L_METAL_STREAM_CHUNK 65536u
#define B2L_METAL_MAX_RAW      (24u << 20)
static int b2l_metal_stream_io(vnode_t vp, const uint8_t *zdata, size_t zbytes, size_t raw_bytes, bool writing)
{
    if (!zdata || !zbytes || !raw_bytes || raw_bytes > B2L_METAL_MAX_RAW || zbytes > UINT_MAX) return EINVAL;
    uint8_t *outbuf = kalloc_data(B2L_METAL_STREAM_CHUNK, Z_WAITOK);
    uint8_t *chkbuf = writing ? NULL : kalloc_data(B2L_METAL_STREAM_CHUNK, Z_WAITOK);
    if (!outbuf || (!writing && !chkbuf)) {
        if (outbuf) kfree_data(outbuf, B2L_METAL_STREAM_CHUNK);
        if (chkbuf) kfree_data(chkbuf, B2L_METAL_STREAM_CHUNK);
        return ENOMEM;
    }
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.zalloc = (alloc_func)b2l_seed_za;
    zs.zfree = (free_func)b2l_seed_zf;
    int err = (inflateInit(&zs) == Z_OK) ? 0 : ENOMEM;
    if (!err) {
        zs.next_in = (Bytef *)(uintptr_t)zdata;
        zs.avail_in = (uInt)zbytes;
        size_t offset = 0;
        int zr = Z_OK;
        while (offset < raw_bytes) {
            size_t space = raw_bytes - offset;
            if (space > B2L_METAL_STREAM_CHUNK) space = B2L_METAL_STREAM_CHUNK;
            zs.next_out = outbuf;
            zs.avail_out = (uInt)space;
            zr = inflate(&zs, Z_NO_FLUSH);
            size_t have = space - zs.avail_out;
            if (have > 0) {
                for (size_t done = 0; done < have;) {
                    size_t step = have - done;
                    if (step > 4096) step = 4096;
                    int resid = 0;
                    if (writing) {
                        err = vn_rdwr(UIO_WRITE, vp, (caddr_t)(outbuf + done), (int)step, (off_t)(offset + done),
                            UIO_SYSSPACE, IO_SYNC, kauth_cred_get(), &resid, current_proc());
                        if (err || resid) { err = err ? err : EIO; break; }
                    } else {
                        err = vn_rdwr(UIO_READ, vp, (caddr_t)chkbuf, (int)step, (off_t)(offset + done),
                            UIO_SYSSPACE, 0, kauth_cred_get(), &resid, current_proc());
                        if (err || resid || memcmp(outbuf + done, chkbuf, step) != 0) { err = err ? err : EIO; break; }
                    }
                    done += step;
                }
                if (err) break;
                offset += have;
            }
            if (zr == Z_STREAM_END) break;
            if (zr != Z_OK || have == 0) { err = EIO; break; }
        }
        if (!err && offset == raw_bytes && zr != Z_STREAM_END) {
            uint8_t extra = 0;
            zs.next_out = &extra;
            zs.avail_out = 1;
            zr = inflate(&zs, Z_FINISH);
            if (zr != Z_STREAM_END || zs.avail_out != 1) err = EIO;
        }
        if (!err && (offset != raw_bytes || zr != Z_STREAM_END)) err = EIO;
        inflateEnd(&zs);
    }
    kfree_data(outbuf, B2L_METAL_STREAM_CHUNK);
    if (chkbuf) kfree_data(chkbuf, B2L_METAL_STREAM_CHUNK);
    return err;
}
static int b2l_metal_publish_zlib(const char *final, const char *staging, const uint8_t *zdata, size_t zbytes,
    size_t raw_bytes, unsigned mode, vfs_context_t ctx)
{
    vnode_t vp = NULL;
    int error = vnode_lookup(final, VNODE_LOOKUP_NOFOLLOW_ANY, &vp, ctx);
    if (!error) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_uid); VATTR_WANTED(&va, va_mode); VATTR_WANTED(&va, va_data_size);
        bool match = root_type(vp, VREG) && !vnode_getattr(vp, &va, ctx) &&
            VATTR_IS_SUPPORTED(&va, va_uid) && VATTR_IS_SUPPORTED(&va, va_mode) && VATTR_IS_SUPPORTED(&va, va_data_size) &&
            !va.va_uid && (va.va_mode & 0777) == mode && (uint64_t)va.va_data_size == raw_bytes &&
            !b2l_metal_stream_io(vp, zdata, zbytes, raw_bytes, false);
        vnode_put(vp);
        vp = NULL;
        if (match) return 0;
        error = vnode_open(final, FREAD | FWRITE | O_TRUNC, (int)mode, VNODE_LOOKUP_NOFOLLOW_ANY, &vp, ctx);
        if (error) return error;
        if (root_type(vp, VREG)) {
            error = b2l_metal_stream_io(vp, zdata, zbytes, raw_bytes, true);
            if (!error) error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
        } else {
            error = EINVAL;
        }
        int ce = vnode_close(vp, FREAD | FWRITE, ctx);
        return error ? error : ce;
    }
    if (error != ENOENT) return error;
    error = vnode_open(staging, FREAD | FWRITE | O_CREAT | O_TRUNC, (int)mode, VNODE_LOOKUP_NOFOLLOW_ANY, &vp, ctx);
    if (error) return error;
    error = b2l_metal_stream_io(vp, zdata, zbytes, raw_bytes, true);
    if (!error) error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
    if (!error) {
        struct nameidata nd;
        NDINIT(&nd, CREATE, OP_LINK, LOCKPARENT, UIO_SYSSPACE, CAST_USER_ADDR_T(final), ctx);
        nd.ni_flag |= NAMEI_NOFOLLOW_ANY;
        error = namei(&nd);
        if (!error) {
            if (nd.ni_vp) error = EEXIST;
            else if (!root_type(nd.ni_dvp, VDIR)) error = EXDEV;
            else {
                error = VNOP_LINK(vp, nd.ni_dvp, &nd.ni_cnd, ctx);
                if (!error) error = VNOP_FSYNC(nd.ni_dvp, MNT_WAIT, ctx);
            }
            lookup_done(&nd);
        }
    }
    int ce = vnode_close(vp, FREAD | FWRITE, ctx);
    return error ? error : ce;
}
/* B1L-0044: Metal M1. The plug-in bundle Metal loads by MetalPluginName (B2LMetalV3/B2LMetalv95, B2LMetalAccel.cpp).
 * Kill switch: boot-arg b2l_metal=0. */
static void b2l_metal_publish(vfs_context_t ctx)
{
    int on = 1; PE_parse_boot_argn("b2l_metal", &on, sizeof(on));
    if (!on) { printf("B2L METAL: disabled\n"); return; }
#if B2L_METAL_EMBEDDED
    int e = b2l_apps_directory("/System/Library/Extensions/B2LMetalv95.bundle", ctx);
    if (!e) e = b2l_apps_directory("/usr/local", ctx);
    if (!e) e = b2l_apps_directory("/usr/local/lib", ctx);
    if (!e) e = b2l_apps_directory("/usr/local/lib/b2l-metal-v95", ctx);
    if (!e) e = b2l_metal_publish_zlib("/System/Library/Extensions/B2LMetalv95.bundle/B2LMetalv95",
        "/System/Library/Extensions/B2LMetalv95.bundle/.B2LMetalv95",
        b2l_metal_bundle_bin_z, sizeof(b2l_metal_bundle_bin_z), b2l_metal_bundle_bin_raw_len, 0755, ctx);
    if (!e) e = b2l_metal_publish_zlib("/System/Library/Extensions/B2LMetalv95.bundle/Info.plist",
        "/System/Library/Extensions/B2LMetalv95.bundle/.Info.plist",
        b2l_metal_bundle_plist_z, sizeof(b2l_metal_bundle_plist_z), b2l_metal_bundle_plist_raw_len, 0644, ctx);
    if (!e) e = b2l_metal_publish_zlib("/usr/local/lib/b2l-metal-v95/libhakiosmetal.dylib",
        "/usr/local/lib/b2l-metal-v95/.libhakiosmetal.dylib",
        b2l_metal_libhakiosmetal_z, sizeof(b2l_metal_libhakiosmetal_z), b2l_metal_libhakiosmetal_raw_len, 0755, ctx);
    if (!e) e = b2l_metal_publish_zlib("/usr/local/lib/b2l-metal-v95/libhakiosair.dylib",
        "/usr/local/lib/b2l-metal-v95/.libhakiosair.dylib",
        b2l_metal_libhakiosair_z, sizeof(b2l_metal_libhakiosair_z), b2l_metal_libhakiosair_raw_len, 0755, ctx);
    if (!e) { int de = b2l_open_directory("/b2l-dump", ctx); printf("B2L METAL: /b2l-dump error=%d\n", de); }
    printf("B2L METAL: embedded publication error=%d\n", e);
#else
    int e = b2l_apps_directory("/System/Library/Extensions/B2LMetalV3.bundle", ctx);
    if (!e) e = b2l_gl_publish_one("b2l-metal-v5/B2LMetalV3", "/System/Library/Extensions/B2LMetalV3.bundle/B2LMetalV3",
        "/System/Library/Extensions/B2LMetalV3.bundle/.B2LMetalV3", ctx);
    if (!e) e = b2l_gl_publish_one("b2l-metal-v5/Info.plist", "/System/Library/Extensions/B2LMetalV3.bundle/Info.plist",
        "/System/Library/Extensions/B2LMetalV3.bundle/.Info.plist", ctx);
    if (!e) e = b2l_apps_directory("/usr/local", ctx);
    if (!e) e = b2l_apps_directory("/usr/local/lib", ctx);
    if (!e) e = b2l_apps_directory("/usr/local/lib/b2l-metal-v5", ctx);
    if (!e) e = b2l_gl_publish_one("b2l-metal-v5/libOSMesa.8.dylib", "/usr/local/lib/b2l-metal-v5/libOSMesa.8.dylib",
        "/usr/local/lib/b2l-metal-v5/.libOSMesa.8.dylib", ctx);   /* B1L-0048: Mesa + the Metal core (M2) */
    /* B1L-0051 (M3): Scion's Metal driver for the Mali and its test, beside our M2 test (scripts/build-metal-scion.sh) */
    if (!e) e = b2l_apps_directory("/usr/local/lib/b2l-metal-v95", ctx);
    /* B1L-0055: the v11 set below carries the test library too */
    static const char *const v7[] = { "libhakiosair.dylib", "libhakiosmetal.dylib", "libb2l-metal-drvtest.dylib", "libb2l-metal-test.dylib", "libb2l-metal-app.dylib" };   /* B1L-0079: M6a */
    for (unsigned i = 0; !e && i < sizeof(v7) / sizeof(v7[0]); i++) {
        char src[96], final[128], staging[128];
        snprintf(src, sizeof(src), "b2l-metal-v95/%s", v7[i]);
        snprintf(final, sizeof(final), "/usr/local/lib/b2l-metal-v95/%s", v7[i]);
        snprintf(staging, sizeof(staging), "/usr/local/lib/b2l-metal-v95/.%s", v7[i]);
        e = b2l_gl_publish_one(src, final, staging, ctx);
    }
    /* B1L-0080 (M6b): the plug-in over Scion's driver, chosen by B2LMetalAccel.cpp when b2l_metal_ui is on */
    if (!e) e = b2l_apps_directory("/System/Library/Extensions/B2LMetalv95.bundle", ctx);
    if (!e) e = b2l_gl_publish_one("b2l-metal-v95/B2LMetalv95", "/System/Library/Extensions/B2LMetalv95.bundle/B2LMetalv95",
        "/System/Library/Extensions/B2LMetalv95.bundle/.B2LMetalv95", ctx);
    if (!e) e = b2l_gl_publish_one("b2l-metal-v95/B2LMetalv95-Info.plist", "/System/Library/Extensions/B2LMetalv95.bundle/Info.plist",
        "/System/Library/Extensions/B2LMetalv95.bundle/.Info.plist", ctx);
    { int de = b2l_open_directory("/b2l-dump", ctx); printf("B2L METAL: /b2l-dump error=%d\n", de); }   /* B1L-0086 */
    printf("B2L METAL: publication error=%d\n", e);
#endif
}
/* B1L-0149: Broadcom WiFi firmware and NVRAM for B2LWifi.cpp, from SYSTEM:/b2l-wifi/ into kernel memory.
 * b2l_system_file_copy() is single-threaded and valid only before launchd, so the copy happens here;
 * B2LWifi's thread waits on b2l_wifi_fw_state (0 pending, 1 ready, <0 error) and frees with
 * b2l_wifi_fw_release(). Kill switch: b2l_wifi=0. */
#define B2L_WIFI_FW_CAP    (2u << 20)
#define B2L_WIFI_NVRAM_CAP (64u << 10)
#define B2L_WIFI_CLM_CAP   (64u << 10)   /* B1L-0151: CLM blob (optional) */
#define B2L_WIFI_CONF_CAP  (4u << 10)    /* B1L-0151: wifi.conf, ssid/psk/mac (optional) */
void *b2l_wifi_fw, *b2l_wifi_nvram, *b2l_wifi_clm, *b2l_wifi_conf;
size_t b2l_wifi_fw_len, b2l_wifi_nvram_len, b2l_wifi_clm_len, b2l_wifi_conf_len;
volatile int b2l_wifi_fw_state;
struct b2l_wifi_buf { uint8_t *p; size_t cap; };
static int b2l_wifi_sink(void *ctx, uint64_t off, const void *buf, size_t n)
{
    struct b2l_wifi_buf *b = ctx;
    if (off + n > b->cap) return EFBIG;
    memcpy(b->p + off, buf, n);
    return 0;
}
void b2l_wifi_fw_release(void)
{
    if (b2l_wifi_fw) kfree_data(b2l_wifi_fw, B2L_WIFI_FW_CAP);
    if (b2l_wifi_nvram) kfree_data(b2l_wifi_nvram, B2L_WIFI_NVRAM_CAP);
    if (b2l_wifi_clm) kfree_data(b2l_wifi_clm, B2L_WIFI_CLM_CAP);
    if (b2l_wifi_conf) { memset(b2l_wifi_conf, 0, B2L_WIFI_CONF_CAP); kfree_data(b2l_wifi_conf, B2L_WIFI_CONF_CAP); }
    b2l_wifi_fw = b2l_wifi_nvram = b2l_wifi_clm = b2l_wifi_conf = NULL;
}
static void b2l_wifi_fetch(void)
{
    int on = 1; PE_parse_boot_argn("b2l_wifi", &on, sizeof(on));
    if (!on) { b2l_wifi_fw_state = -1; return; }
    struct b2l_wifi_buf fw = { kalloc_data(B2L_WIFI_FW_CAP, Z_WAITOK | Z_ZERO), B2L_WIFI_FW_CAP };
    struct b2l_wifi_buf nv = { kalloc_data(B2L_WIFI_NVRAM_CAP, Z_WAITOK | Z_ZERO), B2L_WIFI_NVRAM_CAP };
    uint64_t fwLen = 0, nvLen = 0;
    int e = (fw.p && nv.p) ? 0 : ENOMEM;
    if (!e) e = b2l_system_file_copy("b2l-wifi/fw.bin", b2l_wifi_sink, &fw, &fwLen);
    if (!e) e = b2l_system_file_copy("b2l-wifi/nvram.txt", b2l_wifi_sink, &nv, &nvLen);
    b2l_wifi_fw = fw.p; b2l_wifi_nvram = nv.p;
    if (!e) {   /* optional files: a missing one leaves its length at 0 */
        struct b2l_wifi_buf clm = { kalloc_data(B2L_WIFI_CLM_CAP, Z_WAITOK | Z_ZERO), B2L_WIFI_CLM_CAP };
        struct b2l_wifi_buf cf = { kalloc_data(B2L_WIFI_CONF_CAP, Z_WAITOK | Z_ZERO), B2L_WIFI_CONF_CAP };
        uint64_t n = 0;
        b2l_wifi_clm = clm.p; b2l_wifi_conf = cf.p;
        if (clm.p && !b2l_system_file_copy("b2l-wifi/clm.blob", b2l_wifi_sink, &clm, &n)) b2l_wifi_clm_len = (size_t)n;
        n = 0;
        if (cf.p && !b2l_system_file_copy("b2l-wifi/wifi.conf", b2l_wifi_sink, &cf, &n) && n < B2L_WIFI_CONF_CAP) b2l_wifi_conf_len = (size_t)n;
        printf("B2L WIFI FW: fetch clm=%lu conf=%lu\n", (unsigned long)b2l_wifi_clm_len, (unsigned long)b2l_wifi_conf_len);
    }
    if (e) { b2l_wifi_fw_release(); b2l_wifi_fw_state = -e; }
    else { b2l_wifi_fw_len = (size_t)fwLen; b2l_wifi_nvram_len = (size_t)nvLen; __asm__ volatile("dmb ish" ::: "memory"); b2l_wifi_fw_state = 1; }
    printf("B2L WIFI FW: fetch fw=%llu nvram=%llu error=%d\n", fwLen, nvLen, e);
}
static void b2l_userspace_before_init(void)
{
    b2l_early_trace("B2L USERSPACE: before_init begin\n");
    int requested=0;
    PE_parse_boot_argn("b2l_userspace_bootstrap",&requested,sizeof(requested));
    if(requested!=1){
        b2l_early_trace("B2L USERSPACE: skipped (requested=%d)\n", requested);
        return;
    }
    mount_t mp = rootvnode ? vnode_mount(rootvnode) : NULL;
    if(!mp||!b2l_amfi_active()||vfs_isrdonly(mp)||strcmp(vfs_statfs(mp)->f_fstypename,"hfs")){
        b2l_early_trace("B2L USERSPACE: skipped (mp=%d amfi=%d)\n", mp != NULL, b2l_amfi_active());
        return;
    }
    b2l_early_trace("B2L USERSPACE: running bootstrap tasks...\n");
    vfs_context_t ctx=vfs_context_current();int error=b2l_profile_cache_refresh(ctx);
    printf("B2L PROFILE: MobileGestalt refresh error=%d\n",error);
    {
        int sb=1;PE_parse_boot_argn("b2l_sb_inject",&sb,sizeof(sb));
        int e=sb?b2l_apps_directory("/usr/local",ctx):0;
        if(sb&&!e)e=b2l_apps_directory("/usr/local/lib",ctx);
        if(sb&&!e)e=b2l_apps_publish("/usr/local/lib/b2l-springboard-v6.dylib","/usr/local/lib/.b2l-springboard-v6.dylib",b2l_sb_dylib,sizeof(b2l_sb_dylib),0755,ctx);
        printf("B2L SB: dylib publication error=%d\n",e);
    }
    b2l_cc_layout_reset(ctx);
    b2l_gl_publish(ctx);
    b2l_metal_publish(ctx);
    b2l_wifi_fetch();
    b2l_bt_fetch();
    {
        int bt=1;PE_parse_boot_argn("b2l_bt_ios",&bt,sizeof(bt));
        int e=bt?b2l_apps_directory("/usr/local",ctx):0;
        if(bt&&!e)e=b2l_apps_directory("/usr/local/lib",ctx);
        if(bt&&!e)e=b2l_apps_publish("/usr/local/lib/b2l-bluetool-v1.dylib","/usr/local/lib/.b2l-bluetool-v1.dylib",b2l_bt_tool_dylib,sizeof(b2l_bt_tool_dylib),0755,ctx);
        printf("B2L BLUETOOTH: dylib publication error=%d\n",e);
    }
    b2l_kbd_dictation_patch(ctx);   /* B1L-0164 */
    if(b2l_apps_layout(ctx)){
        b2l_early_trace("B2L APPS: system-app layout active\n");
        return;
    }
    error=0;
    static const char *const dirs[]={"/usr/local","/usr/local/libexec","/System/Library/LaunchDaemons","/private/var/db"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++)if((error=b2l_apps_directory(dirs[i],ctx)))goto out;
    error=b2l_apps_publish("/usr/local/libexec/b2l-system-app-bootstrap-v4","/usr/local/libexec/.b2l-system-app-bootstrap-v4",b2l_apps_executable,sizeof(b2l_apps_executable),0755,ctx);
    if(error)goto out;
    error=b2l_apps_publish("/System/Library/LaunchDaemons/dev.b2l.system-app-bootstrap-v4.plist","/System/Library/LaunchDaemons/.b2l-system-app-bootstrap-v4.plist",b2l_apps_job,sizeof(b2l_apps_job),0644,ctx);
    if(error)goto out;
    error=b2l_apps_publish("/private/var/db/b2l-launchd-bootstrap-v5.plist","/private/var/db/.b2l-launchd-bootstrap-v5",b2l_apps_launch_cache,sizeof(b2l_apps_launch_cache),0644,ctx);
    if(!error)error=VFS_SYNC(vnode_mount(rootvnode),MNT_WAIT,ctx);
    if(!error)b2l_apps_cache_ready=1;
out:
    b2l_early_trace("B2L APPS: bootstrap publication error=%d\n", error);
    printf("B2L APPS: native migration bootstrap publication error=%d\n",error);
}
