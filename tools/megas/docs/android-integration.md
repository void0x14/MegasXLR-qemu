# Android host integration: implementation and limits

This is the host side of the existing native Megas command centre. It is not
an Android distribution, an ARM translator, a Google Apps installer or an
Android compatibility certification. The private accessibility provider and its
local build recipe are not in this repository, its tests, CI or packages.

## What runs

One owner thread services QMP, the existing RFB preview, a persistent semantic
channel and a continuous MPEG-TS relay. The relay copies already encoded video
bytes from a guest virtio serial channel. It does not capture screenshots,
decode/re-encode video, concatenate images, run an encoder process, write video
segments to disk, or create worker threads. H.264/MPEG-TS is an implemented
transport choice; HLS playlists are not implemented. Source packets must have
188-byte MPEG-TS framing. PAT/PMT and IDRs must recur promptly so late viewers
can start decoding; the provider must continue sending encoded updates on an
idle display. The host has no video clock synchronization or glass-to-glass
latency measurement yet.

The host bounds each video client's pending output to about 24 KiB, accepts
four video viewers and four independent semantic event subscribers, and disconnects slow viewers instead of accumulating video lag.
It validates TS packet sync, not codec content. Four control clients also have
bounded nonblocking response buffers. The relay stops viewers on source loss.

`megas agent` now returns controller/semantic state and stream discovery
without exporting PNGs or starting OCR. `--vision` enables optional PNG
results; `--text-only` explicitly selects the legacy OCR path. Existing
non-Android CLI commands retain their optional image behavior. A model/harness
must support consuming the video endpoint separately: an MCP text tool response
containing a stream URL does not itself make a model watch a livestream.

## Native VM launch and host budgets

The current launch device model is an **x86 PC**. Megas also detects the
QFI signature on locally supplied qcow2 disks renamed to `.img` or another
extension; existing `.qcow2` filenames retain the earlier format selection.
Unreadable disk or ISO paths now fail admission before QEMU is started. The existing executable
selection is not an implementation of an ARM Android board or Google's
goldfish/ranchu device model. Suitable locally prepared x86 images can use:

```json
{"action":"start","android":true,"disk":"/local/android.qcow2","ram_mb":2048,"accel":"kvm","cpus":1,"cpu_percent":100,"max_threads":48}
```

The GUI exposes the same Android switch and CPU budget. The installer ISO
checkbox requests `boot_source:"iso"`: with an existing disk, QEMU boots
the ISO **once** and returns to the disk on reboot. `boot_source:"disk"`
explicitly boots the disk; `auto` preserves QEMU's default. An ISO must
be configured before ISO boot, and a disk before disk boot. This selects the
device, not an OS installation procedure; modified images still require
compatible kernel, bootloader and hardware drivers. A storage transport selector uses `virtio` by default or `ide` for legacy x86 images that do not expose virtio block devices. A matching `video_adapter` selector uses virtio VGA by default or standard VGA when an older Android x86 kernel has no virtio GPU driver. These are hardware settings, not evidence that every Android major release boots. The selected disk transport is passed as an explicit QEMU device, not a post-boot patch. It does not guarantee that a given image contains the required kernel, partition or graphics support. It does not download an
image or install a provider. There is no Android SDK process in this host path.
Android skips Linux NoCloud account creation and QGA login-policy retries.
Automatic full RAM/disk checkpoints per input are disabled in Android mode;
explicit checkpoints remain available when the selected QEMU supports them.

`auto` selects accessible Linux KVM, otherwise single-threaded TCG for ordinary
VMs, with an explicit 32 MiB TCG translation cache. QEMU otherwise sizes
that cache from host RAM up to 1 GiB and commits it on Windows, which can
exceed a small VM process budget before startup. Android mode refuses TCG. Explicit KVM/WHPX/HVF failures never silently
switch to software emulation. Default vCPU count is one; accepted range is 1–4.

On Linux, a pre-delegated cgroup v2 parent is supplied through the local
`MEGAS_CGROUP_PARENT` environment variable or `cgroup_parent` start field. The
parent needs enabled `cpu`, `memory` and `pids` subtree controllers. Megas
creates only its own randomly named child, writes the limits, then places the
child process in it **before exec**. It does not elevate privileges or change
the parent's controllers. Missing permissions/controllers reject Android
launch before an unbounded QEMU can run. Descendants inherit the limits:

| Limit | Meaning |
| --- | --- |
| `cpu.max` | `cpu_percent` of one logical CPU, 100 ms period |
| `pids.max` | Host tasks, including threads; default 48, accepted 8–128 |
| `memory.max` | Requested guest RAM plus 256 MiB emulator allowance |
| `memory.swap.max` | Zero; this VM does not consume swap |
| `memory.oom.group` | Limit failure kills the VM group, not a chosen host process |

