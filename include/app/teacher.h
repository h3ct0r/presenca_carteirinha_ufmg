#pragma once

// One authorized professor, as listed in config.json's "teachers" array and
// carried around as the logged-in session identity. Each professor may have
// their own password (fallback when the RFID tag isn't available); a session
// copy never keeps it (session_set clears it).
typedef struct {
    char name[48];
    char email[64];
    char rfid_uid[40];
    char password[32];  // empty if the professor has no password fallback
} teacher_t;
