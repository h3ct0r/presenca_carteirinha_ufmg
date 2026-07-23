#include "ui/components/shell.h"

#include "ui/components/status_bar.h"
#include "ui/screen_manager.h"
#include "ui/theme/theme.h"

static void nav_classes_cb(lv_event_t*) { scr_mgr_show(SCREEN_CLASSES, nullptr); }
static void nav_export_cb(lv_event_t*) { scr_mgr_show(SCREEN_EXPORT, nullptr); }
static void nav_wifi_cb(lv_event_t*) { scr_mgr_show(SCREEN_WIFI_EDITOR, nullptr); }
static void nav_admin_cb(lv_event_t*) { scr_mgr_show(SCREEN_ADMIN, nullptr); }

static lv_obj_t* add_footer(lv_obj_t* root) {
    lv_obj_t* footer = lv_obj_create(root);
    lv_obj_remove_style_all(footer);
    // Tall enough for the standard-height nav buttons plus padding.
    lv_obj_set_size(footer, LV_PCT(100), UI_BUTTON_HEIGHT + 16);
    lv_obj_set_style_bg_color(footer, lv_color_hex(THEME_PRIMARY_DARK), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(footer, 8, 0);
    lv_obj_set_style_pad_column(footer, 8, 0);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    struct {
        const char* text;
        lv_event_cb_t cb;
        screen_id_t id;
    } items[] = {{LV_SYMBOL_LIST " Classes", nav_classes_cb, SCREEN_CLASSES},
                 {LV_SYMBOL_SAVE " Export", nav_export_cb, SCREEN_EXPORT},
                 {LV_SYMBOL_WIFI " WiFi\nFile Editor", nav_wifi_cb, SCREEN_WIFI_EDITOR},
                 {LV_SYMBOL_SETTINGS " Admin", nav_admin_cb, SCREEN_ADMIN}};
    for (auto& it : items) {
        lv_obj_t* btn = ui_make_button(footer, it.text, &theme_style_btn_outline, it.cb,
                                       nullptr);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_user_data(btn, (void*)(uintptr_t)it.id);  // which screen this tab opens
        // Footer variant of the outline button: white on navy.
        lv_obj_set_style_bg_color(btn, lv_color_hex(THEME_PRIMARY_DARK), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(THEME_ON_PRIMARY), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(THEME_ON_PRIMARY), 0);
        lv_obj_set_style_text_align(btn, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    }
    return footer;
}

void shell_set_active_nav(shell_t* sh, screen_id_t id) {
    if (!sh || !sh->footer) return;
    uint32_t n = lv_obj_get_child_count(sh->footer);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* btn = lv_obj_get_child(sh->footer, i);
        bool active = (screen_id_t)(uintptr_t)lv_obj_get_user_data(btn) == id;
        // Active tab: filled accent; others blend into the navy footer.
        lv_obj_set_style_bg_color(btn, lv_color_hex(active ? THEME_ACCENT : THEME_PRIMARY_DARK),
                                  0);
        lv_obj_set_style_border_color(
            btn, lv_color_hex(active ? THEME_ACCENT : THEME_ON_PRIMARY), 0);
    }
}

void shell_set_nav_enabled(shell_t* sh, bool enabled) {
    if (!sh || !sh->footer) return;
    uint32_t n = lv_obj_get_child_count(sh->footer);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* btn = lv_obj_get_child(sh->footer, i);
        if (enabled) {
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
        } else {
            lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(btn, LV_OPA_40, 0);  // dim to read as disabled
        }
    }
}

