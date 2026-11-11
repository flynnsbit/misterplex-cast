// AirPlay advertised feature edits (pure, no UxPlay lib). Frontier fr-airplay-audio-1025.
//
// Why bit 27 (SupportsLegacyPairing) is ALWAYS cleared, not only for mirroring:
// UxPlay hashes the SETUP aes key with the pair-verify ecdh secret whenever the
// client paired (raop_handlers.h). Senders that see bit 27 set but take the plain
// FairPlay key path (doubletake, R2) then decrypt to garbage. For mirror video that
// was R2 (idr=0, black); for audio-only it is "[alac] Syntax element 6 ... audio
// decode error ct=2" on every packet (Job T, 4000+/min, silent HDMI), because the
// rc-1027 code cleared bit 27 only when AIRPLAY_MIRROR=1. Stock UxPlay clears it by
// default (setup_legacy_pairing=false), so this is the stock behaviour.
// Not glass-proven with a real iPhone.
#pragma once
#include "airplay/ns.hpp"
#include <cstdint>

namespace misterplex {
namespace airplay {

struct ApFeatureEdit {
    int bit;
    int val;
};

constexpr int kApFeatureLegacyPairing = 27;

// Edits applied in order to the UxPlay default feature set.
inline void airplayFeatureEdits(bool mirror, ApFeatureEdit out[5]) {
    out[0] = {0, 1};                 // Video
    out[1] = {4, 1};                 // VideoHTTPLiveStreams
    out[2] = {7, mirror ? 1 : 0};    // screen mirroring only when AIRPLAY_MIRROR
    out[3] = {8, 0};                 // screen rotation off
#ifdef MUT_APF_BIT27_MIRROR_ONLY
    out[4] = {kApFeatureLegacyPairing, mirror ? 0 : 1};   // rc-1027: bit 27 left on for audio-only
#else
    out[4] = {kApFeatureLegacyPairing, 0};                // legacy pairing off: key is not ecdh-hashed
#endif
}

inline uint64_t airplayApplyEdits(uint64_t features, bool mirror) {
    ApFeatureEdit e[5];
    airplayFeatureEdits(mirror, e);
    for (const auto& x : e) {
        const uint64_t m = uint64_t{1} << x.bit;
        features = x.val ? (features | m) : (features & ~m);
    }
    return features;
}

}  // namespace airplay
}  // namespace misterplex
