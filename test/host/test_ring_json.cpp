// The fluid ring (spec 4): json_lite, per-column loading of the real
// data/ring.json, nearest-forward on the loaded tables, the role-existence
// assertion, and every fallback path.
//
// argv[1] (wired in CMakeLists) is the generated data/ring.json, so this suite
// parses the real artifact rather than a copy.
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
    CHECK_EQ(v.get("a")->as_int(), 1);
    CHECK_EQ(v.get("b")->as_array()->size(), 3u);
    CHECK((*v.get("b")->as_array())[0].boolean);
    CHECK((*v.get("b")->as_array())[1].is_null());
    CHECK((*v.get("b")->as_array())[2].as_str() == "x");
    CHECK_EQ(v.get("c")->get("d")->as_int(), -5);
    CHECK(v.get("nope") == nullptr);

    // Escapes, including the em-dash the manifests carry.
    CHECK(json::parse(R"("a\"b\\c\n\u2014")", v, nullptr));
    CHECK(v.str == "a\"b\\c\n\xE2\x80\x94");
    // Raw multibyte UTF-8 passes through untouched (ringgen writes it raw).
    CHECK(json::parse("\"v3 \xE2\x80\x94 descending\"", v, nullptr));
    CHECK(v.str == "v3 \xE2\x80\x94 descending");

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

    std::string deep(20, '[');
    deep += std::string(20, ']');
    CHECK(!json::parse(deep, v, nullptr));
}

// --------------------------------------------------------------------------
// The real generated artifact: cols 1-4 get ring A, col 5 gets ring B, and
// every lookup matches the compiled fallback.
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
    CHECK(set.validate_roles(&err));

    const RingSet compiled = RingSet::compiled_fallback();
    for (int c = 0; c < N_COLUMNS; ++c) {
        const RingTable& j = set.col(c);
        const RingTable& k = compiled.col(c);
        CHECK_EQ(j.slot_count(), k.slot_count());
        CHECK(j.is_descending());
        for (int i = 0; i < RING_SLOT_COUNT; ++i) {
            CHECK(j.slot(i).id == k.slot(i).id);
            CHECK(j.slot(i).cat == k.slot(i).cat);
        }
        // Nearest-forward agrees from every starting slot, for every digit.
        for (int from = 0; from < RING_SLOT_COUNT; ++from) {
            for (int d = 0; d <= 9; ++d) {
                CHECK_EQ(j.index_for_role(Role::Digit, d, from),
                         k.index_for_role(Role::Digit, d, from));
            }
        }
    }

    // Cols 1-4 share one table; col 5 is genuinely different.
    CHECK_EQ(set.col(0).slots_for_role(Role::Digit, 0).size(), 1u);
    CHECK_EQ(set.col(3).slots_for_role(Role::Digit, 0).size(), 1u);
    CHECK_EQ(set.col(4).slots_for_role(Role::Digit, 0).size(), 2u);
    CHECK_EQ(set.col(4).index_for_role(Role::Wifi), -1);
}

// --------------------------------------------------------------------------
// Fluidity: a reordered full-size ring and per-column overrides still resolve
// by role.  Fixtures are generated from the compiled tables - the drums are
// physical, so a table must be exactly RING_SLOT_COUNT slots.
// --------------------------------------------------------------------------
// The table CYCLICALLY ROTATED by `rot` slots: slot i takes what slot i+rot
// held.  A cyclic shift preserves every adjacency, so the ring stays
// descending - relabelling digits in place would not (it breaks the chain at
// the block edge), which is exactly the kind of table the loader must reject.
std::string slots_json(const RingTable& base, int rot, const char* drop_id) {
    std::string out = "[";
    for (int i = 0; i < RING_SLOT_COUNT; ++i) {
        const int src = ((i + rot) % RING_SLOT_COUNT + RING_SLOT_COUNT) % RING_SLOT_COUNT;
        std::string id = base.slot(src).id;
        std::string cat = ring_category_name(base.slot(src).cat);
        if (drop_id != nullptr && id == drop_id) {
            id = "dropped";
            cat = "glyph";
        }
        out += "{\"i\":" + std::to_string(i) + ",\"id\":\"" + id + "\",\"cat\":\"" + cat + "\"},";
    }
    out.back() = ']';
    return out;
}

