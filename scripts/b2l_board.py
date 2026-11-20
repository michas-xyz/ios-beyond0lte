# SPDX-License-Identifier: BSD-2-Clause
"""Board selection shared by the packaging and recovery scripts.

B2L_BOARD selects the Samsung board; the default keeps the original S10+
behaviour. The XNU kernel and EFI loader are identical on both boards: only
the external UEFI platform, the appended DTB and the identity guards differ.
"""
import os

BOARDS = {
    # Galaxy S10+ reference board.
    'beyond2lte': {'model': 'SM-G975F', 'userdata': '/dev/block/sda31'},
    # Galaxy S10. Same Exynos 9820 board family, DT revision 26: reserved
    # memory, HSI2C10 touch, GPA keys and panel controller match beyond2lte.
    'beyond1lte': {'model': 'SM-G973F', 'userdata': '/dev/block/sda34'},
    # Galaxy S10e. Exynos 9820, DT revision 26, 128GB UFS storage.
    'beyond0lte': {'model': 'SM-G970F', 'userdata': '/dev/block/sda34'},
}


def board():
    name = os.environ.get('B2L_BOARD', 'beyond2lte')
    if name not in BOARDS:
        raise SystemExit(f'unknown B2L_BOARD {name!r}; expected one of {sorted(BOARDS)}')
    return name


def model(name=None):
    return BOARDS[name or board()]['model']


def userdata(name=None):
    return os.environ.get('B2L_USERDATA', BOARDS[name or board()]['userdata'])


def platform_package(name=None):
    return f'{name or board()}Pkg'


def identity_ok(getprop, name=None):
    """Accept the recovery only when its hardware model matches the board.

    ro.boot.em.model comes from the bootloader. ro.product.device can be
    rewritten by custom ROMs (ArtisanROM reports r13s), so it is accepted
    only as the legacy S10+ identity.
    """
    name = name or board()
    if getprop('ro.boot.em.model') == model(name):
        return True
    return name == 'beyond2lte' and getprop('ro.product.device') == 'beyond2lte'
