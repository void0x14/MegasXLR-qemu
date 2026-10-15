# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression tests for runtime dependency collection."""
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "packaging"))
import bundle


class WindowsImportTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "requires native Windows APIs")
    def test_native_system_directory_contains_windows_dlls(self):
        system_directory = bundle.windows_system_directory()
        self.assertIsNotNone(system_directory)
        self.assertTrue(system_directory.is_dir())
        self.assertIn("kernel32.dll", bundle.windows_system_dlls())

    def test_uniscribe_is_supplied_by_windows(self):
        output = "DLL Name: USP10.dll\n"
        with mock.patch.object(bundle.subprocess, "run",
                               return_value=SimpleNamespace(stdout=output)):
            self.assertEqual([], bundle.imports_windows(Path("harfbuzz.dll"), []))

    def test_cng_key_storage_router_is_supplied_by_windows(self):
        output = "DLL Name: NCRYPT.dll\n"
        with mock.patch.object(bundle.subprocess, "run",
                               return_value=SimpleNamespace(stdout=output)):
            self.assertEqual([], bundle.imports_windows(Path("gnutls.dll"), []))

    def test_system_directory_dll_is_supplied_by_windows(self):
        output = "DLL Name: kernelbase.dll\n"
        with tempfile.TemporaryDirectory() as temporary_directory:
            system_directory = Path(temporary_directory) / "System32"
            system_directory.mkdir()
            (system_directory / "kernelbase.dll").touch()
            with mock.patch.object(bundle, "windows_system_directory",
                                   return_value=system_directory):
                with mock.patch.object(bundle.subprocess, "run",
                                       return_value=SimpleNamespace(stdout=output)):
                    self.assertEqual([],
                                     bundle.imports_windows(Path("runtime.dll"), []))

    def test_private_search_match_precedes_system_directory_fallback(self):
        output = "DLL Name: kernelbase.dll\n"
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            system_directory = root / "System32"
            private_directory = root / "private"
            system_directory.mkdir()
            private_directory.mkdir()
            (system_directory / "kernelbase.dll").write_text("system")
            private_dll = private_directory / "kernelbase.dll"
            private_dll.write_text("private")
            with mock.patch.object(bundle, "windows_system_directory",
                                   return_value=system_directory):
                with mock.patch.object(bundle.subprocess, "run",
                                       return_value=SimpleNamespace(stdout=output)):
                    dependencies = bundle.imports_windows(Path("runtime.dll"),
                                                          [private_directory])
            self.assertEqual(["private"], [path.read_text() for path in dependencies])

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
