#!/usr/bin/env python3
"""Write <out>/licenses/third-party.txt: the license texts of everything dist/WPE bundles.

Every file in the package is traced to where it came from: WebKit's build, our FFmpeg, mpv-tsp,
libcedarc, yt-dlp, EasyList or a Debian package (through dpkg's file lists, the bundle manifest
of scripts/bundle-libs.sh and the GStreamer stage's dpkg data in /opt/gstreamer-dpkg). A file of
unknown origin stops the build, so anything newly bundled gets its license added here.

One file instead of a folder of texts: the SD card is exFAT with 128 KB clusters, where each
small file costs 128 KB. Identical texts are written once. Downloaded texts are cached in
build/licenses. Runs inside wpe-tsp-builder, from scripts/package.sh.

Usage: licenses.py <out-dir> <bundle-manifest>
"""
import hashlib
import json
import os
import re
import sys
import tarfile
import urllib.request

OUT, MANIFEST = sys.argv[1:3]
WORK = '/work'
CACHE = f'{WORK}/build/licenses'
MULTIARCH = '/usr/lib/aarch64-linux-gnu'
RUNTIME_DATA = '/opt/runtime-data'
COMMON_LICENSES = '/usr/share/common-licenses'
# (status file, *.list dir, doc dir): the builder's dpkg, then the GStreamer stage's (Dockerfile)
DPKG_DBS = [('/var/lib/dpkg/status', '/var/lib/dpkg/info', '/usr/share/doc'),
            ('/opt/gstreamer-dpkg/status', '/opt/gstreamer-dpkg', '/opt/gstreamer-dpkg/doc')]
CEDARC_LIBS = {'libvdecoder.so', 'libVE.so', 'libMemAdapter.so', 'libcdc_base.so',
               'libvideoengine.so', 'libcwrapper.so', 'liblogwrapper.so', 'libawh264.so'}
GH = 'https://raw.githubusercontent.com'


def die(msg):
    sys.exit(f'licenses.py: {msg}')


def fetch(url):
    """Text at url, cached in build/licenses (the build stays offline once cached)."""
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, re.sub(r'[^A-Za-z0-9._-]+', '_', url.split('://', 1)[1]))
    if not os.path.exists(path):
        try:
            # creativecommons.org refuses Python's default user agent
            req = urllib.request.Request(url, headers={'User-Agent': 'wpe-tsp-package'})
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
        except Exception as e:
            die(f'cannot download {url}: {e}')
        with open(path + '.tmp', 'wb') as f:
            f.write(data)
        os.rename(path + '.tmp', path)
    return read(path)


def read(path):
    with open(path, 'rb') as f:
        return f.read().decode('utf-8', 'replace')


def script_var(script, name):
    m = re.search(rf'^{name}=(\S+)', read(f'{WORK}/scripts/{script}'), re.M)
    return m.group(1) if m else die(f'no {name}= in scripts/{script}')


# --- dpkg: file -> package, package -> version/source/copyright

owners, packages = {}, {}
for status, lists, docs in DPKG_DBS:
    if not os.path.exists(status):
        continue
    for para in read(status).split('\n\n'):
        f = dict(re.findall(r'^([A-Za-z-]+): ?(.*)$', para, re.M))
        if 'install ok installed' not in f.get('Status', '') or f.get('Architecture') not in ('arm64', 'all'):
            continue
        src = f.get('Source', f['Package']).split()
        version = src[1].strip('()') if len(src) > 1 else f['Version']
        packages.setdefault(f['Package'], {'source': src[0], 'version': version, 'docs': docs})
    for name in os.listdir(lists):
        if not name.endswith('.list'):
            continue
        pkg = name[:-5].split(':')[0]
        for line in read(os.path.join(lists, name)).splitlines():
            owners.setdefault(line, pkg)
            if line.startswith('/lib/'):  # merged /usr: /lib is /usr/lib
                owners.setdefault('/usr' + line, pkg)


def owner(path):
    pkg = owners.get(path) or owners.get(os.path.realpath(path))
    if not pkg:
        die(f'no Debian package owns {path}')
    return pkg


