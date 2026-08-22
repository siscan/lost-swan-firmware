// The fluid ring: json_lite parser, RingTable role/name lookup, RingSet
// per-column overrides and fallback behaviour.  Spec 4.
//
// argv[1] (wired in CMakeLists) is the path of the generated data/ring.json,
// so this suite parses the real artifact, not a copy.
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "ring/json_lite.h"
#include "ring/ring_runtime.h"

using namespace swan;

namespace {

std::string read_all(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// --------------------------------------------------------------------------
// json_lite
// --------------------------------------------------------------------------
void test_json_parser() {
    json::Value v;
    std::string err;

    CHECK(json::parse(R"({"a":1,"b":[true,null,"x"],"c":{"d":-5}})", v, &err));
    CHECK(v.get("a") != nullptr);
    CHECK_EQ(v.get("a")->as_int(), 1);
    CHECK(v.get("b")->as_array() != nullptr);
    CHECK_EQ(v.get("b")->as_array()->size(), 3u);
    CHECK((*v.get("b")->as_array())[0].boolean);
    CHECK((*v.get("b")->as_array())[1].is_null());
    CHECK((*v.get("b")->as_array())[2].as_str() == "x");
    CHECK_EQ(v.get("c")->get("d")->as_int(), -5);
    CHECK(v.get("nope") == nullptr);

    // Escapes, including the \u2014 the manifest carries.
    CHECK(json::parse(R"("a\"b\\c\n\u2014")", v, nullptr));
    CHECK(v.str == "a\"b\\c\n\xE2\x80\x94");

    // Rejections - each must fail cleanly, never crash or accept.
    const char* bad[] = {
        "",           "{",       "[1,",     R"({"a"})",   R"({"a":})",
        "01x",        "1.5",     "1e3",     "tru",        R"("unterminated)",
        "[1] garbage", R"({"a":1,})", "\"\\u12\"", "\"\x01\"",
    };
    for (const char* b : bad) {
        err.clear();
        CHECK(!json::parse(b, v, &err));
        CHECK(!err.empty());
    }

    // Depth bomb: 20 nested arrays > MAX_DEPTH 16.
    std::string deep(20, '[');
    deep += std::string(20, ']');
    CHECK(!json::parse(deep, v, nullptr));
}

// --------------------------------------------------------------------------
// The real generated artifact vs the compiled table: identical lookups.
// --------------------------------------------------------------------------
void test_generated_ring_json(const char* path) {
    const std::string text = read_all(path);
    if (text.empty()) {
        CHECK(false);
        std::printf("  cannot read %s\n", path ? path : "(null)");
        return;
    }

    RingSet set;
    std::string err;
    if (!set.load_json(text, &err)) {
        CHECK(false);
        std::printf("  load_json failed: %s\n", err.c_str());
        return;
    }
    CHECK(set.loaded_from_json());

    for (int col = 0; col < N_COLUMNS; ++col) {
        const RingTable& t = set.col(col);
        CHECK_EQ(t.slot_count(), RING_SLOT_COUNT);

        // Role lookups equal the compiled constants...
        CHECK_EQ(t.index_for_role(Role::Blank), RING_HOME_SLOT);
        CHECK_EQ(t.index_for_role(Role::Am), RING_AM_SLOT);
        CHECK_EQ(t.index_for_role(Role::Pm), RING_PM_SLOT);
        CHECK_EQ(t.index_for_role(Role::Wifi), RING_WIFI_SLOT);
        CHECK_EQ(t.index_for_role(Role::Question), RING_QMARK_SLOT);
        for (int d = 0; d <= 9; ++d) {
            CHECK_EQ(t.index_for_role(Role::Digit, d), ring_index_for_digit(d));
        }
        // ...and token lookup agrees with the compiled path for every slot.
        for (int i = 0; i < RING_SLOT_COUNT; ++i) {
            CHECK_EQ(t.index_for_token(RING_TABLE[i].char_id), i);
            CHECK(std::strcmp(t.slot(i).id.c_str(), RING_TABLE[i].char_id) == 0);
        }
        CHECK_EQ(t.index_for_token("_"), RING_HOME_SLOT);
        CHECK_EQ(t.index_for_token("#33"), 33);
        CHECK_EQ(t.index_for_token("AM"), RING_AM_SLOT);  // case-insensitive
        CHECK_EQ(t.index_for_token("#50"), -1);
        CHECK_EQ(t.index_for_token("nosuch"), -1);
    }
}

// --------------------------------------------------------------------------
// Fluidity: a reordered ring and a per-column override still resolve by role.
// --------------------------------------------------------------------------
void test_reordered_and_per_column() {
    // A 6-slot toy ring with digits out of the manifest's positions.
    const char* toy = R"({
      "slot_count": 6,
      "slots": [
        {"i":0,"id":"blank","label":"blank","cat":"blank"},
        {"i":1,"id":"7","label":"digit 7","cat":"digit"},
        {"i":2,"id":"qmark","label":"?","cat":"glyph"},
        {"i":3,"id":"wifi","label":"wifi","cat":"wifi"},
        {"i":4,"id":"AM","label":"AM","cat":"ampm"},
        {"i":5,"id":"0","label":"digit 0","cat":"digit"}],
      "columns": [
        {"scheme":"minutes"},
        {"scheme":"minutes","slots":[
          {"i":0,"id":"blank","cat":"blank"},
          {"i":1,"id":"0","cat":"digit"},
          {"i":2,"id":"7","cat":"digit"}]}
      ]})";

