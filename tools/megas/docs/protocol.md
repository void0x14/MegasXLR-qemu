# Native control contract

One owner process runs the GUI or `megas serve`. Native clients use loopback
TCP and newline-delimited JSON. Every command response includes current state,
the latest live image path and a readable fallback. The continuous RFB connection
is independent of screenshots; screenshots are optional exports from its buffer.

GUI calls `mg_engine_submit` with the same JSON that CLI clients send.
Requests use `action` (aliases: `command`, `cmd`). Actions:

* `preflight`: optional disk. Reports the recorded VM profile for that disk
  (memory, disk bus, video, whether the image exists) and, when nothing is
  recorded, names exactly what has to be asked for: `ram_mb` and `bus`. The
  controller applies no defaults and never answers this with a guess.
* `start`: optional disk/iso/qemu/ram_mb/bus/video/width/height. `bus` is one
  of `ide`, `virtio`, `scsi`, `nvme`. For a disk without a recorded profile,
  `ram_mb` and `bus` are required and the start is refused otherwise; the
  refusal is journaled as `preflight_required` with the reason.

## A VM keeps its own memory and disk bus

The memory and the disk controller belong to the VM, not to the controller.
A start therefore uses, in this order, the value stated in the request, then
the value recorded for that exact disk, then the value of the same VM already
running in this controller. There is no fourth source: a value that cannot be
resolved fails closed instead of falling back to a built-in number, so an
existing disk is never silently reattached to another controller (which turns
a Windows guest into a blue screen) and memory is never quietly replaced by a
smaller default. Memory and bus change only when the user asks for it or the
job needs more memory.

Every start writes its governing configuration to the audit journal: the
`action_request` record carries `effective_ram_mb`, `effective_bus`,
`effective_video` and `effective_disk` (or `preflight: required` plus the
unresolved reason), and a `vm_profile` record is written at spawn. The
profile is stored per disk under the runtime directory, so a later start of
the same disk reuses the exact values, including after a controller restart.
An intercepted raw QEMU command line is recorded the same way, which is how a
VM launched by its own script keeps its memory and controller when it is next
started by name. Every response reports the current VM under `vm`: `disk`,
`memory_mb`, `bus`, `video`, `profile_known` and `image_exists`.
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