def debian_copyright(pkg):
    for docs in [packages[pkg]['docs']] + [d for _, _, d in DPKG_DBS]:
        path = f'{docs}/{pkg}/copyright'
        if os.path.exists(path):
            return read(path)
    die(f'no copyright file for Debian package {pkg}')


# --- where each packaged file comes from

manifest = {}
for line in read(MANIFEST).splitlines():
    soname, src = line.split('\t')
    manifest[soname] = src

origin = {}          # package-relative path -> component key
debian = {}          # Debian package -> package-relative paths
for root, dirs, files in os.walk(OUT):
    dirs.sort()
    for name in sorted(files):
        rel = os.path.relpath(os.path.join(root, name), OUT)
        top = rel.split('/')[0]
        key = None
        if top == 'licenses':
            continue
        elif top == 'lib':
            src = manifest.get(name) if os.path.dirname(rel) == 'lib' else None
            if name in CEDARC_LIBS:
                key = 'cedarc'
            elif src and src.startswith(f'{WORK}/build/ffmpeg/'):
                key = 'ffmpeg'
            elif rel.startswith('lib/wpe-webkit-2.0/') or (src and src.startswith(OUT + '/')):
                key = 'webkit'
            elif src:
                key = 'deb:' + owner(src)
            elif rel.startswith(('lib/gstreamer-1.0/', 'lib/gio/')):
                key = 'deb:' + owner(os.path.join(MULTIARCH, rel[4:]))
        elif top == 'libexec':
            key = 'webkit'
        elif top == 'mpv':
            key = 'cedarc' if name in CEDARC_LIBS else 'mpv'
        elif rel == 'bin/yt-dlp':
            key = 'ytdlp'
        elif rel.startswith('share/fonts/'):
            key = 'deb:fonts-dejavu-core'
        elif rel.startswith('share/xkb'):
            key = 'deb:xkb-data'
        elif rel.startswith('share/mime/'):
            key = 'deb:shared-mime-info'
        elif rel.startswith('share/adblock/'):
            key = 'easylist'
        elif rel == 'bin/wpe-tsp' or '/' not in rel:
            key = 'own'
        if not key:
            die(f'unknown origin of {rel}: add it to scripts/licenses.py')
        origin[rel] = key
        if key.startswith('deb:'):
            debian.setdefault(key[4:], []).append(rel)

# Runtime data packages were unpacked with dpkg -x: not in dpkg's database
for pkg in ('fonts-dejavu-core', 'xkb-data', 'shared-mime-info'):
    packages.setdefault(pkg, {'source': pkg, 'version': '(bookworm)', 'docs': f'{RUNTIME_DATA}/usr/share/doc'})


def files_of(key):
    return sorted(rel for rel, k in origin.items() if k == key)


# --- output

seen = {}            # text hash -> label of its first copy
common = set()       # /usr/share/common-licenses texts referenced
sections = []


def text(label, body, dedupe=True):
    """A license text, or a pointer to the identical one written earlier."""
    common.update(n.rstrip('.') for n in re.findall(r'/usr/share/common-licenses/([A-Za-z0-9.+_-]+)', body))
    h = hashlib.sha256(body.strip().encode()).hexdigest()
    if dedupe and h in seen:
        return f'---- {label}: the same text as "{seen[h]}" above\n'
    seen[h] = label
    return f'---- {label}\n\n{body.rstrip()}\n\n'


def section(title, files, about, texts):
    out = ['=' * 100, title, '=' * 100]
    if files:
        shown = files if len(files) <= 12 else files[:10] + [f'... and {len(files) - 10} more']
        out.append('Files: ' + '\n       '.join(shown))
    out += [about.strip(), '']
    out += [text(label, body) for label, body in texts]
    sections.append((title, '\n'.join(out)))


