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
        "\"\\u0000\"",  // NUL escape would smuggle a terminator into C strings
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
// Fluidity: a reordered full-size ring and a per-column override still resolve
// by role.  Tables must be exactly RING_SLOT_COUNT slots - the drums are
// physical - so the fixtures are generated from the compiled table.
// --------------------------------------------------------------------------
std::string slots_json(int rotate_digits_by, bool drop_pm) {
    // The compiled ring with the digit block rotated by `rotate_digits_by`
    // (so digit d sits at slot RING_DIGIT_FIRST + (d + rot) % 10), optionally
    // with PM replaced by a glyph to exercise the failure contract.
    std::string out = "[";
    for (int i = 0; i < RING_SLOT_COUNT; ++i) {
        int src = i;
        if (i >= RING_DIGIT_FIRST && i <= RING_DIGIT_LAST) {
            const int d = (i - RING_DIGIT_FIRST + rotate_digits_by) % 10;
            src = RING_DIGIT_FIRST + d;
        }
        const char* id = RING_TABLE[src].char_id;
        const char* cat = ring_category_name(RING_TABLE[src].category);
        if (drop_pm && src == RING_PM_SLOT) {
            id = "notpm";
            cat = "glyph";
        }
        out += "{\"i\":";
        out += std::to_string(i);
        out += ",\"id\":\"";
        out += id;
        out += "\",\"cat\":\"";
        out += cat;
        out += "\"},";
    }
    out.back() = ']';
    return out;
}

void test_reordered_and_per_column() {
    // Shared ring: digits rotated by 3; column 1 override (spec key "ring"):
    // digits rotated by 5; column 2 override via the "slots" alias.
    std::string doc = "{\"slots\":" + slots_json(3, true) +
                      ",\"columns\":[{},{\"ring\":" + slots_json(5, false) +
                      "},{\"slots\":" + slots_json(1, false) + "}]}";

    RingSet set;
    std::string err;
    if (!set.load_json(doc, &err)) {
        CHECK(false);
        std::printf("  load failed: %s\n", err.c_str());
        return;
    }

    // Shared table: digit d found at its rotated slot, other roles unmoved.
    for (int d = 0; d <= 9; ++d) {
        CHECK_EQ(set.col(0).index_for_role(Role::Digit, d),
                 RING_DIGIT_FIRST + ((d + 10 - 3) % 10));
    }
    CHECK_EQ(set.col(0).index_for_role(Role::Question), RING_QMARK_SLOT);
    CHECK_EQ(set.col(0).index_for_role(Role::Wifi), RING_WIFI_SLOT);
    CHECK_EQ(set.col(0).index_for_role(Role::Pm), -1);  // dropped in this table

    // Column overrides: "ring" (spec 4) and the "slots" alias both work.
    CHECK_EQ(set.col(1).index_for_role(Role::Digit, 0), RING_DIGIT_FIRST + 5);
    CHECK_EQ(set.col(1).index_for_role(Role::Pm), RING_PM_SLOT);
    CHECK_EQ(set.col(2).index_for_role(Role::Digit, 0), RING_DIGIT_FIRST + 9);
    // Columns without an override fall back to the shared table.
    CHECK_EQ(set.col(3).index_for_role(Role::Digit, 0), RING_DIGIT_FIRST + 7);

    // The spec 4 failure contract: unknown role -> blank + diagnostic.
    bool diag = false;
    CHECK_EQ(set.index_for_role(0, Role::Pm, 0, &diag), RING_HOME_SLOT);
    CHECK(diag);
    diag = false;
    CHECK_EQ(set.index_for_role(1, Role::Pm, 0, &diag), RING_PM_SLOT);
    CHECK(!diag);

    // A table of the wrong size is rejected outright: the drum has exactly
    // RING_SLOT_COUNT flaps and T(i) is compiled for that geometry.
    RingSet wrong;
    CHECK(!wrong.load_json(R"({"slots":[
        {"i":0,"id":"blank","cat":"blank"},
        {"i":1,"id":"0","cat":"digit"},
        {"i":2,"id":"1","cat":"digit"}]})", &err));
    CHECK(!wrong.loaded_from_json());
    CHECK_EQ(wrong.col(0).slot_count(), RING_SLOT_COUNT);  // fallback active
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