That allowance is a budget, not proof every image fits it. Startup also checks
available host RAM against guest RAM, the emulator allowance and 256 MiB host
reserve. This is admission control, not a promise that unrelated host processes
cannot consume memory later. The cgroup is removed after its process group
exits. Generic Linux VMs without a delegated parent explicitly report
`hard_task_limit:false`. Their requested CPU/task fields are not enforced caps.

Windows Job Objects enforce VM memory and CPU budgets for ordinary guests.
They have no equivalent hard per-job thread limit in this implementation, so
strict Android launch is rejected there. It is not advertised as completed
Windows Android support. Thread count and memory usage are sampled at most
twice a second and displayed in the command centre and `resources` response.
These are QEMU host threads, not guest Android application threads.

## Public transport boundary

Two QEMU `virtserialport` devices are added only in Android mode:

| Guest channel | QEMU loopback backend | Host use |
| --- | --- | --- |
| `org.megas.control` | `android_port`, default 49395 | Persistent bounded JSONL |
| `org.megas.video` | `video_port`, default 49396 | Already encoded MPEG-TS |
| Host video endpoint | `stream_port`, default 49397 | `GET /live.ts` |

The public HTTP endpoint requires `Authorization: Bearer <session credential>`
using the owner's existing protected session credential. No credential is put
in a stream URL, state response or journal. Endpoints bind only to loopback.
The response's `livestream.available` requires recent valid TS bytes. Missing
video returns 503, invalid authentication returns 403. The relay is deliberately
small; its HTTP authentication header uses the documented spelling above.

The host also exposes authenticated `GET /events` on the same loopback
HTTP port, with `Content-Type: text/event-stream` (SSE). The guest's
validated state transition emits `event: state`; guest-result processing emits
`event: result`; reset/invalidation emits `event: reset`. The stream
also emits `event: video` with value 1 when encoded packets resume and 0
when the producer disconnects or stalls. Each event has
a monotonically increasing connection-lifetime event ID and a payload with
only a public numeric value and host monotonic timestamp. **The private
accessibility tree, node identifiers, user text and screenshots are never
put into SSE events.** Consumers are signaled to call `android_observe` to
obtain fresh semantic state; the SSE message is not independent causality
proof. The same existing bearer credential is required and must be sent as
a header, never in a URL or repository file. Existing native owner loop
services these connections without spawning a worker. Four clients per channel type (eight total) are allowed; slow subscribers
are disconnected instead of retaining an unbounded backlog. Heartbeat
comments keep idle event subscribers aware of the connection.

The semantic provider is separate software built and installed on the user's
computer. The public host treats its `payload` as opaque. It must emit:

```json
{"type":"state","epoch":"boot-session-id","revision":1,"guest_monotonic_ms":100,"cause_action_id":0,"targets":[{"id":"provider-issued-node-id","operations":["click","focus"]}],"payload":{}}
```

This example defines only the public envelope. It contains no private tree
representation. The provider must include up to 256 `targets`, each with a stable
nonempty `id` and the explicit `operations` currently permitted for that ID.
The host rejects targets absent from its last observed revision, operations
not advertised for that target, and malformed target envelopes. The provider
must independently revalidate on execution; this check is not authority over
private guest accessibility state. Revisions increase within an epoch. A bounded heartbeat may
confirm an unchanged state using `type:heartbeat`, `epoch` and `revision`.
`guest_monotonic_ms` and cause relationships are provider reports, not host
clock synchronization or inferred proof of causality. State payloads remain in
memory and are not written to the action journal.

`android_observe` also reports measured host-to-host state-event intervals, guest-reported monotonic intervals, revision gaps and guest-declared action causes. Host and guest clocks are not synchronized; causes remain provider reports, never independently verified causal evidence. This telemetry is memory-only and never stores private tree contents in the journal.

`android_observe` returns the latest state plus its host freshness, stream
availability and action status. `android_act` accepts `epoch`,
`expected_revision`, provider-issued `target`, `operation` and optional `text`.
Operations are `click`, `long_click`, `set_text`, `scroll_forward`,
`scroll_backward` and `focus`. Coordinates are rejected, and ordinary raw
click/key/type actions are refused in Android mode. The private provider must
revalidate the target and revision immediately before executing its native
semantic action. The public host cannot validate an opaque target itself.

