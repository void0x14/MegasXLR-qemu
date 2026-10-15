#!/usr/bin/env python3
"""Gate real Android 4–17 sessions using live guest semantics and TS bytes.

A matrix entry refers to an already running, locally owned Megas Android VM.
Nothing here inspects QEMU logs, takes screenshots, calls ADB, infers coordinates,
or claims the fixture booted a missing Android image.
"""
import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

VERSIONS = set(range(4, 18))
OPERATIONS = {"click", "long_click", "set_text", "scroll_forward", "scroll_backward", "focus"}


class GateError(Exception):
    pass


def require(predicate, message):
    if not predicate:
        raise GateError(message)


def pointer(document, path):
    require(isinstance(path, str) and path.startswith("/"), "invalid JSON pointer")
    value = document
    for part in path[1:].split("/"):
        part = part.replace("~1", "/").replace("~0", "~")
        require(isinstance(value, dict) and part in value, "state predicate path unavailable")
        value = value[part]
    return value


def validate_matrix(document):
    require(isinstance(document, dict) and isinstance(document.get("devices"), list),
            "matrix requires devices list")
    entries = document["devices"]
    versions = [entry.get("version") for entry in entries if isinstance(entry, dict)]
    require(len(entries) == 14 and all(type(v) is int for v in versions) and
            set(versions) == VERSIONS and len(versions) == len(set(versions)),
            "matrix requires exactly one entry per Android major release 4 through 17")
    for entry in entries:
        require(isinstance(entry.get("runtime_dir"), str) and
                Path(entry["runtime_dir"]).is_dir(), "existing runtime_dir required")
        for key in ("control_port", "stream_port"):
            port = entry.get(key)
            require(type(port) is int and 1 <= port <= 65535, "valid " + key + " required")
        require(isinstance(entry.get("target"), str) and entry["target"], "semantic target ID required")
        require(entry.get("operation") in OPERATIONS, "unsupported semantic operation")
        require(isinstance(entry.get("postcondition_path"), str) and
                entry["postcondition_path"].startswith("/"), "postcondition_path required")
        require("postcondition_equals" in entry, "postcondition_equals required")
        command = entry.get("oracle_cmd")
        require(isinstance(command, list) and 1 <= len(command) <= 32 and
                all(isinstance(arg, str) and arg and chr(0) not in arg for arg in command),
                "independent oracle_cmd argv is required")
        require(isinstance(entry.get("oracle_id"), str) and entry["oracle_id"],
                "independent oracle_id required")
        require(isinstance(entry.get("version_path"), str) and
                entry["version_path"].startswith("/"), "version_path required")
    return entries


def send(megas, entry, payload):
    args = [megas, "--runtime", entry["runtime_dir"], "--port",
            str(entry["control_port"]), "send", json.dumps(payload, separators=(",", ":"))]
    try:
        reply = subprocess.run(args, capture_output=True, text=True, timeout=8)
        result = json.loads(reply.stdout)
    except (subprocess.TimeoutExpired, OSError, ValueError) as exc:
        raise GateError("native controller response unavailable") from exc
    require(isinstance(result, dict) and result.get("ok") is True and
            reply.returncode == 0, "native controller rejected command")
    return result


def state(megas, entry):
    reply = send(megas, entry, {"action": "android_observe", "include_image": False})
    android = reply.get("android", {})
    observed = android.get("observed")
    require(isinstance(observed, dict) and observed.get("type") == "state",
            "live guest semantic state not observed")
    require(android.get("provider_connected") is True, "guest provider disconnected")
    require(android.get("state_age_ms", 999999) < 2000, "guest state is stale")
    require(reply.get("livestream", {}).get("available") is True,
            "independent encoded livestream unavailable")
    return reply, observed


def verify_decoded_video(ts_bytes):
    # Qualification-only FFmpeg decoder. The live host has no FFmpeg worker and
    # the agent is never served PNG frames from this test.
    try:
        result = subprocess.run([
            "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
            "-f", "mpegts", "-i", "pipe:0", "-map", "0:v:0",
            "-frames:v", "1", "-progress", "pipe:1", "-f", "null", "-",
        ], input=ts_bytes, capture_output=True, timeout=5)
    except (subprocess.TimeoutExpired, OSError) as exc:
        raise GateError("test-only video decoder unavailable") from exc
    frames = []
    for row in result.stdout.decode("ascii", "replace").splitlines():
        if row.startswith("frame="):
            try:
                frames.append(int(row.split("=", 1)[1].strip()))
            except ValueError:
                pass
    require(result.returncode == 0 and any(value >= 1 for value in frames),
            "TS packets did not contain an actually decodable video frame")


