# Android host integration: implementation and limits

This is the host side of the existing native Megas command centre. It is not
an Android distribution, an ARM translator, a Google Apps installer or an
Android compatibility certification. The private accessibility provider and its
local build recipe are not in this repository, its tests, CI or packages.

## GPU video path under development

Android launch uses the fork's GPU display backend with an explicit render node:
`-display megas,gl=on,rendernode=/dev/dri/renderD128,port=49396,fps=30,bitrate=1500`.
The listener exports GL scanout as DMA-BUF descriptors; existing DMA-BUF scanout
can also be referenced directly. It has no raw pixel pointer, shadow framebuffer,
CPU pixel copy, `glReadPixels`, RGB-to-YUV CPU conversion or software encoder.
The encoder graph is fixed to DRM PRIME, `hwmap` with `read+direct`, VAAPI GPU
color conversion/orientation, and `h264_vaapi`. Automatic filter conversion is
disabled. Direct import failure is an error, never an upload/download fallback.
These are implementation constraints, not a hardware zero-copy qualification.
Driver behavior and actual output still require measurement on a real GPU.

One encoding worker owns one in-flight descriptor. Native GPU fences precede
consumption, and QEMU blocks reuse of the producer scanout until the worker has
released the imported frames. Duplicating a DMA-BUF FD alone does not make its
pixels immutable. No delayed raw image queue is maintained. The configured
encoder has no B frames, one asynchronous submission, and must return the same
PTS/DTS as the submitted frame; delayed/reordered output fails the stream.

An idle source needs only clock packets between periodic refreshes. Slow output
connections close instead of accumulating video latency. The MPEG-TS mux and
HTTP relay still handle **encoded bytes** in CPU buffers. They are not a
zero-copy network transport. GPU color conversion writes GPU-owned NV12 output;
it does not transfer raw pixels through CPU memory. No end-to-end zero-copy or
zero-latency result is claimed by compilation or by an absent-GPU rejection test.

Android launch does not enable VNC/RFB, which could introduce a readback path.
The existing RFB preview continues to serve ordinary non-Android VMs. A native
GPU preview in the command centre and a model adapter that actually consumes the
encoded stream are still required; returning a stream URL does not itself make
a model watch live video. HLS playlists are not implemented.

The controller relays 188-byte MPEG-TS packets on an authenticated loopback HTTP
endpoint, accepts four viewers, and bounds pending encoded output to about
24 KiB per viewer. It does not take screenshots to produce video. Screenshots
and OCR remain explicit optional operations where supported, not agent defaults.

## Native VM launch and host budgets

The current launch device model is an **x86 PC**. The GPU device is currently `virtio-vga-gl`. This does not implement legacy
Android graphics drivers, an ARM Android board or Google's goldfish/ranchu model. Suitable locally prepared x86 images can use:

```json
{"action":"start","android":true,"disk":"/local/android.qcow2","ram_mb":2048,"accel":"kvm","rendernode":"/dev/dri/renderD128","cpus":1,"cpu_percent":100,"max_threads":48}
```

The GUI exposes the same Android switch and CPU budget. It does not download an
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

One QEMU `virtserialport` device is added only in Android mode:

| Guest channel | QEMU loopback backend | Host use |
| --- | --- | --- |
| `org.megas.control` | `android_port`, default 49395 | Persistent bounded JSONL |
| Native QEMU display (no guest port) | `video_port`, default 49396 | H.264/MPEG-TS source for the relay |
| Host video endpoint | `stream_port`, default 49397 | `GET /live.ts` |

The public HTTP endpoint requires `Authorization: Bearer <session credential>`
using the owner's existing protected session credential. No credential is put
in a stream URL, state response or journal. Endpoints bind only to loopback.
The response's `livestream.available` requires recent valid TS bytes. Missing
video returns 503, invalid authentication returns 403. The relay is deliberately
small; its HTTP authentication header uses the documented spelling above.

The semantic provider is separate software built and installed on the user's
computer. The public host treats its `payload` as opaque. It must emit:

```json
{"type":"state","epoch":"boot-session-id","revision":1,"guest_monotonic_ms":100,"cause_action_id":0,"payload":{}}
```

This example defines only the public envelope. It contains no private tree
representation. Revisions increase within an epoch. A bounded heartbeat may
confirm an unchanged state using `type:heartbeat`, `epoch` and `revision`.
`guest_monotonic_ms` and cause relationships are provider reports, not host
clock synchronization or inferred proof of causality. State payloads remain in
memory and are not written to the action journal.

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

## Acceptance evidence and remaining work

`megas-qemu.yml` builds the GPU path with compiler warnings treated as errors,
builds the real GPU fixture, and checks rejection of missing hardware. Its cloud
runner has no qualification GPU. Passing this job is evidence of compilation
and negative behavior only. It is not an Android or zero-copy hardware pass.
The earlier software encoder and BIOS video fixtures have been removed.

`gpu-video-fixture /dev/dri/renderD128 > gpu-colors.ts` renders the source with
GL, exports its DMA-BUF, imports directly into the hardware pipeline, and encodes
60 color frames. It must run on real hardware and its output must be decoded
independently to test channel order, frame completeness and timestamps. This is
an encoder test, not proof of Android UI state or application correctness.

Android qualification must use the actual live stream, actual accessibility
state and an independent oracle for the effects of semantic actions. A boot
property, a log message or a decoder accepting a video packet does not prove
that an Android task succeeded. The former boot-property acceptance experiment
has been removed. Each Android API/release and applicable application ABI needs
its own real execution and state evidence; one Android 9 run cannot establish
Android 4–17 coverage. No such complete set of executions exists here yet.

`make test-stream` tests encoded relay behavior. `make test-android` uses a
synthetic semantic provider to check revision guards, coordinate rejection,
queue/result distinction, uncertainty and privacy. These synthetic host tests
do not qualify real Android behavior. `test-task-budget` validates real task
reports against the human baseline; its unit tests are not task measurements.

`make test-isolation` checks real kernel thread, memory and CPU caps when a
parent cgroup is supplied. Otherwise it explicitly skips enforcement testing.
The native UI redraws on input and changed VM pixels/state, with a four-Hz idle
refresh. Linux CI measures actual XDamage events and control response time,
without screenshots. This is a UI/IPC test, not an Android task benchmark.

Work still includes actual GPU qualification, native GPU preview, the user's
local-only private accessibility fork/build, the live-video model adapter,
per-release Android 4–17 graphics/device/image integration, ARM application
support, Play/GApps image management, modified image coverage, Windows GPU and
thread isolation, and real application/human-time comparisons. The reported
343-thread Android instance and the user's local PC are not connected here;
its resource usage has not been measured or corrected by these cloud tests.
The private accessibility source and its build recipe must remain solely on
that local PC and must not be placed in this public repository or cloud CI.
