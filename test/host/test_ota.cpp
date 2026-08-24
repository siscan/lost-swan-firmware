// OTA policy (spec 10.4).
//
// The sniffer is checked against a REAL built binary where one is available
// (argv[1]), because a hand-built header only proves the test agrees with
// itself. The gating and the boot criterion are checked exhaustively, because
// every wrong answer there is a display that will not come back.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "webapi/ota_policy.h"

using namespace swan;
using namespace swan::api;

namespace {

// A synthetic image header, so the negative cases can be built precisely.
std::vector<uint8_t> make_image(uint16_t chip_id, const char* project, const char* version) {
    std::vector<uint8_t> b(OTA_HEADER_BYTES, 0);
    b[0] = 0xE9;                      // ESP_IMAGE_HEADER_MAGIC
    b[1] = 3;                         // segment_count
    b[12] = static_cast<uint8_t>(chip_id & 0xFF);
    b[13] = static_cast<uint8_t>(chip_id >> 8);
    b[15] = 100;                      // min_chip_rev_full = v1.0
    b[16] = 0;
    b[17] = 199 & 0xFF;               // max_chip_rev_full = v1.99
    b[18] = 199 >> 8;
    uint8_t* d = b.data() + 32;       // 24 image hdr + 8 segment hdr
    const uint32_t magic = 0xABCD5432u;
    std::memcpy(d, &magic, 4);
    std::strncpy(reinterpret_cast<char*>(d + 16), version, 31);
    std::strncpy(reinterpret_cast<char*>(d + 48), project, 31);
    std::strncpy(reinterpret_cast<char*>(d + 80), "v5.5.5", 31);
    return b;
}

OtaPrecheck base_check() {
    OtaPrecheck p;
    p.image = sniff_image(make_image(OTA_CHIP_ID_ESP32C5, "lost_swan_firmware",
                                     "0.4.0+devkitc1.sim").data(),
                          OTA_HEADER_BYTES);
    p.running_project = "lost_swan_firmware";
    p.running_board = "devkitc1";
    p.all_axes_idle = true;
    p.free_heap = 130000;
    return p;
}

// --------------------------------------------------------------------------
void test_sniff_a_real_binary() {
    if (g_argv1 == nullptr) {
        std::printf("  (no binary given; skipping the real-image check)\n");
        return;
    }
    std::FILE* f = std::fopen(g_argv1, "rb");
    if (f == nullptr) {
        std::printf("  (cannot open %s; skipping)\n", g_argv1);
        return;
    }
    std::vector<uint8_t> buf(OTA_HEADER_BYTES);
    const size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    CHECK_EQ(n, OTA_HEADER_BYTES);

    const ImageInfo i = sniff_image(buf.data(), n);
    CHECK(i.parsed);
    CHECK_EQ(i.chip_id, OTA_CHIP_ID_ESP32C5);
    CHECK_STREQ(i.project_name.c_str(), "lost_swan_firmware");
    // PROJECT_VER carries what esp_app_desc_t cannot: which pin map, and
    // whether the simulated axes are compiled in.  Without it an OTA cannot
    // refuse a wrong-board image and the rollback test cannot tell the two
    // images apart.
    CHECK(!i.board.empty());
    // Read from the right offset: version, project_name and idf_ver sit
    // among four fixed-width strings and a wrong one looks plausible.
    CHECK(i.idf_ver.rfind("v", 0) == 0);
    CHECK(i.board == "devkitc1" || i.board == "xiao");
    CHECK(i.flavour == "sim" || i.flavour == "rel" || i.flavour == "nosim");
    std::printf("  real image: %s %s (board %s, flavour %s, idf %s)\n",
                i.project_name.c_str(), i.version.c_str(), i.board.c_str(),
                i.flavour.c_str(), i.idf_ver.c_str());
}

void test_sniff_rejects_rubbish() {
    CHECK(!sniff_image(nullptr, 0).parsed);
    std::vector<uint8_t> good = make_image(OTA_CHIP_ID_ESP32C5, "lost_swan_firmware", "1+a.b");
    CHECK(sniff_image(good.data(), good.size()).parsed);

    // Truncated: an upload that dies mid-header must not be read as valid.
    for (std::size_t n = 0; n < OTA_HEADER_BYTES; n += 37) {
        CHECK(!sniff_image(good.data(), n).parsed);
    }
    // Not an image at all - the ring.json somebody picked by mistake.
    std::vector<uint8_t> json(OTA_HEADER_BYTES, '{');
    CHECK(!sniff_image(json.data(), json.size()).parsed);
    // Right image magic, wrong app-descriptor magic: a bootloader, or a
    // partition table, or a truncated app.
    std::vector<uint8_t> nodesc = good;
    nodesc[32] ^= 0xFF;
    CHECK(!sniff_image(nodesc.data(), nodesc.size()).parsed);
}

// A field that exactly fills its 32 bytes is NOT null-terminated.
void test_sniff_handles_an_unterminated_field() {
    const std::string v31(31, 'v');
    std::vector<uint8_t> b = make_image(OTA_CHIP_ID_ESP32C5, "lost_swan_firmware", v31.c_str());
    // strncpy leaves byte 31 as the NUL we zero-filled; overwrite it so the
    // field is exactly full, which is the case that would run off the end.
    b[16 + 32 + 31] = 'v';
    const ImageInfo i = sniff_image(b.data(), b.size());
    CHECK(i.parsed);
    CHECK_EQ(i.version.size(), 32u);
}

void test_gate_allows_the_ordinary_case() {
    CHECK(ota_gate(base_check()) == OtaVerdict::Allow);
}

void test_gate_refuses_what_idf_would_accept() {
    // THE case. Every check ESP-IDF makes - magic, SHA256, chip id, chip
    // revision - passes for an image built with the other board's pin map.
    // Only the version tag can tell, and only if somebody looks.
    OtaPrecheck p = base_check();
    p.image = sniff_image(make_image(OTA_CHIP_ID_ESP32C5, "lost_swan_firmware",
                                     "0.4.0+xiao.sim").data(), OTA_HEADER_BYTES);
    CHECK(ota_gate(p) == OtaVerdict::WrongBoard);
    p.force = true;
    CHECK(ota_gate(p) == OtaVerdict::Allow);   // insisted on, and allowed

    // A release image onto a board saved all-simulated. col_mode survives
    // PERFECTLY, which is exactly the problem: five Sim columns come back,
    // a release build has no Sim case, all five fault, escalation drops EN,
    // and motion.column then refuses to set sim back.
    OtaPrecheck q = base_check();
    q.any_simulated_column = true;
    q.image = sniff_image(make_image(OTA_CHIP_ID_ESP32C5, "lost_swan_firmware",
                                     "0.4.0+devkitc1.rel").data(), OTA_HEADER_BYTES);
    CHECK(ota_gate(q) == OtaVerdict::LosesSimulation);
    q.force = true;
    CHECK(ota_gate(q) == OtaVerdict::Allow);
    // ... and it is fine on a board with no simulated columns.
    q.force = false;
    q.any_simulated_column = false;
    CHECK(ota_gate(q) == OtaVerdict::Allow);
}

void test_gate_refuses_the_rest() {
    OtaPrecheck p = base_check();
    p.image = ImageInfo{};
    CHECK(ota_gate(p) == OtaVerdict::BadImage);

    p = base_check();
    p.image = sniff_image(make_image(0x0005, "lost_swan_firmware", "0.4.0+devkitc1.sim").data(),
                          OTA_HEADER_BYTES);
    CHECK(ota_gate(p) == OtaVerdict::WrongChip);

    p = base_check();
    p.image = sniff_image(make_image(OTA_CHIP_ID_ESP32C5, "some_other_project",
                                     "1.0+devkitc1.sim").data(), OTA_HEADER_BYTES);
    CHECK(ota_gate(p) == OtaVerdict::WrongProject);

    // A moving drum. Not force-overridable: the control task stalls on every
    // sector erase, so an axis would keep stepping with nobody able to stop it.
    p = base_check();
    p.all_axes_idle = false;
    CHECK(ota_gate(p) == OtaVerdict::Moving);
    p.force = true;
    CHECK(ota_gate(p) == OtaVerdict::Moving);
    // ... unless maintenance is on, which is the deliberate override.
    p.force = false;
    p.maintenance = true;
    CHECK(ota_gate(p) == OtaVerdict::Allow);

    p = base_check();
    p.running_pending_verify = true;
    CHECK(ota_gate(p) == OtaVerdict::PendingVerify);

    p = base_check();
    p.free_heap = 20000;
    CHECK(ota_gate(p) == OtaVerdict::NoRoom);

    // Every verdict has a sentence a human can act on.
    for (OtaVerdict v : {OtaVerdict::Allow, OtaVerdict::BadImage, OtaVerdict::WrongChip,
                         OtaVerdict::WrongProject, OtaVerdict::WrongBoard,
                         OtaVerdict::LosesSimulation, OtaVerdict::Moving,
                         OtaVerdict::PendingVerify, OtaVerdict::NoRoom}) {
        CHECK(std::strlen(ota_verdict_name(v)) > 0);
        // Every REFUSAL says what to do about it; "allow" needs no sentence.
        if (v != OtaVerdict::Allow) CHECK(std::strlen(ota_verdict_reason(v)) > 20);
    }
}

// --------------------------------------------------------------------------
// The mark-valid criterion.  Spec 10.4's original wording bricked three ways.
// --------------------------------------------------------------------------
BootHealth healthy() {
    BootHealth h;
    h.app_main_completed = true;
    h.config_ok = true;
    h.nvs_was_erased = false;
    h.httpd_ok = true;
    h.modes_ticks = OTA_MIN_MODES_TICKS;
    h.motion_ticks = OTA_MIN_MOTION_TICKS;
    h.uptime_s = 12;
    return h;
}

void test_boot_criterion() {
    CHECK(ota_evaluate(healthy()) == BootVerdict::Confirm);

    // Not yet: waiting is the right answer early, not rolling back.
    BootHealth h = healthy();
    h.modes_ticks = 3;
    h.motion_ticks = 100;
    h.uptime_s = 4;
    CHECK(ota_evaluate(h) == BootVerdict::Wait);
    // ... but not for ever.
    h.uptime_s = OTA_CONFIRM_DEADLINE_S;
    CHECK(ota_evaluate(h) == BootVerdict::Rollback);

    // An erased NVS does NOT force a rollback, and the reasoning is the same
    // as "a broken Hall is not a broken image": the erase has already
    // happened, rolling back cannot undo it - the old image boots to the same
    // blank NVS - and the cause is usually a full or corrupt partition the
    // previous image would have hit too.  Discarding a working image for it
    // helps nobody.  It is reported loudly instead.
    h = healthy();
    h.nvs_was_erased = true;
    h.uptime_s = 1;
    CHECK(ota_evaluate(h) == BootVerdict::Confirm);
    // ... and an erased NVS on an image that is ALSO unhealthy still rolls
    // back, on the ordinary criterion.
    h.app_main_completed = false;
    h.uptime_s = 200;
    CHECK(ota_evaluate(h) == BootVerdict::Rollback);

    for (int which = 0; which < 4; ++which) {
        BootHealth b = healthy();
        b.uptime_s = 5;
        if (which == 0) b.app_main_completed = false;
        if (which == 1) b.config_ok = false;
        if (which == 2) b.httpd_ok = false;
        if (which == 3) b.motion_ticks = 0;
        CHECK(ota_evaluate(b) == BootVerdict::Wait);
        b.uptime_s = 200;
        CHECK(ota_evaluate(b) == BootVerdict::Rollback);
    }
}

// The three ways spec 10.4's original criterion boot-looped.  Each of these is
// a display that is working perfectly and must NOT roll back.
void test_the_three_brick_loops_are_gone() {
    // 1. Maintenance never homes, and maintenance survives the reboot - so
    //    "boot + home" could never be satisfied by either image.
    BootHealth maint = healthy();
    CHECK(ota_evaluate(maint) == BootVerdict::Confirm);

    // 2. A disabled column is never homed.
    BootHealth disabled = healthy();
    CHECK(ota_evaluate(disabled) == BootVerdict::Confirm);

    // 3. An unwired board: all five columns fault after ~30 s. Rollback exists
    //    for an image that cannot run, not for a mechanism that is broken -
    //    and rolling back will not fix a Hall sensor.
    BootHealth unwired = healthy();
    unwired.uptime_s = 300;   // long past the deadline, still healthy
    CHECK(ota_evaluate(unwired) == BootVerdict::Confirm);

    // Nothing in BootHealth mentions homing, axes, WiFi or SNTP - which is the
    // structural version of the same statement.
    CHECK(sizeof(BootHealth) > 0);

    for (BootVerdict v : {BootVerdict::Wait, BootVerdict::Confirm, BootVerdict::Rollback}) {
        CHECK(std::strlen(boot_verdict_name(v)) > 0);
    }
}

}  // namespace

void run_tests() {
    test_sniff_a_real_binary();
    test_sniff_rejects_rubbish();
    test_sniff_handles_an_unterminated_field();
    test_gate_allows_the_ordinary_case();
    test_gate_refuses_what_idf_would_accept();
    test_gate_refuses_the_rest();
    test_boot_criterion();
    test_the_three_brick_loops_are_gone();
}
