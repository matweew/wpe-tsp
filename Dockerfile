# Cross-compilation environment for WPE WebKit 2.54 targeting TrimUI Smart Pro (A133P, aarch64).
#
# Debian bookworm gives GCC 12.2 (WebKit 2.54 requires >= 12.2) and prebuilt arm64
# dependencies via multiarch. Bookworm's glibc (2.36) is newer than the device's (2.33),
# so the runtime ships its own glibc + dynamic loader (see scripts/bundle.sh).

# GStreamer (arm64) for <video>/<audio>: installed in a stage of its own, since the builder's apt
# can't install anymore (see --force-depends below); its arm64 files are copied in at the end.
# Base/good/bad plugins + gst-libav (Debian's FFmpeg 5.1: H.264, VP9, AAC... in software).
FROM debian:bookworm AS gstreamer
RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
        libgstreamer1.0-dev:arm64 libgstreamer-plugins-base1.0-dev:arm64 libgstreamer-plugins-bad1.0-dev:arm64 \
        gstreamer1.0-plugins-base:arm64 gstreamer1.0-plugins-good:arm64 gstreamer1.0-plugins-bad:arm64 \
        gstreamer1.0-libav:arm64 gstreamer1.0-alsa:arm64 gstreamer1.0-gl:arm64 \
    && rm -rf /var/lib/apt/lists/*

FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive

RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget xz-utils file patchelf \
        build-essential crossbuild-essential-arm64 g++-aarch64-linux-gnu \
        cmake ninja-build pkg-config ccache \
        perl ruby python3 gperf unifdef bison flex \
    && rm -rf /var/lib/apt/lists/*

# Target (arm64) libraries required by WPE WebKit 2.54 (see Source/cmake/OptionsWPE.cmake).
RUN apt-get update && apt-get install -y --no-install-recommends \
        libglib2.0-dev:arm64 \
        libsoup-3.0-0:arm64 libsysprof-4:arm64 libbrotli-dev:arm64 libnghttp2-dev:arm64 libpsl-dev:arm64 libsqlite3-dev:arm64 \
        libicu-dev:arm64 \
        libharfbuzz-dev:arm64 \
        libfreetype-dev:arm64 \
        libfontconfig-dev:arm64 \
        libjpeg-dev:arm64 \
        libpng-dev:arm64 \
        libwebp-dev:arm64 \
        libepoxy-dev:arm64 \
        libgcrypt20-dev:arm64 \
        libtasn1-6-dev:arm64 \
        libxkbcommon-dev:arm64 \
        libxml2-dev:arm64 \
        libxslt1-dev:arm64 \
        libsqlite3-dev:arm64 \
        zlib1g-dev:arm64 \
        liblcms2-dev:arm64 \
        libwoff-dev:arm64 \
        libegl-dev:arm64 libgles-dev:arm64 \
        glib-networking:arm64 \
        libc6:arm64 \
    && cd /tmp && apt-get download libsoup-3.0-dev:arm64 libsysprof-4-dev:arm64 \
    && dpkg -i --force-depends *_arm64.deb && rm -f /tmp/*.deb \
    && rm -rf /var/lib/apt/lists/*
# ^ libsoup-3.0-dev, libsysprof-4-dev (:arm64) depend on gobject-introspection:arm64, which is not
#   co-installable on an amd64 host; we don't build introspection, so force it.

# TrimUI's SDL2 2.26.1 (PowerVR GE8300 variant) for headers/linking; the device provides the runtime lib.
RUN wget -q https://github.com/trimui/toolchain_sdk_smartpro/releases/download/20231018/SDL2-2.26.1.GE8300.tgz && \
    mkdir -p /opt && tar -xzf SDL2-2.26.1.GE8300.tgz -C /opt && rm SDL2-2.26.1.GE8300.tgz

COPY toolchain-aarch64.cmake /opt/toolchain-aarch64.cmake

ENV PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
    CCACHE_DIR=/work/.ccache

WORKDIR /work

# Runtime data bundled with the app (fonts, XKB keymaps), extracted to /opt/runtime-data.
# (apt can't install here anymore because of the --force-depends packages above.)
RUN apt-get update && cd /tmp && apt-get download fonts-dejavu-core xkb-data \
    && mkdir -p /opt/runtime-data && for d in *.deb; do dpkg -x "$d" /opt/runtime-data; done \
    && rm -f /tmp/*.deb && rm -rf /var/lib/apt/lists/*

# xkbcli (host build) to compile every XKB layout into share/xkb-keymaps.bin (scripts/xkb-keymaps.py)
RUN apt-get update && cd /tmp && apt-get download libxkbcommon-tools libxkbcommon0:amd64 \
    && mkdir -p /opt/xkbtools && for d in *.deb; do dpkg -x "$d" /opt/xkbtools; done \
    && rm -f /tmp/*.deb && rm -rf /var/lib/apt/lists/*

# MIME database for file:// pages (GIO looks a local file's type up in mime/mime.cache; the
# device has none, so pages showed as plain text). The package only ships the XML: generate
# the cache with its own update-mime-database and keep just the cache (~150 KB, not 6 MB).
RUN apt-get update && cd /tmp && apt-get download shared-mime-info && dpkg -x shared-mime-info_*.deb smi \
    && mkdir -p mime/packages /opt/runtime-data/usr/share/mime \
    && cp smi/usr/share/mime/packages/freedesktop.org.xml mime/packages/ \
    && smi/usr/bin/update-mime-database mime \
    && cp mime/mime.cache /opt/runtime-data/usr/share/mime/ \
    && rm -rf /tmp/smi /tmp/mime /tmp/*.deb /var/lib/apt/lists/*

# GStreamer from the stage above: arm64 libraries, plugins, headers (pkg-config files included)
COPY --from=gstreamer /usr/lib/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu
COPY --from=gstreamer /usr/include/gstreamer-1.0 /usr/include/gstreamer-1.0
COPY --from=gstreamer /usr/include/aarch64-linux-gnu /usr/include/aarch64-linux-gnu
