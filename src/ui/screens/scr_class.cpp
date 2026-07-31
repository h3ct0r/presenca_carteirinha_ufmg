#include "ui/screens/scr_class.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app/auth.h"
#include "app/class_stats.h"
#include "app/uid.h"
#include "audio/beeper.h"
#include "esp32-hal-log.h"
#include "esp_timer.h"
#include "services/roster_service.h"
#include "storage/attendance_store.h"
#include "ui/components/keyboard.h"
#include "ui/components/shell.h"
#include "ui/components/student_photo.h"
#include "ui/components/toast.h"
#include "ui/screen_manager.h"
#include "ui/sd_resync.h"
#include "ui/theme/theme.h"
#include "ui/ui.h"

// FontAwesome glyph merged into the custom Montserrat font (see
// src/ui/assets/), used as UTF-8.
LV_FONT_DECLARE(font_montserrat_custom_20);
#define GLYPH_KIOSK "\xEE\x96\x95"  // U+E595 fa-users-viewfinder

// The class screen is a small view stack: a hub (Session / History), the open
// session (roll call + Kiosk / Enroll), history, and the enroll sub-flow.
typedef enum { VIEW_HUB,
               VIEW_SESSION,
               VIEW_HISTORY,
               VIEW_ENROLL } view_id_t;
typedef enum { ENROLL_SEARCH,
               ENROLL_MANUAL,
               ENROLL_WAIT } enroll_state_t;

static constexpr int MAX_DATES = 24;  // recent/history dates listed

static shell_t s_sh;
static lv_obj_t* s_content = nullptr;  // rebuilt on view switch

static const class_rec_t* s_cls = nullptr;
static view_id_t s_view = VIEW_HUB;
static bool s_pending_session_view = false;  // set by scr_class_request_session_view()
static lv_obj_t* s_session_badge = nullptr;  // header "session open" chip

// Session/date-picker state.
static char s_sel_date[12] = "";     // date chosen in the calendar
static char s_dates[MAX_DATES][12];  // listed past dates
static int s_dates_count = 0;
static lv_calendar_date_t s_highlight;  // must outlive the calendar

// Enroll sub-flow state.
// Cap on rows DRAWN in the enroll search (matches are still counted in full and
// reported). Each row costs 3 LVGL objects from the fixed LVGL pool, so this
// bounds the worst case regardless of class size; the header tells the user to
// narrow the search when it bites.
static constexpr int ENROLL_MAX_ROWS = 15;
static enroll_state_t s_enroll_state = ENROLL_SEARCH;
static bool s_unregistered_only = true;  // toggle: only students without a card
static lv_obj_t* s_search_ta = nullptr;
static lv_obj_t* s_results = nullptr;
static lv_obj_t* s_manual_id_ta = nullptr;
static lv_obj_t* s_manual_name_ta = nullptr;
static lv_obj_t* s_manual_turma_ta = nullptr;
static int s_sel_idx = -1;         // selected existing student (registry index)
static bool s_sel_is_new = false;  // selecting a manually-entered new student
static char s_new_id[20];
static char s_new_name[48];
static char s_new_turma[16];  // optional class-group tag (class.json only)

// Roll-call search (open-session view), shaped like the enroll one: every match
// is counted, but at most ROLL_MAX_ROWS chips are built. A chip costs 5 objects
// from the fixed LVGL pool, and the whole list used to be rebuilt on
// every card tap and every name tap — which is what made a big class crawl.
static constexpr int ROLL_MAX_ROWS = 10;
static lv_obj_t* s_roll_search_ta = nullptr;
static lv_obj_t* s_roll_list = nullptr;   // holds only the chips; rebuilt alone
static lv_obj_t* s_roll_count = nullptr;  // "N/M present" in the summary header
static lv_obj_t* s_roll_bar = nullptr;    // present-progress bar

// Transient "presence registered" feedback overlay (photo + name + status),
// shown on each session card tap and auto-dismissed.
static lv_obj_t* s_fb_overlay = nullptr;
static lv_timer_t* s_fb_timer = nullptr;

// EVERY static pointing into s_content, dropped together before it is cleaned.
// Half of these used to be cleared and half not, which is exactly how a stale
// pointer survives a refactor.
static void forget_view_widgets(void);
static void rebuild_content(void);
static void update_header(void);
static void apply_keyboard_pad(void);
static void enroll_goto(enroll_state_t st);
static void update_results(void);
static void on_enroll_card(const char* uid_hex);
static void build_session(void);
static void build_session_open(void);
static void update_roll_call(void);
static bool istr_has(const char* hay, const char* needle);
static void on_session_card(const char* uid_hex);
static void build_hub(void);
static void go_view(view_id_t v);
static void kiosk_cb(lv_event_t*);
static void enroll_btn_cb(lv_event_t*);

// Back steps up the view stack: enroll -> session, session/history -> hub, and
// from the hub out to the classes list.
static void back_cb(lv_event_t*) {
    switch (s_view) {
        case VIEW_ENROLL:
            go_view(VIEW_SESSION);
            break;
        case VIEW_SESSION:
        case VIEW_HISTORY:
            go_view(VIEW_HUB);
            break;
        default:
            scr_mgr_show(SCREEN_CLASSES, nullptr);
            break;
    }
}

// --- Session view: date picker + roll call ----------------------------------

// This student's turma in the current class (by registry index), or "".
static const char* class_turma_for_student(int student_idx) {
    if (!s_cls) return "";
    for (int j = 0; j < s_cls->roster_count; j++) {
        if (s_cls->roster[j] == student_idx) return s_cls->roster_turma[j];
    }
    return "";
}

static int roster_student_by_uid(const char* uid_hex) {
    char norm[32];
    uid_normalize(uid_hex, norm, sizeof(norm));
    if (!norm[0]) return -1;
    for (int j = 0; j < s_cls->roster_count; j++) {
        int idx = s_cls->roster[j];
        const student_t* st = roster_student_at(idx);
        if (!st || !st->rfid_uid[0]) continue;
        char sn[32];
        uid_normalize(st->rfid_uid, sn, sizeof(sn));
        if (strcmp(norm, sn) == 0) return idx;
    }
    return -1;
}

// Tears down the transient presence-feedback overlay and frees the photo it
// decoded. Safe to call when nothing is showing.
static void close_presence_feedback(void) {
    if (s_fb_timer) {
        lv_timer_delete(s_fb_timer);
        s_fb_timer = nullptr;
    }
    if (s_fb_overlay) {
        lv_obj_delete(s_fb_overlay);  // LVGL frees the decoded image cache itself
        s_fb_overlay = nullptr;
    }
}

