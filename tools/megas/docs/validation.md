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
to 4,096, its input/output scratch buffers to 16/8 KiB, checkpoint metadata
to 16 retained names, in-memory history to 128 actions, and mutations stop
when the journal reaches 64 MiB. The OCR child is limited to 48 MiB, 8 CPU
seconds, a 10-second wall deadline and 4,096 bytes of returned text. This run
exercised normal use and OCR memory sampling; it did not fill the journal to
its cap or force checkpoint eviction at the 16-checkpoint boundary.

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

## Validation follow-up — 2026-10-14

The Windows Server 2022 native CI job (`38452749483`, job `115487561860`)
passed all 15 live-QEMU checks, the platform process test, the NoCloud seed
test and the agent stdio test. Its four pixel-verified checkpoint restores
took 309.66–313.62 ms. Tesseract
returned nonempty text for current image generation 6 and its measured peak
process memory was 32,931,840 bytes, below the 48 MiB limit. This is Windows
Server 2022 runner evidence; it does not establish Windows 11 desktop GUI,
QGA, or Doom end-to-end behavior. The live test now samples Windows process
working set through the native process-memory API and requires those owner RSS
measurements to be available.

The latest Linux Actions job (`38452749483`, job `115487561239`) failed the
required live OCR check. The controller reported `OCR did not produce a
bounded successful result (exit 1, memory cap 48 MiB); no text was inferred.`
The nonempty OCR text, current-generation match and 48 MiB measurement remain
required; this failure is not counted as a pass. On OCR assertion failures,
`tests/test_live.py` now records the text-only adapter reply before validating
it and preserves a bounded status excerpt and tail of the combined OCR child
log in the JSON report, so the captured child diagnostics can identify why
Tesseract exited unsuccessfully.

A local Linux reproduction then passed all 15 live-QEMU checks with QEMU 8.2.2.
It produced 58 OCR text bytes for current generation 7 and measured a peak of
30,056,448 bytes. This local run does not replace or clear the failed Actions
result above. A separate forced-exit test made a temporary Tesseract wrapper
write a fixture diagnostic to stderr and exit 1; the failure report retained
the adapter response, owner status, and bounded OCR log tail before cleanup.
