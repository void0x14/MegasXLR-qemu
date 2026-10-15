# Validation and measured limits

`tests/test_live.py` drives an actual QEMU process through the authenticated
native controller. Its 512-byte BIOS guest is embedded in the test, so the
checks do not depend on a general-purpose operating system or guest account.
The guest writes received keys into VGA memory, and the controller receives
screen changes over its persistent native RFB connection.

The test validates PNG chunks, CRCs, scanlines and decoded pixels. It pauses
the guest, queues a key, resumes it and checks that the pixels change only
after execution resumes. It saves `past` after showing `a`, saves `future`
after showing `b`, and restores both in both directions. Each restore must
reproduce the corresponding guest pixels; a successful command reply alone
does not satisfy the check.

Other live checks cover token authentication, one owner per runtime, input,
repairable bad commands, checkpoint history, reset, current image generation,
agent PNG output on both successful and rejected tool calls, Tesseract OCR,
and QEMU cleanup after stop and owner exit. The status-freshness assertion
checks the age of the last QMP status query. `frame_age_ms` instead measures
time since framebuffer pixels changed; a static screen can have an old frame
while the native RFB connection is healthy.

Run from `tools/megas` with QEMU, `qemu-img` and the other optional development
dependencies on `PATH`:

```sh
make all headless test
python3 tests/test_live.py build/megas-headless --report /tmp/megas-live.json
```

Set `MEGAS_TESSERACT` to the Tesseract executable when it is outside `PATH`.
Use `--qemu /absolute/path/to/qemu-system-x86_64` and `--qemu-img
/absolute/path/to/qemu-img` when those tools are outside `PATH`. The live test
creates a temporary local disk and does not download a guest OS.

## Validation record — 2026-10-13

The final working tree was freshly built on Linux in
`build/luna-validate-final`; `make test` passed. It covered native RFB,
platform process handling, NoCloud seed ISO, engine QMP behavior, the fake-QGA
bridge, 15 live-QEMU checks and agent stdio. The source was uncommitted while
these checks ran, so no source commit hash identifies this result.

The environment was Linux `6.18.44-x86_64`, glibc 2.39, 9 logical CPUs, with
an 8 GiB cgroup memory limit and an 8-CPU quota. Live VM checks used the
available Debian QEMU `8.2.2` system emulator on TCG with 64 MiB of guest RAM;
that emulator was external to this repository and was not built from its QEMU
source. Tesseract 5.3.4 and the English trained data were installed and
selected for the OCR test.

The live run received framebuffer generation 3 at the first status check,
with `fresh_state_age_ms` of 0. The final OCR result matched image generation
8, returned 58 text bytes and sampled a peak of 30,220,288 bytes (about 28.8
MiB), below the controller's 48 MiB OCR-process limit. The screenshot was
720×400 and 871,372 bytes; its decoded-pixel SHA-256 was
`8ae59fdc05ab358df03dc2850ce508ae0044b54c7e97c93936f53e1452e585ff`.
Pixel-verified restores took 796.66, 563.37, 312.36 and 330.41 ms in this
run. The controller's observed RSS was 1,016 KiB idle, 2,512 KiB live and
2,772 KiB after restores. QEMU's observed RSS was 60,440 KiB.

The controller keeps one current RFB framebuffer rather than a frame queue.
The reviewed source limits the RFB image to 4,194,304 pixels, each dimension
to 4,096, its input/output scratch buffers to 16/8 KiB, and the checkpoint
catalog to 64 entries total. Automatic garbage collection retains at most 16
controller-owned checkpoints; foreign checkpoints are never automatically
collected. In-memory history is limited to 128 actions, and mutations stop
when the journal reaches 64 MiB. The OCR child is limited to 48 MiB, 8 CPU
seconds, a 10-second wall deadline and 4,096 bytes of returned text. This run
exercised normal use and OCR memory sampling; it did not fill the journal to
its cap or trigger eviction at the 16-owned-checkpoint retention limit.

