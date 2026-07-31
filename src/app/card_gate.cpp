#include "app/card_gate.h"

#include <string.h>

static constexpr int MAX_UID = 10;  // ISO14443A UIDs are 4, 7 or 10 bytes
// Distinct cards remembered while the field is occupied. Only needs to be big
// enough to notice alternation; a realistic pile-up on the reader is 2-3.
static constexpr int MAX_SEEN = 4;

namespace {

struct Uid {
    uint8_t bytes[MAX_UID];
    uint8_t len;
};

bool uid_eq(const Uid& a, const uint8_t* uid, uint8_t len) {
    return a.len == len && a.len > 0 && memcmp(a.bytes, uid, len) == 0;
}

void uid_set(Uid& a, const uint8_t* uid, uint8_t len) {
    if (len > MAX_UID) len = MAX_UID;
    memcpy(a.bytes, uid, len);
    a.len = len;
}

// The current run of identical polls.
Uid s_run;
int s_run_len = 0;

// The card already reported for this presentation, so holding it still does not
// re-register it.
Uid s_accepted;

// Distinct UIDs seen since the field last went empty, for alternation detection.
Uid s_seen[MAX_SEEN];
int s_seen_count = 0;
bool s_collision_reported = false;

bool seen_contains(const uint8_t* uid, uint8_t len) {
    for (int i = 0; i < s_seen_count; i++) {
        if (uid_eq(s_seen[i], uid, len)) return true;
    }
    return false;
}

void seen_add(const uint8_t* uid, uint8_t len) {
    if (s_seen_count >= MAX_SEEN) return;  // full: alternation was already detected
    uid_set(s_seen[s_seen_count++], uid, len);
}

}  // namespace

void card_gate_reset(void) {
    s_run.len = 0;
    s_run_len = 0;
    s_accepted.len = 0;
    s_seen_count = 0;
    s_collision_reported = false;
}

card_gate_action_t card_gate_poll(const uint8_t* uid, uint8_t uid_len, uint8_t* out_uid,
                                  uint8_t* out_uid_len) {
    // Empty field: the reader is clear, so the next card starts fresh and a
    // card that was just lifted may be presented again.
    if (!uid || uid_len == 0) {
        card_gate_reset();
        return CARD_GATE_IGNORE;
    }

    const bool continues_run = uid_eq(s_run, uid, uid_len);
    if (continues_run) {
        if (s_run_len < CARD_GATE_CONFIRM_POLLS) s_run_len++;
    } else {
        // The run broke. If this UID was already seen while the field has been
        // occupied, the reader is flipping between cards — more than one is on
        // it. Report that once per presentation.
        const bool alternating = seen_contains(uid, uid_len);
        uid_set(s_run, uid, uid_len);
        s_run_len = 1;
        if (!alternating) {
            seen_add(uid, uid_len);
        } else if (!s_collision_reported) {
            s_collision_reported = true;
            return CARD_GATE_COLLISION;
        }
    }

    // Not confirmed yet, or this card was already registered and is just still
    // sitting on the reader.
    if (s_run_len < CARD_GATE_CONFIRM_POLLS) return CARD_GATE_IGNORE;
    if (uid_eq(s_accepted, uid, uid_len)) return CARD_GATE_IGNORE;

    // Once several cards have been seen, accept nothing until the field clears:
    // the professor must present one card on its own.
    if (s_collision_reported) return CARD_GATE_IGNORE;

    uid_set(s_accepted, uid, uid_len);
    if (out_uid && out_uid_len) {
        memcpy(out_uid, s_accepted.bytes, s_accepted.len);
        *out_uid_len = s_accepted.len;
    }
    return CARD_GATE_ACCEPT;
}
