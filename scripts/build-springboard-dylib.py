#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Build b2l-springboard-v9.dylib (arm64e) and its kernel seed header."""
from pathlib import Path
import argparse,hashlib,subprocess,tempfile,struct
r=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
sdk_ver=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-version'],text=True).strip()
with tempfile.TemporaryDirectory(prefix='b2l-sb-') as td:
 # B1L-0125: the owner's photo, cropped to the panel (1440x3040, centred: the surplus goes at the top and bottom)
 # and re-encoded as an 8-bit sRGB JPEG, linked into the library as __DATA,__b2l_wall
 wall=r/'userspace/wallpaper/fond-proprietaire.jpg'; wallargs=[]
 if wall.exists():
  out=Path(td)/'wall.jpg'
  subprocess.run(['sips','--cropToHeightWidth','3040','1440','-s','format','jpeg','-s','formatOptions','88',str(wall),'--out',str(out)],check=True,capture_output=True)
  wallargs=['-Wl,-sectcreate,__DATA,__b2l_wall,'+str(out)]
  print('wallpaper:',out.stat().st_size,'bytes')
 else:
  print('wallpaper: none (the generated picture stays)')
 lib=Path(td)/'b2l-springboard-v9.dylib'
 subprocess.run(['xcrun','--sdk','iphoneos','clang','-target',f'arm64e-apple-ios{sdk_ver}','-isysroot',sdk,'-Os','-fobjc-arc','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
  '-dynamiclib','-install_name','/usr/local/lib/b2l-springboard-v9.dylib','-framework','Foundation','-framework','UIKit','-framework','CoreGraphics',str(r/'userspace/b2l-springboard.m'),'-o',str(lib)]+wallargs,check=True)
 subprocess.run(['/usr/bin/codesign','--force','--sign','-','--identifier','dev.b2l.springboard',str(lib)],check=True)
 subprocess.run(['/usr/bin/codesign','--verify',str(lib)],check=True)
 data=lib.read_bytes()
cputype,subtype,ftype=struct.unpack_from('<III',data,4)
if cputype!=0x100000c or subtype&0xffffff!=2 or ftype!=6:raise SystemExit('not an arm64e dylib')
if len(data)>2*1024*1024:raise SystemExit('dylib too large')   # B1L-0125: with the photo
a.output.write_text('/* Generated: b2l-springboard-v9.dylib (userspace/b2l-springboard.m). */\nstatic const unsigned char b2l_sb_dylib[] = {\n'+''.join(' '+','.join('0x%02x'%x for x in data[i:i+20])+',\n' for i in range(0,len(data),20))+'};\n')
print('Built b2l-springboard-v9.dylib:',len(data),'bytes; sha256',hashlib.sha256(data).hexdigest())
