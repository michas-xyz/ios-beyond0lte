#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Derive the Galaxy S10 (beyond1lte) or S10e (beyond0lte) UEFI platform from the S10+ overlay.

Run on a disposable Mu-Silicium tree that already contains the beyond2lte
overlay. The hardware sources (memory map, UFS, HSI2C touch, keypad) are kept
unchanged: on DT revision 26 the S10, S10e and S10+ share reserved memory, UFS,
HSI2C10/sec_ts, GPA keys, SPEEDY/PMIC and DECON nodes. Only the platform
identity, the appended DTB and the boot configuration name differ.

The upstream beyond1ltePkg is replaced: its DXE heap overlaps the 0xCA000000
display reservation, which the S10+ memory map already corrects.
"""
import argparse
import os
from pathlib import Path
import shutil
import struct

# Upstream Mu-Silicium identities and generated beyond0lte identity
B0L_PLATFORM_GUID = '4F92A8E1-2856-4BD3-A0A1-3C389E8D970F'
B1L_PLATFORM_GUID = '4E670446-C67E-4FEB-8D15-A0A05C824B80'
B2L_PLATFORM_GUID = '556BDA6E-EBC8-4D8F-8EA6-DAA2D7AD7A68'

BOARD_SPECS = {
    'beyond1lte': {
        'package': 'beyond1ltePkg',
        'guid': B1L_PLATFORM_GUID,
        'smbios': (
            ('"Galaxy S10+"', '"Galaxy S10"'),
            ('"Galaxy_S10+_beyond2lte"', '"Galaxy_S10_beyond1lte"')
        ),
        'dtb_magic': b'BEYOND1LTE',
        'display_name': 'Galaxy S10',
        'fb_width': 1440,
        'fb_height': 3040,
    },
    'beyond0lte': {
        'package': 'beyond0ltePkg',
        'guid': B0L_PLATFORM_GUID,
        'smbios': (
            ('"Galaxy S10+"', '"Galaxy S10e"'),
            ('"Galaxy_S10+_beyond2lte"', '"Galaxy_S10e_beyond0lte"')
        ),
        'dtb_magic': b'BEYOND0LTE',
        'display_name': 'Galaxy S10e',
        # S10e native panel geometry: 1080x2280 AMOLED (s6e3fa7).
        # Pitch is 1080 pixels (4320 bytes), matching 3x 360-pixel hardware window slices.
        'fb_width': 1080,
        'fb_height': 2280,
    },
}

TEXT_SUFFIXES = {'.c', '.h', '.S', '.inf', '.inc', '.dsc', '.fdf', '.dec', '.py'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--board', choices=sorted(BOARD_SPECS.keys()),
                   default=os.environ.get('B2L_BOARD', 'beyond1lte'),
                   help='target board to generate (default: beyond1lte or $B2L_BOARD)')
    p.add_argument('--mu', type=Path, required=True, help='disposable Mu-Silicium tree with the beyond2lte overlay')
    p.add_argument('--dtb', type=Path, required=True, help='merged target DTB (dtb.img base + dtbo revision 26)')
    a = p.parse_args()

    board = a.board
    spec = BOARD_SPECS[board]
    package = spec['package']

    root = a.mu.resolve()
    if not (root / 'Mu_Basecore').is_dir():
        p.error('--mu must be a Mu-Silicium build tree')
    source = root / 'Platforms/Samsung/beyond2ltePkg'
    target = root / f'Platforms/Samsung/{package}'
    dsc = source / 'beyond2lte.dsc'
    if not dsc.is_file() or B2L_PLATFORM_GUID not in dsc.read_text():
        p.error('the beyond2lte overlay is not applied to this tree')
    dtb = a.dtb.read_bytes()
    if len(dtb) < 40 or struct.unpack_from('>I', dtb)[0] != 0xd00dfeed:
        p.error('--dtb is not a flattened device tree')
    if spec['dtb_magic'] not in dtb:
        p.error(f'--dtb does not describe {board}')

    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(source, target)
    for name in ('dsc', 'fdf', 'dec'):
        (target / f'beyond2lte.{name}').rename(target / f'{board}.{name}')
    for path in target.rglob('*'):
        if not path.is_file() or path.suffix not in TEXT_SUFFIXES:
            continue
        text = path.read_text()
        new = text.replace('beyond2ltePkg', package).replace('samsung-beyond2lte', f'samsung-{board}')
        # Package files renamed above (dsc, fdf, dec) are referenced by path.
        new = new.replace(f'{package}/beyond2lte.', f'{package}/{board}.')
        if path.name == f'{board}.dsc':
            new = new.replace(B2L_PLATFORM_GUID, spec['guid'])
            new = new.replace('PLATFORM_NAME                  = beyond2lte', f'PLATFORM_NAME                  = {board}')
            new = new.replace('Build/beyond2lte', f'Build/{board}')
            new = new.replace('|"beyond2lte"', f'|"{board}"')
            if 'fb_width' in spec:
                new = new.replace('gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1440',
                                  f'gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|{spec["fb_width"]}')
            if 'fb_height' in spec:
                new = new.replace('gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|3040',
                                  f'gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|{spec["fb_height"]}')
            for old, replacement in spec['smbios']:
                new = new.replace(old, replacement)
            new += '\n[BuildOptions]\n  *_*_AARCH64_CC_FLAGS = -Wno-error=unused-but-set-variable -Wno-error=unused-variable\n'
        if path.name == 'B2LGoster.c':
            new = new.replace('UINT32                 Parca;', 'UINT32                 Parca __attribute__((unused));')
            new = new.replace('Parca++;', '// Parca++;')
        if path.name == 'KeypadDeviceLib.c':
            old_gpio_loop = (
                '  if (mGpioProtocol == NULL) {\n'
                '    return EFI_SUCCESS;\n'
                '  }\n\n'
                '  for (UINTN Index = 0; Index < (sizeof(KeyList) / sizeof(KeyList[0])); Index++) {\n'
                '    KEY_CONTEXT_PRIVATE *Context = KeyList[Index];\n\n'
                '    Status = mGpioProtocol->GetState (Context->BankId, Context->BankNumber, Context->Pin, &IsPressed);\n'
                '    if (EFI_ERROR (Status)) {\n'
                '      return Status;\n'
                '    }\n\n'
                '    LibKeyUpdateKeyStatus (&Context->EfiKeyContext, KeypadReturnApi, !IsPressed, Delta);\n'
                '  }'
            )
            new_gpio_loop = (
                '  UINT32 Gpa0 = *(volatile UINT32 *)(UINTN)0x15850004;\n'
                '  UINT32 Gpa2 = *(volatile UINT32 *)(UINTN)0x15850044;\n'
                '  BOOLEAN VolUp = !(Gpa0 & (1u << 3));\n'
                '  BOOLEAN VolDown = !(Gpa0 & (1u << 4));\n'
                '  BOOLEAN Bixby = !(Gpa0 & (1u << 6));\n'
                '  BOOLEAN Power = !(Gpa2 & (1u << 4));\n'
                '  LibKeyUpdateKeyStatus (&KeyContextVolumeUp.EfiKeyContext, KeypadReturnApi, VolUp, Delta);\n'
                '  LibKeyUpdateKeyStatus (&KeyContextVolumeDown.EfiKeyContext, KeypadReturnApi, VolDown, Delta);\n'
                '  LibKeyUpdateKeyStatus (&KeyContextPower.EfiKeyContext, KeypadReturnApi, Power, Delta);\n'
                '  LibKeyUpdateKeyStatus (&KeyContextSpecial.EfiKeyContext, KeypadReturnApi, Bixby, Delta);'
            )
            if old_gpio_loop in text:
                new = new.replace(old_gpio_loop, new_gpio_loop)
        if path.name == 'DeviceBuild.py':
            new = new.replace("'beyond2lte'", f"'{board}'").replace('"beyond2lte"', f'"{board}"')
        if new != text:
            path.write_text(new)

    dsc_text = (target / f'{board}.dsc').read_text()
    for needle in (spec['guid'], f'PLATFORM_NAME                  = {board}',
                   f'{package}/{board}.fdf'):
        if needle not in dsc_text:
            p.error(f'{board}.dsc is missing {needle!r}')
    for _, smbios_val in spec['smbios']:
        if smbios_val not in dsc_text:
            p.error(f'{board}.dsc is missing SMBIOS string {smbios_val!r}')
    if 'fb_width' in spec:
        needle = f'PcdFrameBufferWidth|{spec["fb_width"]}'
        if needle not in dsc_text:
            p.error(f'{board}.dsc is missing {needle!r}')
    if 'fb_height' in spec:
        needle = f'PcdFrameBufferHeight|{spec["fb_height"]}'
        if needle not in dsc_text:
            p.error(f'{board}.dsc is missing {needle!r}')

    leftovers = [str(path.relative_to(target)) for path in target.rglob('*')
                 if path.is_file() and path.suffix in {'.dsc', '.fdf', '.inf', '.inc', '.py'}
                 and 'beyond2lte' in path.read_text()]
    if leftovers:
        p.error('beyond2lte references remain in build files: ' + ', '.join(leftovers))

    configs = root / 'Resources/Configs'
    if (configs / 'beyond2lte.toml').exists():
        shutil.copyfile(configs / 'beyond2lte.toml', configs / f'{board}.toml')
    dtb_dir = root / 'Resources/DTBs'
    dtb_dir.mkdir(parents=True, exist_ok=True)
    (dtb_dir / f'{board}.dtb').write_bytes(dtb)
    stale = dtb_dir / f'{board}.dts'
    if stale.exists():
        stale.unlink()
    print(f'Prepared {target} from the beyond2lte overlay; DTB {len(dtb)} bytes')


if __name__ == '__main__':
    main()
