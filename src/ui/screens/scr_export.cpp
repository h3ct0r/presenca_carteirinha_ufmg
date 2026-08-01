#include "ui/screens/scr_export.h"

#include <stdio.h>

#include "app/roster.h"
#include "app/session.h"
#include "services/export_service.h"
#include "services/roster_service.h"
#include "ui/components/progress.h"
#include "ui/components/modal.h"
#include "ui/components/shell.h"
#include "ui/components/toast.h"
#include "ui/screen_manager.h"
#include "ui/theme/theme.h"

static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_list = nullptr;        // class cards, rebuilt on_show
static lv_obj_t* s_export_btn = nullptr;  // "Export Selected (N)", below the list
static lv_obj_t* s_select_all = nullptr;  // "Select all" / "Clear all" shortcut
static lv_obj_t* s_status = nullptr;      // result / instruction message
static lv_obj_t* s_confirm = nullptr;     // overwrite-all modal

// One checkbox per shown class, parallel to the class it selects.
static lv_obj_t* s_checks[ROSTER_MAX_CLASSES];
static const class_rec_t* s_check_cls[ROSTER_MAX_CLASSES];
static int s_check_count = 0;

static void build_list(void);
static void update_export_btn(void);

// --- selection --------------------------------------------------------------

static int count_selected(void) {
    int n = 0;
    for (int i = 0; i < s_check_count; i++) {
        if (s_checks[i] && lv_obj_has_state(s_checks[i], LV_STATE_CHECKED)) n++;
    }
    return n;
}

