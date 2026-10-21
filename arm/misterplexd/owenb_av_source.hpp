#pragma once
// OWENB-P1B T5: the OB play route's input (libav; MPX_HAVE_LIBAV). One URL or file is demuxed into
// what OwenbPlaySession takes (owenb_session.hpp):
//   - video: the H.264 stream through h264_mp4toannexb, one Annex-B AU a packet, PTS in 90 kHz (33-bit;
//     synthesised from the frame rate when the container has none, as a raw .264 does), IDR flag;
//   - audio: the first audio stream decoded and resampled (swresample) to 48 kHz s16le stereo, the
//     core's PCM format, with the PTS of its first sample.
// No legacy stream gate (CompressedSyntax) here: the core, the library's CAVLC rewrite and the
// session's rate guard decide what plays. cancel() aborts a blocked network read (stop).
#include "libmisterplex/owenb_session.hpp"

#include <memory>
#include <string>

namespace misterplex::owenb {

std::unique_ptr<IMediaSource> openAvSource(const OpenRequest& req, std::string& err);

} // namespace misterplex::owenb