void test_per_column_loading() {
    const RingSet base = RingSet::compiled_fallback();
    // Shared table = ring A rotated by 3; col 5 keeps ring B; col 2 gets its
    // own copy of ring A via the "slots" alias.
    const std::string doc =
        "{\"slots\":" + slots_json(base.col(0), 3, nullptr) +
        ",\"columns\":[{},{},{\"slots\":" + slots_json(base.col(0), 0, nullptr) +
        "},{},{\"ring\":" + slots_json(base.col(4), 0, nullptr) + "}]}";

    RingSet set;
    std::string err;
    if (!set.load_json(doc, &err)) {
        CHECK(false);
        std::printf("  load failed: %s\n", err.c_str());
        return;
    }

    // Shared table rotated by 3: everything ring A held at slot s now sits at
    // s-3, so digit d moved from 49-d to 46-d.
    for (int d = 0; d <= 9; ++d) {
        const int want = 46 - d;
        CHECK_EQ(set.col(0).slots_for_role(Role::Digit, d)[0], want);
        CHECK_EQ(set.col(3).slots_for_role(Role::Digit, d)[0], want);
    }
    // The rotation moved the blank off the home slot - legal, and the role
    // lookup follows the data rather than assuming slot 0.
    CHECK(set.col(0).index_for_role(Role::Blank) != RING_HOME_SLOT);
    // Column 3 carries its own un-rotated ring A.
    CHECK_EQ(set.col(2).slots_for_role(Role::Digit, 0)[0], 49);
    // Column 5 keeps its double block.
    CHECK_EQ(set.col(4).slots_for_role(Role::Digit, 0).size(), 2u);
    // Rotation preserves descending on every column.
    for (int c = 0; c < N_COLUMNS; ++c) CHECK(set.col(c).is_descending());
}

// --------------------------------------------------------------------------
// The role-existence assertion: a table that cannot render something its
// column will be asked for is rejected at LOAD, not at render time.
// --------------------------------------------------------------------------
void test_role_assertion_rejects_at_load() {
    const RingSet base = RingSet::compiled_fallback();
    std::string err;

    // Ring B on every column: col 1 loses AM/PM, col 3 loses the WiFi glyph.
    {
        RingSet set;
        const std::string doc = "{\"slots\":" + slots_json(base.col(4), 0, nullptr) + "}";
        CHECK(!set.load_json(doc, &err));
        CHECK(err.find("cannot render") != std::string::npos);
        CHECK(!set.loaded_from_json());        // fell back
        CHECK(set.col(0).index_for_role(Role::Am) >= 0);  // ...to a working table
    }
    // A shared table missing a digit fails for every column.
    {
        RingSet set;
        const std::string doc = "{\"slots\":" + slots_json(base.col(0), 0, "7") + "}";
        CHECK(!set.load_json(doc, &err));
        CHECK(err.find("digit 7") != std::string::npos);
    }
    // A per-column override that drops '?' fails - the qmarks preset asks
    // every column for it.
    {
        RingSet set;
        const std::string doc = "{\"slots\":" + slots_json(base.col(0), 0, nullptr) +
                                ",\"columns\":[{},{},{},{},{\"ring\":" +
                                slots_json(base.col(4), 0, "qmark") + "}]}";
        CHECK(!set.load_json(doc, &err));
        CHECK(err.find("column 5") != std::string::npos);
        CHECK(err.find("question") != std::string::npos);
    }
    // Column 5 legitimately lacking AM/PM is NOT an error: it is never asked.
    {
        RingSet set;
        const std::string doc = "{\"slots\":" + slots_json(base.col(0), 0, nullptr) +
                                ",\"columns\":[{},{},{},{},{\"ring\":" +
                                slots_json(base.col(4), 0, nullptr) + "}]}";
        CHECK(set.load_json(doc, &err));
        CHECK(set.loaded_from_json());
    }
}

// --------------------------------------------------------------------------
// Every failure path lands on the compiled fallback, never a dead table.
// --------------------------------------------------------------------------
void test_fallback_on_bad_input() {
    const char* bad[] = {
        "not json at all",
        "{}",                                              // no slots
        R"({"slots": 5})",                                 // slots not an array
        R"({"slots": [{"i":1,"id":"x","cat":"blank"}]})",  // not dense from 0
        R"({"slots": [{"i":0,"id":"x","cat":"weird"}]})",  // unknown category
        R"({"slots": [{"i":0,"id":"a","cat":"blank"},{"i":1,"id":"b","cat":"glyph"}]})",  // size
    };
    for (const char* b : bad) {
        RingSet set;
        std::string err;
        CHECK(!set.load_json(b, &err));
        CHECK(!err.empty());
        CHECK(!set.loaded_from_json());
        // Still fully functional on the compiled tables.
        CHECK_EQ(set.col(0).index_for_role(Role::Blank), RING_HOME_SLOT);
        CHECK_EQ(set.col(4).slots_for_role(Role::Digit, 9).size(), 2u);
        CHECK(set.validate_roles(nullptr));
    }
}

}  // namespace

void run_tests() {
    test_json_parser();
    test_generated_ring_json(g_argv1);
    test_per_column_loading();
    test_role_assertion_rejects_at_load();
    test_fallback_on_bad_input();
}
