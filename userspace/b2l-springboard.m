// SPDX-License-Identifier: BSD-2-Clause
// b2l-springboard-v5.dylib, inserted into SpringBoard by the kernel
// (b2l_exec_graphics_diag.h; AMFI dyld policy call 90 for that pid only).
// PosterBoard has no working poster renderer on the CPU path, so the wallpaper
// windows stay black. Like libherosb in wardevsa/ios-a5xelte (herolte), this
// puts a plain placeholder behind them: a solid blue view at the bottom of
// every window whose class names a wallpaper. Window class names are logged
// once to /private/var/mobile/Library/Logs/b2l-springboard.log for the next
// iteration. Kill switch: boot-arg b2l_sb_inject=0 (the kernel then does not
// insert this library).
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <math.h>
#include <dlfcn.h>
#include <mach-o/getsect.h>

static const NSInteger kB2LTag = 0x42324c42;
static unsigned rounds;
static FILE *b2l_logfile;

static void b2l_log(NSString *s)
{
    if (!b2l_logfile) b2l_logfile = fopen("/private/var/mobile/Library/Logs/b2l-springboard.log", "a");
    if (b2l_logfile) { fprintf(b2l_logfile, "%s\n", s.UTF8String); fflush(b2l_logfile); }
}

static BOOL wants_placeholder(UIWindow *w)
{
    NSString *n = NSStringFromClass([w class]);
    NSString *r = w.rootViewController ? NSStringFromClass([w.rootViewController class]) : @"";
    return [n rangeOfString:@"Wallpaper" options:NSCaseInsensitiveSearch].location != NSNotFound ||
           [r rangeOfString:@"Wallpaper" options:NSCaseInsensitiveSearch].location != NSNotFound;
}

// v2 (B1L-0027): in a wallpaper window the view goes ON TOP of the poster's
// remote view (opaque black, which hid v1's view at index 0) and is brought
// back to the front every round; in SBHomeScreenWindow it goes behind
// everything, as libherosb does.
// v3 (B1L-0116): a real picture instead of the solid blue, so that blur, glass and transparency have something to
// show. Drawn once with CoreGraphics at the screen's size (no file to install): a diagonal gradient, soft colour
// blobs, waves and a grid of dots. Standard range (8 bits a channel): a wide-colour renderer would make an RGBA16Float
// image, a format the Mali driver of this port refuses at full screen.
// v4 (B1L-0125): the owner's photo, built into this library (section __DATA,__b2l_wall: a JPEG, sRGB, 8 bits a
// channel, already cropped to the panel's 1440x3040 by scripts/build-springboard-dylib.py). The generated picture
// below stays as the fallback when the section is missing or does not decode.
static UIImage *b2l_owner_wallpaper(void)
{
    Dl_info info;
    unsigned long size = 0;
    if (!dladdr((const void *)&b2l_owner_wallpaper, &info) || !info.dli_fbase) return nil;
    uint8_t *data = getsectiondata((const struct mach_header_64 *)info.dli_fbase, "__DATA", "__b2l_wall", &size);
    if (!data || size < 1024) return nil;
    UIImage *jpeg = [UIImage imageWithData:[NSData dataWithBytesNoCopy:data length:size freeWhenDone:NO]];
    UIImage *img = nil;
    if (jpeg) {
        /* B1L-0128: drawn once into an RGBA bitmap of the screen's size, standard range. Left as the JPEG, iOS hands
         * the render server a YCbCr (multi-plane) IOSurface decoded from it, which this port's Mali driver cannot
         * import: the wallpaper stayed black at B1L-0125. Aspect fill: the surplus is cut at the top and bottom. */
        CGSize screen = UIScreen.mainScreen.bounds.size;
        UIGraphicsImageRendererFormat *f = [UIGraphicsImageRendererFormat preferredFormat];
        f.opaque = YES;
        f.preferredRange = UIGraphicsImageRendererFormatRangeStandard;
        UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:screen format:f];
        img = [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
            (void)ctx;
            CGSize js = jpeg.size;
            CGFloat k = MAX(screen.width / js.width, screen.height / js.height);
            CGSize d = CGSizeMake(js.width * k, js.height * k);
            [jpeg drawInRect:CGRectMake((screen.width - d.width) / 2, (screen.height - d.height) / 2, d.width, d.height)];
        }];
    }
    b2l_log([NSString stringWithFormat:@"owner wallpaper: %lu bytes, %@", size,
        img ? [NSString stringWithFormat:@"%.0fx%.0f", img.size.width * img.scale, img.size.height * img.scale] : @"does not decode"]);
    return img;
}

