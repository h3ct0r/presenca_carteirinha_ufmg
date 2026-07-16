#include "lcd/st7701_lcd.h"

#include "driver/gpio.h"
#include "esp32-hal-log.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "lcd/esp_lcd_st7701.h"

#define LCD_H_RES 480
#define LCD_V_RES 800

#define MIPI_DPI_PX_FORMAT (LCD_COLOR_PIXEL_FORMAT_RGB565)

// VDD_MIPI_DPHY must be supplied 2.5V; here it comes from internal LDO channel 3.
#define MIPI_DSI_PHY_PWR_LDO_CHAN 3
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500

// Backlight is a plain GPIO on this board (active high).
#define PIN_NUM_BK_LIGHT GPIO_NUM_23
#define LCD_BK_LIGHT_ON_LEVEL 1

// Panel reset line, fixed on this board.
#define PIN_NUM_LCD_RST GPIO_NUM_5

static const char* TAG = "st7701";

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;

st7701_lcd::st7701_lcd(int8_t lcd_rst) : _lcd_rst(lcd_rst) {}

// Power the MIPI DSI PHY: bring it from "no power" up to the "shutdown" state.
void st7701_lcd::enable_dsi_phy_power() {
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY powered on");
}

void st7701_lcd::init_backlight() {
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
}

void st7701_lcd::set_backlight(uint32_t level) {
    gpio_set_level(PIN_NUM_BK_LIGHT, level);
}

void st7701_lcd::begin() {
    enable_dsi_phy_power();
    init_backlight();

    // Create the MIPI DSI bus (also initializes the DSI PHY).
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = ST7701_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    // LCD commands and parameters go over the DBI interface.
    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    esp_lcd_dbi_io_config_t dbi_config = ST7701_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &s_io_handle));

    esp_lcd_dpi_panel_config_t dpi_config = ST7701_480_360_PANEL_60HZ_DPI_CONFIG(MIPI_DPI_PX_FORMAT);
    st7701_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
        .flags = {
            .use_mipi_interface = 1,
        },
    };

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(s_io_handle, &panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    set_backlight(LCD_BK_LIGHT_ON_LEVEL);
}

void st7701_lcd::lcd_draw_bitmap(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end,
                                 uint16_t* color_data) {
    esp_lcd_panel_draw_bitmap(s_panel_handle, x_start, y_start, x_end, y_end, color_data);
}

uint16_t st7701_lcd::width() const {
    return LCD_H_RES;
}

uint16_t st7701_lcd::height() const {
    return LCD_V_RES;
}

void st7701_lcd::get_handle(bsp_lcd_handles_t* ret_handles) const {
    ret_handles->io = s_io_handle;
    ret_handles->mipi_dsi_bus = NULL;
    ret_handles->panel = s_panel_handle;
    ret_handles->control = NULL;
}
