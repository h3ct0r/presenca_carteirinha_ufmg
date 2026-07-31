#include "ui/screens/scr_about.h"

#include <stdio.h>
#include <string.h>

#include "app/version.h"
#include "lvgl.h"
#include "ui/components/shell.h"
#include "ui/screen_manager.h"
#include "ui/theme/theme.h"

// Logos live in flash (src/ui/assets/logo_*.c) rather than on the SD card: the
// About screen must render even on a freshly formatted / missing card.
LV_IMAGE_DECLARE(logo_verlab);
LV_IMAGE_DECLARE(logo_gear);

static const char* REPO_URL = "https://github.com/verlab/presenca_carteirinha_ufmg";

// NOTE: all copy here is deliberately ASCII-only. The fonts in this build cover
// 0x20-0x7F plus a few FontAwesome glyphs, so accented characters ("Hector
// Azpurua", "Avancados", "Robotica") would render as blanks. Restore the proper
// accents once the custom fonts are regenerated with the Latin-1 range
// (0xC0-0xFF) — see docs/software/CUSTOM_FONT_GENERATION.md.
static const char* DESCRIPTION =
    "Classroom attendance device. Students register presence by tapping their "
    "university RFID card, or by typing their ID in kiosk mode. Attendance is "
    "stored on the SD card and exported as CSV.";

static void back_cb(lv_event_t*) { scr_mgr_show(SCREEN_ADMIN, nullptr); }

// A titled block inside a card: small muted caption above its content.
static lv_obj_t* section(lv_obj_t* parent, const char* caption) {
    lv_obj_t* card = ui_make_card(parent);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    if (caption) {
        lv_obj_t* c = ui_make_label(card, caption, THEME_MUTED, &lv_font_montserrat_14);
        lv_obj_set_style_text_letter_space(c, 1, 0);
    }
    return card;
}

static lv_obj_t* create(void) {
    shell_t sh = shell_create("About", "Project, credits and version", false);
    shell_set_back(&sh, back_cb);

    // --- what it is -------------------------------------------------------
    lv_obj_t* intro = section(sh.body, nullptr);
    lv_obj_t* name = ui_make_label(intro, "Presenca Carteirinha UFMG", THEME_TEXT,
                                   &lv_font_montserrat_20);
    lv_obj_set_width(name, LV_PCT(100));

    lv_obj_t* ver = ui_make_label(intro, APP_VERSION_FULL, THEME_ACCENT, &lv_font_montserrat_14);
    lv_obj_set_style_text_letter_space(ver, 1, 0);

    lv_obj_t* desc = ui_make_label(intro, DESCRIPTION, THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, LV_PCT(100));

    // --- authors ----------------------------------------------------------
    lv_obj_t* who = section(sh.body, "AUTHORS");
    ui_make_label(who, "Prof. Hector Azpurua", THEME_TEXT, &lv_font_montserrat_20);
    ui_make_label(who, "Prof. Paulo Rezeck", THEME_TEXT, &lv_font_montserrat_20);
    ui_make_label(who, "Prof. Douglas G. Macharet", THEME_TEXT, &lv_font_montserrat_20);
    ui_make_label(who, "Aline Molinar", THEME_TEXT, &lv_font_montserrat_20);
    lv_obj_t* dept = ui_make_label(
        who, "Computer Science Department (DCC)\nUniversidade Federal de Minas Gerais",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(dept, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(dept, LV_PCT(100));

    // --- support ----------------------------------------------------------
    // The logos are JPEGs on a white background, so they sit directly on the
    // white surface card with no visible seam (no alpha channel needed).
    lv_obj_t* thanks = section(sh.body, "WITH SUPPORT FROM");
    lv_obj_t* row = lv_obj_create(thanks);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_image_set_src(lv_image_create(row), &logo_verlab);
    lv_image_set_src(lv_image_create(row), &logo_gear);

    lv_obj_t* orgs = ui_make_label(thanks,
                                   "VeRLab - Laboratory of Computer Vision and Robotics\n"
                                   "GEAR - Grupo de Estudos Avancados em Robotica",
                                   THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(orgs, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(orgs, LV_PCT(100));

    // --- source -----------------------------------------------------------
    // The device has no browser, so the QR is the usable link; the URL text is
    // there for anyone reading over a shoulder.
    lv_obj_t* src = section(sh.body, "SOURCE CODE");
    // The card is a flex column, so a style align on the QR would be ignored —
    // center it with a full-width row instead.
    lv_obj_t* qr_row = lv_obj_create(src);
    lv_obj_remove_style_all(qr_row);
    lv_obj_set_size(qr_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(qr_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(qr_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(qr_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t* qr = lv_qrcode_create(qr_row);
    lv_qrcode_set_size(qr, 160);
    lv_qrcode_set_dark_color(qr, lv_color_hex(THEME_TEXT));
    lv_qrcode_set_light_color(qr, lv_color_hex(THEME_SURFACE));
    lv_qrcode_update(qr, REPO_URL, (uint32_t)strlen(REPO_URL));

    lv_obj_t* url = ui_make_label(src, REPO_URL, THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(url, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(url, LV_PCT(100));

    // --- third-party notices (FontAwesome Free and LVGL require attribution)
    lv_obj_t* legal = section(sh.body, "BUILT WITH");
    lv_obj_t* lic = ui_make_label(legal,
                                  "LVGL (MIT) - Montserrat (SIL OFL 1.1)\n"
                                  "Font Awesome Free (CC BY 4.0) - ESP-DL (Apache 2.0)",
                                  THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(lic, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lic, LV_PCT(100));

    return sh.root;
}

const screen_t scr_about = {
    .create = create,
    .on_show = nullptr,
    .on_hide = nullptr,
};
