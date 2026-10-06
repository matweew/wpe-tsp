# WPE Browser for TrimUI Smart Pro

A web browser for the **TrimUI Smart Pro** handheld (Allwinner A133P, 1 GB RAM, 1280×720), built on
**WPE WebKit 2.54** with GPU rendering on the device's PowerVR GE8300, a gamepad-driven pointer and an
on-screen keyboard.

- [Architecture](#architecture)
- [Requirements](#requirements)
- [Building](#building)
- [Installing and running](#installing-and-running)
- [Settings](#settings)
- [Features](#features)
- [Controls](#controls)
- [WebKit patches](#webkit-patches)
- [Repository layout](#repository-layout)
- [Troubleshooting](#troubleshooting)
- [Known limitations](#known-limitations)

## Architecture

```
┌────────────── UI process: bin/wpe-tsp (app/browser.c) ───────────────┐
│ SDL2 window + GLES2 renderer (device's PowerVR build of SDL2)        │
│ Custom WPEPlatform:  WPEDisplaySDL · WPEToplevelSDL · WPEViewSDL     │
│                      WPEInputMethodContextSDL                         │
│ Gamepad → pointer / scroll / key events      On-screen keyboard (osk)│
│ Memory watchdog, SELECT menu (menu.c), load-progress bar             │
│ wpe-tsp:// pages (History, Downloads), downloads (downloads.c)       │
│ Content filter (ad blocking), user scripts (video/embed → mpv)       │
│ Video: bundled yt-dlp → mpv (player.c), screen handover, yt-dlp updates│
└───────▲───────────────────────────────────────────────┬──────────────┘
        │ frames: DMA-bufs from ION (WPEBufferDMABuf),   │ IPC
        │ zero-copy; shared memory as the fallback       ▼
┌───────┴──────── WPEWebProcess ──────────┐   ┌──── WPENetworkProcess ────┐
│ WebCore + JavaScriptCore (JIT)          │   │ libsoup 3 + GnuTLS        │
│ Skia (Ganesh GL) + compositor on the GPU│   │ (glib-networking module)  │
│ surfaceless EGL on PowerVR GE8300       │   │ disk cache on ext4        │
│ renders straight into ION DMA-bufs      │   └───────────────────────────┘
└─────────────────────────────────────────┘
```

**Rendering path.** WPE WebKit 2.54 bundles the *WPEPlatform* API, which lets an application provide its
own display backend. The device has no KMS/GBM display or Wayland compositor (the screen is an fbdev
framebuffer driven by TrimUI's SDL2 build). Its PowerVR EGL 1.4 driver can't export GPU buffers to another
process (no `EGL_MESA_image_dma_buf_export`), and there is no GBM to allocate shareable ones, so stock
WebKit falls back to copying every frame through shared memory. The driver can *import* DMA-bufs
(`EGL_EXT_image_dma_buf_import`), and the kernel has the Android ION allocator (`/dev/ion`), so
WebKit patch 0008 allocates the frames there:

1. The **WebProcess** renders pages on the GPU (Skia + WebKit's compositor) in a surfaceless EGL context
   on `EGL_DEFAULT_DISPLAY`. `launch.sh` sets `WEBKIT_DMABUF_ION`, so each swap-chain buffer is an
   ION DMA-buf (heap 0, `sys_user`: ordinary pages, since the GPU has an MMU; linear ARGB8888, rows padded
   to 32 pixels). It is imported as an EGLImage and bound to a texture that Skia renders into, and its
   fd is sent to the UI process. The frame's GPU work is waited for (EGL fence) before it is handed over.
2. The **UI process** (`app/browser.c`, `app/dmabuf.c`) implements the WPEPlatform classes.
   `WPEViewSDL::render_buffer` receives a `WPEBufferDMABuf`. The first time it sees a buffer, it imports it
   into the SDL renderer's GLES context as an EGLImage and binds it as the storage of an SDL texture.
   Showing a frame is then a plain `SDL_RenderCopy`, with the pointer, keyboard, menu and progress bar
   drawn on top. A buffer goes back to WebKit only after the GPU has finished the frame that showed it.
3. Without `/dev/ion`, if an ION buffer can't be created, or with `WPE_TSP_ZERO_COPY=0` in the environment
   (diagnostics), WebKit's own "SharedMemory" mode is used
   instead: `glReadPixels` into shared memory, and the UI uploads the damaged rectangle into an SDL
   streaming texture.
4. Pages are laid out at a **device scale** (default 1.5: pages see an 853×480 CSS viewport) and rendered
   at the full 1280×720 (1279×720, from rounding the 853.3 px viewport).

Measured with an auto-scrolling test page: **59 fps zero-copy vs 25 fps with copies** (which also move
~90 MB/s of pixels through the CPU).

`webkit://gpu` on the device reports *2D canvas: Accelerated*, *GPU threaded rendering*,
*GL_RENDERER: PowerVR Rogue GE8300*.

**Input.** The gamepad drives a virtual mouse (left stick), wheel scrolling (right stick), arrow keys
(d-pad) and navigation. Text input goes through `WPEInputMethodContextSDL`:
- when WebKit focuses an editable field after a click, the on-screen keyboard (`app/osk.c`) opens;
- typed text is sent with the `committed` signal; Backspace and Enter are sent as key events;
- the page view shrinks above the keyboard so the field stays visible.

**Memory (1 GB RAM, no swap, no zram in the kernel).**
- One web process at a time: process-swap-on-navigation and the web-process cache are disabled (WebKit
  patch 0007, enabled from `launch.sh`). Several processes at once exhausted the RAM.
- WebKit memory-pressure settings: the page process frees caches at 40% / 60% of its limit and is killed
  at the limit (`PAGE_MEMORY_LIMIT_MB`, default 550). The network process has a 200 MB limit and is never
  killed.
- Cache model `DOCUMENT_BROWSER`: no back/forward page cache, small in-memory caches.
- `MALLOC_ARENA_MAX=2`: about 50 MB less in the network process.
- An app **watchdog** closes the page when `MemAvailable` drops below 90 MB. Without swap, the kernel
  thrashes (re-reading code from the SD card) long before its OOM killer would act.
- Page processes get `oom_score_adj` 1000 (network 500), so the kernel kills a page, not the UI.
- A page killed for memory shows a "needs more memory" page instead of reloading in a loop. Real crashes
  are reloaded up to 3 times.
- The **mobile user agent** (default) gets lighter sites. Measured page/network process peaks, mobile vs
  desktop UA: YouTube 125/66 vs 199/34 MB (served as m.youtube.com); w3schools 107/88 MB vs killed (network
  process past 228 MB); mezha.ua and Wikipedia are responsive and roughly equal.
- **Ad/tracker blocking** (default on) removes the biggest memory spikes. Without it, the network process on
  ad-heavy pages grew to 150–370 MB and kept that memory. Measured on vs off: mezha.ua page/network 37/40
  vs 119/146 MB, loaded in 4 s vs 11 s; w3schools (desktop UA) 54/20 MB vs killed (network process at
  351 MB). YouTube is unchanged, since its ads are first-party.

**Runtime packaging.** The app is built against Debian bookworm (glibc 2.36). The device has glibc 2.33, so
the package ships its own glibc and dynamic loader:
- every binary's interpreter is set to `/mnt/SDCARD/Apps/WPE/lib/ld-linux-aarch64.so.1`, with a `DT_RPATH`
  of `lib/`, then `/usr/trimui/lib` (SDL2), `/usr/lib` (PowerVR GLES/EGL);
- the **full** glibc set is bundled (libc, libdl, libpthread, librt, libm, …). Otherwise the PowerVR blobs
  pull the device's 2.33 pieces into the process and fail on `GLIBC_PRIVATE` symbols;
- the SD card is exFAT (no symlinks), so libraries are copied as real files named by SONAME. Its
  128 KB clusters make every small file cost 128 KB, so data sets are kept small: only the XKB files
  for the one keymap WPE compiles are shipped (`scripts/xkb-minimal.py`, 37 files instead of 292),
  and the browser's own keyboard layouts come precompiled in a single file (see *Keyboard layouts*);
- the device has no MIME database, so GIO couldn't tell a `file://` page's type and WebKit showed HTML
  as plain text. The builder generates shared-mime-info's `mime.cache` (one 148 KB file, without the
  6 MB of XML it's built from), shipped as `share/mime/mime.cache`; `launch.sh` puts `share/` first in
  `XDG_DATA_DIRS`;
- fonts: the bundled DejaVu set comes first, and the device's own Source Han Sans
  (`/usr/trimui/res/full.ttf`) is the fallback for Chinese, Japanese and Korean, at no extra package size.
  Other system libraries are deliberately **not** reused: the device's GLib (2.50), libstdc++ (GCC 10),
  libxml2 and glibc are older than WPE 2.54 needs, and the few with matching SONAMEs add up to ~8 MB.
  Only the hardware-specific PowerVR GLES/EGL drivers and TrimUI's SDL2 come from the device;
- WebKit's install prefix **is** the device path `/mnt/SDCARD/Apps/WPE`, so it finds `libexec/` helpers at
  its compiled-in location. The app must be installed exactly there.

## Requirements

**Build host:** Linux x86-64 with **Docker**, about 15 GB of free disk space, and 16 GB+ RAM recommended
(WebKit uses about 1.5 GB per compile job). A full WebKit build takes about 1–1.5 hours with 12 jobs.
Rebuilds after small changes take minutes (ccache plus the kept build directory).

**Build container (`Dockerfile`, image `wpe-tsp-builder`):**

| Component | Version / source |
|---|---|
| Base | Debian bookworm |
| Cross compiler | `aarch64-linux-gnu-g++-12` (GCC 12.2; WebKit 2.54 requires ≥ 12.2) |
| Build tools | CMake 3.25, Ninja, ccache, Perl, Ruby, Python 3, gperf, unifdef, patchelf |
| Target libraries (`:arm64` multiarch) | GLib 2.74, libsoup 3.2, ICU 72, HarfBuzz 6, FreeType, Fontconfig, libjpeg, libpng, libwebp, libepoxy, libgcrypt, libtasn1, libxkbcommon, libxml2, libxslt, SQLite, zlib, lcms2, WOFF2, EGL/GLES headers, glib-networking (GnuTLS) |
| Runtime data | DejaVu fonts, XKB keymaps (extracted into `/opt/runtime-data`) |
| SDL2 headers | TrimUI SDK `SDL2-2.26.1.GE8300` (into `/opt/SDL2-2.26.1`) |

Two arm64 dev packages (`libsoup-3.0-dev`, `libsysprof-4-dev`) are force-installed with
`dpkg --force-depends`, because they depend on `gobject-introspection:arm64`, which can't be co-installed
on an amd64 host. Introspection isn't built. As a result, later Dockerfile layers can't use `apt-get install`.

**From the device** (`sysroot-device/`, created by `scripts/fetch-device-sdl.sh`): the device's
`libSDL2-2.0.so.0` (2.30.x, PowerVR build) and `libSDL2_ttf-2.0.so.0` to link against, plus `SDL_ttf.h`.
At runtime the app uses the copies in `/usr/trimui/lib`.

**Device:** TrimUI Smart Pro with stock Tina Linux (kernel 4.9, glibc 2.33), SSH access (`root`), and Wi-Fi.
About 250 MB on the SD card (incl. mpv 27 MB and yt-dlp 40 MB), plus the ~10 MB rule list. The profile,
HTTP cache, history and compiled ad-block list use `/mnt/UDISK/wpe-browser` (internal ext4). Everything
the browser needs, video playback included, is in its own directory: no other app is required.

**Prebuilt binaries in `runtime/`** (not built by this project):
- `runtime/mpv/`: `mpv` and `lib/` come from the mpv-trimui-build project (`build.sh` → `dist/`): mpv 0.36
  with an SDL2 GLES context for the GE8300 and its libraries (FFmpeg 6.1, libass, dav1d, …, built against
  the device's glibc), including the **Allwinner Cedar hardware H.264 decoder** (`h264_cedar` in libavcodec,
  plus the libcedarc libraries `libvdecoder`, `libVE`, `libvideoengine`, `libawh264`, …; see that project's
  `cedar/README.md`). `mpv.conf`, `input.conf` and `ca-certificates.crt` are the browser's own (originally
  from the youtube-tsp client); `mpv.conf` adds `vd=h264_cedar,`. `package.sh` copies the folder to `mpv/`
  as is (no bundling or patching).
- `runtime/yt-dlp`: the official `yt-dlp_linux_aarch64` release. It is only the initial copy: on the
  device the browser keeps `bin/yt-dlp` up to date itself (see Video playback), and `deploy.sh` installs
  it only if the device has none.

These binaries are not in git (`.gitignore`): copy `mpv` and `lib/` from mpv-trimui-build's `dist/`
into `runtime/mpv/`, and download `yt-dlp_linux_aarch64` from the yt-dlp releases as `runtime/yt-dlp`
before running `package.sh`. Their config (`mpv.conf`, `input.conf`, `ca-certificates.crt`) is tracked.

## Building

All commands run from the repository root. Run containers as your own user (`--user`), otherwise build
directories become root-owned.

```bash
# 1. Build the cross-compilation image (once, ~10 min)
docker build -t wpe-tsp-builder .

# 2. Get the device's SDL2 libraries for linking (once; device reachable over SSH)
echo root@192.168.31.36 > .device    # the device's ssh target, used by all device scripts
scripts/fetch-device-sdl.sh

# 3. Build WPE WebKit: downloads the 2.54.0 tarball into src/, applies patches/, builds and
#    installs into build/stage/ (first run ~1-1.5 h; the argument is the number of jobs)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work \
    wpe-tsp-builder scripts/build-webkit.sh 12

# 4. Build the app and assemble the device package in dist/WPE (~1 min; downloads EasyList and
#    EasyPrivacy into build/adblock on the first run, ADBLOCK_REFRESH=1 to update them)
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work wpe-tsp-builder scripts/package.sh
```

- `scripts/build-webkit.sh` holds the WebKit CMake options (see the `-D…` list). Disabled: GStreamer
  (video/audio/WebRTC/Web Codecs/EME), GBM/libdrm, the DRM and
  Wayland platforms, the legacy libwpe API, the GPU process, bubblewrap sandbox, WebDriver, spellcheck,
  speech, gamepad API, ATK, AVIF/JPEG XL, hyphenation, introspection and docs.
- To rebuild WebKit after editing `src/`, run step 3 again (incremental). Patches are applied only once
  (marker file `src/wpewebkit-2.54.0/.patched`). After changing a patch, delete `src/wpewebkit-2.54.0` so it
  is re-extracted and re-patched.
- `scripts/package.sh` compiles `app/*.c`, copies WebKit's libraries and helpers, the GnuTLS GIO module,
  fonts, keyboard layouts, XKB data and the ad-block rule list. It then runs `scripts/bundle-libs.sh`, which resolves the
  shared-library closure, bundles glibc, and sets the interpreter and rpath.
- The app icon is generated by `scripts/make_icon.py app/icon.png` (Python 3 + Pillow).

## Installing and running

Put the device's SSH target in `.device` once (the scripts read it; `DEVICE=root@<ip>` overrides it
per command):

```bash
echo root@192.168.31.36 > .device
```

Tip: give the device a fixed address with a DHCP reservation in your router, so `.device` doesn't need
updating.

```bash
scripts/deploy.sh             # install or update the app on the device
scripts/deploy.sh --dry-run   # only list what would be sent
scripts/deploy.sh --delete    # also remove files on the device that are no longer in the package
```

`scripts/deploy.sh` works without rsync (the device has none):
- it checksums the files on both sides (`md5sum`, ~3 s); if the device can't be listed, it stops without
  sending anything (an unreachable device must not look like an empty one);
- it sends **only the changed files** in one tar stream over ssh, e.g. ~200 KB after an app-only change,
  instead of 171 MB;
- it stops the browser first when something changed, because running binaries can't be overwritten;
- it installs `settings.conf` only if the device has none, so your settings survive updates. New
  settings (keys the device's file doesn't have) are appended with their comments; existing values are
  never changed. Likewise `bin/yt-dlp` is installed only if missing, since the browser updates it on the
  device. Use `--delete` to clean up leftovers; it keeps `settings.conf`, `fonts.conf`, the log and
  `bin/yt-dlp*`.

The app always lives at `/mnt/SDCARD/Apps/WPE` (see Architecture). It appears in TrimUI's **Apps** menu
as **WPE Browser** (`config.json`, `icon.png`).

- **Starting over SSH:** `scripts/run-on-device.sh [url]` starts it the way MainUI does (an optional URL is
  opened first; Home stays `HOME_URL`). It writes
  `/tmp/cmd_to_run.sh`, which `/usr/trimui/bin/runtrimui.sh` runs, then `killall -9 MainUI` (a plain kill
  is ignored). Starting `launch.sh` directly from SSH leaves MainUI drawing on the screen.
- **Screenshot:** `scripts/screenshot.sh out.png` captures the framebuffer of the running device.
  While the page moves, the capture (36 ms) spans several screen refreshes and shows horizontal bands
  from different frames; that's the capture, not the screen.
- **Log:** `/mnt/SDCARD/Apps/WPE/wpe-tsp.log` (stdout/stderr of the app and WebKit, overwritten at each start).

## Settings

Edit `/mnt/SDCARD/Apps/WPE/settings.conf` on the device (shell syntax; the defaults are in
`app/settings.conf`). Changes apply at the next start. Updates never overwrite this file. A setting that is missing
from it uses the browser's built-in default below (`launch.sh` passes on only what the file sets).

| Setting | Default | Meaning |
|---|---|---|
| `HOME_URL` | `https://mezha.ua` (in the shipped `settings.conf`) | Start page and SELECT → Home. No built-in default: unset or empty, the browser starts on WebKit's empty page |
| `KEYBOARD_LAYOUTS` | `us,ua,ru` | Keyboard layouts (XKB names, optional variant: `de(nodeadkeys)`) for the on-screen and USB/Bluetooth keyboards; any xkb-data layout works (see *Keyboard layouts*). Switched with the language key / Alt+Shift / Super+Space |
| `START_PAGE` | `home` | What opens at start: `home` (`HOME_URL`), `last` (newest History entry; `HOME_URL` if History is empty) or `address` (empty page with the address bar open). A URL given to `launch.sh` overrides it |
| `SEARCH_URL` | `https://www.google.com/search?q=` | Address-bar text that isn't a URL is searched here (query appended) |
| `SCALE` | `1.5` | Page scale; 1.5 = text 1.5× larger, still rendered at 1280×720 |
| `PAGE_MEMORY_LIMIT_MB` | `550` | Memory a page may use before it's closed (150–900) |
| `POINTER_HIDE_SECONDS` | `10` | Hide the pointer after this long without using it (0 = never) |
| `HISTORY_SIZE` | `20` | Recently visited pages kept in History (0 = don't record, max 60) |
| `DOWNLOAD_DIR` | `/mnt/SDCARD/Downloads` | Where downloads are saved (created if missing) |
| `PLAY_YOUTUBE_IN_MPV` | `1` | Play videos in mpv: YouTube pages, embeds and `<video>`/`<audio>` (0 = off) |
| `USER_AGENT` | `mobile` | `mobile` (iPhone Safari), `desktop` (WPE's own Linux UA) or a full UA string |
| `AD_BLOCK` | `1` | Block ads and trackers (EasyList + EasyPrivacy domains); 0 = off |
| `WEBGL` | `0` | WebGL (via ANGLE on the PowerVR GPU); 1 = on. See Features |

`launch.sh` (not meant for editing; it's replaced by updates) also sets:
- a wake lock, so the device doesn't sleep while browsing;
- `WEBKIT_SKIA_MSAA_SAMPLE_COUNT=0` (4× MSAA is costly on this GPU);
- `MALLOC_ARENA_MAX=2`, `WEBKIT_DISABLE_PSON=1`, `WEBKIT_DISABLE_WEB_PROCESS_CACHE=1`;
- the font, TLS-module and XKB paths;
- the profile and cache directory, `/mnt/UDISK/wpe-browser`.

WebKit's disk cache needs hard links, which exFAT lacks, so it lives on the internal ext4 partition.

Diagnostics:
- `WPE_TSP_STATS=1` logs frames per second, upload MB/s and the share of each frame that changed, every
  5 s;
- `WPE_TSP_CONSOLE=1` writes the pages' JavaScript console messages to the log.

## Features

### Mobile user agent

By default the browser identifies as **iPhone Safari** (`USER_AGENT=mobile`), so sites serve
their mobile versions, which are much lighter on memory and CPU (see the measurements in *Memory*). iPhone
Safari was chosen over Android Chrome because the engine *is* WebKit, so sites' Safari code paths fit.
**SELECT → Request desktop site** reloads the page with WPE's own desktop UA, for sites whose mobile
version lacks something; the item then reads *Request mobile site*. The switch lasts for the session.

**SELECT → Portrait mode** lays the page out for a 720×1280 screen and shows it turned 90°
counterclockwise, the same way mpv shows portrait videos (hold the device turned clockwise, right stick at
the bottom). The d-pad and both sticks turn with the device: physical left acts as up, right as down,
down as left, up as right, in
pages, the menu and the on-screen keyboard alike. Everything is drawn into a 720×1280 render target
that is rotated onto the screen (one extra full-screen copy per frame; landscape draws directly).
*Landscape mode* switches back; the choice lasts for the session.

### Ad and tracker blocking

The browser uses WebKit's native content blocking (`WebKitUserContentFilterStore`, the
mechanism behind Safari content blockers), so blocked requests are never made:
- `scripts/make-adblock.py` converts the **domain rules** of EasyList and EasyPrivacy (`||host^`,
  `$third-party`, `@@` exceptions) into WebKit's JSON format: about 94,000 rules,
  `share/adblock/rules.json`. Element-hiding rules are not converted.
- The browser compiles the list **once per rules file**, about 25 s in the background after an update
  (peak +140 MB in the browser process), into a 33 MB file in
  `/mnt/UDISK/wpe-browser/data/wpe-browser/content-filters`. Later starts load it instantly. WebKit maps
  the compiled list from disk, so it doesn't cost RAM per page. Older compiled versions are deleted.
- `package.sh` caches the lists in `build/adblock/`; `ADBLOCK_REFRESH=1` downloads fresh copies.

### History

SELECT → History opens a page served by the app at `wpe-tsp://history`:
- it lists the last `HISTORY_SIZE` visited web pages, newest first, with title, site and time, and a
  *Clear history* link;
- the d-pad moves the highlight, START opens the entry, B goes back, and pointer clicks work too (a hint
  bar at the bottom of the History and Downloads pages says so);
- the list is stored in `/mnt/UDISK/wpe-browser/data/wpe-browser/history.txt` (`uri<TAB>time<TAB>title`
  per line), since WPE itself only keeps the session's back/forward list.

### Downloads

Downloads (`app/downloads.c`) are saved to `DOWNLOAD_DIR`. WebKit streams them straight to the SD
card, so large files don't use RAM. There are three ways to start one:
- **Clicking a link to a file the browser can't show** (video, audio, archives…), or one the server sends as
  an attachment, opens a prompt: *Download name? — size · type · free space* → Download / Cancel.
  (`decide-policy` → `webkit_policy_decision_download()`.)
- **SELECT → Downloads → Save link under pointer** saves the target of the link under the pointer (WebKit's
  hit test), even an ordinary page.
- **SELECT → Downloads → Save video/audio from this page** lists the `<video>`/`<audio>`/`<source>`
  addresses and media-file links found on the page, so you can pick one. Streams (`blob:`, HLS/DASH, e.g.
  YouTube) and DRM media can't be saved.

While a download runs, a status strip at the bottom shows the name, % and MB; a "Saved: path" (or error)
notice follows for a few seconds. `wpe-tsp://downloads` (SELECT → Downloads → Show downloads) lists:
- this session's active downloads (A cancels), plus failed and cancelled ones;
- **all files in `DOWNLOAD_DIR`** (from any session), newest first, with size and date. Video and audio
  files can be played (A: play);
- the free space. *Clear list* only clears the session entries; files are never deleted from the page.

Existing files are never overwritten (`name (1).ext`), and partial files are removed when a download fails
or is cancelled.

### WebGL

WebKit is built with WebGL (ANGLE, translating to the device's GLES 3.2). It is **off by default**
(`WEBGL=0`), so pages see no WebGL and fall back to their 2D versions. With `WEBGL=1`:
- a WebGL canvas shares its texture with WebKit's compositor and reaches the screen through the
  zero-copy path: a full-screen animated shader (1279×648) runs at **47 fps**;
- it costs memory: +16 MB in the web process for that one small scene, more for real 3D content;
- some sites switch to much heavier versions when WebGL is available (Google Maps' vector map instead of
  image tiles), which matters with 1 GB of RAM.
- The reported GPU is "Apple GPU": WebKit masks the real renderer name behind the iPhone user agent.

### Video playback in mpv

Playback (`app/player.c`) uses the bundled `mpv/mpv` and `bin/yt-dlp`, with the same options as the
youtube-tsp client (`--no-ytdl --config-dir=mpv`, subtitles fonts from `share/fonts`; yt-dlp format: H.264 +
AAC up to 720p, the screen's resolution). H.264 is decoded by the Allwinner hardware decoder (`vd=h264_cedar,`
in `mpv.conf`); other codecs and H.264 the hardware can't do (10-bit, 4:2:2, 4:4:4) fall back to FFmpeg's
software decoders automatically. Measured with mpv on screen (20 s clips, whole system, 4 cores):

| H.264 | software | hardware |
|---|---|---|
| 720p30 | 34% CPU | 14% CPU |
| 720p60 | 69% CPU, 44 frames dropped | 28% CPU, 10 dropped |
| 1080p30 (downloaded files) | 65% CPU, 4 dropped | 19% CPU, none dropped |
| 1080p60 | 95% CPU, 988 of 1192 dropped | 29% CPU, 309 dropped (limited by copying frames to the GPU) |

It mirrors that app's `src/player.c`:
- **YouTube video pages** (`youtube.com/watch?v=`, `m.youtube.com`, `youtu.be/`, `/shorts/`) are detected
  when the page address changes, which covers link clicks and YouTube's in-page navigation. The page
  keeps loading (description, likes, comments) while the browser runs `yt-dlp -f <format> --print live_status --print resolution -g` (with an HLS retry; portrait videos
  are rotated). While it runs, the status strip shows *Loading video… B: cancel*; errors (e.g. *Video
  unavailable*) are shown there too. A just-finished live stream (`post_live`) is reported instead of
  played (see Known limitations).
- **Resolved URLs are cached** in memory per video (16 most recent), so watching again or coming back to
  a video starts mpv right away. An entry is used until 10 minutes before the links' own expiry (the
  `expire=` value googlevideo.com puts in them, about 6 hours); links without one and live streams
  aren't cached. If mpv fails within 10 s on cached links (e.g. they were tied to another IP address),
  the entry is dropped and yt-dlp runs again.
- The browser then releases its SDL window and starts mpv fullscreen, with mpv's own libraries
  (`LD_LIBRARY_PATH=mpv/lib:/usr/trimui/lib`) and certificates (`mpv/ca-certificates.crt`, also used by
  yt-dlp).
- While mpv plays, the page is unmapped (hidden), so WebKit stops rendering it and throttles its timers.
  Measured with a YouTube watch page loaded: MemAvailable stayed above 320 MB during playback.
- When mpv exits, the window is recreated, the buttons pressed meanwhile are discarded, and the browser
  **stays on the video page** to read the comments or like the video. The page's own player (which could
  only show YouTube's "can't play" notice) is covered by the video's thumbnail with a **▶ Play in mpv**
  button to watch again: a user script on YouTube's top frame that follows in-page navigation and posts
  the page address to the same `wpeTspPlay` handler as embedded players. The same video doesn't play again when YouTube rewrites the page URL;
  leaving the page and coming back to it (or clicking another video) plays again.
- **Embedded YouTube players** (`youtube.com/embed/…` and `youtube-nocookie.com` iframes on other sites)
  can't play in this build. A user script injected into those frames covers them with a **▶ Play in mpv**
  button, which sends the video to the browser through a script message handler (`wpeTspPlay`). The
  browser plays it the same way, and you stay on the page afterwards. The button is built with DOM calls,
  because YouTube enforces Trusted Types, which reject `innerHTML` strings.
- **`<video>`/`<audio>` elements** on any page are replaced with a box (keeping the element's size and
  poster) labelled *Play video · file* or *Play audio · file*. A click sends the source URL (`src` or
  the first `<source>`) to mpv, including HLS `.m3u8` and DASH `.mpd`. Only `blob:` sources, which exist
  only inside the page's JavaScript, can't be played.
- **Video/audio files:** the download prompt offers **Play** / Download / Cancel, which plays the file's
  URL directly in mpv. On `wpe-tsp://downloads`, saved video/audio files show **A: play** and play from
  the SD card.

**yt-dlp updates** (`app/ytdlp.c`, ported from youtube-tsp's `src/updater.c`). YouTube changes break
old yt-dlp versions, so 30 s after start the browser compares `bin/yt-dlp --version` (cached in
`bin/yt-dlp.version` until the binary changes) with the latest GitHub release (looked up at most once a
day, cached in `bin/yt-dlp.checked`):
- a newer release found by that day's lookup is offered in a prompt: *Download and install* / *Later*;
- if yt-dlp is missing, the browser offers to download it (also when a YouTube video is opened);
- installing shows progress in the status strip, then a report: *yt-dlp updated from … to …*, or what
  went wrong. The download goes to `bin/yt-dlp.new` and must match the release's `SHA2-256SUMS` and run
  `--version`; the old binary is kept as `bin/yt-dlp.bak` and the new one is renamed into place. A failed
  step never touches the working binary.

## Controls

Browsing (A is the main action, B only cancels):

| Control | Action |
|---|---|
| Left stick | Move the pointer (light tilt = precise); shows a hidden pointer |
| A | Left click (hold to drag/select); if the pointer is hidden, only shows it |
| B | Stop loading; on the app's own pages (History, Downloads) go back |
| Right stick | Smooth scroll |
| D-pad | Arrow keys (with repeat) |
| L1 / R1 | Page up / page down |
| L2 / R2 | Top / bottom of the page |
| X | Reload |
| Y | Address bar |
| START | Enter |
| SELECT | Menu: Back, Forward, Home, History, Downloads, Request desktop/mobile site, Portrait/Landscape mode, Address bar, Zoom in/out/reset, Exit |
| MENU | Left to the system |

**USB/Bluetooth keyboard and mouse** (`hid.c`). The device's SDL opens input devices but can't
use udev, so it doesn't report them properly. The browser reads keyboards (devices with letter
keys) and mice (relative X/Y + left button) straight from `/dev/input/event*`, picks them up
when plugged in (inotify on `/dev/input`), and doesn't grab them, so the system's volume keys
keep working. Keys are translated with one XKB keymap per layout in `KEYBOARD_LAYOUTS` (default
`us,ua,ru`; WPE's own keymap is US only) and go to the page with their modifiers. With Ctrl, Alt or
Super held the first layout is used, so Ctrl+C/V/L work in any layout. The mouse moves the same pointer (not rotated in portrait mode: it's used in
the user's frame), clicks with left/middle/right, and scrolls with the wheel. Like A, the first
click with a hidden pointer only shows it.

| Key | Action |
|---|---|
| Alt+Shift / Super+Space | Next keyboard layout (shown briefly in the status strip; the on-screen keyboard switches along) |
| Ctrl+L / F6 | Address bar (type, ←/→/Home/End, Backspace/Delete, Enter, Esc closes) |
| F5 / Ctrl+R | Reload |
| Alt+← / Alt+→ | Back / forward |
| Menu key | SELECT menu (↑/↓, Enter, Esc) |

Typing into a page field closes the on-screen keyboard, and it stops popping up for fields until
the next mouse click or A press.

On-screen keyboard (Android-style; opens for the address bar and when a text field is clicked):

| Control | Action |
|---|---|
| D-pad / left stick | Move between keys (with repeat) |
| A | Press the key |
| B | Close the keyboard |
| X | Backspace |
| Y | Space (in the address bar: first press keeps a selected URL for editing) |
| L1 | Shift (twice = caps lock) |
| R1 / language key | Next layout (those in `KEYBOARD_LAYOUTS`, e.g. EN, UA, RU) |
| SELECT / `?123` | Letters ↔ symbols |
| START / ↵ | Enter, labelled **Go** or **Search** in the address bar |

Address bar specifics:
- the current URL opens **selected**: typing replaces it, and ⌫ clears it;
- up from the top row reaches the **✕** button: A clears the line, and d-pad left/right moves the text
  cursor;
- an empty bar hints "Search Google or type a URL", and input with spaces or without a dot is searched.

**Keyboard layouts** (on-screen and physical keyboards alike) come from `KEYBOARD_LAYOUTS` in
`settings.conf`: a comma-separated list of XKB layouts, optionally with a variant, e.g.
`KEYBOARD_LAYOUTS="us,ua,de(nodeadkeys)"`. The first is the default, and the language key / Alt+Shift
go through them in that order. To add a layout, add its name there and restart the browser; names are
those of `localectl list-x11-keymap-layouts` / `…-variants <layout>` on any Linux desktop (or
`rules/evdev.lst` of xkb-data). Keep a Latin layout (e.g. `us`) in the list for typing addresses on the
on-screen keyboard. A name that doesn't exist is reported in `wpe-tsp.log` and skipped.

All 577 layouts/variants of xkb-data are precompiled at build time (`scripts/xkb-keymaps.py`, using
`xkbcli` from libxkbcommon-tools) into `share/xkb-keymaps.bin`: 5.8 MB in one file (each keymap
zlib-compressed on its own; the browser inflates only the ones configured, ~65 KB each) instead of
the XKB tree's hundreds of small files, 128 KB each on the exFAT card. The on-screen keyboard's letter
rows are read from the same keymaps: the letter keys of the Q, A and Z rows, plus letters on the keys
next to them (like Ukrainian ґ on the backslash key). Digits and symbols are built in.

Button numbering of the device's SDL joystick ("Xbox 360 Controller"): B=0, A=1, Y=2, X=3, L1=4, R1=5,
SELECT=6, START=7, MENU=8. L2/R2 are axes 2/5 (rest at −32768), and the sticks are axes 0/1 and 3/4, with
no stick clicks. See `app/gamepad.h`.

## WebKit patches

All in `patches/`, applied by `scripts/build-webkit.sh` to the 2.54.0 release tarball:

| Patch | Why |
|---|---|
| `0001-surfaceless-default-display-fallback` | PowerVR EGL 1.4 has no client extensions (`EGL_MESA_platform_surfaceless`, `eglGetPlatformDisplay`). Fall back to `eglGetDisplay(EGL_DEFAULT_DISPLAY)` with `EGL_KHR_surfaceless_context`, so the WebProcess gets a GPU context and uses shared-memory frames. |
| `0002-gcc12-layoutrect-non-constexpr` | GCC 12 rejects a `constexpr` function calling a non-constexpr constructor. |
| `0003-guard-jshtmlmediaelement-custom-video` | Missing `ENABLE(VIDEO)` guard breaks builds without video. |
| `0004-webkit-gio-unix-include-dir` | `gio-unix-2.0` include path isn't propagated to the WebKit target in this cross setup. |
| `0005-drm-fourcc-fallback-without-libdrm` | `DRM_FORMAT_XRGB8888` used without libdrm. |
| `0006-skia-epoxy-core-gles3-proc-fallback` | The PowerVR `eglGetProcAddress` returns NULL for core GLES 3 functions (`glGetStringi`), so Skia's GL interface failed and the WebProcess crashed. Fall back to `dlsym(libGLESv2)`. |
| `0007-env-single-web-process` | `WEBKIT_DISABLE_PSON` / `WEBKIT_DISABLE_WEB_PROCESS_CACHE` environment switches. WPE otherwise always swaps processes per site and caches suspended ones. |
| `0008-ion-dmabuf-render-targets` | Zero-copy frames without GBM or dma-buf export: with `WEBKIT_DMABUF_ION=<heap mask>`, the UI process offers hardware buffers and the WebProcess's surfaceless swap chain allocates its render targets from ION (legacy ≤4.11 and current uAPI), imports them with `EGL_EXT_image_dma_buf_import` and sends them as DMA-buf buffers. Falls back to shared memory per buffer if allocation or import fails. |
| `0009-angle-gcc12-resourcemap-static-assert` | ANGLE doesn't compile with GCC 12, which evaluates a `static_assert` inside a discarded `if constexpr` branch (`ResourceMap.h`): made it a runtime `ASSERT`. |
| `0010-angle-gles-proc-dlsym-fallback` | ANGLE's GL backend looks functions up with `eglGetProcAddress`, then in `libEGL`. PowerVR returns NULL for core GLES functions and exports them only from `libGLESv2`: look there as well (same issue as patch 0006). |

## Repository layout

```
Dockerfile, toolchain-aarch64.cmake   cross-compilation container and CMake toolchain
patches/                              WebKit patches (see above)
app/
  browser.c                           UI process: WPEPlatform on SDL2, input, menu, memory watchdog
  osk.c / osk.h                       on-screen keyboard
  menu.c / menu.h                     SELECT menu, prompts, status strip
  downloads.c / downloads.h           download manager and wpe-tsp://downloads page
  player.c / player.h                 video playback via the bundled mpv + yt-dlp
  ytdlp.c / ytdlp.h                   yt-dlp version check and safe update
  dmabuf.c / dmabuf.h                 zero-copy frames: DMA-buf → EGLImage → SDL texture
  pages.h                             shared look + d-pad navigation of the wpe-tsp:// pages
  gamepad.h                           device button/axis numbering
  launch.sh                           launcher (environment, wake lock)
  settings.conf                       user settings (see Settings)
  config.json, icon.png               TrimUI app entry
runtime/
  mpv/                                prebuilt mpv + libraries + config (from youtube-tsp)
  yt-dlp                              initial yt-dlp binary (updated on the device by the browser)
scripts/
  build-webkit.sh                     fetch/patch/configure/build WPE WebKit → build/stage
  package.sh                          build the app, assemble dist/WPE
  bundle-libs.sh                      library closure, glibc bundling, interpreter/rpath
  fetch-device-sdl.sh                 copy the device's SDL2 libs into sysroot-device/
  deploy.sh                           checksum-based sync of dist/WPE to the device
  run-on-device.sh, screenshot.sh     device helpers (launch like MainUI, framebuffer capture)
  device.sh                           reads the device's ssh target from .device
  make_icon.py                        generates app/icon.png
  xkb-minimal.py                      minimal XKB data for WPE's own (us) keymap
  xkb-keymaps.py                      every XKB layout precompiled into share/xkb-keymaps.bin
  make-adblock.py                     EasyList/EasyPrivacy domain rules -> WebKit content-blocker JSON
.device                               ssh target of the device, e.g. root@192.168.31.36 (local)
sysroot-device/                       device SDL2/SDL2_ttf for linking (generated)
src/                                  WebKit tarball + patched tree (generated)
build/webkit, build/stage             WebKit build tree and install staging (generated)
dist/WPE                              the installable app (generated)
.ccache                               compiler cache (generated)
```

## Troubleshooting

- **Page blank, garbled or wrong colors after an update:** start the browser with `WPE_TSP_ZERO_COPY=0`
  in the environment (e.g. in `/tmp/cmd_to_run.sh`) to compare with copied frames, and check `wpe-tsp.log` for `ION`/`dmabuf` messages. `WPE_TSP_STATS=1` shows
  `upload 0.0 MB/s` when zero-copy frames are in use.
- **Black screen, or `web process terminated` repeating in the log:** the WebProcess can't create its GPU
  context. Check that patches 0001 and 0006 are applied, and that `/usr/lib/libEGL.so.1` and
  `libGLESv2.so.2` exist on the device.
- **`symbol lookup error … GLIBC_PRIVATE`:** a glibc piece from the device was loaded. Make sure all of
  `libc.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libm.so.6 …` are in `lib/` (`scripts/bundle-libs.sh`).
- **`Failed to create hard link` in the log:** the cache is on exFAT. `launch.sh` must point
  `XDG_CACHE_HOME` to `/mnt/UDISK`.
- **No HTTPS:** `GIO_MODULE_DIR` must point to `lib/gio/modules` (`libgiognutls.so`). CA certificates come
  from the device's `/etc/ssl/certs/ca-certificates.crt`.
- **Device gets slow on heavy pages:** check the log for `low memory` (watchdog) or `page killed`. Lower
  `PAGE_MEMORY_LIMIT_MB` if the device still thrashes. Raise it if pages are closed while plenty of memory
  is free.
- **A video doesn't play:** the status strip shows yt-dlp's error. mpv's own log is `/tmp/mpv_last.log`.
  YouTube changes break old yt-dlp versions: the browser offers updates (at most once a day); to check
  again now, delete `bin/yt-dlp.checked` and restart. `bin/yt-dlp.bak` is the previous version, if the new
  one misbehaves.
- **Diagnostics:** `WPE_TSP_STATS=1` (frame upload stats) and `WPE_TSP_CONSOLE=1` (page console messages)
  can be added to the `launch.sh` command, e.g. via `/tmp/cmd_to_run.sh`.
- **Debugging a crash:** WebKit is built without debug info. The unstripped
  `build/stage/mnt/SDCARD/Apps/WPE/lib/libWPEWebKit-2.0.so.1.*` still has symbols, so
  `aarch64-linux-gnu-addr2line -f -C -e <lib> <offset>` (inside the container) resolves backtrace offsets.

## Known limitations

- **No audio/video inside pages** (Web Audio, WebRTC, MediaSource/`blob:` players): GStreamer isn't built.
  YouTube pages and embeds, `<video>`/`<audio>` with real file or HLS/DASH sources, and media-file links
  play in mpv instead (see Controls). Players built on `blob:` streams don't play.
- **WebGL is off by default** (`WEBGL=1` enables it, see Features). Only WebGL 1/2 via ANGLE's GLES
  backend; no WebGPU.
- **Recently finished YouTube live streams** (yt-dlp `live_status=post_live`) can't be played. Until
  YouTube processes them into normal videos (usually within hours), they exist only as DASH fragments:
  `yt-dlp -g` returns a single fragment (the last few seconds), and mpv's ytdl_hook can't join fragments
  without durations. The browser detects this and says so instead of playing those seconds.
- **Single page, no tabs.** Back/Forward reload pages from the disk cache, since there is no page cache.
- **Swap isn't used:** the kernel has no zram, and swap on the internal eMMC would wear the flash that holds
  the OS.
