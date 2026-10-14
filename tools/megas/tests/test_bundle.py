# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression tests for runtime dependency collection."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "packaging"))
import bundle


class WindowsImportTests(unittest.TestCase):
    def test_uniscribe_is_supplied_by_windows(self):
        output = "DLL Name: USP10.dll\n"
        with mock.patch.object(bundle.subprocess, "run",
                               return_value=SimpleNamespace(stdout=output)):
            self.assertEqual([], bundle.imports_windows(Path("harfbuzz.dll"), []))

    def test_api_set_contracts_are_supplied_by_windows(self):
        output = ("DLL Name: api-ms-win-core-synch-l1-2-0.dll\n"
                  "DLL Name: ext-ms-win-kernel32-package-current-l1-1-0.dll\n")
        with mock.patch.object(bundle.subprocess, "run",
                               return_value=SimpleNamespace(stdout=output)):
            self.assertEqual([], bundle.imports_windows(Path("runtime.dll"), []))

    def test_unknown_application_dll_still_fails(self):
        output = "DLL Name: megas-private-runtime.dll\n"
        with mock.patch.object(bundle.subprocess, "run",
                               return_value=SimpleNamespace(stdout=output)):
            with self.assertRaisesRegex(RuntimeError, "Unresolved application DLL"):
                bundle.imports_windows(Path("megas.exe"), [])

    def test_private_application_dll_is_collected_case_insensitively(self):
        output = "DLL Name: LIBPRIVATE-0.DLL\n"
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            private_dll = directory / "libprivate-0.dll"
            private_dll.touch()
            with mock.patch.object(bundle.subprocess, "run",
                                   return_value=SimpleNamespace(stdout=output)):
                dependencies = bundle.imports_windows(Path("megas.exe"), [directory])
                def normalized(path):
                    return str(path).replace("\\", "/").casefold()

                self.assertEqual([normalized(path) for path in dependencies],
                                 [normalized(private_dll)])


if __name__ == "__main__":
    unittest.main()
