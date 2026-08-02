#pragma once

#include "ui/screen.h"

// Staff: the global student registry (/students/students.json), scoped to one
// class. Reached from the class enroll view, and the way a student who was never
// in the authored roster gets into a class on the device:
//
//   - search every student on the card, not just this class's roster, and add
//     one who is already registered (typically from another class);
//   - or create a new registry entry from a form.
//
// Neither path needs the student's card: the entry is stored with no rfid_uid,
// exactly like an imported one, and binds itself on their first tap. The
// card-in-hand flow is still the enroll view's "Add new student".
//
// on_show arg: const class_rec_t* from roster_service (its static storage, so
// the pointer stays valid while shown). Pass NULL to re-show the last class.
extern const screen_t scr_students;