def webkit_licenses(version):
    """LICENSE/COPYING files of the WebKit release tarball (cached: reading it takes a while)."""
    cache = f'{CACHE}/wpewebkit-{version}.json'
    if os.path.exists(cache):
        return json.loads(read(cache))
    tarball = f'{WORK}/src/wpewebkit-{version}.tar.xz'
    if not os.path.exists(tarball):
        die(f'{tarball} is missing (scripts/build-webkit.sh downloads it)')
    found = {}
    with tarfile.open(tarball) as tar:
        for m in tar:
            path = m.name.split('/', 1)[-1]
            if (m.isfile() and re.match(r'(COPYING|LICEN[CS]E)', os.path.basename(path), re.I)
                    and not re.search(r'test', path, re.I)):
                found[path] = tar.extractfile(m).read().decode('utf-8', 'replace')
    os.makedirs(CACHE, exist_ok=True)
    with open(cache, 'w') as f:
        json.dump(found, f)
    return found


def ytdlp_release(path):
    """The yt-dlp release whose aarch64 binary is this file (matched by SHA-256)."""
    with open(path, 'rb') as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    cache = f'{CACHE}/yt-dlp-{digest}.tag'
    if os.path.exists(cache):
        return read(cache).strip()
    with urllib.request.urlopen('https://api.github.com/repos/yt-dlp/yt-dlp/releases?per_page=30', timeout=60) as r:
        tags = [rel['tag_name'] for rel in json.load(r)]
    for tag in tags:
        if digest in fetch(f'https://github.com/yt-dlp/yt-dlp/releases/download/{tag}/SHA2-256SUMS'):
            with open(cache, 'w') as f:
                f.write(tag)
            return tag
    die('runtime/yt-dlp matches none of the last 30 yt-dlp releases')


webkit_version = script_var('build-webkit.sh', 'WEBKIT_VERSION')
webkit = webkit_licenses(webkit_version)
section(f'WPE WebKit {webkit_version}', files_of('webkit'), f"""
The web engine. LGPL-2.1 and BSD-style licenses, plus those of the third-party code in its tree
(ANGLE, Skia, ICU helpers, PDF.js, ...): every license file of the release follows.
Source: https://wpewebkit.org/releases/wpewebkit-{webkit_version}.tar.xz, with the changes in patches/
of https://github.com/matweew/wpe-tsp (the build: scripts/build-webkit.sh).
""", sorted(webkit.items()))

ffmpeg_version = script_var('build-ffmpeg.sh', 'VERSION')
ffmpeg_src = f'{WORK}/src/ffmpeg-{ffmpeg_version}'
section(f'FFmpeg {ffmpeg_version}', files_of('ffmpeg'), f"""
Decoders for in-page video and audio (through GStreamer's gst-libav). LGPL-2.1-or-later: built without
--enable-gpl and --enable-nonfree, plus the h264_cedar decoder (ffmpeg-cedar/cedardec.c, LGPL-2.1-or-later).
Source: https://ffmpeg.org/releases/ffmpeg-{ffmpeg_version}.tar.xz, ffmpeg-cedar/cedardec.c and
scripts/build-ffmpeg.sh in https://github.com/matweew/wpe-tsp.
""", [('LICENSE.md', read(f'{ffmpeg_src}/LICENSE.md')), ('COPYING.LGPLv2.1', read(f'{ffmpeg_src}/COPYING.LGPLv2.1'))])

mpv_licenses = f'{OUT}/mpv/licenses/third-party.txt'
if not os.path.exists(mpv_licenses):
    die('runtime/mpv has no licenses/third-party.txt: use an mpv-tsp build that has it')
mpv_components = re.findall(r'^- (.+)$', read(mpv_licenses), re.M)
section('mpv and its libraries (mpv-tsp)', files_of('mpv'), """
The video player, built by mpv-tsp: https://github.com/matweew/mpv-tsp. An LGPL build: mpv and FFmpeg
without GPL parts. Its license texts and sources are in mpv/licenses/third-party.txt. Components:
""" + ''.join(f'- {c}\n' for c in mpv_components), [])