`tests/test_platform.c` passed natively. It checks exclusive file creation
without overwriting an existing file, literal argv delivery with spaces,
quotes, backslashes and shell metacharacters, and cleanup of an owned process
tree. Windows x64 GUI and headless builds were cross-compiled with MinGW-w64;
the platform test was also cross-compiled. This Linux host did not execute the
Windows build, so it does not verify Windows desktop launch, installer
behavior or runtime job cleanup.

This live suite uses a BIOS fixture. It does not validate an arbitrary guest's
password or screen-lock policy, a full Linux desktop, or playable Doom. The
controller and OCR child RSS are additional process costs. QEMU's observed RSS
includes its resident guest pages plus emulator overhead. The configured 64
MiB guest RAM is a capacity setting, not another 64 MiB to add to QEMU RSS; the
resident amount depends on pages touched by the guest. The controller's
sub-100-MiB observation applies only to the control layer and is not a total
memory claim for the VM system.

The Linux GUI was freshly linked and visually checked in a 1360×900 Xvfb
session with `LANG=tr_TR.UTF-8` and `LC_ALL=C.UTF-8`. Turkish labels were
readable through the available core-font transliteration fallback; controls
fit, and the default username field displayed `megas` without a trailing
artifact. The extracted X11 static archive lacked its xcb dependency, so this
host GUI build linked the installed shared `libX11.so.6`.

The test's 5-second per-restore assertion is a measured target for this
environment, not a guarantee for arbitrary operating systems, VM sizes, host
storage or hardware. Resize success requires a guest display stack that
supports QEMU's advertised display change; changing the host window size does
not establish that the guest resolution changed.

A full-file `scripts/checkpatch.pl` scan of the 18 authored C source, header
and test files (excluding vendored headers) reported 7,693 errors and 345
warnings, mostly whitespace, brace and line-length style diagnostics from the
fork's compact formatting. Review of the non-format findings found no concrete
functional defect; no broad style rewrite was made.

## Historical validation follow-up — 2026-10-14 (before checkpoint catalog update)

The Windows Server 2022 native CI job (`38452749483`, job `115487561860`)
passed all 15 live-QEMU checks, the platform process test, the NoCloud seed
test and the agent stdio test. Its four pixel-verified checkpoint restores
took 309.66–313.62 ms. Tesseract
returned nonempty text for current image generation 6 and its measured peak
process memory was 32,931,840 bytes, below the 48 MiB limit. This is Windows
Server 2022 runner evidence; it does not establish Windows 11 desktop GUI,
QGA, or Doom end-to-end behavior. That run predates the Windows native RSS
sampling change; at the time this historical note was written, CI confirmation
of the sampler was pending. See the newer native CI record below.

The initial Linux Actions job (`38452749483`, job `115487561239`) failed the
required live OCR check with `OCR did not produce a bounded successful result
(exit 1, memory cap 48 MiB); no text was inferred.` Later diagnostic runs
(`38497904385` for push and `38497909496` for the PR) exposed the child log:
`libgomp` could not create a thread (`Resource temporarily unavailable`),
followed by `std::bad_alloc`. The measured OCR RSS was 36,081,664 bytes in the
push run and 35,885,056 bytes in the PR run, both below 48 MiB. The cause was
the Linux `RLIMIT_DATA` limit constraining OpenMP thread data and virtual stack
allocations as well as ordinary data, while the independent RSS measurements
stayed under budget. The nonempty OCR text, current-generation match and RSS
limit remain required. On OCR assertion failures, `tests/test_live.py` records
the text-only adapter reply before validating it and preserves a bounded
status excerpt and tail of the combined OCR child log in the JSON report.

A local Linux reproduction then passed all 15 live-QEMU checks with QEMU 8.2.2.
It produced 58 OCR text bytes for current generation 7 and measured a peak of
30,056,448 bytes. This local run does not replace or clear the failed Actions
result above. A separate forced-exit test made a temporary Tesseract wrapper
write a fixture diagnostic to stderr and exit 1; the failure report retained
the adapter response, owner status, and bounded OCR log tail before cleanup.