static void update_export_btn(void) {
    int n = count_selected();
    char txt[40];
    snprintf(txt, sizeof(txt), LV_SYMBOL_SAVE "  Export Selected (%d)", n);
    lv_label_set_text(lv_obj_get_child(s_export_btn, 0), txt);
    if (n > 0) {
        lv_obj_add_flag(s_export_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(s_export_btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_remove_flag(s_export_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(s_export_btn, LV_OPA_40, 0);  // dimmed = disabled
    }
}

// Reflects the current selection on the shortcut: "Clear all" when everything
// is ticked, "Select all" otherwise; disabled when there are no classes.
static void update_select_all(void) {
    bool has = s_check_count > 0;
    bool all = has && count_selected() == s_check_count;
    lv_label_set_text(lv_obj_get_child(s_select_all, 0), all ? "Clear all" : "Select all");
    if (has) {
        lv_obj_add_flag(s_select_all, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(s_select_all, LV_OPA_COVER, 0);
    } else {
        lv_obj_remove_flag(s_select_all, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(s_select_all, LV_OPA_40, 0);
    }
}

static void check_changed_cb(lv_event_t*) {
    update_export_btn();
    update_select_all();
}

static void select_all_cb(lv_event_t*) {
    bool all = count_selected() == s_check_count;  // all ticked -> clear, else select all
    for (int i = 0; i < s_check_count; i++) {
        if (!s_checks[i]) continue;
        if (all) {
            lv_obj_remove_state(s_checks[i], LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(s_checks[i], LV_STATE_CHECKED);
        }
    }
    update_export_btn();
    update_select_all();
}

// --- export -----------------------------------------------------------------

static void do_export_selected(void) {
    int ok = 0, fail = 0;
    uint32_t total = 0;

    // Each class re-reads every one of its session files, so a multi-class
    // export over a full year is tens of seconds of blocked UI. The context line
    // tracks classes; export_write_csv drives the bar over that class's days.
    const int selected = count_selected();
    int n = 0;
    ui_progress_open("Exporting CSV");
    for (int i = 0; i < s_check_count; i++) {
        if (!s_checks[i] || !lv_obj_has_state(s_checks[i], LV_STATE_CHECKED)) continue;
        char ctx[64];
        // ASCII only: the fonts cover 0x20-0x7F, so an em dash draws as a box.
        snprintf(ctx, sizeof(ctx), "Class %d of %d - %s", ++n, selected,
                 s_check_cls[i] ? s_check_cls[i]->code : "");
        ui_progress_context(ctx);
        export_result_t r = export_write_csv(s_check_cls[i], ui_progress_cb, nullptr);
        if (r.ok) {
            ok++;
            total += r.size_bytes;
        } else {
            fail++;
        }
    }
    ui_progress_close();

    char msg[128];
    bool good = (ok > 0 && fail == 0);
    if (good) {
        uint32_t kb100 = (total * 100 + 512) / 1024;
        snprintf(msg, sizeof(msg),
                 LV_SYMBOL_OK "  Exported %d file(s) to " EXPORT_DIR " (%u.%02u kB)", ok,
                 (unsigned)(kb100 / 100), (unsigned)(kb100 % 100));
    } else if (ok > 0) {
        snprintf(msg, sizeof(msg), "Exported %d, %d failed", ok, fail);
    } else {
        snprintf(msg, sizeof(msg), "Export failed");
    }
    lv_label_set_text(s_status, msg);
    lv_obj_set_style_text_color(s_status, lv_color_hex(good ? THEME_SUCCESS : THEME_DANGER), 0);
    ui_toast_show(good ? "Export complete" : msg, good);

    build_list();  // refresh the "already exported" hints, clears the ticks
}

// --- overwrite-all confirm modal --------------------------------------------

static void close_confirm(void) {
    if (s_confirm) {
        lv_obj_delete(s_confirm);
        s_confirm = nullptr;
    }
}

static void confirm_cancel_cb(lv_event_t*) { close_confirm(); }

static void confirm_overwrite_cb(lv_event_t*) {
    close_confirm();
    do_export_selected();
}

// A single overwrite prompt for the whole batch (not one per file).
static void open_overwrite_confirm(int existing) {
    if (s_confirm) return;

    lv_obj_t* card;
    s_confirm = ui_modal_create(s_root, 80, &card);

    ui_modal_title(card, "Overwrite existing exports?", THEME_PRIMARY);
    char line[96];
    snprintf(line, sizeof(line), "%d of the selected class(es) already have a CSV in " EXPORT_DIR
                                 ". Exporting will replace them.",
             existing);
    ui_modal_body(card, line);
    ui_modal_actions(card, "Overwrite all", &theme_style_btn_primary, confirm_overwrite_cb,
                     confirm_cancel_cb);
}

static void export_cb(lv_event_t*) {
    int selected = 0, existing = 0;
    for (int i = 0; i < s_check_count; i++) {
        if (!s_checks[i] || !lv_obj_has_state(s_checks[i], LV_STATE_CHECKED)) continue;
        selected++;
        if (export_exists(s_check_cls[i])) existing++;
    }
    if (selected == 0) return;  // button is disabled in this case anyway
    if (existing > 0) {
        open_overwrite_confirm(existing);  // ask once for the whole batch
    } else {
        do_export_selected();
    }
}

// --- class cards ------------------------------------------------------------

static void add_class_card(const class_rec_t* cls) {
    if (s_check_count >= ROSTER_MAX_CLASSES) return;
    int idx = s_check_count;
    export_metrics_t m = export_metrics(cls);

    lv_obj_t* card = ui_make_card(s_list);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(cls->color), 0);
    lv_obj_set_style_border_width(card, 4, 0);

    // The class name doubles as the checkbox label; tapping the row toggles it.
    lv_obj_t* cb = lv_checkbox_create(card);
    lv_checkbox_set_text(cb, cls->name);
    lv_obj_set_style_text_font(cb, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cb, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_pad_column(cb, 14, 0);   // gap between box and name
    lv_obj_set_style_pad_ver(cb, 6, 0);       // taller row = easier to tap
    // Big, easy-to-tap tick box: fixed 34 px square with a 32 px check glyph.
    lv_obj_set_style_width(cb, 34, LV_PART_INDICATOR);
    lv_obj_set_style_height(cb, 34, LV_PART_INDICATOR);
    lv_obj_set_style_radius(cb, 8, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(cb, &lv_font_montserrat_32, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(cb, lv_color_hex(THEME_BORDER), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(cb, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cb, lv_color_hex(THEME_SOFT), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(cb, lv_color_hex(THEME_ON_PRIMARY),
                                LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(cb, lv_color_hex(THEME_PRIMARY),
                              LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_set_style_border_color(cb, lv_color_hex(THEME_PRIMARY),
                                  LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_add_event_cb(cb, check_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    s_checks[idx] = cb;
    s_check_cls[idx] = cls;
    s_check_count++;

    char metrics[80];
    snprintf(metrics, sizeof(metrics), "%d students  |  %d days", m.student_count, m.day_count);
    ui_make_label(card, metrics, THEME_MUTED, &lv_font_montserrat_14);

    char range[48];
    if (m.day_count > 0) {
        snprintf(range, sizeof(range), "%s  " LV_SYMBOL_RIGHT "  %s", m.start_date, m.end_date);
    } else {
        snprintf(range, sizeof(range), "No sessions recorded yet");
    }
    ui_make_label(card, range, THEME_MUTED, &lv_font_montserrat_14);

    if (export_exists(cls)) {
        ui_make_label(card, LV_SYMBOL_WARNING "  Already exported", THEME_WARNING,
                      &lv_font_montserrat_14);
    }
}

static void add_notice(const char* title, const char* msg) {
    lv_obj_t* card = ui_make_card(s_list);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    ui_make_label(card, title, THEME_TEXT, &lv_font_montserrat_14);
    lv_obj_t* m = ui_make_label(card, msg, THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, LV_PCT(100));
}

static void build_list(void) {
    lv_obj_clean(s_list);
    s_check_count = 0;

    if (roster_get_status() != ROSTER_OK) {
        char err[160];
        roster_get_error(err, sizeof(err));
        add_notice("No class data", err[0] ? err : "Check the SD card data files.");
        update_export_btn();
        update_select_all();
        return;
    }

    const teacher_t* t = session_get();
    const char* email = t ? t->email : "";

    int shown = 0;
    for (int i = 0; i < roster_class_count(); i++) {
        const class_rec_t* cls = roster_class_at(i);
        if (!cls || !roster_class_matches_teacher(cls, email)) continue;
        add_class_card(cls);
        shown++;
    }

    if (shown == 0) add_notice("No classes", "No classes are assigned to this account.");
    update_export_btn();
    update_select_all();
}

static lv_obj_t* create(void) {
    shell_t sh = shell_create("CSV Export", "Select classes to export", true);
    shell_set_active_nav(&sh, SCREEN_EXPORT);
    s_root = sh.root;

    lv_obj_t* info = ui_make_label(
        sh.body, "Tick the classes to export, then tap Export Selected. CSV files are written to "
                 EXPORT_DIR " on the SD card.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, LV_PCT(100));

    // Right-aligned "Select all / Clear all" shortcut above the list.
    lv_obj_t* bar = lv_obj_create(sh.body);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    s_select_all = ui_make_button(bar, "Select all", &theme_style_btn_outline, select_all_cb,
                                  nullptr);
    lv_obj_set_height(s_select_all, 40);  // compact, not the standard 72 px button
    lv_obj_set_width(s_select_all, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_select_all, 18, 0);

    s_list = lv_obj_create(sh.body);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 10, 0);

    s_export_btn = ui_make_button(sh.body, "", &theme_style_btn_primary, export_cb, nullptr);
    lv_obj_set_width(s_export_btn, LV_PCT(100));

    s_status = ui_make_label(sh.body, "", THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, LV_PCT(100));

    return sh.root;
}

static void on_show(void*) {
    lv_label_set_text(s_status, "");  // clear any prior result
    build_list();
}

static void on_hide(void) {
    close_confirm();
    ui_progress_close();  // defensive: an orphan here blocks touch everywhere
}

const screen_t scr_export = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
