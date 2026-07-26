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