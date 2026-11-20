#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Unit tests for scripts/b2l_board.py."""
import os
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0, str(ROOT / 'scripts'))
import b2l_board


class BoardProfileTests(unittest.TestCase):
    def test_registry_contains_all_boards(self):
        self.assertIn('beyond0lte', b2l_board.BOARDS)
        self.assertIn('beyond1lte', b2l_board.BOARDS)
        self.assertIn('beyond2lte', b2l_board.BOARDS)

    def test_models(self):
        self.assertEqual(b2l_board.model('beyond0lte'), 'SM-G970F')
        self.assertEqual(b2l_board.model('beyond1lte'), 'SM-G973F')
        self.assertEqual(b2l_board.model('beyond2lte'), 'SM-G975F')

    def test_userdata_partitions(self):
        self.assertEqual(b2l_board.userdata('beyond0lte'), '/dev/block/sda34')
        self.assertEqual(b2l_board.userdata('beyond1lte'), '/dev/block/sda34')
        self.assertEqual(b2l_board.userdata('beyond2lte'), '/dev/block/sda31')

    def test_userdata_override(self):
        with patch.dict(os.environ, {'B2L_USERDATA': '/dev/block/custom'}):
            self.assertEqual(b2l_board.userdata('beyond0lte'), '/dev/block/custom')

    def test_platform_package_names(self):
        self.assertEqual(b2l_board.platform_package('beyond0lte'), 'beyond0ltePkg')
        self.assertEqual(b2l_board.platform_package('beyond1lte'), 'beyond1ltePkg')
        self.assertEqual(b2l_board.platform_package('beyond2lte'), 'beyond2ltePkg')

    def test_board_selection_environment(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(b2l_board.board(), 'beyond2lte')

        with patch.dict(os.environ, {'B2L_BOARD': 'beyond0lte'}):
            self.assertEqual(b2l_board.board(), 'beyond0lte')
            self.assertEqual(b2l_board.model(), 'SM-G970F')
            self.assertEqual(b2l_board.userdata(), '/dev/block/sda34')
            self.assertEqual(b2l_board.platform_package(), 'beyond0ltePkg')

        with patch.dict(os.environ, {'B2L_BOARD': 'invalid_board'}):
            with self.assertRaises(SystemExit):
                b2l_board.board()

    def test_identity_ok(self):
        # beyond0lte accepts ro.boot.em.model == SM-G970F
        getprop_s10e = lambda k: 'SM-G970F' if k == 'ro.boot.em.model' else ''
        self.assertTrue(b2l_board.identity_ok(getprop_s10e, 'beyond0lte'))

        # beyond0lte rejects wrong model
        getprop_s10 = lambda k: 'SM-G973F' if k == 'ro.boot.em.model' else ''
        self.assertFalse(b2l_board.identity_ok(getprop_s10, 'beyond0lte'))

        # beyond0lte rejects fake ro.product.device
        getprop_fake = lambda k: 'beyond0lte' if k == 'ro.product.device' else 'unknown'
        self.assertFalse(b2l_board.identity_ok(getprop_fake, 'beyond0lte'))

        # beyond2lte accepts legacy ro.product.device == beyond2lte
        getprop_legacy = lambda k: 'beyond2lte' if k == 'ro.product.device' else 'unknown'
        self.assertTrue(b2l_board.identity_ok(getprop_legacy, 'beyond2lte'))

        # beyond1lte accepts SM-G973F
        self.assertTrue(b2l_board.identity_ok(getprop_s10, 'beyond1lte'))


if __name__ == '__main__':
    unittest.main()
