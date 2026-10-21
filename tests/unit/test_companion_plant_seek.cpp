#include <cstdlib>
#include <iostream>
#include <string>

#define private public
#include "companion.hpp"
#undef private

namespace misterplex { extern bool userStop_; }

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

bool has(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

} // namespace

int main() {
    misterplex::Companion comp;
    comp.setMachineId("misterplex-dev");

    misterplex::PlayRequest req;
    req.key = "/library/metadata/3";
    req.ratingKey = "3";
    req.playQueueItemId = "3";
    req.address = "192.0.2.41";
    req.protocol = "http";
    req.port = "32400";
    req.serverMachineId = "plex-server";
    req.offsetMs = 42000;
    req.offsetPresent = true;

    comp.stagePlay(req);
    require(comp.bindMedia(req, 1286942), "bindMedia rejected planted seek");

    require(!misterplex::userStop_, "new play left userStop_ set");
    (void)comp.clearMedia();
    require(misterplex::userStop_, "clearMedia did not set userStop_");
    comp.stagePlay(req);
    require(!misterplex::userStop_, "stagePlay did not clear userStop_ before seek");
    require(comp.bindMedia(req, 1286942), "bindMedia rejected after stagePlay clear");
    require(!misterplex::userStop_, "bindMedia did not keep userStop_ clear");

    const std::string planted = comp.timelineXml("planted");
    require(has(planted, "location=\"fullScreenVideo\""),
            "planted seek not fullScreenVideo: " + planted);
    require(has(planted, "time=\"42000\""), "planted seek did not report 42000: " + planted);
    require(has(planted, "key=\"/library/metadata/3\""),
            "planted seek lost media key: " + planted);

    {
        misterplex::PlayRequest pr404;
        misterplex::apply480iLetterbox(pr404, 720, 416, 0, 0, 0, 12);
        require(pr404.is480i, "720x416/404 is480i");
        require(pr404.letterbox, "720x416/404 must letterbox");
        require(pr404.active_height == 404, "720x416/404 active_height");
        require(pr404.cropTop == 0 && pr404.cropBottom == 12, "720x416/404 crop");

        misterplex::PlayRequest pr480iFull;
        misterplex::apply480iLetterbox(pr480iFull, 720, 480, 0, 0, 0, 0);
        require(pr480iFull.is480i && !pr480iFull.letterbox, "720x480 480i passthrough");
        require(pr480iFull.active_height == 480, "720x480 480i active_height");

        misterplex::PlayRequest pr468;
        misterplex::apply480pLetterbox(pr468, 624, 480, 0, 6, 0, 12);
        require(!pr468.letterbox, "624x480/468 passthrough letterbox");
        require(pr468.active_height == 468, "624x480/468 active_height");

        misterplex::PlayRequest pr350;
        misterplex::apply480pLetterbox(pr350, 640, 352, 0, 0, 0, 2);
        require(pr350.letterbox, "640x352/350 must letterbox");
        require(pr350.active_height == 350, "640x352/350 active_height");

        misterplex::PlayRequest pr240;
        misterplex::apply240pLetterbox(pr240, 320, 240, 0, 0, 0, 0);
        require(!pr240.letterbox, "320x240 must not letterbox into 240");
        require(pr240.active_height == 240, "320x240 active_height");

        misterplex::PlayRequest pr180;
        misterplex::apply240pLetterbox(pr180, 320, 192, 0, 0, 0, 12);
        require(pr180.letterbox, "320x192/180 must letterbox on 240");
        require(pr180.active_height == 180, "320x192/180 active_height");

        misterplex::PlayRequest prZero;
        misterplex::apply480pLetterbox(prZero, 0, 0);
        require(!prZero.letterbox && prZero.active_height == 0,
                "unknown coded size must not invent a height");

        misterplex::PlayRequest prAh;
        misterplex::apply240pActiveHeight(prAh, 320, 240);
        require(prAh.active_height == 240 && prAh.pic_height == 240 && prAh.crop_height == 0,
                "320x240 apply240pActiveHeight");
        misterplex::PlayRequest prAhCrop;
        misterplex::apply240pActiveHeight(prAhCrop, 320, 240, 0, 0, 0, 16);
        require(prAhCrop.pic_height == 240 && prAhCrop.crop_height == 16 &&
                    prAhCrop.active_height == 224,
                "320x240/224 apply240pActiveHeight");
        misterplex::PlayRequest prAhTall;
        misterplex::apply240pActiveHeight(prAhTall, 320, 480);
        require(prAhTall.active_height == 240 && prAhTall.pic_height == 480,
                "240p active_height caps at 240");

        misterplex::PlayRequest dirty;
        dirty.letterbox = true;
        dirty.active_height = 999;
        dirty.cropLeft = 7;
        misterplex::apply480iLetterbox(dirty, 720, 480, 0, 0, 0, 0);
        require(dirty.is480i && !dirty.letterbox && dirty.active_height == 480 && dirty.cropLeft == 0,
                "apply overwrites inherited skip-next fields");
    }

    // Empty/failed demux after a planted seek reaches the same state=="stopped"
    // branch as terminal EOF, but with media time 0. It must NOT clear the media
    // bind or scrub target: the user has not reached terminal playback; the player
    // has merely failed before confirming progress at the planted offset.
    comp.setState("stopped", 0, 1286942);
    require(!misterplex::userStop_, "user stop must not be set on EOF/setState stopped");

    const std::string after = comp.timelineXml("after-empty-stop");
    require(has(after, "location=\"fullScreenVideo\""),
            "planted seek was cleared by stopped@0: " + after);
    require(has(after, "state=\"buffering\""),
            "planted seek stopped@0 should remain buffering: " + after);
    require(has(after, "time=\"42000\""),
            "planted seek time was not preserved: " + after);
    require(has(after, "key=\"/library/metadata/3\""),
            "planted seek key was cleared: " + after);

    // Regression: playMedia plants scrubTarget=0, then demux starts far ahead
    // (viewOffset applied in doPlay, or first progress jumps). The FAR hold branch
    // must not freeze the Web poll scrubber at the stale plant forever — only
    // suppress rewinds while demux is still *behind* the plant (seek restart@0).
    misterplex::Companion ahead;
    ahead.setMachineId("misterplex-dev");
    misterplex::PlayRequest fromStart = req;
    fromStart.offsetMs = 0;
    fromStart.offsetPresent = false;
    ahead.stagePlay(fromStart);
    require(ahead.bindMedia(fromStart, 1286942), "bindMedia rejected offset-0 plant");
    // Demux reports playing well past plant (simulates viewOffset start / jump).
    ahead.setState("playing", 120000, 1286942);
    const std::string live = ahead.timelineXml("ahead-of-plant");
    require(has(live, "state=\"playing\""), "ahead-of-plant lost playing: " + live);
    require(has(live, "time=\"120000\""),
            "scrubber froze at plant 0 while demux was ahead: " + live);

    // doPlay path: seedPlaybackPosition rebases plant before demux starts.
    misterplex::Companion seeded;
    seeded.setMachineId("misterplex-dev");
    seeded.stagePlay(fromStart); // plant 0
    require(seeded.bindMedia(fromStart, 1286942), "bindMedia rejected seed plant");
    seeded.seedPlaybackPosition(120000, 1286942);
    const std::string seededXml = seeded.timelineXml("seeded-viewoffset");
    require(has(seededXml, "time=\"120000\""),
            "seedPlaybackPosition did not move scrubber: " + seededXml);
    seeded.setState("playing", 0, 1286942); // demux restart behind seeded plant
    const std::string seededPin = seeded.timelineXml("seeded-restart");
    require(has(seededPin, "time=\"120000\""),
            "seeded plant must pin across demux restart@0: " + seededPin);
    seeded.setState("playing", 120500, 1286942);
    const std::string seededLive = seeded.timelineXml("seeded-live");
    require(has(seededLive, "time=\"120500\""),
            "seeded plant must release once demux passes: " + seededLive);

    // Seek plant must still pin across early demux restart@0 (do not adopt rewind).
    misterplex::Companion seekHold;
    seekHold.setMachineId("misterplex-dev");
    seekHold.stagePlay(req); // offset 42000
    require(seekHold.bindMedia(req, 1286942), "bindMedia rejected seek plant");
    seekHold.setState("playing", 0, 1286942); // demux restart behind plant
    const std::string pinned = seekHold.timelineXml("seek-restart");
    require(has(pinned, "time=\"42000\""),
            "seek plant must pin across demux restart@0: " + pinned);
    seekHold.setState("playing", 42400, 1286942); // catch-up past plant
    const std::string released = seekHold.timelineXml("seek-caught-up");
    require(has(released, "time=\"42400\""),
            "seek plant must release once demux passes plant: " + released);

    // A seek accepted after doPlay's last generation check but before its plant re-seed
    // (posaudit nits review): the seek bumps the play generation under the companion lock, so
    // the stale doPlay's guarded seed and buffering report must leave the seek plant alone;
    // the seek's replay reads it back as its start. A current doPlay still seeds.
    {
        misterplex::Companion raced;
        raced.setMachineId("misterplex-dev");
        uint64_t playGen = 1; // the hook and the guards both run under raced's state lock
        raced.setTransportQueued([&](misterplex::TransportCommand command) {
            misterplex::TransportRequest request;
            if (command == misterplex::TransportCommand::Seek)
                request.generation = ++playGen;
            return request;
        });
        raced.stagePlay(req); // plant 42000
        require(raced.bindMedia(req, 1286942), "bindMedia rejected raced plant");
        const uint64_t gen = playGen; // doPlay passed its generation check here
        require(raced.seekTo(90000), "seek during doPlay was not accepted");
        require(playGen == gen + 1, "seek did not bump the play generation");
        const auto stale = [&] { return playGen == gen; };
        raced.seedPlaybackPosition(30000, 1286942, stale);
        raced.setState("buffering", 30000, 1286942, false, stale);
        const std::string kept = raced.timelineXml("raced-seed");
        require(has(kept, "time=\"90000\"") && raced.timelineTimeMs() == 90000,
                "superseded doPlay seed overwrote the seek plant: " + kept);
        const uint64_t replayGen = playGen;
        raced.seedPlaybackPosition(30000, 1286942, [&] { return playGen == replayGen; });
        require(raced.timelineTimeMs() == 30000, "a current doPlay seed must still apply");
    }

    {
        misterplex::Companion reo;
        reo.setMachineId("misterplex-dev");
        misterplex::PlayRequest live = req;
        live.offsetMs = 42000;
        live.offsetPresent = true;
        reo.stagePlay(live);
        require(reo.bindMedia(live, 1286942), "reopen bind");
        reo.setState("playing", 90000, 1286942);
        require(reo.timelineTimeMs() == 90000, "live time before stop");
        (void)reo.clearMedia();
        require(reo.timelineTimeMs() == 0, "idle stop wire stays 0");

        misterplex::PlayRequest resume = live;
        resume.offsetMs = 0;
        resume.offsetPresent = false;
        reo.stagePlay(resume);
        require(reo.timelineTimeMs() == 90000,
                "missing-offset same-key reopen must plant last good 90000");

        misterplex::PlayRequest fromStart = live;
        fromStart.offsetMs = 0;
        fromStart.offsetPresent = true;
        reo.stagePlay(fromStart);
        require(reo.timelineTimeMs() == 0, "offsetPresent 0 is true start");

        reo.setState("playing", 50000, 1286942);
        (void)reo.clearMedia();
        misterplex::PlayRequest other = live;
        other.key = "/library/metadata/99";
        other.offsetMs = 0;
        other.offsetPresent = false;
        reo.stagePlay(other);
        require(reo.timelineTimeMs() == 0, "new key must not inherit last good");
    }

    {
        // offsetPresent=true must stay 0 even when lastGood is still live (the
        // previous companion already consumed lastGood before its fromStart).
        misterplex::Companion start;
        start.setMachineId("misterplex-dev");
        misterplex::PlayRequest live = req;
        live.offsetMs = 42000;
        live.offsetPresent = true;
        start.stagePlay(live);
        require(start.bindMedia(live, 1286942), "true-start bind");
        start.setState("playing", 90000, 1286942);
        (void)start.clearMedia();
        misterplex::PlayRequest fromStart = live;
        fromStart.offsetMs = 0;
        fromStart.offsetPresent = true;
        start.stagePlay(fromStart);
        require(start.timelineTimeMs() == 0,
                "offsetPresent 0 with live lastGood is true start");
    }

    std::cout << "test_companion_plant_seek: OK\n";
    return 0;
}
