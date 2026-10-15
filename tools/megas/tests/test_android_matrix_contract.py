#!/usr/bin/env python3
"""Contract regressions only; a simulated test cannot certify an Android version."""
import tempfile
import unittest
import sys
import shutil
import subprocess
from pathlib import Path

from android_matrix import GateError, VERSIONS, pointer, validate_matrix, check_entry, independent_oracle, verify_decoded_video


class MatrixContractTests(unittest.TestCase):
    def entry(self, runtime, version):
        return {"version": version, "runtime_dir": str(runtime),
                "control_port": 49393, "stream_port": 49397,
                "target": "provider-issued", "operation": "click",
                "version_path": "/payload/system/release_major",
                "postcondition_path": "/payload/ui/dialog/open",
                "postcondition_equals": True,
                "oracle_cmd": ["/nonexistent/local-oracle"],
                "oracle_id": "external-app-check"}

    def test_requires_all_real_versions(self):
        self.assertEqual(VERSIONS, set(range(4, 18)))
        with tempfile.TemporaryDirectory() as directory:
            entries = [self.entry(Path(directory), version) for version in range(4, 18)]
            self.assertEqual(len(validate_matrix({"devices": entries})), 14)
            with self.assertRaises(GateError):
                validate_matrix({"devices": entries[:13]})
            entries[-1]["version"] = 16
            with self.assertRaises(GateError):
                validate_matrix({"devices": entries})

    def test_missing_independent_oracle_never_certifies_a_version(self):
        with tempfile.TemporaryDirectory() as directory:
            entries = [self.entry(Path(directory), version) for version in range(4, 18)]
            del entries[0]["oracle_cmd"]
            with self.assertRaises(GateError):
                validate_matrix({"devices": entries})

    def test_state_predicates_do_not_infer_unknown_elements(self):
        state = {"payload": {"system": {"release_major": 14},
                             "ui": {"dialog": {"open": False}}}}
        self.assertEqual(pointer(state, "/payload/system/release_major"), 14)
        self.assertIs(pointer(state, "/payload/ui/dialog/open"), False)
        with self.assertRaises(GateError):
            pointer(state, "/payload/ui/guessed_target")

    def test_independent_oracle_must_return_matching_success(self):
        with tempfile.TemporaryDirectory() as directory:
            entry = self.entry(Path(directory), 4)
            entry["oracle_cmd"] = [sys.executable, "-c",
                "print('{\\\"oracle_id\\\":\\\"external-app-check\\\",\\\"passed\\\":true}')"]
            independent_oracle(entry)
            entry["oracle_id"] = "unmatched"
            with self.assertRaises(GateError):
                independent_oracle(entry)

    @unittest.skipUnless(shutil.which("ffmpeg"), "test-only ffmpeg not installed")
    def test_transport_requires_decodable_real_video(self):
        source = subprocess.run([
            "ffmpeg", "-v", "error", "-f", "lavfi",
            "-i", "testsrc2=size=160x96:rate=10",
            "-frames:v", "20", "-c:v", "libx264",
            "-threads", "1", "-preset", "ultrafast",
            "-tune", "zerolatency", "-g", "5",
            "-f", "mpegts", "pipe:1",
        ], capture_output=True)
        if source.returncode:
            self.skipTest("test-only libx264 encoder not available")
        verify_decoded_video(source.stdout)
        with self.assertRaises(GateError):
            verify_decoded_video((b"\\x47\\x1f\\xff\\x10" + b"\\xff" * 184) * 128)

    def test_no_controller_does_not_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(GateError):
                check_entry("/nonexistent/megas", self.entry(Path(directory), 4))


if __name__ == "__main__":
    unittest.main()