After the child-only fix, local `make test` passed. Its live report shows all
15 checks passing, with nonempty Tesseract output for current image generation
7, a matching adapter generation, and peak OCR memory of 30,130,176 bytes.
On Linux, the bounded child launcher now sets `OMP_THREAD_LIMIT=1` only after
fork and only for memory-capped children; the only current bounded call site
is the OCR process. This keeps the OpenMP worker allocations within the
existing `RLIMIT_DATA` budget while leaving the controller environment and
48 MiB RSS watchdog unchanged. At the time of this historical note, the
post-fix Actions rerun and Windows RSS sampler CI check were still pending;
the later CI record below supersedes that status.

## Checkpoint budget and re-owner validation — 2026-10-14

Checkpoint allocation is measured from QEMU's `actual-size` for the exact
`megasdisk` qcow2 node, relative to the startup baseline. The growth budget is
`max(2 GiB, 2 × configured VM RAM + 64 MiB)`. Before a save, the controller
reserves configured VM RAM within that budget and requires at least 512 MiB
plus configured VM RAM of host filesystem free space. It remeasures after each
oldest-owned deletion. The catalog is bounded to 64 entries total, with at
most 16 Megas-owned checkpoints retained; foreign checkpoints are never
automatically deleted. QMP tags use the internal `megas-owned:` prefix while
display labels stay unchanged. Re-imported foreign checkpoints can be
explicitly restored only when QEMU reports saved VM state; disk-only entries
are listed as non-restorable.

The local Linux `make test` run passed all engine budget/catalog cases, the
bundle tests (8 passed, 1 skipped), all 17 live-QEMU checks, and the agent
stdio test. The engine tests cover allocation pressure and bounded eviction,
the 16-owned/64-total limits, preservation of foreign labels including the
legacy `megas-owned-important`, and fail-closed handling of unknown or
malformed checkpoint state. The live tests used QEMU 8.2.2 on Linux TCG. A
replacement controller imported all 16 owned checkpoints in QMP order, chose
unique `step-46-before-2` and `step-46-after-2` labels when action 46 collided
with imported history, then restored `past` and `future` from the previous
owner. Both post-restart pixel hashes matched their original guest states:
`dab5ed8f3335b5bdd5570b85ee0ae33402df94cf50f35bb6b068c87f22e6935a` and
`8ae59fdc05ab358df03dc2850ce508ae0044b54c7e97c93936f53e1452e585ff`.
Tesseract returned 58 bytes for current image generation 8 and sampled a peak
of 30,134,272 bytes, below 48 MiB. This local validation predates the native CI
record below; the earlier CI results above do not validate the checkpoint
catalog changes.

## Native CI record — 2026-10-14, commit b7bdb815