def stream_evidence(entry):
    token_file = Path(entry["runtime_dir"]) / ("session-%d.token" % entry["control_port"])
    try:
        token = token_file.read_text(encoding="ascii").strip()
        require(len(token) == 64, "session credential unavailable")
        with socket.create_connection(("127.0.0.1", entry["stream_port"]), timeout=2) as sock:
            sock.settimeout(2)
            request = ("GET /live.ts HTTP/1.1\r\nHost: localhost\r\n"
                       "Authorization: Bearer " + token + "\r\n\r\n").encode("ascii")
            sock.sendall(request)
            data = b""
            deadline = time.monotonic() + 3
            while b"\r\n\r\n" not in data and time.monotonic() < deadline:
                block = sock.recv(8192)
                require(block, "livestream disconnected during handshake")
                data += block
            header, _, body = data.partition(b"\r\n\r\n")
            require(header.startswith(b"HTTP/1.1 200"), "livestream did not return HTTP 200")
            require(b"video/mp2t" in header.lower(), "livestream is not MPEG-TS")
            # Observe actual continuous transport, not a single packet or
            # old cached bytes. Cap both wall time and in-memory evidence.
            limit = 1024 * 1024
            while time.monotonic() < deadline and len(body) < limit:
                block = sock.recv(min(8192, limit - len(body)))
                require(block, "livestream disconnected before TS evidence")
                body += block
    except (OSError, UnicodeError) as exc:
        raise GateError("live encoded stream cannot be consumed") from exc
    require(len(body) >= 188 * 16, "no sustained encoded TS packets")
    packets = [body[i:i + 188] for i in range(0, len(body) - 187, 188)]
    require(all(packet[0] == 0x47 for packet in packets), "TS packet alignment invalid")
    pids = {((packet[1] & 0x1F) << 8) | packet[2] for packet in packets}
    require(any(pid not in (0, 0x1FFF) for pid in pids),
            "no non-null TS media transport observed")
    verify_decoded_video(body[:len(packets) * 188])
    return len(packets)


def independent_oracle(entry):
    # This is an external task-specific judge, not an accessibility-state flag,
    # QEMU log line or Megas client response. It must be provisioned locally.
    try:
        process = subprocess.run(entry["oracle_cmd"], capture_output=True, text=True,
                                 timeout=3, cwd=entry["runtime_dir"])
        outcome = json.loads(process.stdout)
    except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
        raise GateError("independent task oracle unavailable") from exc
    require(process.returncode == 0 and isinstance(outcome, dict) and
            outcome.get("oracle_id") == entry["oracle_id"] and
            outcome.get("passed") is True, "independent task oracle did not pass")


def check_entry(megas, entry):
    started = time.monotonic()
    before, observed = state(megas, entry)
    require(pointer(observed, entry["version_path"]) == entry["version"],
            "observed guest version does not match test entry")
    require(pointer(observed, entry["postcondition_path"]) != entry["postcondition_equals"],
            "postcondition already true; cannot attribute success to action")
    allowed = any(item.get("id") == entry["target"] and
                  entry["operation"] in item.get("operations", [])
                  for item in observed.get("targets", []) if isinstance(item, dict))
    require(allowed, "semantic target/capability absent from observed state")
    packets = stream_evidence(entry)
    action = {"action": "android_act", "epoch": observed["epoch"],
              "expected_revision": observed["revision"],
              "target": entry["target"], "operation": entry["operation"]}
    if entry["operation"] == "set_text":
        require(isinstance(entry.get("text"), str), "set_text requires text")
        action["text"] = entry["text"]
    reply = send(megas, entry, action)
    action_id = reply.get("android", {}).get("pending_action_id")
    require(type(action_id) is int and action_id > 0, "action was not queued")
    deadline = started + min(entry.get("max_seconds", 180), 180)
    while time.monotonic() < deadline:
        current, changed = state(megas, entry)
        controller = current["android"]
        require(not controller.get("uncertain"), "action delivery uncertain; never retry blindly")
        if controller.get("outcome") == "rejected":
            raise GateError("guest provider rejected action")
        if (controller.get("last_action_id") == action_id and
                controller.get("outcome") == "applied" and
                changed["epoch"] == observed["epoch"] and
                changed["revision"] > observed["revision"] and
                changed.get("cause_action_id") == action_id and
                pointer(changed, entry["postcondition_path"]) == entry["postcondition_equals"]):
            independent_oracle(entry)
            require(time.monotonic() <= deadline, "task exceeded time budget including independent verification")
            return {"version": entry["version"], "passed": True,
                    "elapsed_ms": int((time.monotonic() - started) * 1000),
                    "ts_packets_observed": packets, "revision": changed["revision"]}
        time.sleep(.025)
    raise GateError("no confirmed state transition and postcondition before time budget")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--megas", required=True, help="native Megas executable")
    parser.add_argument("--matrix", type=Path, required=True, help="private local matrix JSON")
    args = parser.parse_args()
    try:
        entries = validate_matrix(json.loads(args.matrix.read_text(encoding="utf-8")))
    except (OSError, ValueError, GateError) as exc:
        print(json.dumps({"passed": False, "error": str(exc)}))
        return 2
    results = []
    for entry in sorted(entries, key=lambda x: x["version"]):
        try:
            results.append(check_entry(args.megas, entry))
        except GateError as exc:
            results.append({"version": entry["version"], "passed": False, "error": str(exc)})
    passed = len(results) == 14 and all(item["passed"] for item in results)
    print(json.dumps({"passed": passed, "results": results}, ensure_ascii=False))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
