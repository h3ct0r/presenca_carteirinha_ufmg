#include "app/session.h"

#include <string.h>

static teacher_t s_teacher;
static bool s_active = false;

void session_set(const teacher_t* t) {
    if (t) {
        s_teacher = *t;
        // The session is an identity, not a credential store: don't keep the
        // professor's password around after login.
        memset(s_teacher.password, 0, sizeof(s_teacher.password));
        s_active = true;
    } else {
        memset(&s_teacher, 0, sizeof(s_teacher));
        s_active = false;
    }
}

const teacher_t* session_get(void) { return s_active ? &s_teacher : nullptr; }

bool session_active(void) { return s_active; }
