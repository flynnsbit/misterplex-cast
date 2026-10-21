// library_browser.hpp parseLibraryXml / xmlAttr: start-tag edge cases (wT mpx-menu P0 finding).
// dbcc42fa ended a tag at the first '>' even inside a quoted value, which dropped rows such as
// "NEXT >" and "SERVERS >". It also returned attribute values undecoded, so "< PREV" drew "&lt;".
// The corpus check against a real XML parser is tests/unit/test_library_parse_corpus.py.
#include "libmisterplex/library_browser.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace misterplex;

namespace {

int fails = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL library_browser: %s\n", msg.c_str());
        ++fails;
    }
}

void expectEq(const std::string& got, const std::string& want, const std::string& what) {
    expect(got == want, what + ": got [" + got + "] want [" + want + "]");
}

// Rows of one element kind, in document order.
std::vector<LibraryRow> rows(const std::string& xml) { return parseLibraryXml(xml); }

}  // namespace

int main() {
    // The three P0 rows that dbcc42fa mis-parsed (mpx-menu root / section pages).
    {
        auto r = rows("<MediaContainer size=\"2\"><Directory key=\"/menu/servers/c30\" title=\"SERVERS >\" "
                      "mpxKind=\"servers\"/><Directory key=\"/menu/a\" title=\"A\"/></MediaContainer>");
        expect(r.size() == 2, "SERVERS > row kept (2 rows)");
        if (r.size() == 2) {
            expectEq(r[0].title, "SERVERS >", "SERVERS > title");
            expectEq(r[0].key, "/menu/servers/c30", "SERVERS > key");
            expectEq(r[1].title, "A", "row after SERVERS >");
        }
    }
    {
        auto r = rows("<MediaContainer><Directory key=\"/p/s0\" title=\"&lt; PREV\"/>"
                      "<Directory key=\"/p/s40\" title=\"NEXT >\"/></MediaContainer>");
        expect(r.size() == 2, "PREV and NEXT rows (2 rows)");
        if (r.size() == 2) {
            expectEq(r[0].title, "< PREV", "&lt; decoded");
            expectEq(r[1].title, "NEXT >", "NEXT > kept");
            expectEq(r[1].key, "/p/s40", "NEXT > key");
        }
    }
    // '>' and "/>" inside values, both quote styles, '"' inside a single-quoted value.
    {
        auto r = rows("<Directory key='/k1' title='A > B'/><Directory key=\"/k2\" title=\"x/>y\"/>"
                      "<Directory key='/k3' title='say \"hi\" > bye'/>");
        expect(r.size() == 3, "three rows with '>' in values");
        if (r.size() == 3) {
            expectEq(r[0].title, "A > B", "single-quoted '>'");
            expectEq(r[1].title, "x/>y", "\"/>\" inside a value");
            expectEq(r[2].title, "say \"hi\" > bye", "'\"' inside single quotes");
            expectEq(r[2].key, "/k3", "key after a tricky value");
        }
    }
    // Exact attribute names: no match inside a longer name or inside another value.
    {
        auto r = rows("<Video monkey=\"m\" parentKey=\"/p\" summary='key=\"/fake\"' key=\"/k\" "
                      "grandparentTitle=\"G\" title=\"T\" ratingKey=\"9\" type=\"episode\"/>");
        expect(r.size() == 1, "one Video row");
        if (r.size() == 1) {
            expectEq(r[0].key, "/k", "key, not monkey/parentKey/summary text");
            expectEq(r[0].title, "T", "title, not grandparentTitle");
            expectEq(r[0].ratingKey, "9", "ratingKey");
            expectEq(r[0].type, "episode", "type");
            expect(!r[0].directory, "Video is not a directory");
        }
    }
    // Text content with '>' between elements, open/close pairs, and child elements.
    {
        auto r = rows("<MediaContainer><Directory key=\"/a\" title=\"A\">x > y &gt; z</Directory>"
                      "<Video key=\"/v\" title=\"V\"><Media id=\"1\"><Part key=\"/part\"/></Media></Video>"
                      "<Directory key=\"/b\" title=\"B\"></Directory></MediaContainer>");
        expect(r.size() == 3, "rows around text with '>'");
        if (r.size() == 3) {
            expectEq(r[0].title, "A", "first Directory");
            expectEq(r[1].title, "B", "second Directory (Directory rows come first)");
            expectEq(r[2].key, "/v", "Video key is the Video's own, not the Part's");
        }
    }
    // A longer element name does not open a row; Track rows still parse.
    {
        auto r = rows("<Videos key=\"/no\" title=\"no\"/><Directoryx key=\"/no2\" title=\"no\"/>"
                      "<Track key=\"/t\" title=\"T &amp; U\" ratingKey=\"5\"/>");
        expect(r.size() == 1, "only the Track row");
        if (r.size() == 1) {
            expectEq(r[0].title, "T & U", "&amp; decoded");
            expectEq(r[0].ratingKey, "5", "Track ratingKey");
        }
    }
    // Entities: predefined, numeric, unknown and malformed ones.
    {
        expectEq(xmlAttr("<D t=\"&quot;Q&quot; &apos;s&#39; &#x41;&#66;\"/>", "t"), "\"Q\" 's' AB",
                 "predefined and numeric entities");
        expectEq(xmlAttr("<D t=\"caf&#xE9; &#x1F600;\"/>", "t"), "caf\xC3\xA9 \xF0\x9F\x98\x80",
                 "numeric entities as UTF-8");
        expectEq(xmlAttr("<D t=\"a & b &foo; &#; &#xZZ; &#0; &#xD800; &\"/>", "t"),
                 "a & b &foo; &#; &#xZZ; &#0; &#xD800; &", "unknown and malformed kept verbatim");
        expectEq(xmlAttr("<D t=\"&amp;lt;\"/>", "t"), "&lt;", "single decode pass");
    }
    // Whitespace around '=', newlines and tabs between attributes; absent and empty attributes.
    {
        auto r = rows("<Directory\n\tkey = \"/k\"\r\n  title =\t'T'\n/>");
        expect(r.size() == 1, "row with spaced '='");
        if (r.size() == 1) {
            expectEq(r[0].key, "/k", "spaced key");
            expectEq(r[0].title, "T", "spaced title");
            expectEq(r[0].ratingKey, "", "absent ratingKey is empty");
        }
        expect(rows("<Directory title=\"\" key=\"\"/>").empty(), "row with only empty fields is dropped");
        expectEq(xmlAttr("<D a=\"1\" b>", "b"), "", "attribute without a value");
        expectEq(xmlAttr("<D a=1 t=\"x\">", "t"), "", "unquoted value stops the scan");
    }
    // Truncated input: no crash, no row from an unterminated tag.
    {
        expect(rows("<Directory key=\"/k\" title=\"NEXT >").empty(), "unterminated quote");
        expect(rows("<Directory key=\"/k\" title=\"T\"").empty(), "unterminated tag");
        expect(rows("<Directory").empty(), "bare open at end");
        expect(rows("").empty(), "empty input");
        auto r = rows("<Directory key=\"/a\" title=\"A\"/><Directory key=\"/b\" title=\"B");
        expect(r.size() == 1 && r[0].key == "/a", "rows before a truncated tail survive");
    }
    // Cursor rests on selectable rows only (review 09:38): the highlighted row is the row Select acts on.
    {
        auto mk = [](const char* t, const char* kind) {
            LibraryRow r;
            r.title = t;
            r.key = "/k";
            r.mpxKind = kind;
            return r;
        };
        // A /a1 section: the "1-8/8" header, items, an info row, an item.
        LibraryBrowser b;
        b.show({mk("1-8/8", "header"), mk("A", "item"), mk("B", "item"), mk("note", "info"), mk("C", "item")});
        expect(b.cursor() == 1, "show settles on the first selectable row (header skipped)");
        b.move(1);
        expect(b.cursor() == 2, "down to B");
        b.move(1);
        expect(b.cursor() == 4, "down skips the info row");
        b.move(1);
        expect(b.cursor() == 1, "down wraps past the header");
        b.move(-1);
        expect(b.cursor() == 4, "up wraps to C");
        b.move(-2);
        expect(b.cursor() == 1, "up 2 skips the info row");
        // The detail page: two info rows, then the actions.
        b.push({mk("Title (2026)", "info"), mk("2026  10m", "info"), mk("PLAY", "action")});
        expect(b.cursor() == 2 && b.current() && b.current()->title == "PLAY", "a pushed detail opens on PLAY");
        b.move(1);
        expect(b.cursor() == 2, "the only action: down stays on it");
        // A refresh that keeps the cursor index lands on a selectable row (Stop reopen).
        b.stack.back() = {mk("Title (2026)", "info"), mk("2026  10m", "info"), mk("RESUME FROM 01:00", "action"),
                          mk("PLAY FROM START", "action")};
        b.clamp();
        expect(b.cursor() == 2, "kept index 2 is RESUME");
        b.cursors.back() = 0;
        b.clamp();
        expect(b.cursor() == 2, "an info index settles forward");
        b.cursors.back() = 9;
        b.clamp();
        expect(b.cursor() == 3, "past the end clamps to the last row");
        // No selectable row at all: the cursor stays, nothing moves.
        b.push({mk("EMPTY", "info")});
        b.move(1);
        expect(b.cursor() == 0 && b.current() && b.current()->title == "EMPTY", "all-info level keeps cursor 0");
        expect(b.pop() && b.cursor() == 3, "pop restores the parent's cursor");
        // Legacy PMS rows (no mpxKind): move is the plain (cursor + delta) mod n, as before.
        for (int start = 0; start < 5; ++start)
            for (int delta = -7; delta <= 7; ++delta) {
                LibraryBrowser l;
                l.show({mk("a", ""), mk("b", ""), mk("c", ""), mk("d", ""), mk("e", "")});
                l.cursors.back() = start;
                l.move(delta);
                expect(l.cursor() == ((start + delta % 5 + 5) % 5),
                       "legacy move start=" + std::to_string(start) + " delta=" + std::to_string(delta));
            }
    }
    if (fails) {
        std::fprintf(stderr, "test_library_browser: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_library_browser: OK\n");
    return 0;
}
