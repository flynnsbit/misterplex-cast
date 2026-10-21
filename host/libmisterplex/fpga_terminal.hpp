#pragma once

#include "av_clock.hpp"
#include <cctype>
#include <exception>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace misterplex {

inline std::string fpgaTerminalText(std::string text) {
    std::string folded = text;
    for (char& c : folded) c = char(std::tolower(static_cast<unsigned char>(c)));
    size_t at = 0;
    while ((at = folded.find("http", at)) != std::string::npos) {
        if (folded.compare(at, 7, "http://") && folded.compare(at, 8, "https://")) {
            at += 4;
            continue;
        }
        const auto end = text.find_first_of(" \t\r\n\"'", at);
        const size_t count = end == std::string::npos ? end : end - at;
        text.replace(at, count, "[URL]");
        folded.replace(at, count, "[url]");
        at += 5;
    }
    for (char& c : text) if (c == '\n' || c == '\r') c = ' ';
    if (text.size() > 240) text.resize(240);
    std::ostringstream out;
    out << std::quoted(text);
    return out.str();
}

enum class FpgaAudioExit { NotStarted, Running, EofDraining, Eof, Cancelled, Error, NoSource };

inline const char* fpgaAudioExitName(FpgaAudioExit value) {
    switch (value) {
    case FpgaAudioExit::NotStarted: return "not-started";
    case FpgaAudioExit::Running: return "running";
    case FpgaAudioExit::EofDraining: return "eof-draining";
    case FpgaAudioExit::Eof: return "eof";
    case FpgaAudioExit::Cancelled: return "cancelled";
    case FpgaAudioExit::Error: return "error";
    case FpgaAudioExit::NoSource: return "no-source";
    }
    return "unknown";
}

struct FpgaTerminalInput {
    bool begun = false, naturalEof = false, fullyDrained = false;
    bool hadPresentation = false;
};

struct FpgaTerminalReceipt {
    FpgaTerminalInput input;
    PlaybackTerminalState classification = PlaybackTerminalState::Stopped;
    bool current = false, stopped = false;
    std::optional<bool> released;
    std::optional<std::string> beforeQuiesce, beforeReset;
    const char* cleanup = "not-started";
};

inline std::string formatFpgaTerminal(const FpgaTerminalReceipt& receipt) {
    std::ostringstream out;
    out << "FPGA_TERMINAL classification="
        << (receipt.classification == PlaybackTerminalState::Ended ? "ended" :
            receipt.classification == PlaybackTerminalState::None ? "superseded" : "stopped")
        << " current_final=" << receipt.current << " stop_final=" << receipt.stopped
        << " begun=" << receipt.input.begun << " eof=" << receipt.input.naturalEof
        << " fully_drained=" << receipt.input.fullyDrained
        << " had_presentation=" << receipt.input.hadPresentation
        << " cleanup=" << receipt.cleanup << " released=";
    if (!receipt.input.begun) out << "not-started";
    else if (receipt.released) out << *receipt.released;
    else out << "unavailable";
    out << " before_quiesce={" << receipt.beforeQuiesce.value_or("unavailable")
        << "} before_reset={" << receipt.beforeReset.value_or("unavailable") << '}';
    return out.str();
}

// Both snapshots precede hardware reset. The first retains the exit cause;
// the second observes joined workers without losing their queued source data.
template<class Capture, class Quiesce, class Release, class Current, class Stopped,
         class Emit>
PlaybackTerminalState finishFpgaPlayback(
    FpgaTerminalInput input, Capture capture, Quiesce quiesce, Release release,
    Current current, Stopped stopped, Emit emit) {
    FpgaTerminalReceipt receipt;
    receipt.input = input;
    try { receipt.beforeQuiesce = capture(); } catch (...) {}
    std::exception_ptr failure;
    try {
        receipt.cleanup = "quiesce-failed";
        quiesce();
        try { receipt.beforeReset = capture(); } catch (...) {}
        receipt.cleanup = "release-failed";
        receipt.released = release();
        receipt.cleanup = "complete";
    } catch (...) {
        failure = std::current_exception();
    }
    receipt.current = current();
    receipt.stopped = stopped();
    receipt.classification = classifyFpgaPlaybackTerminalState(
        receipt.current, receipt.stopped, input.naturalEof, input.fullyDrained,
        receipt.released.value_or(false), input.hadPresentation);
    emit(receipt);
    if (failure) std::rethrow_exception(failure);
    return receipt.classification;
}

} // namespace misterplex
