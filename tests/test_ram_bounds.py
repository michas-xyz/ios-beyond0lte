#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Unit and regression tests for memory boundaries and RAM validation logic."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER_COMMON = ROOT / "common/memory/b2l_ram_ranges.h"
HEADER_XNU = ROOT / "xnu-overlay/pexpert/pexpert/arm64/b2l_ram_ranges.h"
ARENA_HEADER = ROOT / "uefi/XnuLoader/Arena.h"


class MemoryHeaderSyncTests(unittest.TestCase):
    def test_headers_byte_for_byte_identical(self):
        """common/memory/b2l_ram_ranges.h and xnu-overlay must match exactly."""
        self.assertEqual(HEADER_COMMON.read_bytes(), HEADER_XNU.read_bytes(),
                         "b2l_ram_ranges.h headers must be byte-for-byte identical")

    def test_header_constants(self):
        content = HEADER_COMMON.read_text()
        # Verify 6G constants
        self.assertIn("#define B2L_RAM_MAX_BYTES_6G    (6ULL * 1024 * 1024 * 1024)", content)
        self.assertIn("#define B2L_RAM_PHYS_CEILING_6G 0x980000000ULL", content)
        self.assertIn("#define B2L_RAM_UPPER_SIZE_6G   0x0f2b00000ULL", content)

        # Verify 8G constants
        self.assertIn("#define B2L_RAM_MAX_BYTES_8G    (8ULL * 1024 * 1024 * 1024)", content)
        self.assertIn("#define B2L_RAM_PHYS_CEILING_8G 0xa00000000ULL", content)
        self.assertIn("#define B2L_RAM_UPPER_SIZE_8G   0x172b00000ULL", content)

        # Verify arithmetic
        upper_base = 0x88ad00000
        upper_size_6g = 0x0f2b00000
        upper_end_6g = upper_base + upper_size_6g
        self.assertEqual(upper_end_6g, 0x97D800000, "6G upper DDR must end at 0x97D800000")
        self.assertLessEqual(upper_end_6g, 0x980000000, "6G upper DDR must not exceed 6GB ceiling")

        upper_size_8g = 0x172b00000
        upper_end_8g = upper_base + upper_size_8g
        self.assertEqual(upper_end_8g, 0x9FD800000, "8G upper DDR must end at 0x9FD800000")

    def test_arena_header_ceiling(self):
        content = ARENA_HEADER.read_text()
        self.assertIn("0x97D800000ULL", content, "Arena.h must define 6G ceiling 0x97D800000ULL for beyond0lte")
        self.assertIn("0x9FD800000ULL", content, "Arena.h must preserve 8G ceiling 0x9FD800000ULL for beyond1lte/beyond2lte")