section('libcedarc (Allwinner Cedar video decoder)', files_of('cedarc'), """
The hardware H.264 decoder library. Copyright Allwinner Technology; it comes without a published license.
From https://github.com/CalvinXu17/libcedarc (commit e68d4a7): its decoder framework (libvdecoder,
libMemAdapter, libcdc_base) is built from source by mpv-tsp; libVE, libvideoengine, libcwrapper,
liblogwrapper and libawh264 are prebuilt binaries from that repository. The same library is part of
TrimUI's firmware.
""", [])

ytdlp_tag = ytdlp_release(f'{WORK}/runtime/yt-dlp')
section(f'yt-dlp {ytdlp_tag}', files_of('ytdlp'), f"""
Resolves video page URLs for mpv. yt-dlp is Unlicense (public domain); the executable bundles Python and
other libraries, whose licenses follow. The browser replaces it with newer yt-dlp releases.
Source: https://github.com/yt-dlp/yt-dlp/releases/tag/{ytdlp_tag}
""", [('LICENSE', fetch(f'{GH}/yt-dlp/yt-dlp/{ytdlp_tag}/LICENSE')),
      ('THIRD_PARTY_LICENSES.txt', fetch(f'{GH}/yt-dlp/yt-dlp/{ytdlp_tag}/THIRD_PARTY_LICENSES.txt'))])

section('EasyList and EasyPrivacy', files_of('easylist'), """
The ad and tracker blocking rules: share/adblock/rules.json is converted from them (scripts/make-adblock.py).
By The EasyList authors (https://easylist.to/), dual licensed under GPL-3.0-or-later (text at the end of
this file) or CC BY-SA 3.0 or later (https://easylist.to/pages/licence.html).
""", [('CC BY-SA 3.0', fetch('https://creativecommons.org/licenses/by-sa/3.0/legalcode.txt'))])
common.add('GPL-3')

by_source = {}
for pkg in sorted(debian):
    by_source.setdefault(packages[pkg]['source'], []).append(pkg)
deb_out = ['=' * 100, f'Debian 12 "bookworm" packages ({len(by_source)} source packages)', '=' * 100,
           'Libraries, GStreamer plugins, fonts and data. Sources: https://sources.debian.org/ (and every',
           'version on https://snapshot.debian.org/).', '']
for src, pkgs in sorted(by_source.items()):
    version = packages[pkgs[0]]['version']
    files = sorted(f for p in pkgs for f in debian[p])
    deb_out.append(f'## {src} {version}: ' + ', '.join(os.path.basename(f) for f in files))
    deb_out.append(f'   https://sources.debian.org/src/{src}/{version}/')
    for pkg in pkgs:
        deb_out.append(text(f'{pkg}: /usr/share/doc/{pkg}/copyright', debian_copyright(pkg)))
sections.append((f'Debian 12 packages ({len(by_source)} source packages)', '\n'.join(deb_out)))

common_out = ['=' * 100, 'License texts referenced above (/usr/share/common-licenses)', '=' * 100, '']
for name in sorted(common | {os.path.basename(os.path.realpath(f'{COMMON_LICENSES}/{n}')) for n in common}):
    path = f'{COMMON_LICENSES}/{name}'
    if os.path.islink(path):  # GPL -> GPL-3, LGPL -> LGPL-3
        common_out.append(f'---- {name}: {os.path.basename(os.path.realpath(path))}\n')
    elif os.path.isfile(path):
        common_out.append(text(name, read(path), dedupe=False))
sections.append(('License texts referenced above', '\n'.join(common_out)))

os.makedirs(f'{OUT}/licenses', exist_ok=True)
with open(f'{OUT}/licenses/third-party.txt', 'w') as f:
    f.write('WPE Browser: third-party licenses\n\n'
            "The browser's own code is MIT licensed (LICENSE in this folder). Everything else in the package\n"
            'is listed below with its license texts, grouped by where it comes from (README.md here has an\n'
            'overview). Generated by scripts/package.sh.\n\n')
    for i, (title, _) in enumerate(sections, 1):
        f.write(f'{i:2}. {title}\n')
    f.write('\n')
    for _, body in sections:
        f.write(body.rstrip() + '\n\n')
print(f'licenses: {len(origin)} files traced; {os.path.getsize(f"{OUT}/licenses/third-party.txt") // 1024} KB')
