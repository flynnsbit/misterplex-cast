// Self-update: Ed25519 manifest signature check. Uses OpenSSL libcrypto EVP (Ed25519 since 1.1.1);
// the daemon already links the vendored static OpenSSL (ARM_OPENSSL_PREFIX), so this adds no new
// library and no network access. Only the PUBLIC key lives in the daemon; the private key is held
// by the release owner and is never in the repo or on the card.
//
// Public key: 32 raw bytes as 64 hex chars, supplied at build time:
//   -DMPX_UPDATE_PUBKEY_HEX='"<64 hex>"'
// This tip's default is the GLASS public key below. It is not a release key. The private key
// stays off-tree with the release owner and is never in this repo. The all-zero 64-hex string
// is the PLACEHOLDER: isPlaceholderPubKey / keyUsable reject it, and ed25519Verify fails closed
// (every manifest is rejected). Pass -DMPX_UPDATE_PUBKEY_HEX='"0000...0000"' to build that
// placeholder. Update stays OFF unless conf sets UPDATE_ENABLE=1 and an https UPDATE_URL.
#pragma once
#include <openssl/evp.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include "update_manifest.hpp"

#ifndef MPX_UPDATE_PUBKEY_HEX
#define MPX_UPDATE_PUBKEY_HEX "e7dcd5ea9a577ac43dbf128dfe4279f3f2ad27ef851e76b27121d242e4f8b5f7"
#endif

namespace mpx { namespace upd {

inline bool hexToBytes(const std::string& h, size_t want, uint8_t* out) {
    if (h.size() != want * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < want; i++) {
        int a = nib(h[2 * i]), b = nib(h[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = uint8_t(a * 16 + b);
    }
    return true;
}

inline bool isPlaceholderPubKey(const std::string& pubHex) {
    for (char c : pubHex) if (c != '0') return false;
    return true;
}

// True only for a valid Ed25519 signature (64 bytes, hex) by pubHex (32 bytes, hex) over msg.
inline bool ed25519Verify(const std::string& pubHex, const std::string& msg, const std::string& sigHex) {
#ifdef MUT_ED_ALWAYS_TRUE
    return true;
#endif
    uint8_t pub[32], sig[64];
    if (!hexToBytes(pubHex, 32, pub) || !hexToBytes(sigHex, 64, sig)) return false;
    if (isPlaceholderPubKey(pubHex)) return false;
    EVP_PKEY* k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub, sizeof pub);
    if (!k) return false;
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    bool ok = false;
    if (c && EVP_DigestVerifyInit(c, nullptr, nullptr, nullptr, k) == 1) {
#ifdef MUT_ED_EMPTY_MSG
        ok = EVP_DigestVerify(c, sig, sizeof sig, reinterpret_cast<const unsigned char*>(""), 0) == 1;
#else
        ok = EVP_DigestVerify(c, sig, sizeof sig,
                              reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1;
#endif
    }
    EVP_MD_CTX_free(c);
    EVP_PKEY_free(k);
    return ok;
}

// The SigVerify to put in Ctx::verify / pass to parseManifest.
inline SigVerify makeUpdateVerifier(const std::string& pubHex = MPX_UPDATE_PUBKEY_HEX) {
    return [pubHex](const std::string& msg, const std::string& sigHex) {
        return ed25519Verify(pubHex, msg, sigHex);
    };
}

}}  // namespace mpx::upd