static UIImage *b2l_wallpaper(CGSize size)
{
    static UIImage *img;
    if (img) return img;
    img = b2l_owner_wallpaper();
    if (img) return img;
    if (size.width < 1 || size.height < 1) size = UIScreen.mainScreen.bounds.size;
    UIGraphicsImageRendererFormat *f = [UIGraphicsImageRendererFormat preferredFormat];
    f.opaque = YES;
    f.preferredRange = UIGraphicsImageRendererFormatRangeStandard;
    UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:size format:f];
    img = [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
        CGContextRef c = ctx.CGContext;
        CGFloat W = size.width, H = size.height;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGFloat comps[] = { 0.13, 0.05, 0.32, 1,   0.04, 0.30, 0.50, 1,   0.93, 0.42, 0.22, 1 };
        CGFloat locs[] = { 0, 0.55, 1 };
        CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, 3);
        CGContextDrawLinearGradient(c, g, CGPointMake(0, 0), CGPointMake(W, H), 0);
        CGGradientRelease(g);
        const CGFloat blobs[][6] = {
            { 0.18, 0.12, 0.55, 1.00, 0.22, 0.55 }, { 0.88, 0.30, 0.45, 0.15, 0.90, 0.70 },
            { 0.25, 0.62, 0.55, 0.55, 0.30, 1.00 }, { 0.80, 0.86, 0.50, 1.00, 0.80, 0.20 },
            { 0.55, 0.42, 0.30, 0.20, 0.55, 1.00 },
        };
        for (unsigned i = 0; i < sizeof(blobs) / sizeof(blobs[0]); i++) {
            CGFloat bc[] = { blobs[i][3], blobs[i][4], blobs[i][5], 0.80, blobs[i][3], blobs[i][4], blobs[i][5], 0.0 };
            CGFloat bl[] = { 0, 1 };
            CGGradientRef rg = CGGradientCreateWithColorComponents(cs, bc, bl, 2);
            CGPoint p = CGPointMake(blobs[i][0] * W, blobs[i][1] * H);
            CGContextDrawRadialGradient(c, rg, p, 0, p, blobs[i][2] * W, 0);
            CGGradientRelease(rg);
        }
        CGContextSetLineWidth(c, 1.5);
        for (int k = 0; k < 14; k++) {
            CGFloat y0 = H * (0.05 + k * 0.07), amp = 18 + 6 * (k % 3), alpha = 0.10 + 0.03 * (k % 4);
            CGContextSetRGBStrokeColor(c, 1, 1, 1, alpha);
            CGContextMoveToPoint(c, 0, y0);
            for (CGFloat x = 0; x <= W; x += 4)
                CGContextAddLineToPoint(c, x, y0 + amp * sin(x / W * 6.2832 * 1.5 + k * 0.7));
            CGContextStrokePath(c);
        }
        for (CGFloat y = 24; y < H; y += 48)
            for (CGFloat x = 24 + ((int)(y / 48) % 2) * 24; x < W; x += 48) {
                CGContextSetRGBFillColor(c, 1, 1, 1, 0.10);
                CGContextFillEllipseInRect(c, CGRectMake(x - 2, y - 2, 4, 4));
            }
        CGColorSpaceRelease(cs);
    }];
    return img;
}

static void give_placeholder(UIWindow *w, BOOL on_top)
{
    UIView *v = [w viewWithTag:kB2LTag];
    if (v) {
        if (on_top && w.subviews.lastObject != v) [w bringSubviewToFront:v];
        return;
    }
    UIImageView *iv = [[UIImageView alloc] initWithFrame:w.bounds];
    iv.image = b2l_wallpaper(UIScreen.mainScreen.bounds.size);
    iv.contentMode = UIViewContentModeScaleAspectFill;
    iv.clipsToBounds = YES;
    v = iv;
    v.tag = kB2LTag;
    v.userInteractionEnabled = NO;
    v.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    v.backgroundColor = [UIColor colorWithRed:0.07 green:0.30 blue:0.78 alpha:1.0];   /* under the picture, if it fails */
    if (on_top) [w addSubview:v]; else [w insertSubview:v atIndex:0];
    b2l_log([NSString stringWithFormat:@"placeholder %@ in %@ (%@) %.0fx%.0f level %.1f",
        on_top ? @"on top" : @"behind", NSStringFromClass([w class]), NSStringFromClass([w.rootViewController class]),
        w.bounds.size.width, w.bounds.size.height, (double)w.windowLevel]);
}

