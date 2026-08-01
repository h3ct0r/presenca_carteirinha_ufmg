#pragma once

// One authorized professor, as listed in config.json's "teachers" array and
// carried around as the logged-in session identity. Each professor may have
// their own password (fallback when the RFID tag isn't available); a session
// copy never keeps it (session_set clears it).
typedef struct {
    char name[48];
    char email[64];
    // Both hold a KEYED FINGERPRINT once loaded, never the card UID or the
    // password itself (app/credential.h). Sized for the stored form, not for
    // what a human types: rfid_uid keeps its 40 (a fingerprint is 24), and
    // password needs "v1:" + 64 hex + NUL. The authoring limits are separate —
    // CONFIG_MAX_PASSWORD_PLAINTEXT in services/config_service.h.
    char rfid_uid[40];
    char password[68];  // empty if the professor has no password fallback
} teacher_t;
