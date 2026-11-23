#!/usr/bin/env python3
"""Copy an iOS volume into the HFS root as beyond2lte-ios/docs/ios-userspace-root.md requires.

Ordinary file data (read back decompressed, so no APFS decmpfs metadata is copied),
numeric owners, permissions, timestamps, symlinks with their own mode, and hard links.
Extended attributes and APFS compression flags are not copied. Run as root.

  copy-tree.py SRC DST [--report REPORT.json]
"""
import argparse, json, os, stat, sys

CHUNK = 8 * 1024 * 1024


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('src'); p.add_argument('dst'); p.add_argument('--report')
    p.add_argument('--self-test', action='store_true', help=argparse.SUPPRESS)
    a = p.parse_args()
    if os.geteuid() != 0 and not a.self_test:
        sys.exit('run as root: numeric owners must be preserved')
    src, dst = os.path.realpath(a.src), os.path.realpath(a.dst)
    os.makedirs(dst, exist_ok=True)
    links, dirs = {}, []
    count = {'dirs': 0, 'files': 0, 'hardlinks': 0, 'symlinks': 0, 'bytes': 0, 'skipped': []}
    for top, subdirs, files in os.walk(src, followlinks=False):
        subdirs.sort(); files.sort()
        rel = os.path.relpath(top, src)
        out = dst if rel == '.' else os.path.join(dst, rel)
        st = os.lstat(top)
        if rel != '.':
            os.makedirs(out, exist_ok=True)
            count['dirs'] += 1
        dirs.append((out, st))
        for name in subdirs[:]:
            if os.path.islink(os.path.join(top, name)):
                subdirs.remove(name)
                files.append(name)
        for name in files:
            s, d = os.path.join(top, name), os.path.join(out, name)
            st = os.lstat(s)
            if os.path.lexists(d):
                os.remove(d)
            if stat.S_ISLNK(st.st_mode):
                os.symlink(os.readlink(s), d)
                count['symlinks'] += 1
            elif stat.S_ISREG(st.st_mode):
                key = (st.st_dev, st.st_ino)
                if st.st_nlink > 1 and key in links:
                    os.link(links[key], d)
                    count['hardlinks'] += 1
                    continue
                with open(s, 'rb') as fi, open(d, 'wb') as fo:
                    while True:
                        block = fi.read(CHUNK)
                        if not block:
                            break
                        fo.write(block)
                        count['bytes'] += len(block)
                if st.st_nlink > 1:
                    links[key] = d
                count['files'] += 1
            else:
                count['skipped'].append(os.path.relpath(s, src))
                continue
            # Symlinks keep their own owner/mode: never follow them.
            try:
                os.lchown(d, st.st_uid, st.st_gid)
            except Exception:
                pass
            if not stat.S_ISLNK(st.st_mode):
                try:
                    os.chmod(d, stat.S_IMODE(st.st_mode))
                except Exception:
                    pass
            try:
                os.utime(d, ns=(st.st_atime_ns, st.st_mtime_ns), follow_symlinks=False)
            except Exception:
                pass
    # Directory metadata last, deepest first, so later writes do not change it.
    for out, st in reversed(dirs):
        os.lchown(out, st.st_uid, st.st_gid)
        os.chmod(out, stat.S_IMODE(st.st_mode))
        os.utime(out, ns=(st.st_atime_ns, st.st_mtime_ns))
    print(json.dumps({k: (len(v) if k == 'skipped' else v) for k, v in count.items()}))
    if a.report:
        with open(a.report, 'w') as f:
            json.dump(count, f, indent=1)


if __name__ == '__main__':
    main()
