# Native control contract

One owner process runs the GUI or `megas serve`. Native clients use loopback
TCP and newline-delimited JSON. Every command response includes current state and a readable fallback.
The default stdio agent does not export PNGs or start OCR. Use `agent --vision`,
`include_image:true` or `screenshot` to request an image; `agent --text-only`
explicitly selects the legacy OCR adapter. The continuous RFB connection
is independent of screenshots; screenshots are optional exports from its buffer.

GUI calls `mg_engine_submit` with the same JSON that CLI clients send.
Requests use `action` (aliases: `command`, `cmd`). Actions:

* `start`: optional disk/iso/qemu/ram_mb/width/height.
* `status`, `pause`, `resume`, `stop`, `reset`.
* `key`: key name or keysym; balanced press/release.
* `click`: x,y, optional button (1 left,2 middle,3 right).
* `type`: text; use explicit key events, no host shell interpretation.
* `resize`: width,height; change guest display, never just scale GUI.
* `screenshot`: optional path; PNG with state text metadata.
* `checkpoint`: optional name. `restore`: name. `steps`: history/statistics.
* `password`: enabled boolean; guest account policy requires supported guest helper.
* `repair`: copyable forensic repair prompt; `harness`: name and prompt.

QMP snapshot tags for controller-owned checkpoints use the reserved
`megas-owned:` prefix; the prefix is internal, and displayed checkpoint labels
remain unchanged. Existing snapshots with valid legacy labels such as
`megas-owned-important` remain foreign checkpoints: they stay readable and
restorable when their state is available, and are never automatically
garbage-collected.

No action may claim success on a QEMU error. Unknown actions return a repair
prompt and current state without killing the controller. Logs are append-only
JSONL and include requested/normalized action, result, timestamps and events.
No unbounded frame, action or event queues. Loopback only; per-session credentials
must protect the control channel before it is exposed to non-owner applications.


Android host integration is described in [android-integration.md](android-integration.md).
Its continuous encoded video channel is independent of RFB and PNG exports.
