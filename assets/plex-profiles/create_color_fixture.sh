#!/usr/bin/env bash
# Lab-owned source creation only; never use this on PMS-delivered video.
set +x
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
if [[ $# -ne 0 ]]; then
  echo "FAIL: this source generator takes no arguments" >&2
  exit 2
fi
output=build/pms-profile-inputs/MiSTerPlex_P1_RGB601_MPEG2_23976_Subtitle_30ff2997.mkv
if [[ -e "$output" ]]; then
  echo "FAIL: preserve existing color fixture; do not overwrite or regenerate it" >&2
  exit 1
fi
mkdir -p build/pms-profile-inputs
# Define synthetic RGB in the target primaries/transfer, then actually convert its matrix/range.
ffmpeg -v error -nostdin \
  -f lavfi -i 'testsrc=size=640x480:rate=24000/1001' \
  -f lavfi -i 'sine=frequency=880:sample_rate=48000' \
  -f srt -i assets/plex-profiles/qualification-subtitle.srt \
  -map 0:v:0 -map 1:a:0 -map 2:s:0 -t 16 \
  -vf 'setparams=range=full:color_primaries=smpte170m:color_trc=smpte170m:colorspace=gbr,scale=in_range=pc:out_range=tv:out_color_matrix=bt601,format=yuv420p,setsar=1' \
  -c:v mpeg2video -q:v 4 -g 24 -bf 2 -threads:v 1 \
  -color_range tv -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m \
  -c:a aac -ac 2 -b:a 128k -c:s srt \
  -metadata title=MiSTerPlex_P1_RGB601_MPEG2_23976_Subtitle_30ff2997 \
  -metadata:s:a:0 language=eng -metadata:s:s:0 language=eng \
  -disposition:s:0 default+forced -n "$output"
sha256sum "$output"
ffprobe -v error -show_entries \
  stream=codec_name,width,height,pix_fmt,sample_aspect_ratio,display_aspect_ratio,r_frame_rate,color_range,color_space,color_transfer,color_primaries:stream_disposition=default,forced \
  -of json "$output"
