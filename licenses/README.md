# Licenses

WPE Browser's own code (the app, scripts and build files) is under the [MIT license](../LICENSE).
Two parts of this repository belong to other projects and keep their licenses:

- `patches/`: changes to WPE WebKit, under WebKit's licenses (LGPL-2.1 / BSD-style, as stated in each
  changed file).
- `ffmpeg-cedar/cedardec.c`: part of FFmpeg, LGPL-2.1-or-later (see its header).

## What the package contains

The `WPE/` folder of a release bundles the software below. Its `licenses/` folder holds this file, the MIT
license and `third-party.txt`, which has every license text, the exact versions and the source of each
part. `scripts/package.sh` generates it (`scripts/licenses.py`) and stops when a bundled file's origin is
unknown. It is a single file because the SD card is exFAT: every small file takes 128 KB there.

| Component | License | Source |
|---|---|---|
| WPE WebKit (`lib/libWPEWebKit-2.0.so.1`, `libexec/`) | LGPL-2.1, BSD-style; ANGLE, Skia, PDF.js, … BSD/MIT/Apache | [wpewebkit.org](https://wpewebkit.org/) release tarball + `patches/` |
| FFmpeg, minimal build for in-page video (`lib/libav*`) | LGPL-2.1-or-later (no GPL or nonfree parts) | [ffmpeg.org](https://ffmpeg.org/) release + `ffmpeg-cedar/`, `scripts/build-ffmpeg.sh` |
| mpv and its libraries (`mpv/`) | LGPL-2.1-or-later build of mpv and FFmpeg; OpenSSL Apache-2.0, dav1d BSD, libass ISC, FreeType FTL, HarfBuzz/libxml2/Expat/fontconfig MIT, FriBidi LGPL, SDL2 zlib; CA list MPL-2.0 | [mpv-tsp](https://github.com/matweew/mpv-tsp) |
| libcedarc (`lib/libvdecoder.so`, `libVE.so`, … and in `mpv/lib/`) | © Allwinner Technology, no published license (also part of TrimUI's firmware) | [CalvinXu17/libcedarc](https://github.com/CalvinXu17/libcedarc) |
| yt-dlp (`bin/yt-dlp`) | Unlicense; its bundled Python and libraries: PSF-2.0, MIT, BSD, … | [yt-dlp releases](https://github.com/yt-dlp/yt-dlp/releases) |
| EasyList, EasyPrivacy (`share/adblock/rules.json`, converted) | GPL-3.0-or-later or CC BY-SA 3.0+ | [easylist.to](https://easylist.to/) |
| Debian 12 packages: glibc, GLib, libsoup, GnuTLS, ICU, GStreamer and its plugins, …, DejaVu fonts, xkeyboard-config, shared-mime-info | each package's `copyright` (LGPL, GPL, MIT, BSD, …) | [sources.debian.org](https://sources.debian.org/) |

The device's own libraries that the browser uses (PowerVR EGL/GLES, TrimUI's SDL2, ALSA) are not bundled.
