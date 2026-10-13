# Native GUI validation

![Megas native GUI running under Linux/Xvfb](gui-proof.png)

`gui-proof.png` is a byte-for-byte copy of the final 1360×900 Xvfb root capture.
It preserves the full screen, including the 1180×760 Megas window at the top-left
and the unused Xvfb desktop around it. The native Linux/X11 application was run
with `LANG=tr_TR.UTF-8` and `LC_ALL=C.UTF-8`; the available Xvfb core font
rendered complete Turkish labels using transliteration, and the default username
appeared without trailing glyphs. The image was captured from the running
application; no UI content was added or redrawn.

In the default Settings view, disk, installation image, memory, resolution and
account controls remain visible. The QEMU executable selector is inside the
closed-by-default advanced/developer section; its help says the package selects
the program automatically. A GUI-input harness clicked Start and verified that
both the default executable and a package-provided `MEGAS_QEMU` path are preserved
in the start request. Expanding the advanced section exposes the selector.

This verifies Linux/X11 rendering under Xvfb only. The VM was stopped, so the
image does not show live guest pixels or verify guest-agent policy or Doom launch.
No Windows 11 desktop or GDI runtime validation is claimed.