class RamValidationLogicTests(unittest.TestCase):
    """Test memory validation logic matching b2l_ram_ranges.h implementation."""

    PAGE = 16384
    MAX_RANGES = 128

    ALLOWLIST_BASE = [
        (0x80004000, 0x6ffc000),
        (0x87200000, 0xee00000),
        (0xb4800000, 0x6300000),
        (0xc0080000, 0x8f80000),
        (0xc9440000, 0xbc0000),
        (0xcb400000, 0x3c00000),
        (0xcf050000, 0xfb0000),
        (0xd1600000, 0xea00000),
        (0xe1900000, 0x9a08000),
        (0xed210000, 0x2df0000),
    ]

    def b2l_ram_allowed(self, base, size, is_6g=False):
        if size == 0 or base > 0xFFFFFFFFFFFFFFFF - size:
            return False
        upper_size = 0x0f2b00000 if is_6g else 0x172b00000
        allowlist = self.ALLOWLIST_BASE + [(0x88ad00000, upper_size)]
        for entry_base, entry_size in allowlist:
            if base >= entry_base and (base + size) <= (entry_base + entry_size):
                return True
        return False

    def b2l_ram_validate(self, ranges, primary, primary_size, is_6g=False):
        ceiling = 0x980000000 if is_6g else 0xa00000000
        max_bytes = 6 * 1024 * 1024 * 1024 if is_6g else 8 * 1024 * 1024 * 1024
        upper_max = 0x97D800000 if is_6g else 0x9FD800000

        primary_count = 0
        total = 0
        last = 0
        if not ranges or len(ranges) == 0 or len(ranges) > self.MAX_RANGES:
            return -1

        for i, (base, size) in enumerate(ranges):
            if (base & (self.PAGE - 1)) or (size & (self.PAGE - 1)):
                return -1
            if not self.b2l_ram_allowed(base, size, is_6g=is_6g):
                return -1
            if i > 0 and base < last:
                return -1
            if base + size > ceiling:
                return -1
            if base >= 0x88ad00000 and (base + size > upper_max):
                return -1
            if base == primary and size == primary_size:
                primary_count += 1
            last = base + size
            total += size

        if primary_count != 1 or total > max_bytes:
            return -1
        return 0

    def test_valid_beyond0lte_6g_layout(self):
        """A valid 6 GB memory allocation must validate successfully."""
        primary_base = 0x900000000
        primary_size = 0x77000000  # ~1.85 GiB
        ranges = [
            (0xb4800000, 0x6300000),       # 99 MiB
            (0xcb404000, 0x3bfc000),       # ~60 MiB
            (0x88ad00000, 0x70000000),     # 1.75 GiB
            (primary_base, primary_size),  # ~1.85 GiB (ends at 0x977000000 <= 0x97D800000)
        ]
        res = self.b2l_ram_validate(ranges, primary_base, primary_size, is_6g=True)
        self.assertEqual(res, 0, "Valid 6G layout must return 0")

    def test_beyond0lte_rejects_exceeding_physical_ceiling(self):
        """Any allocation above 0x980000000 on beyond0lte must be rejected."""
        primary_base = 0x900000000
        primary_size = 0x80000000  # 2 GiB (ends at 0x980000000)
        # Attempt to map memory in Bank 2 (0x980000000..0x980010000)
        ranges = [
            (primary_base, primary_size),
            (0x980000000, 0x10000),
        ]
        res = self.b2l_ram_validate(ranges, primary_base, primary_size, is_6g=True)
        self.assertEqual(res, -1, "Allocation >= 0x980000000 on 6G device must be rejected")

    def test_beyond0lte_rejects_exceeding_upper_ddr_end(self):
        """Upper DDR allocation exceeding 0x97D800000 on beyond0lte must be rejected."""
        primary_base = 0x900000000
        # Allocation extending into the EL2 early memory reservation (0x97D800000..0x980000000)
        primary_size = 0x7E000000  # ends at 0x97E000000 > 0x97D800000
        ranges = [
            (primary_base, primary_size),
        ]
        res = self.b2l_ram_validate(ranges, primary_base, primary_size, is_6g=True)
        self.assertEqual(res, -1, "Range extending beyond 0x97D800000 on 6G device must be rejected")

    def test_beyond0lte_rejects_total_over_6gb(self):
        """Total memory exceeding 6 GiB on beyond0lte must be rejected."""
        primary_base = 0x900000000
        primary_size = 0x70000000
        # Construct ranges where total exceeds 6 GiB
        ranges = [
            (0x80004000, 0x6ffc000),
            (0x87200000, 0xee00000),
            (0xb4800000, 0x6300000),
            (0xc0080000, 0x8f80000),
            (0x88ad00000, 0x0f2b00000),    # Full upper DDR: ~3883 MiB
            (primary_base, primary_size),   # Would push total > 6 GiB
        ]
        res = self.b2l_ram_validate(ranges, primary_base, primary_size, is_6g=True)
        self.assertEqual(res, -1, "Total > 6 GiB on 6G device must be rejected")

    def test_beyond1lte_and_beyond2lte_accepts_8gb(self):
        """8 GB models continue to accept memory up to 8 GiB and 0x9FD800000."""
        primary_base = 0x900000000
        primary_size = 4024 * 1024 * 1024  # 4024 MiB
        ranges = [
            (0xb4800000, 0x6300000),
            (0xcb404000, 0x3bfc000),
            (0x88ad00000, 0x73000000),
            (primary_base, primary_size),  # ends at 0x9FB800000 <= 0x9FD800000
        ]
        res = self.b2l_ram_validate(ranges, primary_base, primary_size, is_6g=False)
        self.assertEqual(res, 0, "8G models must continue to validate allocations up to 8GB")

    def test_alignment_and_order_enforced(self):
        """16 KiB alignment and ascending order must be strictly enforced."""
        primary_base = 0x900000000
        primary_size = 0x10000

        # Misaligned base (4 KiB instead of 16 KiB)
        bad_base_ranges = [(primary_base + 4096, primary_size)]
        self.assertEqual(self.b2l_ram_validate(bad_base_ranges, primary_base + 4096, primary_size, is_6g=True), -1)

        # Misaligned size
        bad_size_ranges = [(primary_base, 0x5000)]
        self.assertEqual(self.b2l_ram_validate(bad_size_ranges, primary_base, 0x5000, is_6g=True), -1)

        # Unsorted ranges
        unsorted = [
            (primary_base, primary_size),
            (0xb4800000, 0x6300000),
        ]
        self.assertEqual(self.b2l_ram_validate(unsorted, primary_base, primary_size, is_6g=True), -1)


if __name__ == '__main__':
    unittest.main()
