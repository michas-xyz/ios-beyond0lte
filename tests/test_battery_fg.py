#!/usr/bin/env python3
"""Host-side validation of B2LBattery MAX77705 fuel gauge decoding."""
from pathlib import Path
import subprocess, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / 'xnu-overlay/iokit/Kernel/arm/B2LBattery.cpp').read_text()
FN = SRC[SRC.index('static inline void b2l_fg_decode('):SRC.index('class B2LBattery :')]


class BatteryFuelGaugeTest(unittest.TestCase):
    def test_decode_samples(self):
        harness = r'''
#include <cassert>
#include <cstdint>
''' + FN + r'''
int main() {
    unsigned soc = 0, mv = 0, rep = 0, full = 0, cyc = 0;
    int ma = 0;
    bool chg = false, ext = false;

    // Live dump sample from beyond0lte at 3% charging: 0x06=0x033c, 0x09=0xbeec, 0x0b=0x06be, 0x05=0x0061, 0x10=0x0bb8
    b2l_fg_decode(0x033c, 0xbeec, (int16_t)0x06be, 0x0061, 0x0bb8, 0x0003,
                  &soc, &mv, &ma, &rep, &full, &cyc, &chg, &ext);
    assert(soc == 3 && mv == 3818 && ma == 539 && rep == 97 && full == 3000 && cyc == 3 && chg && ext);

    // Live dump sample at 1% charging: 0x06=0x01ce, 0x09=0xbb4e, 0x0b=0x06d8
    b2l_fg_decode(0x01ce, 0xbb4e, (int16_t)0x06d8, 0x0036, 0x0bb8, 0x0003,
                  &soc, &mv, &ma, &rep, &full, &cyc, &chg, &ext);
    assert(soc == 1 && mv == 3746 && ma == 547 && rep == 54 && full == 3000 && chg && ext);

    // Discharging at 64% (-420 mA -> -1344 raw = 0xfac0)
    b2l_fg_decode(0x4080, 0xc800, (int16_t)0xfac0, 1920, 3000, 12,
                  &soc, &mv, &ma, &rep, &full, &cyc, &chg, &ext);
    assert(soc == 64 && mv == 4000 && ma == -420 && !chg && !ext);

    // Full on charger (100%, +50 mA)
    b2l_fg_decode(0x6400, 0xd800, 160, 3000, 3000, 12,
                  &soc, &mv, &ma, &rep, &full, &cyc, &chg, &ext);
    assert(soc == 100 && !chg && ext);

    // Over-100% clamp & 0.5% non-zero floor at healthy voltage
    b2l_fg_decode(0x6900, 0xd800, 0, 3100, 3000, 0,
                  &soc, &mv, &ma, &rep, &full, &cyc, &chg, &ext);
    assert(soc == 100);
    b2l_fg_decode(0x0080, 0xba00, -100, 10, 3000, 0,
                  &soc, &mv, &ma, &rep, &full, &cyc, &chg, &ext);
    assert(soc == 1 && !chg && !ext);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='b2l-bat-') as td:
            p = Path(td)
            (p / 'test.cpp').write_text(harness)
            exe = p / 'test.exe'
            for cc in ('clang++', 'g++'):
                try:
                    subprocess.run([cc, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(p / 'test.cpp'), '-o', str(exe)], check=True)
                    subprocess.run([str(exe)], check=True)
                    return
                except FileNotFoundError:
                    continue
            self.skipTest('No host C++ compiler (clang++/g++) found')


if __name__ == '__main__':
    unittest.main()
