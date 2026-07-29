# LVGL Custom Font Generation Guide

This guide explains how to generate a custom LVGL font that merges standard English text (e.g., Montserrat) with custom icons (e.g., FontAwesome lock symbols).

## 1. Prerequisites (Get the Right Files)

To avoid silent conversion errors, you must use clean, desktop-grade font files. 
* **Text Font:** Download a standard `.ttf` like Montserrat from Google Fonts.
* **Symbol Font:** Download the **Desktop Free** version of FontAwesome (version 5 or 6). 
  * *Important:* Extract the `.otf` or `.ttf` file. Do **not** use `.woff` or web-optimized files.
  * *Important:* Rename the symbol font file to something simple without spaces (e.g., `fa-solid.otf`).

## 2. Configure the LVGL Font Converter

1. Go to the [LVGL Online Font Converter](https://lvgl.io/tools/fontconverter).
2. **Name:** Enter a descriptive name (e.g., `font_montserrat_custom_14`).
3. **Size:** Set your desired font size (e.g., `14`).
4. **Bpp (Anti-aliasing):** Set to `1` or `2` for SPI displays to prevent rendering glitches. (Use `4` only if your display driver and memory fully support it).

## 3. Setup Font 1 (Standard Text)

1. Under the **Font 1** section, upload your text font (e.g., `Montserrat-Regular.ttf`).
2. In the **Range** input box, type exactly: `0x20-0x7F`
   *(This ensures all standard letters, numbers, and punctuation are generated).*

> ### ⚠️ Known gap: no accented characters (open issue)
>
> `0x20-0x7F` is **ASCII only**. It does not include the Latin-1 Supplement
> (`0xC0-0xFF`), where the accented Portuguese characters live — `á é í ó ú ã õ
> ç Á É Ç …`. The stock LVGL `lv_font_montserrat_*` fonts have the same limit
> (`range_start = 32, range_length = 95`).
>
> **Consequence:** any accented text renders as blanks/boxes on the device. This
> affects **student names imported from the Diário CSV** (the importer decodes
> accents correctly, but the font cannot draw them) and is why the About screen
> ships ASCII-only copy ("Hector Azpurua", "Avancados em Robotica").
>
> **Fix:** regenerate all three sizes with the range `0x20-0x7F, 0xC0-0xFF`,
> keeping the same FontAwesome merge (see §4 — the glyphs currently in use are
> `U+E595`, `U+F023`, `U+F09C`, `U+F2C2`). Then switch the UI from
> `lv_font_montserrat_*` to `font_montserrat_custom_*` and set
> `LV_FONT_MONTSERRAT_14/20/32` to `0` in `include/lv_conf.h` — dropping the
> now-unused built-ins reclaims more flash than the extra glyphs cost.
>
> A CLI alternative to the web converter:
> ```sh
> npx lv_font_conv --font Montserrat-Regular.ttf \
>   --range 0x20-0x7F --range 0xC0-0xFF \
>   --font "Font Awesome 7 Free-Solid-900.ttf" \
>   --range 0xE595,0xF023,0xF09C,0xF2C2 \
>   --size 14 --bpp 2 --format lvgl --no-compress \
>   -o src/ui/assets/font_montserrat_custom_14.c
> ```

## 4. Setup Font 2 (Custom Symbols)

1. Click the **Add another font** button.
2. Under the new **Font 2** section, upload your renamed symbol font (e.g., `fa-solid.otf`).
3. In the **Symbols** or **Range** input box, type the Unicode hex codes for the icons you need, separated by commas. 
   *(Example: `0xf023, 0xf09c` for lock and unlock icons).*

## 5. Generate and Verify

1. Click **Generate** and download the resulting `.c` file.
2. **Crucial Verification Step:** Open the `.c` file in a text editor and scroll to the bottom of the `glyph_bitmap[]` array. 
   * You should see comments explicitly listing your symbol uncodes (e.g., `/* U+F023 */`).
   * If the array ends at `/* U+007E "~" */`, the converter failed to extract your symbols. Verify your `.otf` file has no spaces in the name and try again.

## 6. Integration into Project

1. Move the verified `.c` file into your project's `src/` or `ui/assets/` directory.
2. Declare the font and define the symbol macro in your UI header file:

```c
// Declare the generated font
LV_FONT_DECLARE(font_montserrat_custom_14);

// Define the UTF-8 macro for the lock icon (U+F023)
#define LV_SYMBOL_LOCK "\xef\x80\xa3"

// Apply the font to your target LVGL objects:
lv_obj_t * label = lv_label_create(lv_scr_act());
lv_obj_set_style_text_font(label, &font_montserrat_custom_14, 0);
lv_label_set_text(label, LV_SYMBOL_LOCK " Screen Locked");
```