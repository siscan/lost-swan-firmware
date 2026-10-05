#include "webapi/ota_policy.h"

#include <cstring>

namespace swan {
namespace api {
namespace {

constexpr std::size_t IMAGE_HDR = 24;
constexpr std::size_t SEGMENT_HDR = 8;
constexpr std::size_t DESC_OFF = IMAGE_HDR + SEGMENT_HDR;
constexpr uint8_t IMAGE_MAGIC = 0xE9;
constexpr uint32_t DESC_MAGIC = 0xABCD5432u;

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// A fixed-width char array in esp_app_desc_t: NUL-terminated when it fits, and
// NOT terminated when the string exactly fills the field.
std::string field(const uint8_t* p, std::size_t max) {
    std::size_t n = 0;
    while (n < max && p[n] != '\0') ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

}  // namespace

ImageInfo sniff_image(const uint8_t* data, std::size_t len) {
    ImageInfo info;
    if (data == nullptr || len < OTA_HEADER_BYTES) return info;
    if (data[0] != IMAGE_MAGIC) return info;

    info.chip_id = rd16(data + 12);
    info.min_chip_rev_full = rd16(data + 15);
    info.max_chip_rev_full = rd16(data + 17);

    const uint8_t* d = data + DESC_OFF;
    if (rd32(d) != DESC_MAGIC) return info;

    // esp_app_desc_t, field by field, because getting this wrong reads a
    // neighbouring string and looks plausible:
    //   0  magic_word(4)  4 secure_version(4)  8 reserv1(8)
    //   16 version(32)   48 project_name(32)  80 time(16)  96 date(16)
    //   112 idf_ver(32)
    info.version = field(d + 16, 32);
    info.project_name = field(d + 48, 32);
    info.idf_ver = field(d + 112, 32);
    info.parsed = true;

    // The board map and the sim/release flavour live in the version tag,
    // because esp_app_desc_t has nowhere else to put them:
    //     0.4.0+devkitc1.sim            0.4.0+devkitc1.sim.unlimited
    // The flavour is the FIRST segment after the board; anything after it is a
    // modifier, and "unlimited" is the one that exists - the image lifts the
    // dispatcher's ranges (motion/limits_policy.h).
    const std::size_t plus = info.version.find('+');
    if (plus != std::string::npos) {
        const std::string tag = info.version.substr(plus + 1);
        const std::size_t dot = tag.find('.');
        if (dot == std::string::npos) {
            info.board = tag;
        } else {
            info.board = tag.substr(0, dot);
            const std::string rest = tag.substr(dot + 1);
            const std::size_t next = rest.find('.');
            info.flavour = rest.substr(0, next);
            // Whole segments only: "unlimited" must not be found inside some
            // future word that merely contains it.
            for (std::size_t at = next; at != std::string::npos;) {
                const std::size_t from = at + 1;
                const std::size_t end = rest.find('.', from);
                if (rest.compare(from, end == std::string::npos ? std::string::npos : end - from,
                                 "unlimited") == 0) {
                    info.unlimited = true;
                }
                at = end;
            }
        }
    }
    return info;
}

const char* ota_verdict_name(OtaVerdict v) {
    switch (v) {
        case OtaVerdict::Allow:           return "allow";
        case OtaVerdict::BadImage:        return "bad_image";
        case OtaVerdict::WrongChip:       return "wrong_chip";
        case OtaVerdict::WrongProject:    return "wrong_project";
        case OtaVerdict::WrongBoard:      return "wrong_board";
        case OtaVerdict::LosesSimulation: return "loses_simulation";
        case OtaVerdict::GainsUnlimited:  return "gains_unlimited";
        case OtaVerdict::Moving:          return "moving";
        case OtaVerdict::PendingVerify:   return "pending_verify";
        case OtaVerdict::NoRoom:          return "no_room";
    }
    return "?";
}

const char* ota_verdict_reason(OtaVerdict v) {
    switch (v) {
        case OtaVerdict::Allow:
            return "ok";
        case OtaVerdict::BadImage:
            return "not an ESP-IDF application image";
        case OtaVerdict::WrongChip:
            return "built for a different chip";
        case OtaVerdict::WrongProject:
            return "a different firmware entirely";
        case OtaVerdict::WrongBoard:
            return "built for the other board's pin map - it would drive STEP on the "
                   "wrong GPIOs. Send {\"force\":true} if you mean it";
        case OtaVerdict::LosesSimulation:
            return "a release image cannot simulate, and this board has simulated columns "
                   "saved: they would come back as real, fault unwired, and drop EN, with no "
                   "way to set them back. Send {\"force\":true} if you mean it";
        case OtaVerdict::GainsUnlimited:
            return "this image lifts the speed, accel and hall_tol ranges, and the one "
                   "running does not: it is a different safety contract, and nothing on "
                   "the wall would say so afterwards but the version tag. Send "
                   "{\"force\":true} if you mean it";
        case OtaVerdict::Moving:
            return "a column is still moving; wait for it to settle or enter maintenance";
        case OtaVerdict::PendingVerify:
            return "the running image has not confirmed itself yet - wait a few seconds";
        case OtaVerdict::NoRoom:
            return "not enough free heap to run an update safely";
    }
    return "?";
}

OtaVerdict ota_gate(const OtaPrecheck& p) {
    if (!p.image.parsed) return OtaVerdict::BadImage;
    if (p.image.chip_id != OTA_CHIP_ID_ESP32C5) return OtaVerdict::WrongChip;
    if (!p.running_project.empty() && p.image.project_name != p.running_project) {
        return OtaVerdict::WrongProject;
    }
    // esp_ota_begin refuses outright while the running image is PENDING_VERIFY,
    // so say why rather than surfacing a generic failure.
    if (p.running_pending_verify) return OtaVerdict::PendingVerify;

    // Motion is held for the whole write (spec 10.4).  The step ISR is
    // IRAM-safe and keeps firing, but the flash-resident 1 kHz control task
    // stalls on every sector erase - so a moving axis would keep stepping at
    // its last published velocity with nobody able to decelerate it, re-base on
    // a Hall edge, or notice an overdue one.
    if (!p.all_axes_idle && !p.maintenance) return OtaVerdict::Moving;

    if (p.free_heap != 0 && p.free_heap < 40000) return OtaVerdict::NoRoom;

    if (!p.force) {
        // The two refusals only a look at the version tag can make.  IDF's own
        // checks - magic, SHA256, chip id, chip revision - all PASS for both.
        if (!p.image.board.empty() && !p.running_board.empty() &&
            p.image.board != p.running_board) {
            return OtaVerdict::WrongBoard;
        }
        // Surviving state the new image cannot honour is worse than state that
        // did not survive: col_mode persists perfectly, five Sim columns are
        // restored, and a release build compiles the Sim case to nothing - so
        // all five neither drive nor simulate, fault, and escalation drops EN.
        // motion.column then refuses to set sim back, so the UI cannot fix it.
        if (p.any_simulated_column && p.image.flavour == "rel") {
            return OtaVerdict::LosesSimulation;
        }
        // THE OTHER DIRECTION OF THE SAME IDEA.  An unlimited image is a different
        // SAFETY CONTRACT (motion/limits_policy.h): its dispatcher accepts speeds,
        // accel and a hall_tol that a normal one refuses, so putting one onto a
        // board that is running a normal image is a decision, and the wrong file
        // picked on the Update page must not make it.  Same reason the bench cap is
        // in the version tag.  The REVERSE - a normal image onto an unlimited one -
        // is the safe direction and is never refused: NVS holds only normal-range
        // values in every flavour, so there is nothing the new image cannot honour.
        if (p.image.unlimited && !p.running_unlimited) {
            return OtaVerdict::GainsUnlimited;
        }
    }
    return OtaVerdict::Allow;
}

const char* boot_verdict_name(BootVerdict v) {
    switch (v) {
        case BootVerdict::Wait:     return "wait";
        case BootVerdict::Confirm:  return "confirm";
        case BootVerdict::Rollback: return "rollback";
    }
    return "?";
}

BootVerdict ota_evaluate(const BootHealth& h) {
    // An erased NVS used to force an immediate rollback.  That was wrong twice
    // over: the erase has ALREADY happened, so rolling back cannot undo it -
    // the old image boots to the same blank NVS - and it makes an unrelated
    // failure (a full or corrupt partition, which the previous image would
    // have hit too) look like a bad update, so the good image gets discarded
    // for nothing.
    //
    // It is still a strong signal that the settings are gone, and it is
    // REPORTED loudly, but confirming is the safer answer: an image that runs
    // is worth more than an image that does not, and the erase is a
    // configuration problem, not an image problem.  Same reasoning as "a
    // broken Hall is not a broken image".
    const bool healthy = h.app_main_completed && h.config_ok && h.httpd_ok &&
                         h.modes_ticks >= OTA_MIN_MODES_TICKS &&
                         h.motion_ticks >= OTA_MIN_MOTION_TICKS;
    if (healthy) return BootVerdict::Confirm;
    if (h.uptime_s >= OTA_CONFIRM_DEADLINE_S) return BootVerdict::Rollback;
    return BootVerdict::Wait;
}

}  // namespace api
}  // namespace swan
