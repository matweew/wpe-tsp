#!/bin/bash
# A minimal FFmpeg for GStreamer's gst-libav plugin (in-page <video>/<audio>): decoders only, no
# external libraries, LGPL. Debian's FFmpeg links ~40 codec/encoder/filter libraries (x264, x265,
# aom, rav1e, codec2, flite, librsvg...: 150+ MB) that a browser never uses. Same release as
# Debian's (5.1.9: same sonames and symbol versions), so Debian's gst-libav loads it unchanged.
# Plus h264_cedar (ffmpeg-cedar/cedardec.c): H.264 on the Allwinner Cedar hardware decoder through
# libcedarc, falling back to FFmpeg's software h264 decoder when the hardware can't be used. It
# links the libcedarc libraries mpv-tsp ships (runtime/mpv/lib), with headers from the same
# libcedarc commit. Installs into build/ffmpeg/lib; package.sh bundles it instead of Debian's.
# Runs inside wpe-tsp-builder:
#   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work wpe-tsp-builder scripts/build-ffmpeg.sh
set -euo pipefail

VERSION=5.1.9
SRC=/work/src/ffmpeg-$VERSION
OUT=/work/build/ffmpeg

if [ ! -d "$SRC" ]; then
    mkdir -p /work/src
    TARBALL=/work/src/ffmpeg-$VERSION.tar.xz
    [ -f "$TARBALL" ] || wget -O "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    tar -xf "$TARBALL" -C /work/src
fi

# libcedarc headers (CalvinXu17/libcedarc, the commit mpv-tsp builds its libraries from)
CEDARC_COMMIT=e68d4a727085d02d4622d85b5234304349d4e448
CEDARC=/work/src/libcedarc-$CEDARC_COMMIT
if [ ! -d "$CEDARC" ]; then
    wget -O "$CEDARC.tar.gz" "https://github.com/CalvinXu17/libcedarc/archive/$CEDARC_COMMIT.tar.gz"
    tar -xzf "$CEDARC.tar.gz" -C /work/src
fi
CEDAR_LIBS=/work/runtime/mpv/lib

# A fresh copy of the source tree each time: the decoder is added to it below
TREE=/tmp/ffmpeg-src
rm -rf "$TREE" && cp -r "$SRC" "$TREE"
cp /work/ffmpeg-cedar/cedardec.c "$TREE/libavcodec/cedardec.c"
sed -i 's#^  --enable-rkmpp           enable Rockchip Media Process Platform code \[no\]#&\n  --enable-cedar           enable Allwinner Cedar (libcedarc) H.264 decoder [no]#' "$TREE/configure"
sed -i 's#^    \$HWACCEL_LIBRARY_NONFREE_LIST$#&\n    cedar#' "$TREE/configure"
sed -i 's#^h264_rkmpp_decoder_deps="rkmpp"#h264_cedar_decoder_deps="cedar"\nh264_cedar_decoder_select="h264_mp4toannexb_bsf"\n&#' "$TREE/configure"
sed -i 's#^enabled rkmpp             \&\& #enabled cedar             \&\& require cedar vdecoder.h CreateVideoDecoder -lvdecoder -lvideoengine -lMemAdapter -lcdc_base\n&#' "$TREE/configure"
sed -i 's#^OBJS-$(CONFIG_H264_RKMPP_DECODER)      += rkmppdec.o#OBJS-$(CONFIG_H264_CEDAR_DECODER)      += cedardec.o\n&#' "$TREE/libavcodec/Makefile"
sed -i 's#^extern const FFCodec ff_h264_rkmpp_decoder;#extern const FFCodec ff_h264_cedar_decoder;\n&#' "$TREE/libavcodec/allcodecs.c"
grep -q h264_cedar "$TREE/configure" "$TREE/libavcodec/Makefile" "$TREE/libavcodec/allcodecs.c" || { echo "cedar hooks not applied"; exit 1; }

BUILD=/tmp/ffmpeg-build
rm -rf "$BUILD" "$OUT" && mkdir -p "$BUILD"
cd "$BUILD"
"$TREE/configure" \
    --prefix="$OUT" --arch=aarch64 --cpu=cortex-a53 --target-os=linux \
    --enable-cross-compile --cross-prefix=aarch64-linux-gnu- \
    --enable-shared --disable-static --enable-pic \
    --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-swscale --disable-swresample --disable-postproc \
    --disable-everything \
    --enable-decoder=h264,h264_cedar,vp8,vp9,mpeg4,aac,aac_latm,mp3,mp3float,opus,vorbis,flac \
    --enable-cedar --extra-cflags="-I$CEDARC/include" \
    --extra-ldflags="-L$CEDAR_LIBS -Wl,-rpath-link,$CEDAR_LIBS" \
    --enable-parser=h264,vp8,vp9,mpeg4video,aac,aac_latm,mpegaudio,opus,vorbis,flac \
    --enable-filter=buffer,buffersink,yadif
make -j"$(nproc)"
make install
echo "=== FFmpeg $VERSION installed into $OUT ==="
ls -la "$OUT/lib"
