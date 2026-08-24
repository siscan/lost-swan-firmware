// The streaming ring loader against the DOM one: same document, same table.
//
// This exists because the DOM path is the last place a bad upload can still
// reach the allocator hard enough to abort the board - a json::Value is ~64
// bytes against a 2-byte token, and the 700-node cap sits against a real
// ring.json's 535.  The streaming parser's memory does not depend on the
// document, so the interesting assertions here are the hostile ones.
#include <cstdio>
#include <string>

#include "check.h"
#include "ring/json_lite.h"
#include "ring/json_write.h"
#include "ring/json_stream.h"
#include "ring/ring_runtime.h"

using namespace swan;

namespace {

std::string read_file(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return {};
    std::string out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// Counts events, so a test can assert the parser saw the structure without
// building anything.
struct Counter final : json::StreamHandler {
    int objects = 0, arrays = 0, keys = 0, strings = 0, numbers = 0;
    bool on_object_begin() override { ++objects; return true; }
    bool on_array_begin() override { ++arrays; return true; }
    bool on_key(std::string_view) override { ++keys; return true; }
    bool on_string(std::string_view) override { ++strings; return true; }
    bool on_number(double) override { ++numbers; return true; }
};

bool parse_all(const std::string& text, Counter& c, std::size_t chunk = 7) {
    json::StreamParser p(c);
    for (std::size_t off = 0; off < text.size(); off += chunk) {
        if (!p.feed(std::string_view(text).substr(off, chunk))) return false;
    }
    return p.finish();
}

void test_tokenizer_survives_any_chunk_boundary() {
    const std::string doc =
        R"({"a":"x\ny","b":[1,-2.5e3,true,false,null],"c":{"d":"\u00e9"},"e":""})";
    // A chunk boundary must be allowed to fall inside a string, a number, an
    // escape and a \u sequence - which is the whole difficulty of streaming.
    for (std::size_t chunk = 1; chunk <= doc.size(); ++chunk) {
        Counter c;
        if (!parse_all(doc, c, chunk)) {
            std::printf("  FAILED at chunk size %u\n", static_cast<unsigned>(chunk));
            CHECK(false);
            return;
        }
        CHECK_EQ(c.objects, 2);
        CHECK_EQ(c.arrays, 1);
        CHECK_EQ(c.keys, 5);
        CHECK_EQ(c.numbers, 2);
    }
}

void test_tokenizer_refuses_what_it_should() {
    const char* bad[] = {
        "{",                       // truncated
        "[1,2",                    // truncated
        "{\"a\":}",                // missing value
        "{\"a\" 1}",               // missing colon
        "{'a':1}",                 // single quotes
        "{\"a\":1}}",              // trailing data
        "[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]",   // deeper than STREAM_DEPTH_MAX
        "{\"a\":\"\\u0000\"}",     // a NUL smuggled into a C string
        "",                        // empty
    };
    for (const char* b : bad) {
        Counter c;
        const bool ok = parse_all(b, c, 3);
        if (ok) std::printf("  accepted what it should not: %s\n", b);
        CHECK(!ok);
    }
    // ... and a string longer than the token cap, which is the bound that
    // makes the parser's memory independent of the input.
    std::string big = "{\"a\":\"";
    big.append(json::STREAM_TOKEN_MAX + 10, 'x');
    big += "\"}";
    Counter c;
    CHECK(!parse_all(big, c, 64));
}

void test_streaming_and_dom_agree_on_the_real_ring() {
    const std::string doc = read_file(g_argv1 != nullptr ? g_argv1 : "data/ring.json");
    if (doc.empty()) {
        std::printf("  SKIPPED: no ring.json passed as argv[1]\n");
        return;
    }
    RingSet dom;
    std::string e1;
    CHECK(dom.load_json(doc, &e1));
    RingSet str;
    std::string e2;
    if (!str.load_json_streaming(doc, &e2)) {
        std::printf("  streaming loader rejected the real ring: %s\n", e2.c_str());
        CHECK(false);
        return;
    }

    // Same tables, slot for slot, on every column - the assertion that matters,
    // because a "faster parser" that quietly differs is worse than no parser.
    for (int c = 0; c < N_COLUMNS; ++c) {
        CHECK_EQ(str.col(c).slot_count(), dom.col(c).slot_count());
        for (int i = 0; i < dom.col(c).slot_count(); ++i) {
            CHECK_STREQ(str.col(c).slot(i).id.c_str(), dom.col(c).slot(i).id.c_str());
            CHECK_STREQ(str.col(c).slot(i).label.c_str(), dom.col(c).slot(i).label.c_str());
            CHECK(str.col(c).slot(i).cat == dom.col(c).slot(i).cat);
        }
    }
    CHECK_EQ(str.loaded_from_json(), true);
    // The colour schemes travel with the ring to the browser (spec 17,
    // 2026-08-22).  They are presentation, so they are carried verbatim rather
    // than interpreted - but "carried" has to mean carried: dropping them would
    // render columns 4-5 in the wrong scheme after every upload.
    CHECK(!str.schemes_json().empty());
    CHECK_EQ(str.schemes_json().empty(), dom.schemes_json().empty());
    // It must be VALID JSON, and it must mean the same thing.  Re-serialising
    // it from events put the comma after the colon - {"default":,"#181818"} -
    // which the whole control panel then failed to parse.  Comparing to the DOM
    // path's copy is the assertion that catches that class outright.
    {
        json::Value a, b;
        CHECK(json::parse(str.schemes_json(), a, nullptr));
        CHECK(json::parse(dom.schemes_json(), b, nullptr));
        json::Writer wa, wb;
        json::serialize_into(a, wa);
        json::serialize_into(b, wb);
        CHECK_STREQ(wa.take().c_str(), wb.take().c_str());
    }
    for (int c = 0; c < N_COLUMNS; ++c) {
        CHECK_STREQ(str.scheme(c).c_str(), dom.scheme(c).c_str());
    }
    // Roles resolve identically, which is what the renderers actually use.
    for (int c = 0; c < N_COLUMNS; ++c) {
        for (int d = 0; d <= 9; ++d) {
            CHECK_EQ(str.index_for_role(c, Role::Digit, d, 0),
                     dom.index_for_role(c, Role::Digit, d, 0));
        }
        CHECK_EQ(str.index_for_role(c, Role::Blank, 0, 0), dom.index_for_role(c, Role::Blank, 0, 0));
    }
}

void test_streaming_rejects_the_same_documents_the_dom_does() {
    const std::string good = read_file(g_argv1 != nullptr ? g_argv1 : "data/ring.json");
    if (good.empty()) {
        std::printf("  SKIPPED: no ring.json\n");
        return;
    }
    struct Case {
        const char* what;
        std::string doc;
    };
    // Generated rather than carved out of the real file: string surgery on a
    // pretty-printed document silently did nothing, and a test that quietly
    // stops testing is worse than no test.
    const auto ring_of = [](int n) {
        std::string d = "{\"slots\":[";
        for (int i = 0; i < n; ++i) {
            if (i) d.push_back(',');
            d += "{\"i\":" + std::to_string(i) + ",\"id\":\"g" + std::to_string(i) +
                 "\",\"cat\":\"glyph\"}";
        }
        return d + "]}";
    };

    const Case cases[] = {
        {"empty", ""},
        {"not json", "this is not json at all"},
        {"no slots array", R"({"schemes":{}})"},
        {"slots not an array", R"({"slots":42})"},
        {"a slot missing its id", R"({"slots":[{"i":0,"cat":"blank"}]})"},
        {"a slot with an unknown category", R"({"slots":[{"i":0,"id":"x","cat":"nope"}]})"},
        {"49 slots", ring_of(49)},
        {"51 slots", ring_of(51)},
        {"truncated mid-document", good.substr(0, good.size() / 2)},
    };
    for (const Case& c : cases) {
        RingSet s;
        std::string err;
        const bool ok = s.load_json_streaming(c.doc, &err);
        if (ok) std::printf("  accepted %s\n", c.what);
        CHECK(!ok);
        CHECK(!err.empty());
        // A rejection must leave a WORKING table, exactly as the DOM path does.
        CHECK_EQ(s.col(0).slot_count(), RING_SLOT_COUNT);
    }
}

void test_a_flood_costs_nothing() {
    // The document that panicked the board through the DOM path: a huge array
    // of tiny tokens.  Through the streaming parser it is refused at the slot
    // count with no allocation proportional to the input at all.
    std::string flood = R"({"slots":[)";
    for (int i = 0; i < 20000; ++i) {
        if (i) flood.push_back(',');
        flood += "{\"i\":" + std::to_string(i) + ",\"id\":\"x\",\"cat\":\"glyph\"}";
    }
    flood += "]}";
    std::printf("  flood document is %u bytes\n", static_cast<unsigned>(flood.size()));

    RingSet s;
    std::string err;
    CHECK(!s.load_json_streaming(flood, &err));
    // Refused EARLY - at slot 51, not after reading 20,000 of them.
    CHECK(err.find("more slots") != std::string::npos ||
          err.find("slots; the drum") != std::string::npos);
    CHECK_EQ(s.col(0).slot_count(), RING_SLOT_COUNT);
}


// A surrogate PAIR is one codepoint and must come out as one 4-byte UTF-8
// sequence; half a pair is not a character at all.  Encoding each half
// separately produces CESU-8, which is not valid UTF-8 - and it would land in a
// ring slot id and come back out of /api/ring as mojibake in a browser.
struct Collector final : json::StreamHandler {
    std::vector<std::string> strings;
    bool on_string(std::string_view v) override { strings.emplace_back(v); return true; }
};

void test_surrogate_pairs_and_lone_halves() {
    Collector c;
    json::StreamParser p(c);
    CHECK(p.feed(R"({"a":"\ud83d\ude00"})"));
    CHECK(p.finish());
    CHECK_EQ(c.strings.size(), 1u);
    // U+1F600, the four bytes F0 9F 98 80.
    CHECK(c.strings[0] == std::string("\xF0\x9F\x98\x80"));

    const char* bad[] = {
        R"({"a":"\ud83d"})",        // a high half, then the string ends
        R"({"a":"\ude00"})",         // a low half with nothing before it
        R"({"a":"\ud83d\n"})",        // a high half followed by a plain escape
        R"({"a":"\ud83dA"})",       // a high half followed by an ordinary character
    };
    for (const char* b : bad) {
        Counter cc;
        const bool ok = parse_all(b, cc, 5);
        if (ok) std::printf("  accepted a broken surrogate: %s" "\n", b);
        CHECK(!ok);
    }
}

// Keys are matched at an exact DEPTH as well as an exact state.  The format
// skips anything unrecognised by depth, so an unrecognised object nested inside
// a column could otherwise carry a "ring" key and be read as that column's
// table - and one nested inside a slot could overwrite the slot's own id.
void test_a_nested_key_is_not_the_real_one() {
    const std::string doc = read_file(g_argv1 != nullptr ? g_argv1 : "data/ring.json");
    if (doc.empty()) { std::printf("  SKIPPED: no ring.json" "\n"); return; }

    RingSet plain;
    CHECK(plain.load_json_streaming(doc, nullptr));
    const std::string want = plain.col(4).slot(0).id;

    // The SAME document, with the load-bearing key names planted one level
    // deeper inside column 1 - an object the format is entitled to skip.  It is
    // inserted rather than appended so the column positions do not move: the
    // question is whether "ring" at the wrong depth is read as a ring, not
    // whether an extra column shifts anything.
    std::string doc2 = doc;
    const std::string needle = "\"columns\": [";
    std::size_t at = doc2.find(needle);
    if (at == std::string::npos) at = doc2.find("\"columns\":[");
    CHECK(at != std::string::npos);
    const std::size_t brace = doc2.find('{', at);
    CHECK(brace != std::string::npos);
    doc2.insert(brace + 1,
                "\"meta\":{\"ring\":[{\"i\":0,\"id\":\"WRONG\",\"cat\":\"glyph\"}],"
                "\"scheme\":\"nonsense\",\"slots\":[]},");

    RingSet nested;
    std::string err;
    const bool ok = nested.load_json_streaming(doc2, &err);
    if (!ok) std::printf("  rejected: %s" "\n", err.c_str());
    CHECK(ok);
    // The junk went to column 1, which must be untouched, and nothing became a
    // 1-slot table.
    CHECK_EQ(nested.col(0).slot_count(), RING_SLOT_COUNT);
    CHECK(nested.col(4).slot(0).id == want);
}

}  // namespace

void run_tests() {
    test_tokenizer_survives_any_chunk_boundary();
    test_tokenizer_refuses_what_it_should();
    test_streaming_and_dom_agree_on_the_real_ring();
    test_streaming_rejects_the_same_documents_the_dom_does();
    test_a_flood_costs_nothing();
    test_surrogate_pairs_and_lone_halves();
    test_a_nested_key_is_not_the_real_one();
}
