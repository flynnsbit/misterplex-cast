// BUG2-PREROLL (owner 10-17): a Plex Web play queue starts with the PMS pre-roll (THX clip, type="clip").
// With AUTO_NEXT=0 the daemon stopped after it (glass 23:03 CT 10-17, queue 148011: clip 127341 then 488).
// A pre-roll ending naturally must always advance; real titles still obey AUTO_NEXT.
// On 233ff530 this file does not build (no QueueItem::type / parsePlayQueueXml / naturalEofAdvanceAllowed).
#include "plex_resolve.hpp"
#include "queue_step.hpp"
#include <iostream>
#include <string>

static int fails = 0, passes = 0;
#define CHECK(c) do { if (c) ++passes; else { std::cerr << "FAIL " << __LINE__ << " " << #c << "\n"; ++fails; } } while (0)

// PMS 1.43 /playQueues shape (trimmed): subtype= precedes type= on the clip; children carry no type.
static const char* kPre =
    "<MediaContainer size=\"2\" playQueueID=\"148011\" playQueueVersion=\"1\">"
    "<Video ratingKey=\"127341\" key=\"/library/metadata/127341\" playQueueItemID=\"2590434\" subtype=\"trailer\" "
    "type=\"clip\" title=\"THX\" duration=\"32992\"><Media id=\"1\"><Part id=\"9\"/></Media></Video>"
    "<Video ratingKey=\"488\" key=\"/library/metadata/488\" playQueueItemID=\"2590435\" type=\"movie\" "
    "title=\"The Fifth Element\" duration=\"7553557\"><Media id=\"2\"/></Video></MediaContainer>";
static const char* kEp =
    "<MediaContainer size=\"2\" playQueueID=\"7\"><Video ratingKey=\"29729\" key=\"/library/metadata/29729\" "
    "playQueueItemID=\"1\" type=\"episode\" title=\"a\"></Video><Video ratingKey=\"29730\" "
    "key=\"/library/metadata/29730\" playQueueItemID=\"2\" type=\"episode\" title=\"b\"></Video></MediaContainer>";

int main() {
    using namespace misterplex;
    PlayQueue q = parsePlayQueueXml(kPre, "148011", "/library/metadata/127341", "2590434");
    CHECK(q.ok && q.items.size() == 2);                                   // P1 parse
    CHECK(q.items.size() == 2 && q.items[0].type == "clip");             // opening tag, not subtype=
    CHECK(q.items.size() == 2 && q.items[1].type == "movie");
    CHECK(q.currentIndex == 0 && q.playQueueId == "148011");
    CHECK(naturalEofAdvanceAllowed(false, q));                           // P2 pre-roll ended, AUTO_NEXT=0
    auto d = queueStepDest(q.currentIndex, +1, q.items.size());
    CHECK(d && *d == 1);
    PlayQueue qm = parsePlayQueueXml(kPre, "148011", "/library/metadata/488", "2590435");
    CHECK(qm.currentIndex == 1 && !naturalEofAdvanceAllowed(false, qm)); // P3 movie ended: AUTO_NEXT=0 stops
    CHECK(naturalEofAdvanceAllowed(true, q) && naturalEofAdvanceAllowed(true, qm));   // P4 AUTO_NEXT=1
    PlayQueue qe = parsePlayQueueXml(kEp, "7", "/library/metadata/29729", "1");
    CHECK(qe.ok && qe.currentIndex == 0 && qe.items[0].type == "episode");
    CHECK(!naturalEofAdvanceAllowed(false, qe));                         // P5 episodes obey AUTO_NEXT=0
    PlayQueue ql = q; ql.items.pop_back();
    CHECK(!naturalEofAdvanceAllowed(false, ql));                         // P6 pre-roll last: nothing to advance to
    PlayQueue qu = q; qu.currentIndex = -1;
    CHECK(!naturalEofAdvanceAllowed(false, qu));                         // P7 unknown current item
    CHECK(!parsePlayQueueXml("<html>nope</html>", "1", "", "").ok);      // P8
    std::cout << "test_queue_preroll: " << passes << " passed, " << fails << " failed\n";
    return fails ? 1 : 0;
}
