# Nuklear sources

Vendored from the official `Immediate-Mode-UI/Nuklear` GitHub repository.
The headers retain their original license notices. We select the MIT license
in LICENSE. No downloaded executable code is installed at runtime.

* `nuklear.h`: upstream blob `f1912193cc5832b6b649c5939131799027c973cb`
  (version 4.13.3), fetched through the GitHub connector.
* `nuklear_xlib.h`: upstream `demo/x11/nuklear_xlib.h`, blob
  `e17088cbfb3e2236d68a707ec51adfb1b6f93017`, fetched 2026-10-13.
* `nuklear_gdi.h`: upstream `demo/gdi/nuklear_gdi.h`, blob
  `aaaf0443b2c3189da995a453b3428df1e2756253`, fetched 2026-10-13.
* `LICENSE`: upstream `LICENSE`, blob
  `6d1be10648640f9cf193c108c5335187ca3bed7d`, fetched 2026-10-13.

Native Xlib/GDI backends avoid a browser, SDL, OpenGL, FFmpeg or video frame
queue. The app draws its current live RFB framebuffer directly into the native
viewport. The backend headers are unchanged; application integration is in
`../src/gui.c`. Integration uses persistent text editors for asynchronous X11
clipboard paste. GDI bitmap resize and teardown are handled by the application
to release each old bitmap after deselecting it and close the memory DC using
`DeleteDC`.