shell_t shell_create(const char* title, const char* subtitle, bool with_footer) {
    shell_t sh = {};

    sh.root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(sh.root, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_text_color(sh.root, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_pad_all(sh.root, 0, 0);
    // Keep the header clear of the global status bar overlay.
    lv_obj_set_style_pad_top(sh.root, STATUS_BAR_HEIGHT, 0);
    lv_obj_remove_flag(sh.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sh.root, LV_FLEX_FLOW_COLUMN);

    // Header is a row: [optional back button] + a title/subtitle column that
    // grows to fill the rest. A row keeps a leading back button aligned with
    // the text.
    lv_obj_t* header = lv_obj_create(sh.root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(header, lv_color_hex(THEME_PRIMARY), 0);
    lv_obj_set_style_bg_grad_color(header, lv_color_hex(THEME_PRIMARY_DARK), 0);
    lv_obj_set_style_bg_grad_dir(header, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(header, 14, 0);
    lv_obj_set_style_pad_ver(header, 8, 0);
    lv_obj_set_style_pad_column(header, 10, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    sh.header = header;

    lv_obj_t* tcol = lv_obj_create(header);
    lv_obj_remove_style_all(tcol);
    lv_obj_set_height(tcol, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tcol, 1);  // takes the width left by the back button
    lv_obj_set_flex_flow(tcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(tcol, LV_OBJ_FLAG_SCROLLABLE);

    // Long class names / schedules truncate with an ellipsis instead of
    // overflowing the header.
    sh.title = ui_make_label(tcol, title, THEME_ON_PRIMARY, &lv_font_montserrat_20);
    lv_label_set_long_mode(sh.title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sh.title, LV_PCT(100));
    sh.subtitle = ui_make_label(tcol, subtitle ? subtitle : "", THEME_HEADER_SUBTITLE,
                                &lv_font_montserrat_14);
    lv_label_set_long_mode(sh.subtitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sh.subtitle, LV_PCT(100));

    sh.body = lv_obj_create(sh.root);
    lv_obj_remove_style_all(sh.body);
    lv_obj_set_width(sh.body, LV_PCT(100));
    lv_obj_set_flex_grow(sh.body, 1);
    lv_obj_set_style_pad_all(sh.body, 12, 0);
    lv_obj_set_style_pad_row(sh.body, 10, 0);
    lv_obj_set_flex_flow(sh.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(sh.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sh.body, LV_SCROLLBAR_MODE_AUTO);

    if (with_footer) sh.footer = add_footer(sh.root);
    return sh;
}

void shell_set_back(shell_t* sh, lv_event_cb_t cb) {
    // Compact icon button (44 px) sized to fit the 64 px header — not the
    // standard action-button height.
    lv_obj_t* back = lv_button_create(sh->header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 44, 44);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(THEME_ON_PRIMARY), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, 0);  // subtle on the gradient
    lv_obj_add_event_cb(back, cb, LV_EVENT_CLICKED, nullptr);
    ui_add_press_feedback(back);

    lv_obj_t* icon = lv_label_create(back);
    lv_label_set_text(icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(icon, lv_color_hex(THEME_ON_PRIMARY), 0);
    lv_obj_center(icon);

    lv_obj_move_to_index(back, 0);  // leftmost, before the title column
}

lv_obj_t* shell_add_action(shell_t* sh, const char* text, const lv_font_t* font,
                           lv_event_cb_t cb, const char* label) {
    // Same compact style as the back button. Appended after the flex-grow title
    // column, so it lands on the right edge. Icon-only = fixed 44 px square;
    // with a label it grows to fit the icon + word in a row.
    lv_obj_t* btn = lv_button_create(sh->header);
    lv_obj_remove_style_all(btn);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(THEME_ON_PRIMARY), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    ui_add_press_feedback(btn);

    lv_obj_t* icon = lv_label_create(btn);
    lv_label_set_text(icon, text);
    if (font) lv_obj_set_style_text_font(icon, font, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(THEME_ON_PRIMARY), 0);

    if (!label) {
        lv_obj_set_size(btn, 44, 44);
        lv_obj_center(icon);
        return btn;
    }

    // Pill: icon + word in a centered row, height matched to the square.
    lv_obj_set_height(btn, 44);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 8, 0);

    lv_obj_t* word = lv_label_create(btn);
    lv_label_set_text(word, label);
    lv_obj_set_style_text_font(word, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(word, lv_color_hex(THEME_ON_PRIMARY), 0);
    return btn;
}