    RingSet set;
    std::string err;
    CHECK(set.load_json(toy, &err));

    // Shared table on col 0: roles found wherever they live.
    CHECK_EQ(set.col(0).index_for_role(Role::Digit, 7), 1);
    CHECK_EQ(set.col(0).index_for_role(Role::Digit, 0), 5);
    CHECK_EQ(set.col(0).index_for_role(Role::Question), 2);
    CHECK_EQ(set.col(0).index_for_role(Role::Wifi), 3);
    CHECK_EQ(set.col(0).index_for_role(Role::Am), 4);
    CHECK_EQ(set.col(0).index_for_role(Role::Pm), -1);  // absent here

    // Column 1 carries its own ring.
    CHECK_EQ(set.col(1).slot_count(), 3);
    CHECK_EQ(set.col(1).index_for_role(Role::Digit, 7), 2);
    CHECK_EQ(set.col(1).index_for_role(Role::Digit, 0), 1);
    // Columns 2..4 fall back to the shared table.
    CHECK_EQ(set.col(2).index_for_role(Role::Digit, 7), 1);

    // The spec 4 failure contract: unknown role -> blank + diagnostic.
    bool diag = false;
    CHECK_EQ(set.index_for_role(0, Role::Pm, 0, &diag), 0);
    CHECK(diag);
    diag = false;
    CHECK_EQ(set.index_for_role(0, Role::Digit, 7, &diag), 1);
    CHECK(!diag);
}

// --------------------------------------------------------------------------
// Every failure path lands on the compiled fallback, never on a dead table.
// --------------------------------------------------------------------------
void test_fallback_on_bad_input() {
    const char* bad[] = {
        "not json at all",
        "{}",                                             // no slots
        R"({"slots": 5})",                                // slots not an array
        R"({"slots": [{"i":1,"id":"x","cat":"blank"}]})", // not dense from 0
        R"({"slots": [{"i":0,"id":"x","cat":"weird"}]})", // unknown category
        R"({"slots": [{"i":0,"id":"","cat":"blank"},{"i":1,"id":"y","cat":"glyph"}]})",  // empty id
        R"({"slots": [{"i":0,"id":"a","cat":"glyph"},{"i":1,"id":"b","cat":"glyph"}]})", // no blank
    };
    for (const char* b : bad) {
        RingSet set;
        std::string err;
        CHECK(!set.load_json(b, &err));
        CHECK(!err.empty());
        CHECK(!set.loaded_from_json());
        // Still fully functional on the compiled table.
        CHECK_EQ(set.col(0).index_for_role(Role::Blank), RING_HOME_SLOT);
        CHECK_EQ(set.col(4).index_for_role(Role::Digit, 9), ring_index_for_digit(9));
    }

    // compiled_fallback alone behaves like the compiled constants.
    const RingSet fb = RingSet::compiled_fallback();
    CHECK(!fb.loaded_from_json());
    CHECK_EQ(fb.col(0).index_for_role(Role::Question), RING_QMARK_SLOT);
    CHECK_EQ(fb.col(0).index_for_token("ankh"), ring_index_for_token("ankh"));
}

}  // namespace

void run_tests() {
    test_json_parser();
    test_generated_ring_json(g_argv1);
    test_reordered_and_per_column();
    test_fallback_on_bad_input();
}