This record is for [commit
`b7bdb81500c222c8d878f46f9f3a5799a1542b15`](https://github.com/void0x14/MegasXLR-qemu/commit/b7bdb81500c222c8d878f46f9f3a5799a1542b15),
[push run 38503004757](https://github.com/void0x14/MegasXLR-qemu/actions/runs/38503004757)
and [PR run 38503008393](https://github.com/void0x14/MegasXLR-qemu/actions/runs/38503008393).
All four native test steps passed their 17 real-QEMU checks, including
checkpoint re-import, unique automatic checkpoint labels after owner restart,
pixel-verified restores and process RSS sampling. The two Linux jobs completed
successfully, including Linux bundle smoke testing and artifact upload:
[push job 115654057179](https://github.com/void0x14/MegasXLR-qemu/actions/runs/38503004757/job/115654057179),
[PR job 115654068514](https://github.com/void0x14/MegasXLR-qemu/actions/runs/38503008393/job/115654068514).

Both Linux jobs used QEMU 8.2.2 on TCG with 64 MiB configured guest RAM. They
imported 16 owned checkpoints; the replacement owner restored `past` and
`future` to matching framebuffer hashes
(`dab5ed8f3335b5bdd5570b85ee0ae33402df94cf50f35bb6b068c87f22e6935a` and
`8ae59fdc05ab358df03dc2850ce508ae0044b54c7e97c93936f53e1452e585ff`).
After restart, action 48 collided with imported history and safely produced
`step-48-before-2` and `step-48-after-2`. OCR returned 58 bytes for current
image generation 8; peak OCR process RSS was 38,256,640 bytes on push and
38,440,960 bytes on the PR run. Controller RSS ranged from 1,992–2,056 KiB
idle and 3,552–3,616 KiB live; QEMU RSS was 67,088–78,000 KiB.

Both Windows Server 2022 jobs used QEMU 11.1.2 on TCG with 64 MiB configured
guest RAM. They passed the same 17 live checks and the agent stdio test. Each
imported 16 owned checkpoints and verified the same `past` and `future` pixel
hashes after owner restart. Collision-safe labels were created at step 55 on
push and step 52 on the PR run. OCR returned 58 bytes for current image
generation 5; peak OCR process RSS was 32,956,416 bytes on push and 33,058,816
bytes on the PR run. Controller RSS ranged from 4,028–4,044 KiB idle and
5,516–5,536 KiB live; QEMU RSS was 49,128–50,148 KiB. The native test records
do not establish Windows 11 desktop behavior.

The Windows private bundle built, extracted, and passed its isolated installer,
installed command-line, QEMU, Tesseract and English-data checks. The packaged
GUI opened a native window and closed after a normal close request, but both
Windows jobs then failed the final exit-code assertion because PowerShell
reported a blank process `ExitCode`. The [push Windows job
115654057480](https://github.com/void0x14/MegasXLR-qemu/actions/runs/38503004757/job/115654057480)
and [PR Windows job
115654068673](https://github.com/void0x14/MegasXLR-qemu/actions/runs/38503008393/job/115654068673)
are therefore failures overall; the Windows bundle upload steps were skipped.
This entry records those exact runs and remains historical if the smoke check
is later changed and rerun.

## Android task-time acceptance gate

Android agent speed is a release requirement measured against a human doing the
same task. For every Android task/profile advertised as supported, the agent
must reach the independently verified goal at the same quality with:

`T_agent(task, profile) <= T_human(task, profile) + 300 seconds`

This is a hard per-scenario gate. Three minutes is not a fixed task limit;
the allowed completion time is the human reference time plus at most five
minutes. An aggregate average or percentile cannot hide a scenario that misses
the bound.

The comparison uses the same task instructions, Android image and API level,
ABI, VM profile, starting guest state, network access, and success conditions.
The clock starts when the task is handed to the operator or agent and stops
only when an independent task-specific oracle verifies the required final
state and side effects. If VM startup is part of the task, include it in both
times; otherwise start both from the same ready state. Record the boundary
before timing begins.

Set the human reference before measuring the agent. For repeated human trials,
record every successful run and use the median as `T_human`; do not select a
reference after seeing the agent result. A run passes only when the task oracle
confirms the same quality and the elapsed-time bound is met. A fast but
incorrect or incomplete result fails.

Each benchmark record must include task and profile identifiers, Android API,
image variant, ABI or translation layer, host and VM configuration, starting
state identifier, human samples and reference time, agent completion time,
time difference, oracle result, and failure reason. Record observation-to-action
and action-to-confirmation times as diagnostic breakdowns so slow decisions,
missing state, and delayed feedback can be located.

A profile cannot be marked supported until every benchmark scenario assigned
to it passes this gate. A miss means the design or implementation must be
corrected before support is claimed. The existing BIOS guest timing and
controller latency checks above do not establish Android task-time compliance;
no Android human-baseline measurements are present in this validation record.

The machine-checkable report gate is implemented in `tests/check_task_budget.py`.
Run `make test-task-budget` for its contract tests and
`python3 tests/check_task_budget.py /path/to/android-task-report.json` to check a
captured report. The report's `support_matrix` lists every profile and required
scenario; the checker rejects missing scenarios and requires coverage of every
Android API level from 14 through 37. Each scenario records its task and profile,
API/image/ABI, host and VM configuration, starting state and task digests, oracle,
human timing samples, the time the baseline was frozen, and all agent runs.
Every agent run must start after the human baseline is frozen, pass its oracle,
and finish no later than 300 seconds after the human median.
