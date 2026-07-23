#include "app/auth.h"

#include "services/config_service.h"

// Access control is a thin front for the config lookups. Those iterate the
// loaded config under its own lock and copy out only the matched teacher, so
// nothing here puts a whole device_config_t on the (LVGL-thread) stack.

bool auth_lookup_uid(const char* uid_hex, teacher_t* out) {
    return config_find_teacher_by_uid(uid_hex, out);
}

bool auth_lookup_password(const char* password, teacher_t* out) {
    return config_find_teacher_by_password(password, out);
}