Acceptance returns `outcome:queued` with an action ID. The guest must return
`{"type":"result","id":1,"outcome":"applied"}` or `rejected`. This records a
provider acknowledgement, not an independent task-success oracle. Only one
mutation may be pending. A two-second result timeout closes the control
connection, discards unsent transport data, and holds the action as uncertain.
There is no automatic replay. After inspecting its effects, a caller may send
`android_ack_uncertain` with the exact `id`; this invalidates old handles and
requires a fresh observation. Disconnects, resets, restores, malformed state
and regressing revisions invalidate handles. Completed/queued host timestamps,
round-trip duration, event/rejection counters, stream age/bytes and disconnected
slow-viewer counts are observable. They do not perform online model training.

## Real Android 4–17 acceptance gate

`tests/android_matrix.py` is an executable **fail-closed** qualification gate.
It takes one user-owned, already running native Megas Android session per major
release 4 through 17. The sessions and their source images must be real; no
synthetic provider fixture contributes a passing major release. Run from the
local owner computer:

```sh
python3 tools/megas/tests/android_matrix.py --megas tools/megas/build/megas-headless --matrix /private/local/android-matrix.json
```

The private JSON is deliberately not committed. It contains a `devices` array
with **exactly fourteen** objects, with integer `version` from 4 to 17 and
`runtime_dir`, `control_port`, `stream_port`, provider-issued `target`,
`operation`, `version_path`, `postcondition_path` and
`postcondition_equals`, `oracle_cmd` (a list of local executable arguments), and `oracle_id`.
The external judge must exit successfully and return JSON `{"oracle_id":"same-id","passed":true}` after the guest-reported state change. It must independently verify the actual app/task outcome rather than trust the guest provider or boot logs. The two paths are JSON pointers into the provider's
observed state; this public repository neither specifies nor stores the
private accessibility-tree layout. `set_text` additionally requires `text`.
An optional `max_seconds` is capped at 180 including the independent oracle check.

Each real session must provide its own observed Android major-version field,
current semantic target capability, authenticated continuous MPEG-TS packets,
a queued action that the guest reports as applied, a newer state reporting
that action's ID, and a postcondition that was false before and true after.
The entire test set fails if **any** release lacks evidence. The host does
not equate a producer's version string with independent OS attestation; image
provenance and actual decoded-video verification still require separate checks.
This gate never reads boot logs, uses ADB, takes PNGs or estimates coordinates.
It cannot certify unprovided devices, ARM apps, Play Store, or timing against
human baselines.

`make test-android-matrix-contract` tests this gate's shape and failure
conditions on CI without simulating Android compatibility success.

## Validation and remaining work

`make test-stream` checks authenticated HTTP, source availability, fragmented
byte-exact transfer, malformed-stream shutdown, and (when FFmpeg is installed
for tests) end-to-end decoding of ten actual H.264 frames. `make test-android`
uses a synthetic provider over real sockets to verify revision guards,
coordinate rejection, result/queue distinction, uncertainty and privacy.
`make test-isolation` additionally exercises real kernel thread, memory and CPU
caps when `MEGAS_TEST_CGROUP_PARENT` is explicitly supplied. Without a delegated
parent it reports a skip, not enforcement success. Linux CI supplies a separate
temporary cgroup and checks thread-creation refusal, a contained OOM kill, CPU
throttling counters, and group cleanup. These are host tests, not Android boot
or application tests. The existing
`test-task-budget` gate still requires real human/agent task reports and an
independent success oracle; no Android scenario has been certified by these
synthetic fixtures.

Remaining implementation/qualification includes the local private provider and
its research/fork/build, a guest encoder, per-version Android 4–17 image/device
profiles, verified ARM ABI translation or matching ARM hardware, Play/GApps
image management, modified-image coverage, Windows hard thread isolation, and
real application and human-time comparisons. No measured reduction of the
reported 343-thread Android instance has been established: that instance and
the user's local PC are not connected to this environment.

Primary-source feasibility checks: [Android accessibility node API](https://developer.android.com/reference/android/view/accessibility/AccessibilityNodeInfo),
[scrcpy architecture](https://github.com/Genymobile/scrcpy/blob/master/doc/develop.md),
[scrcpy prerequisites](https://github.com/Genymobile/scrcpy#prerequisites),
[Appium server](https://github.com/appium/appium-uiautomator2-server),
[OpenATX server](https://github.com/openatx/android-uiautomator-server),
[Android 17 setup](https://developer.android.com/about/versions/17/get).
Scrcpy's documented API 21 minimum cannot cover Android 4. Its MediaCodec video
path is evidence for an encoded source design, not an accessibility tree or a
fork integrated here. No third-party private fork has been claimed or uploaded.