static void fb_timer_cb(lv_timer_t*) {
    // One-shot timer: LVGL deletes it after this callback returns, so drop our
    // handle first — close_presence_feedback() must not delete it a second time.
    s_fb_timer = nullptr;
    close_presence_feedback();
}
static void fb_dismiss_cb(lv_event_t*) { close_presence_feedback(); }

// Shows a full-screen feedback card for a card tap: the student's photo (if one
// exists on the SD card, otherwise a placeholder), their name/id, and a colored
// status line. `st` is NULL for an unrecognized card. Auto-dismisses; a tap or
// the next scan closes it early. The photo is shown whether or not the
// attendance write succeeded — the caller picks the color/text accordingly.
static void show_presence_feedback(const student_t* st, const char* turma, uint32_t color,
                                   const char* status_icon, const char* status_text) {
    close_presence_feedback();  // replace any prior overlay (and free its photo)

    // Parented to the screen root, not s_content, so the roll-call rebuild that
    // follows a scan doesn't delete it.
    s_fb_overlay = lv_obj_create(s_sh.root);
    lv_obj_remove_style_all(s_fb_overlay);
    lv_obj_add_flag(s_fb_overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_fb_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_fb_overlay, lv_color_hex(THEME_SCRIM), 0);
    lv_obj_set_style_bg_opa(s_fb_overlay, LV_OPA_40, 0);
    lv_obj_add_flag(s_fb_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_fb_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_fb_overlay, fb_dismiss_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = ui_make_card(s_fb_overlay);
    lv_obj_set_width(card, LV_PCT(80));
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(card, 3, 0);

    // Photo when one exists on the card, fitted to AVATAR_MAX_PX with its
    // aspect ratio kept; a neutral placeholder avatar otherwise.
    if (!st || !student_photo_image(card, st->id, AVATAR_MAX_PX)) {
        const int SZ = 120;
        lv_obj_t* ph = lv_obj_create(card);
        lv_obj_remove_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(ph, SZ, SZ);
        lv_obj_set_style_radius(ph, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ph, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(ph, LV_OPA_20, 0);
        lv_obj_set_style_border_width(ph, 0, 0);
        lv_obj_center(ui_make_label(ph, LV_SYMBOL_IMAGE, color, &lv_font_montserrat_32));
    }

    if (st) {
        ui_make_label(card, st->name, THEME_TEXT, &lv_font_montserrat_20);
        char idline[40];
        snprintf(idline, sizeof(idline), "ID %s", st->id);
        ui_make_label(card, idline, THEME_MUTED, &lv_font_montserrat_14);
        if (turma && turma[0]) {
            char tline[32];
            snprintf(tline, sizeof(tline), "Turma %s", turma);
            ui_make_label(card, tline, THEME_PRIMARY, &lv_font_montserrat_20);
        }
    }

    char status[80];
    snprintf(status, sizeof(status), "%s  %s", status_icon, status_text);
    ui_make_label(card, status, color, &lv_font_montserrat_20);

    s_fb_timer = lv_timer_create(fb_timer_cb, 2600, nullptr);
    lv_timer_set_repeat_count(s_fb_timer, 1);
}

// Registers a recognized student (timed or single) and shows the feedback
// overlay. Extracted so it runs directly (no capture) or after a face-verify.
static void register_recognized(int idx) {
    const student_t* st = roster_student_at(idx);
    if (!st) return;
    const char* turma = class_turma_for_student(idx);
    if (s_cls->timed_attendance) {
        att_state_t s = attendance_tap(st->id, esp_timer_get_time(), s_cls->min_attendance_min);
        uint32_t color = THEME_ACCENT;
        const char* icon = LV_SYMBOL_OK;
        char text[56];
        switch (s.status) {
            case ATT_PRESENT:
                color = THEME_SUCCESS;
                snprintf(text, sizeof(text), "Presence registered - %d min", s.minutes);
                beeper_beep();
                break;
            case ATT_ALREADY_PRESENT:
                color = THEME_SUCCESS;
                snprintf(text, sizeof(text), "Already registered - %d min", s.minutes);
                beeper_beep();
                break;
            case ATT_TOO_EARLY:
                color = THEME_WARNING;
                icon = LV_SYMBOL_WARNING;
                snprintf(text, sizeof(text), "Too soon - %d min left", s.remaining);
                beeper_error();
                break;
            case ATT_IN_PROGRESS:
            default:
                snprintf(text, sizeof(text), "Checked in - tap again in %d min", s.remaining);
                beeper_beep();
                break;
        }
        show_presence_feedback(st, turma, color, icon, text);
    } else {
        bool ok = attendance_set(st->id, true);
        if (ok) {
            beeper_beep();
        } else {
            beeper_error();
        }
        show_presence_feedback(st, turma, ok ? THEME_SUCCESS : THEME_WARNING,
                               ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                               ok ? "Presence registered" : "Marked (save failed)");
    }
}

static void on_session_card(const char* uid_hex) {
    int idx = roster_student_by_uid(uid_hex);
    if (idx < 0) {
        beeper_error();
        show_presence_feedback(nullptr, nullptr, THEME_DANGER, LV_SYMBOL_CLOSE,
                               "Card not recognized");
    } else {
        // Face-verified capture is kiosk-only; a roll-call tap registers directly.
        register_recognized(idx);
    }
    update_roll_call();                    // only the chips changed
    ui_set_card_capture(on_session_card);  // the capture is one-shot; re-arm it
}

static void roll_toggle_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const student_t* st = roster_student_at(idx);
    if (!st) return;
    attendance_set(st->id, !attendance_is_present(st->id));
    update_roll_call();
}

static void roll_search_changed_cb(lv_event_t*) { update_roll_call(); }

static void close_session_cb(lv_event_t*) {
    attendance_close();
    rebuild_content();  // back to the picker
}

// Orders two registry indices alphabetically by student name (case-insensitive)
// for the roll-call list.
static int roster_name_cmp(const void* a, const void* b) {
    const student_t* sa = roster_student_at(*(const int*)a);
    const student_t* sb = roster_student_at(*(const int*)b);
    return strcasecmp(sa ? sa->name : "", sb ? sb->name : "");
}

static void build_session_open(void) {
    int total = s_cls->roster_count;
    int present = attendance_present_count();

    lv_obj_t* summary = ui_make_card(s_content);
    lv_obj_set_flex_flow(summary, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(summary, 8, 0);

    lv_obj_t* head = lv_obj_create(summary);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    ui_make_label(head, attendance_date(), THEME_PRIMARY, &lv_font_montserrat_20);
    char count_txt[32];
    snprintf(count_txt, sizeof(count_txt), "%d/%d present", present, total);
    // Kept: update_roll_call() retargets these two on every tap instead of
    // rebuilding the card they sit in.
    s_roll_count = ui_make_label(head, count_txt, THEME_SUCCESS, &lv_font_montserrat_14);

    s_roll_bar = lv_bar_create(summary);
    lv_obj_t* bar = s_roll_bar;
    lv_obj_set_size(bar, LV_PCT(100), 8);
    lv_bar_set_range(bar, 0, total > 0 ? total : 1);
    lv_bar_set_value(bar, present, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_SUCCESS), LV_PART_INDICATOR);

    // Read here rather than on screen entry: this summary is the only place that
    // shows it, and the hub the class list lands on must not pay for SD I/O.
    // Card scans rebuild only the chips (update_roll_call), not this view.
    char photo_txt[48];
    snprintf(photo_txt, sizeof(photo_txt), LV_SYMBOL_IMAGE "  %d/%d with photo",
             student_photo_count_for_class(s_cls), total);
    ui_make_label(summary, photo_txt, THEME_MUTED, &lv_font_montserrat_14);

    char turma_txt[160];
    if (class_turma_breakdown(s_cls, turma_txt, sizeof(turma_txt))) {
        char line[192];
        snprintf(line, sizeof(line), LV_SYMBOL_LIST "  Turmas:  %s", turma_txt);
        lv_obj_t* tl = ui_make_label(summary, line, THEME_MUTED, &lv_font_montserrat_14);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(tl, LV_PCT(100));
    }

    lv_obj_t* close = ui_make_button(summary, "Close session", &theme_style_btn_danger,
                                     close_session_cb, nullptr);
    lv_obj_set_width(close, LV_PCT(100));

    // Kiosk (unattended check-in) and Enroll, available during the open session.
    // No password: the professor is already logged in, and kiosk's own exit gate
    // covers the unattended case.
    lv_obj_t* actions = lv_obj_create(s_content);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 10, 0);
    lv_obj_remove_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* kiosk = ui_make_button(actions, GLYPH_KIOSK "  Kiosk", &theme_style_btn_outline,
                                     kiosk_cb, nullptr);
    lv_obj_set_style_text_font(lv_obj_get_child(kiosk, 0), &font_montserrat_custom_20, 0);
    lv_obj_set_flex_grow(kiosk, 1);
    lv_obj_t* enroll = ui_make_button(actions, "Enroll", &theme_style_btn_outline, enroll_btn_cb,
                                      nullptr);
    lv_obj_set_flex_grow(enroll, 1);

    if (total == 0) {
        lv_obj_t* card = ui_make_card(s_content);
        lv_obj_center(ui_make_label(card, "No students enrolled in this class yet.",
                                    THEME_MUTED, &lv_font_montserrat_14));
    }

    // Instructional callout — deliberately NOT button-shaped: soft orange fill
    // with an amber left rule and alert icon so it reads as a notice, not a tap
    // target. Icon + text sit in a left-aligned row.
    lv_obj_t* banner = ui_make_card(s_content);
    lv_obj_set_style_bg_color(banner, lv_color_hex(THEME_WARNING_SOFT), 0);
    lv_obj_set_style_border_color(banner, lv_color_hex(THEME_WARNING), 0);
    lv_obj_set_style_border_width(banner, 5, 0);
    lv_obj_set_style_border_side(banner, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_flex_flow(banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(banner, 12, 0);
    ui_make_label(banner, LV_SYMBOL_WARNING, THEME_WARNING, &lv_font_montserrat_20);
    ui_make_label(banner, "Class open: Tap a card, or tap a name to log it.", THEME_TEXT, &lv_font_montserrat_14);

    // The search box: filtering is what keeps this view responsive on a large
    // class, since only the matches are drawn.
    if (total > 0) {
        lv_obj_t* search = ui_make_card(s_content);
        lv_obj_set_flex_flow(search, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(search, 8, 0);
        s_roll_search_ta = keyboard_make_textarea(search, "Search by name or ID", 47,
                                                  LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_add_event_cb(s_roll_search_ta, roll_search_changed_cb, LV_EVENT_VALUE_CHANGED,
                            nullptr);
    }

    // Chips live in their own container so a tap can rebuild the list alone,
    // leaving the summary, the buttons and the search box (and the keyboard's
    // textarea) untouched.
    s_roll_list = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_roll_list);
    lv_obj_set_width(s_roll_list, LV_PCT(100));
    lv_obj_set_height(s_roll_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_roll_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_roll_list, 10, 0);             // matches s_content's row gap
    lv_obj_remove_flag(s_roll_list, LV_OBJ_FLAG_SCROLLABLE);  // sh.body scrolls

    update_roll_call();
    ui_set_card_capture(on_session_card);
}

// The roll call is the heaviest LVGL screen in the firmware — ROLL_MAX_ROWS
// chips at ~5 objects each, rebuilt from scratch on every tap — so its peak is
// what LV_MEM_SIZE has to be sized for. The boot-time figure in main.cpp is
// measured before any of this exists and says nothing about the worst case.
//
// Logged only when a new high-water mark is reached, so a session produces a
// handful of lines rather than one per tap.
static void log_pool_peak(int chips) {
    static uint32_t s_peak_used = 0;
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    const uint32_t used = (uint32_t)(m.total_size - m.free_size);
    if (used <= s_peak_used) return;
    s_peak_used = used;
    ESP_LOGI("scr_class", "LVGL pool peak: %u/%u B used (%u%%), largest free %u, %d chips",
             (unsigned)used, (unsigned)m.total_size, (unsigned)m.used_pct,
             (unsigned)m.free_biggest_size, chips);
}

// Rebuilds ONLY the roll-call chips and the two summary widgets that track the
// present count. Called on every tap in place of the old whole-view rebuild.
static void update_roll_call(void) {
    if (!s_roll_list) return;
    lv_obj_clean(s_roll_list);

    const int total = s_cls->roster_count;
    const int present = attendance_present_count();
    if (s_roll_count) {
        char count_txt[32];
        snprintf(count_txt, sizeof(count_txt), "%d/%d present", present, total);
        lv_label_set_text(s_roll_count, count_txt);
    }
    if (s_roll_bar) lv_bar_set_value(s_roll_bar, present, LV_ANIM_OFF);

    const char* q = s_roll_search_ta ? lv_textarea_get_text(s_roll_search_ta) : "";

    // Render the roll call alphabetically by name, not roster/registry order.
    int order[ROSTER_MAX_CLASS_STUDENTS];
    for (int j = 0; j < total; j++) order[j] = s_cls->roster[j];
    qsort(order, total, sizeof(order[0]), roster_name_cmp);

    int matched = 0;  // all matches
    int shown = 0;    // chips actually built
    for (int j = 0; j < total; j++) {
        int idx = order[j];
        const student_t* st = roster_student_at(idx);
        if (!st) continue;
        if (q[0] && !istr_has(st->name, q) && !istr_has(st->id, q)) continue;
        matched++;
        if (shown >= ROLL_MAX_ROWS) continue;  // counted, not drawn
        shown++;
        bool here = attendance_is_present(st->id);

        // In timed mode a student may be waiting out the threshold without being
        // present yet; reflect that with an amber chip and a countdown subtitle.
        uint32_t bg = here ? THEME_SUCCESS_SOFT : THEME_SURFACE;
        uint32_t edge = here ? THEME_SUCCESS : THEME_BORDER;
        const char* trail_icon = here ? LV_SYMBOL_OK : "";
        uint32_t trail_color = here ? THEME_SUCCESS : THEME_MUTED;
        char sub[40] = "";
        if (s_cls->timed_attendance) {
            att_state_t s = attendance_tap_state(st->id, esp_timer_get_time(),
                                                 s_cls->min_attendance_min);
            if (s.status == ATT_IN_PROGRESS) {
                bg = THEME_WARNING_SOFT;
                edge = THEME_WARNING;
                trail_icon = LV_SYMBOL_REFRESH;
                trail_color = THEME_WARNING;
                if (s.remaining > 0) {
                    snprintf(sub, sizeof(sub), "in class %d min - %d to go", s.minutes,
                             s.remaining);
                } else {
                    snprintf(sub, sizeof(sub), "in class %d min - can confirm", s.minutes);
                }
            } else if (s.status == ATT_PRESENT) {
                snprintf(sub, sizeof(sub), "present %d min", s.minutes);
            }
        }

        lv_obj_t* chip = ui_make_card(s_roll_list);
        lv_obj_set_style_bg_color(chip, lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(chip, lv_color_hex(edge), 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chip, roll_toggle_cb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
        ui_add_press_feedback(chip);

        lv_obj_t* col = lv_obj_create(chip);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 2, 0);
        // 16, not the 14 used elsewhere: the roll call is read at arm's length
        // while the professor works down the class.
        ui_make_label(col, st->name, THEME_TEXT, &lv_font_montserrat_16);
        ui_make_label(col, sub[0] ? sub : st->id, here ? THEME_SUCCESS : THEME_MUTED,
                      &lv_font_montserrat_16);

        ui_make_label(chip, trail_icon, trail_color, &lv_font_montserrat_20);
    }

    if (matched == 0 && q[0]) {
        lv_obj_t* card = ui_make_card(s_roll_list);
        lv_obj_t* msg = ui_make_label(card, "No matching student in this class", THEME_MUTED,
                                      &lv_font_montserrat_14);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(msg, LV_PCT(100));
        return;
    }

    // Say plainly when the list is cut short — a student missing from the roll
    // call would otherwise look like they are not enrolled. Built last (that is
    // where the counts are known) and moved above the chips.
    bool truncated = matched > shown;
    if (!truncated && !q[0]) return;  // full list, no query: the summary says it all
    char head[96];
    if (truncated) {
        snprintf(head, sizeof(head), "Showing %d of %d - search to narrow the list", shown,
                 matched);
    } else {
        snprintf(head, sizeof(head), "%d match%s", matched, matched == 1 ? "" : "es");
    }
    lv_obj_t* count = ui_make_label(s_roll_list, head, truncated ? THEME_WARNING : THEME_MUTED,
                                    &lv_font_montserrat_14);
    lv_label_set_long_mode(count, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(count, LV_PCT(100));
    lv_obj_move_to_index(count, 0);

    log_pool_peak(shown);
}

static void cal_changed_cb(lv_event_t* e) {
    // The day-click event bubbles up from the calendar's internal button
    // matrix, so get_target() would be that matrix; get_current_target() is
    // the calendar the handler is bound to (required by get_pressed_date).
    lv_obj_t* cal = (lv_obj_t*)lv_event_get_current_target(e);
    lv_calendar_date_t d;
    if (lv_calendar_get_pressed_date(cal, &d) == LV_RESULT_OK) {
        snprintf(s_sel_date, sizeof(s_sel_date), "%04d-%02d-%02d", d.year, d.month, d.day);
        // Move the highlight box to the tapped day so the selection is visible.
        s_highlight = d;
        lv_calendar_set_highlighted_dates(cal, &s_highlight, 1);
    }
}

static void open_selected_cb(lv_event_t*) {
    if (!s_sel_date[0]) return;
    attendance_open(s_cls->dir, s_sel_date);
    rebuild_content();  // -> open roll call
}

// Opens one of the listed dates (recent list or history) and shows it.
static void open_date_cb(lv_event_t* e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_dates_count) return;
    attendance_open(s_cls->dir, s_dates[i]);
    go_view(VIEW_SESSION);  // a tapped history/recent date opens into the session
}

// One tappable "date — N/total present" row. With show_pct, the attendance
// percentage for that session is appended (used on the history view).
static void add_date_row(lv_obj_t* parent, int i, bool show_pct) {
    int present = attendance_present_for(s_cls->dir, s_dates[i]);
    int total = s_cls->roster_count;

    lv_obj_t* row = ui_make_card(parent);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, open_date_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    ui_add_press_feedback(row);

    ui_make_label(row, s_dates[i], THEME_TEXT, &lv_font_montserrat_14);
    char sub[40];
    if (show_pct) {
        int pct = total > 0 ? (present * 100 + total / 2) / total : 0;
        snprintf(sub, sizeof(sub), "%d/%d present  %d%%", present, total, pct);
    } else {
        snprintf(sub, sizeof(sub), "%d/%d present", present, total);
    }
    ui_make_label(row, sub, THEME_MUTED, &lv_font_montserrat_14);
}

static void build_session_closed(void) {
    s_dates_count = attendance_list_dates(s_cls->dir, s_dates, MAX_DATES);

    // Placeholder = last session date, so the picker opens on the right month.
    int y = 2026, m = 1, d = 1;
    if (s_dates_count > 0) sscanf(s_dates[0], "%d-%d-%d", &y, &m, &d);
    snprintf(s_sel_date, sizeof(s_sel_date), "%04d-%02d-%02d", y, m, d);

    lv_obj_t* card = ui_make_card(s_content);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);

    ui_make_label(card, "Open a session", THEME_PRIMARY, &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(
        card, "Pick the class date (no clock on the device), then open the session.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    lv_obj_t* cal = lv_calendar_create(card);
    lv_obj_set_size(cal, LV_PCT(100), 300);
    lv_calendar_set_showed_date(cal, y, m);
    // Highlight = the current selection (placeholder at first, follows taps).
    s_highlight.year = y;
    s_highlight.month = m;
    s_highlight.day = d;
    lv_calendar_set_highlighted_dates(cal, &s_highlight, 1);
    lv_calendar_header_dropdown_create(cal);
    lv_obj_add_event_cb(cal, cal_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* open = ui_make_button(card, "Open session", &theme_style_btn_primary,
                                    open_selected_cb, nullptr);
    lv_obj_set_width(open, LV_PCT(100));

    // Just the most recent session, as a shortcut back into it. The full list
    // lives in the History view; repeating it here meant a scrollable box inside
    // the already-scrolling body, which handed the drag over to the outer
    // scroller mid-gesture whenever the inner one hit its end.
    if (s_dates_count > 0) {
        lv_obj_t* rc = lv_obj_create(s_content);
        lv_obj_remove_style_all(rc);
        lv_obj_set_size(rc, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(rc, 8, 0);
        ui_make_label(rc, "Last session", THEME_PRIMARY, &lv_font_montserrat_20);
        add_date_row(rc, 0, false);  // dates are newest first
    }
}

// True if an attendance session is open for the class being shown.
static bool session_open_here(void) {
    return attendance_is_open() && strcmp(attendance_dir(), s_cls->dir) == 0;
}

static void build_session(void) {
    if (session_open_here()) {
        build_session_open();
    } else {
        build_session_closed();
    }
}

// --- History view -----------------------------------------------------------

static void build_history(void) {
    s_dates_count = attendance_list_dates(s_cls->dir, s_dates, MAX_DATES);

    if (s_dates_count == 0) {
        lv_obj_t* card = ui_make_card(s_content);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 6, 0);
        ui_make_label(card, "No attendance records yet", THEME_TEXT, &lv_font_montserrat_14);
        lv_obj_t* msg = ui_make_label(
            card, "Open a session to record attendance.", THEME_MUTED,
            &lv_font_montserrat_14);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(msg, LV_PCT(100));
        return;
    }

    ui_make_label(s_content, "Past sessions (tap to edit)", THEME_PRIMARY,
                  &lv_font_montserrat_20);
    for (int i = 0; i < s_dates_count; i++) add_date_row(s_content, i, true);
}

// --- Enroll: search ---------------------------------------------------------

// Case-insensitive substring test.
static bool istr_has(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (const char* h = hay; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) return true;
    }
    return false;
}

static void select_existing_cb(lv_event_t* e) {
    s_sel_idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_sel_is_new = false;
    enroll_goto(ENROLL_WAIT);
}

static void add_manual_cb(lv_event_t*) {
    // Prefill the name with the search text (usually a name that wasn't found).
    const char* q = s_search_ta ? lv_textarea_get_text(s_search_ta) : "";
    snprintf(s_new_name, sizeof(s_new_name), "%s", q);
    s_new_id[0] = '\0';
    s_new_turma[0] = '\0';
    enroll_goto(ENROLL_MANUAL);
}

// Rebuilds the autocomplete list from the current query + toggle. Only the
// students enrolled in this class (class.json's roster) are searched.
static void update_results(void) {
    if (!s_results) return;
    lv_obj_clean(s_results);
    const char* q = s_search_ta ? lv_textarea_get_text(s_search_ta) : "";

    // Count EVERY match but build at most ENROLL_MAX_ROWS cards: each row is 3
    // LVGL objects out of the fixed LVGL pool, and exhausting that pool halts
    // the UI thread silently (LV_ASSERT_HANDLER is `while(1);`). Counting is free
    // — it's just string compares — so the header can still report the true
    // total and tell the user to narrow the search.
    int matched = 0;  // all matches
    int shown = 0;    // cards actually built
    for (int j = 0; j < s_cls->roster_count; j++) {
        int idx = s_cls->roster[j];  // index into the global registry
        const student_t* st = roster_student_at(idx);
        if (!st) continue;
        if (s_unregistered_only && st->rfid_uid[0]) continue;
        if (!istr_has(st->name, q) && !istr_has(st->id, q)) continue;

        matched++;
        if (shown >= ENROLL_MAX_ROWS) continue;  // counted, not drawn

        lv_obj_t* row = ui_make_card(s_results);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 2, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, select_existing_cb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
        ui_add_press_feedback(row);

        // Matches the roll-call chips (16): both are student rows picked out at
        // a glance, so they read at the same size.
        ui_make_label(row, st->name, THEME_TEXT, &lv_font_montserrat_16);
        char sub[72];
        snprintf(sub, sizeof(sub), "%s%s", st->id, st->rfid_uid[0] ? "   card registered" : "");
        ui_make_label(row, sub, st->rfid_uid[0] ? THEME_SUCCESS : THEME_MUTED,
                      &lv_font_montserrat_16);
        shown++;
    }

    if (shown > 0) {
        // Always state the true total, and say plainly when the list is cut
        // short — otherwise a student missing from the list looks like they are
        // not enrolled. Built after the rows (that's where the counts are known)
        // and moved to the top.
        char head[96];
        bool truncated = matched > shown;
        if (truncated) {
            snprintf(head, sizeof(head), "Showing %d of %d - keep typing to narrow the search",
                     shown, matched);
        } else if (q[0]) {
            snprintf(head, sizeof(head), "%d match%s", matched, matched == 1 ? "" : "es");
        } else {
            snprintf(head, sizeof(head), "%d student%s in this class", matched,
                     matched == 1 ? "" : "s");
        }
        lv_obj_t* count = ui_make_label(s_results, head, truncated ? THEME_WARNING : THEME_MUTED,
                                        &lv_font_montserrat_14);
        lv_label_set_long_mode(count, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(count, LV_PCT(100));
        lv_obj_move_to_index(count, 0);
        return;
    }

    lv_obj_t* card = ui_make_card(s_results);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    // Nothing listed at all. With no query that is not "type to search" — the
    // list is populated by default — so name the actual reason.
    const char* why;
    if (q[0]) {
        why = "No matching student";
    } else if (s_unregistered_only) {
        why = "Every student in this class already has a card. Turn off the filter to see them.";
    } else {
        why = "No students enrolled in this class yet.";
    }
    lv_obj_t* msg = ui_make_label(card, why, THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, LV_PCT(100));
    if (q[0]) {
        lv_obj_t* add = ui_make_button(card, LV_SYMBOL_PLUS "  Add new student",
                                       &theme_style_btn_outline, add_manual_cb, nullptr);
        lv_obj_set_width(add, LV_PCT(100));
    }
}

static void toggle_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    s_unregistered_only = lv_obj_has_state(sw, LV_STATE_CHECKED);
    update_results();
}

static void search_changed_cb(lv_event_t*) { update_results(); }

static void build_enroll_search(void) {
    // Pin the filter + search box and scroll ONLY the results. s_content fills
    // the body's content area, which rebuild_content() has already shortened by
    // a keyboard's height (320 px bottom pad) — so this is exactly the space
    // above the on-screen keyboard. The card keeps its natural height and the
    // results list takes whatever is left.
    lv_obj_set_height(s_content, LV_PCT(100));

    lv_obj_t* card = ui_make_card(s_content);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);

    lv_obj_t* trow = lv_obj_create(card);
    lv_obj_remove_style_all(trow);
    lv_obj_set_size(trow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(trow, 10, 0);
    lv_obj_t* tlabel = ui_make_label(trow, "Only students without a card", THEME_TEXT,
                                     &lv_font_montserrat_14);
    lv_obj_set_flex_grow(tlabel, 1);
    lv_label_set_long_mode(tlabel, LV_LABEL_LONG_WRAP);
    lv_obj_t* sw = lv_switch_create(trow);
    if (s_unregistered_only) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, toggle_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    s_search_ta = keyboard_make_textarea(card, "Search by name or ID", 47,
                                         LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_search_ta, search_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // The only scroll container on this view: takes the remaining height and
    // scrolls its rows internally, so the search box above never moves.
    s_results = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_results);
    lv_obj_set_width(s_results, LV_PCT(100));
    lv_obj_set_flex_grow(s_results, 1);
    lv_obj_set_flex_flow(s_results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_results, 8, 0);
    lv_obj_set_style_pad_right(s_results, 4, 0);  // keep rows clear of the scrollbar
    lv_obj_add_flag(s_results, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_results, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_results, LV_SCROLLBAR_MODE_AUTO);
    update_results();
}

// --- Enroll: manual entry ---------------------------------------------------

static void manual_back_cb(lv_event_t*) { enroll_goto(ENROLL_SEARCH); }

static void manual_continue_cb(lv_event_t*) {
    keyboard_hide();
    const char* id = lv_textarea_get_text(s_manual_id_ta);
    const char* name = lv_textarea_get_text(s_manual_name_ta);
    if (!id[0] || !name[0]) {
        ui_toast_show("Enter both name and ID", false);
        return;
    }
    snprintf(s_new_id, sizeof(s_new_id), "%s", id);
    snprintf(s_new_name, sizeof(s_new_name), "%s", name);
    snprintf(s_new_turma, sizeof(s_new_turma), "%s", lv_textarea_get_text(s_manual_turma_ta));
    s_sel_is_new = true;
    enroll_goto(ENROLL_WAIT);
}

static void build_enroll_manual(void) {
    lv_obj_t* card = ui_make_card(s_content);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    ui_make_label(card, "Add new student", THEME_PRIMARY, &lv_font_montserrat_20);

    ui_make_label(card, "Student ID", THEME_TEXT, &lv_font_montserrat_14);
    s_manual_id_ta = keyboard_make_textarea(card, "e.g. 2024-0123", 15,
                                            LV_KEYBOARD_MODE_NUMBER);
    if (s_new_id[0]) lv_textarea_set_text(s_manual_id_ta, s_new_id);

    ui_make_label(card, "Full name", THEME_TEXT, &lv_font_montserrat_14);
    s_manual_name_ta = keyboard_make_textarea(card, "Student name", 47,
                                              LV_KEYBOARD_MODE_TEXT_LOWER);
    if (s_new_name[0]) lv_textarea_set_text(s_manual_name_ta, s_new_name);

    ui_make_label(card, "Turma (optional)", THEME_TEXT, &lv_font_montserrat_14);
    s_manual_turma_ta = keyboard_make_textarea(card, "e.g. TE1", 15, LV_KEYBOARD_MODE_TEXT_UPPER);
    if (s_new_turma[0]) lv_textarea_set_text(s_manual_turma_ta, s_new_turma);

    lv_obj_t* cont = ui_make_button(card, "Continue to card", &theme_style_btn_primary,
                                    manual_continue_cb, nullptr);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_t* back = ui_make_button(card, "Back to search", &theme_style_btn_outline,
                                    manual_back_cb, nullptr);
    lv_obj_set_width(back, LV_PCT(100));
}

// --- Enroll: wait for card --------------------------------------------------

static void wait_cancel_cb(lv_event_t*) { enroll_goto(ENROLL_SEARCH); }

// Runs on the LVGL thread (via the card-capture path) when a card is tapped.
static void on_enroll_card(const char* uid_hex) {
    roster_result_t r;
    const char* sid;
    if (s_sel_is_new) {
        r = roster_enroll_new(s_cls->code, s_new_id, s_new_name, uid_hex, s_new_turma);
        sid = s_new_id;
    } else {
        r = roster_enroll_existing(s_cls->code, s_sel_idx, uid_hex);
        const student_t* st = roster_student_at(s_sel_idx);
        sid = st ? st->id : nullptr;
    }
    if (r.ok) {
        beeper_beep();
        // Enrolling always happens during an open session: check the student
        // in right away.
        bool checked_in = false;
        if (sid && session_open_here()) {
            attendance_set(sid, true);
            checked_in = true;
        }

        // The card was just bound to this student, so the class roster resolves
        // it — for a student created seconds ago as much as an existing one.
        int idx = roster_student_by_uid(uid_hex);
        const student_t* st = idx >= 0 ? roster_student_at(idx) : nullptr;

        enroll_goto(ENROLL_SEARCH);  // done; back to the list (also clears capture)

        if (st) {
            // Same card the check-in path shows, so an enroll confirms the face
            // on file belongs to the card that was just registered. A student
            // with no avatar yet gets the placeholder.
            show_presence_feedback(st, class_turma_for_student(idx), THEME_SUCCESS,
                                   LV_SYMBOL_OK,
                                   checked_in ? "Enrolled - checked in" : "Enrolled");
        } else {
            // Lookup failed (unreadable UID): fall back to the plain toast
            // rather than an overlay with no name on it.
            char msg[128];
            snprintf(msg, sizeof(msg), "%s%s", r.message, checked_in ? " - checked in" : "");
            ui_toast_show(msg, true);
        }
    } else {
        beeper_error();
        ui_toast_show(r.message, false);
        ui_set_card_capture(on_enroll_card);  // let them try another card
    }
}

static void build_enroll_wait(void) {
    const char* name;
    const char* id;
    if (s_sel_is_new) {
        name = s_new_name;
        id = s_new_id;
    } else {
        const student_t* st = roster_student_at(s_sel_idx);
        name = st ? st->name : "?";
        id = st ? st->id : "?";
    }

    lv_obj_t* card = ui_make_card(s_content);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_style_pad_ver(card, 20, 0);

    ui_make_label(card, LV_SYMBOL_SD_CARD, THEME_ACCENT, &lv_font_montserrat_32);
    ui_make_label(card, "Tap the student's card", THEME_PRIMARY, &lv_font_montserrat_20);
    char who[80];
    snprintf(who, sizeof(who), "%s  (%s)", name, id);
    ui_make_label(card, who, THEME_TEXT, &lv_font_montserrat_14);
    ui_make_label(card, "Hold the RFID card near the reader", THEME_MUTED,
                  &lv_font_montserrat_14);

    lv_obj_t* cancel = ui_make_button(card, "Cancel", &theme_style_btn_outline,
                                      wait_cancel_cb, nullptr);
    lv_obj_set_width(cancel, LV_PCT(100));

    ui_set_card_capture(on_enroll_card);
}

static void goto_session_cb(lv_event_t*) { go_view(VIEW_SESSION); }

static void build_enroll(void) {
    // Enrollment checks the student into the current session, so it needs one
    // open. Show a prompt (with a shortcut) instead of the search otherwise.
    if (!session_open_here()) {
        lv_obj_t* card = ui_make_card(s_content);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 8, 0);
        ui_make_label(card, "No open session", THEME_PRIMARY, &lv_font_montserrat_20);
        lv_obj_t* m = ui_make_label(
            card,
            "Open a session first. Enrolled students are checked "
            "into it automatically.",
            THEME_MUTED, &lv_font_montserrat_14);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m, LV_PCT(100));
        lv_obj_t* go = ui_make_button(card, "Go to Session", &theme_style_btn_primary,
                                      goto_session_cb, nullptr);
        lv_obj_set_width(go, LV_PCT(100));
        return;
    }

    switch (s_enroll_state) {
        case ENROLL_SEARCH:
            build_enroll_search();
            break;
        case ENROLL_MANUAL:
            build_enroll_manual();
            break;
        case ENROLL_WAIT:
            build_enroll_wait();
            break;
    }
}

static void enroll_goto(enroll_state_t st) {
    s_enroll_state = st;
    ui_set_card_capture(nullptr);
    // Release the shared keyboard BEFORE deleting the sub-state's widgets: the
    // enroll fields are textareas the keyboard points at, and cleaning them
    // while it still holds that pointer leaves a dangling ->ta that crashes the
    // next keyboard_hide()/keypress (use-after-free).
    keyboard_hide();
    forget_view_widgets();
    lv_obj_clean(s_content);
    lv_obj_set_height(s_content, LV_SIZE_CONTENT);  // SEARCH overrides; see build_enroll_search
    update_header();                                // the subtitle names the enroll step
    apply_keyboard_pad();
    build_enroll();
}

// --- Hub / navigation --------------------------------------------------------

static void session_btn_cb(lv_event_t*) { go_view(VIEW_SESSION); }
static void history_btn_cb(lv_event_t*) { go_view(VIEW_HISTORY); }
static void enroll_btn_cb(lv_event_t*) { go_view(VIEW_ENROLL); }

// The class landing: two big actions (Session / History). When a session is
// already open for this class, a "Resume" card sits on top so the professor
// need not re-pick the date (there is no clock on the device).
static void build_hub(void) {
    if (session_open_here()) {
        lv_obj_t* rc = ui_make_card(s_content);
        lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(rc, 8, 0);
        lv_obj_set_style_border_color(rc, lv_color_hex(THEME_SUCCESS), 0);
        lv_obj_set_style_border_width(rc, 2, 0);
        ui_make_label(rc, LV_SYMBOL_PLAY "  Session in progress", THEME_SUCCESS,
                      &lv_font_montserrat_20);
        char sub[56];
        snprintf(sub, sizeof(sub), "%s   %d/%d present", attendance_date(),
                 attendance_present_count(), s_cls->roster_count);
        ui_make_label(rc, sub, THEME_MUTED, &lv_font_montserrat_14);
        lv_obj_t* resume = ui_make_button(rc, "Resume session", &theme_style_btn_primary,
                                          session_btn_cb, nullptr);
        lv_obj_set_width(resume, LV_PCT(100));
    }

    lv_obj_t* session = ui_make_button(s_content, LV_SYMBOL_LIST "  Session",
                                       &theme_style_btn_primary, session_btn_cb, nullptr);
    lv_obj_set_width(session, LV_PCT(100));
    lv_obj_set_height(session, UI_BUTTON_HEIGHT);

    lv_obj_t* history = ui_make_button(s_content, LV_SYMBOL_LOOP "  History",
                                       &theme_style_btn_outline, history_btn_cb, nullptr);
    lv_obj_set_width(history, LV_PCT(100));
    lv_obj_set_height(history, UI_BUTTON_HEIGHT);
}

// Kiosk mode (unattended student check-in) — reachable from the open session.
static void kiosk_cb(lv_event_t*) {
    if (!session_open_here()) {
        ui_toast_show("Open a session to use kiosk mode", false);
        return;
    }
    scr_mgr_show(SCREEN_KIOSK, (void*)const_cast<class_rec_t*>(s_cls));
}

// --- View glue ---------------------------------------------------------------

static void forget_view_widgets(void) {
    s_roll_search_ta = nullptr;
    s_roll_list = nullptr;
    s_roll_count = nullptr;
    s_roll_bar = nullptr;
    s_search_ta = nullptr;
    s_results = nullptr;
    s_manual_id_ta = nullptr;
    s_manual_name_ta = nullptr;
    s_manual_turma_ta = nullptr;
}

// Shows the green header "session open" chip while a session is open for this
// class. Called on every rebuild, so it tracks open/close within the screen.
static void update_session_badge(void) {
    if (!s_session_badge) return;
    if (session_open_here()) {
        lv_obj_remove_flag(s_session_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_session_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

// The keyboard floats over the body, so a view with a search box reserves its
// height as bottom padding — but only while it is actually up, otherwise that
// space is dead and the list is needlessly short.
static void apply_keyboard_pad(void) {
    if (!s_sh.body) return;
    // Enroll and the open session both carry a search box the keyboard serves.
    bool reserve = (s_view == VIEW_ENROLL || s_view == VIEW_SESSION) && keyboard_is_visible();
    lv_obj_set_style_pad_bottom(s_sh.body, reserve ? KEYBOARD_PAD : UI_PAD, 0);
}

static void kb_visibility_cb(bool) { apply_keyboard_pad(); }

// Header reflects where you are in the view stack; the class stays in the
// subtitle so context is never lost.
static void update_header(void) {
    if (!s_cls) {
        lv_label_set_text(s_sh.title, "?");
        lv_label_set_text(s_sh.subtitle, "Student attendance");
        return;
    }
    char subtitle[88];
    if (s_view == VIEW_ENROLL) {
        const char* step = s_enroll_state == ENROLL_MANUAL ? "new student"
                           : s_enroll_state == ENROLL_WAIT ? "waiting for the card"
                                                           : "find the student";
        lv_label_set_text(s_sh.title, "Enroll a card");
        snprintf(subtitle, sizeof(subtitle), "%s  |  %s", s_cls->name, step);
    } else {
        lv_label_set_text(s_sh.title, s_cls->name);
        snprintf(subtitle, sizeof(subtitle), "%s  |  %s", s_cls->code, s_cls->schedule);
    }
    lv_label_set_text(s_sh.subtitle, subtitle);
}

static void rebuild_content(void) {
    // Leaving any sub-flow: no card captured for enroll, no lingering overlays.
    ui_set_card_capture(nullptr);
    keyboard_hide();
    close_presence_feedback();
    update_session_badge();

    forget_view_widgets();
    lv_obj_clean(s_content);
    // Views size themselves to their content and let sh.body scroll. The enroll
    // SEARCH view overrides this to fill the body instead, so its search bar can
    // stay pinned (see build_enroll_search).
    lv_obj_set_height(s_content, LV_SIZE_CONTENT);
    if (!s_cls) return;
    // The enroll and session views both carry a search box; apply_keyboard_pad()
    // reserves the keyboard's height only while it is actually shown, and gives
    // the space back to the list as soon as it is dismissed.
    update_header();
    apply_keyboard_pad();
    switch (s_view) {
        case VIEW_HUB:
            build_hub();
            break;
        case VIEW_SESSION:
            build_session();
            break;
        case VIEW_HISTORY:
            build_history();
            break;
        case VIEW_ENROLL:
            s_enroll_state = ENROLL_SEARCH;  // always start at the search
            build_enroll();
            break;
    }
}

static void go_view(view_id_t v) {
    s_view = v;
    rebuild_content();
}

static lv_obj_t* create(void) {
    // No footer nav here: a detail screen gets a back button instead.
    s_sh = shell_create("", "", false);
    shell_set_back(&s_sh, back_cb);

    // Right-aligned header chip that lights up while a session is open (the
    // title column has flex-grow, so this lands on the right edge).
    s_session_badge = lv_obj_create(s_sh.header);
    lv_obj_remove_style_all(s_session_badge);
    lv_obj_set_size(s_session_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_session_badge, lv_color_hex(THEME_SUCCESS), 0);
    lv_obj_set_style_bg_opa(s_session_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_session_badge, 8, 0);
    lv_obj_set_style_pad_hor(s_session_badge, 8, 0);
    lv_obj_set_style_pad_ver(s_session_badge, 4, 0);
    lv_obj_remove_flag(s_session_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_session_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* badge_lbl = lv_label_create(s_session_badge);
    lv_label_set_text(badge_lbl, LV_SYMBOL_PLAY "  SESSION");
    lv_obj_set_style_text_color(badge_lbl, lv_color_hex(THEME_ON_PRIMARY), 0);
    lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_14, 0);

    // No header actions or tab bar anymore: the body is a small view stack
    // (hub -> session/history -> enroll), navigated with buttons and the
    // context-aware back button.
    s_content = lv_obj_create(s_sh.body);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_content, 10, 0);
    // Let scroll gestures bubble up to sh.body (the vertical scroll container)
    // rather than being caught by this inner container.
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    return s_sh.root;
}

void scr_class_request_session_view(void) { s_pending_session_view = true; }

static void on_show(void* arg) {
    if (arg) s_cls = (const class_rec_t*)arg;
    // Attendance only: s_cls points into roster storage, which a reload rewrites.
    ui_sd_resync_light();
    // Returning from kiosk drops back into the running session; a fresh entry
    // from the class list lands on the hub.
    if (s_pending_session_view) {
        s_view = VIEW_SESSION;
        s_pending_session_view = false;
    } else if (arg) {
        s_view = VIEW_HUB;
    }
    keyboard_set_visibility_cb(kb_visibility_cb);  // reclaim the keyboard's space when it hides
    rebuild_content();                             // sets the header + padding
}

static void on_hide(void) {
    keyboard_set_visibility_cb(nullptr);  // never fire against a torn-down layout
    keyboard_hide();
    ui_set_card_capture(nullptr);
    close_presence_feedback();
}

const screen_t scr_class = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