static NSArray *all_windows(void)
{
    SEL s = NSSelectorFromString(@"allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    if ([UIWindow respondsToSelector:s]) {
        NSArray *(*f)(id, SEL, BOOL, BOOL) = (void *)objc_msgSend;
        return f([UIWindow class], s, YES, NO);
    }
    return @[];
}

static void look(void)
{
    rounds++;
    NSArray *ws = all_windows();
    if (rounds == 1 || rounds == 10 || rounds == 60) {
        NSMutableString *m = [NSMutableString stringWithFormat:@"round %u: %lu windows:", rounds, (unsigned long)ws.count];
        for (UIWindow *w in ws) [m appendFormat:@" %@/%@", NSStringFromClass([w class]), NSStringFromClass([w.rootViewController class])];
        b2l_log(m);
    }
    for (UIWindow *w in ws) {
        if (wants_placeholder(w)) give_placeholder(w, YES);
        else if ([NSStringFromClass([w class]) isEqualToString:@"SBHomeScreenWindow"]) give_placeholder(w, NO);
    }
    double next = rounds < 120 ? 1.0 : 10.0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(next * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ look(); });
}

// Control Center (B1L-0027): every module's metadata requires "arm64e", which
// MobileGestalt does not answer here (CPUArchitecture "arm64"), so
// CCSModuleRepository finds none loadable and the default layout is emptied
// ("Remove invalid module ... for module not loadable"). The emulated CPU does
// run arm64e code, so the requirement is dropped from what the metadata says.
static NSSet *(*caps_was)(id, SEL);
static NSSet *caps_without_arm64e(id self, SEL cmd)
{
    NSSet *s = caps_was(self, cmd);
    if (![s containsObject:@"arm64e"]) return s;
    NSMutableSet *m = [s mutableCopy];
    [m removeObject:@"arm64e"];
    return m;
}
static void hook_module_caps(void)
{
    static BOOL done;
    if (done) return;
    Class c = objc_getClass("CCSModuleMetadata");
    Method meth = c ? class_getInstanceMethod(c, NSSelectorFromString(@"requiredDeviceCapabilities")) : NULL;
    if (!meth) return;
    caps_was = (NSSet *(*)(id, SEL))method_setImplementation(meth, (IMP)caps_without_arm64e);
    done = YES;
    b2l_log(@"CCSModuleMetadata requiredDeviceCapabilities: arm64e dropped");
}
static void (*sb_layout_was)(id, SEL);
static BOOL sb_shift_trailing(UIView *v, UIView *root, NSMutableString *dump, int depth)
{
    BOOL shifted = NO;
    CGRect f = [v convertRect:v.bounds toView:root];
    if (dump && depth <= 4) {
        [dump appendFormat:@" [%d:%@(%.0f,%.0f %.0fx%.0f)]", depth, NSStringFromClass([v class]),
            f.origin.x, f.origin.y, f.size.width, f.size.height];
    }
    if (v != root && f.origin.x >= 180.0 && CGRectGetMaxX(f) >= 310.0 && f.size.width > 0.0 && f.size.width <= 170.0) {
        CGRect inWin = v.window ? [v convertRect:v.bounds toView:v.window] : f;
        if (inWin.origin.y < 55.0) {
            v.transform = CGAffineTransformMakeTranslation(-46.0, 0.0);
            return YES;
        }
    }
    for (UIView *sub in v.subviews) {
        if (sb_shift_trailing(sub, root, dump, depth + 1)) shifted = YES;
    }
    if (v != root && !shifted && !CGAffineTransformIsIdentity(v.transform) && fabs(v.transform.tx + 46.0) < 0.5) {
        v.transform = CGAffineTransformIdentity;
    }
    return shifted;
}
static void sb_layout_subviews(UIView *self, SEL cmd)
{
    if (sb_layout_was) sb_layout_was(self, cmd);
    static int dumps;
    NSMutableString *dump = (dumps < 3 && self.bounds.size.width > 200.0 && self.subviews.count > 0)
        ? [NSMutableString stringWithFormat:@"statusBar layout %.0fx%.0f:", self.bounds.size.width, self.bounds.size.height]
        : nil;
    BOOL moved = sb_shift_trailing(self, self, dump, 0);
    if (dump) {
        dumps++;
        b2l_log([NSString stringWithFormat:@"%@ moved=%d", dump, moved]);
    }
}
static Class (*vp_was)(id, SEL, id, id);
static Class vp_override(id self, SEL cmd, id screen, id info)
{
    Class orig = vp_was ? vp_was(self, cmd, screen, info) : Nil;
    NSString *origName = orig ? NSStringFromClass(orig) : @"<nil>";
    static BOOL logged;
    if (!orig || [origName rangeOfString:@"Fallback"].location != NSNotFound) {
        static const char *const candidates[] = {
            "STUIStatusBarVisualProvider_Split1080",
            "STUIStatusBarVisualProvider_Split1125",
            "STUIStatusBarVisualProvider_Split1170",
            "STUIStatusBarVisualProvider_LegacyPhone",
            "_UIStatusBarVisualProvider_Split1080",
            "_UIStatusBarVisualProvider_Split1125",
            "_UIStatusBarVisualProvider_LegacyPhone",
        };
        for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            Class alt = objc_getClass(candidates[i]);
            if (alt && alt != orig) {
                if (!logged) {
                    logged = YES;
                    b2l_log([NSString stringWithFormat:@"statusBar visualProvider: %@ -> %s", origName, candidates[i]]);
                }
                return alt;
            }
        }
    }
    if (!logged) {
        logged = YES;
        b2l_log([NSString stringWithFormat:@"statusBar visualProvider kept: %@", origName]);
    }
    return orig;
}
static void hook_status_bar(void)
{
    static BOOL done;
    if (done) return;
    SEL sel = NSSelectorFromString(@"visualProviderSubclassForScreen:visualProviderInfo:");
    Class c = objc_getClass("STUIStatusBarVisualProvider_Phone");
    if (!c) c = objc_getClass("_UIStatusBarVisualProvider_Phone");
    Method meth = c ? class_getClassMethod(c, sel) : NULL;
    if (!meth) return;
    vp_was = (Class (*)(id, SEL, id, id))method_setImplementation(meth, (IMP)vp_override);
    done = YES;
    b2l_log([NSString stringWithFormat:@"%@ visualProviderSubclassForScreen: hooked", NSStringFromClass(c)]);
    Class sbCls = objc_getClass("STUIStatusBar");
    if (!sbCls) sbCls = objc_getClass("_UIStatusBar");
    if (sbCls) {
        Method lm = class_getInstanceMethod(sbCls, @selector(layoutSubviews));
        if (lm) {
            sb_layout_was = (void (*)(id, SEL))method_setImplementation(lm, (IMP)sb_layout_subviews);
            b2l_log([NSString stringWithFormat:@"%@ layoutSubviews: hooked", NSStringFromClass(sbCls)]);
        }
    }
    Class split = objc_getClass("STUIStatusBarVisualProvider_Split1080");
    if (!split) split = objc_getClass("_UIStatusBarVisualProvider_Split1080");
    if (split) {
        NSMutableString *ms = [NSMutableString stringWithFormat:@"%@ methods:", NSStringFromClass(split)];
        unsigned mlogged = 0;
        for (Class k = split; k && mlogged < 35; k = class_getSuperclass(k)) {
            const char *kn = class_getName(k);
            if (!strstr(kn, "StatusBarVisualProvider")) break;
            unsigned int mc = 0;
            Method *ml = class_copyMethodList(k, &mc);
            for (unsigned int i = 0; i < mc && mlogged < 35; i++) {
                const char *sn = sel_getName(method_getName(ml[i]));
                if (strstr(sn, "Edge") || strstr(sn, "Inset") || strstr(sn, "Corner") ||
                    strstr(sn, "Margin") || strstr(sn, "Offset") || strstr(sn, "Width") || strstr(sn, "Padding")) {
                    [ms appendFormat:@" %s", sn];
                    mlogged++;
                }
            }
            if (ml) free(ml);
        }
        b2l_log(ms);
    }
    unsigned int cnt = 0;
    Class *list = objc_copyClassList(&cnt);
    if (list) {
        NSMutableString *vp = [NSMutableString stringWithString:@"statusBar classes:"];
        unsigned logged_cnt = 0;
        for (unsigned int i = 0; i < cnt && logged_cnt < 30; i++) {
            const char *nm = class_getName(list[i]);
            if (nm && strstr(nm, "StatusBarVisualProvider")) {
                [vp appendFormat:@" %s", nm];
                logged_cnt++;
            }
        }
        b2l_log(vp);
        free(list);
    }
}
static void image_added(const struct mach_header *mh, intptr_t slide)
{
    (void)mh; (void)slide;
    hook_module_caps();
    hook_status_bar();
}

__attribute__((constructor)) static void b2l_springboard_init(void)
{
    b2l_log(@"b2l-springboard-v8 loaded");
    hook_module_caps();
    hook_status_bar();
    _dyld_register_func_for_add_image(image_added);
    if ([[[NSBundle mainBundle] bundleIdentifier] isEqualToString:@"com.apple.springboard"]) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ look(); });
    }
}
