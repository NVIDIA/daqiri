# SPDX-License-Identifier: Apache-2.0
"""Recovery checks that never load modules or access hardware."""
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import manage_module as manager


class VendorModuleRecoveryTest(unittest.TestCase):
    def recover(self, loaded):
        candidate = "test-mgbe-driver-source"
        data = dict(kernel=manager.os.uname().release, interface="mgbe0_0", ports=[],
                    macsec="0", unloaded=["tegra_capture_coe"], replacement_source=candidate)
        active = [loaded]
        commands = []
        def exists(path):
            return str(path) == "/sys/module/nvethernet" and active[0] is not None
        def execute(*args, **kwargs):
            commands.append(args)
            if args == ("rmmod", "nvethernet"):
                active[0] = None
            if args[:2] == ("modprobe", "nvethernet"):
                active[0] = manager.STOCK_SOURCE
            if args == ("modprobe", "tegra_capture_coe"):
                self.assertEqual(active[0], manager.STOCK_SOURCE)
        with mock.patch.object(Path, "exists", exists), \
             mock.patch.object(Path, "read_text", return_value=json.dumps(data)), \
             mock.patch.object(manager, "read", side_effect=lambda path: active[0]), \
             mock.patch.object(manager, "run", side_effect=execute), \
             mock.patch.object(manager, "ports", return_value=[]), \
             mock.patch.object(manager, "journal"):
            manager.restore(Path("/tmp/fake-module-journal"))
        return commands

    def test_vendor_module_removed_before_stock_and_coe_reload(self):
        commands = self.recover("test-mgbe-driver-source")
        self.assertEqual(commands, [("rmmod", "nvethernet"),
                                    ("modprobe", "nvethernet", "macsec_enable=0"),
                                    ("modprobe", "tegra_capture_coe")])

    def test_failed_load_with_module_absent_recovers_stock(self):
        self.assertEqual(self.recover(None)[0], ("modprobe", "nvethernet", "macsec_enable=0"))

    def test_foreign_replacement_is_not_unloaded(self):
        with self.assertRaisesRegex(RuntimeError, "another nvethernet replacement"):
            self.recover("unrecognized-driver")


if __name__ == "__main__":
    unittest.main()
