/*
 * doomgeneric platform layer for the BalCCon Cyberdeck 0o27
 * ---------------------------------------------------------------
 * Implements the six DG_* hooks doomgeneric needs:
 *   - display: on-board ST7735, DOOM's 320x200 downscaled 2:1 to
 *     160x100 and letterboxed on the 160x128 panel
 *   - input:   the 8 face buttons behind a 74HC165 shift register
 *   - timing:  esp_timer
 *
 * The WAD lives on a SPIFFS partition ("storage") mounted at /spiffs;
 * DOOM is launched with  -iwad /spiffs/doom1.wad.
 */

#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_spiffs.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_lcd_panel_io.h"

#include "doomgeneric.h"
#include "doomkeys.h"

static const char *TAG = "doom";

/* ---- display wiring ---- */
#define PIN_MOSI 11
#define PIN_SCLK 12
#define PIN_CS   10
#define PIN_DC   14
#define PIN_RST  15
#define PIN_BL   16
#define LCD_HOST SPI2_HOST
#define LCD_PIXCLK_HZ (40 * 1000 * 1000)
#define LCD_W 160
#define LCD_H 128
#define LCD_X_OFF 1
#define LCD_Y_OFF 2

#define VIEW_W  160
#define VIEW_H  100
#define VIEW_Y0 ((LCD_H - VIEW_H) / 2)   /* 14px black bar top & bottom */

/* ---- input: 74HC165 (same wiring as the badge's ch405labs_esp_controller) ---- */
#define PIN_SR_DATA 4
#define PIN_SR_CLK  6
#define PIN_SR_CLE  7
#define PIN_SR_LOAD 5
#define BTN_LEFT  0x01
#define BTN_DOWN  0x02
#define BTN_UP    0x04
#define BTN_RIGHT 0x08
#define BTN_X     0x10
#define BTN_Y     0x20
#define BTN_A     0x40
#define BTN_B     0x80

/* ---- WS2812 LEDs (gameplay feedback: red on damage, gold on pickup) ---- */
#define PIN_LED_DATA 48
#define PIN_LED_EN   47
#define NUM_LEDS     6

/* ---- ST7735 ---- */
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_flush_done;
static uint16_t *s_fb;

static void st_cmd(uint8_t c) { esp_lcd_panel_io_tx_param(s_io, c, NULL, 0); }
static void st_cmd_d(uint8_t c, const uint8_t *d, size_t n) { esp_lcd_panel_io_tx_param(s_io, c, d, n); }

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                   esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

static void display_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_BL) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(PIN_BL, 0);
    gpio_set_level(PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI, .sclk_io_num = PIN_SCLK, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_PIXCLK_HZ, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10,
        .on_color_trans_done = on_color_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    st_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));   /* SWRESET */
    st_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(255));   /* SLPOUT  */
    st_cmd_d(0xB1, (uint8_t[]){0x01,0x2C,0x2D}, 3);
    st_cmd_d(0xB2, (uint8_t[]){0x01,0x2C,0x2D}, 3);
    st_cmd_d(0xB3, (uint8_t[]){0x01,0x2C,0x2D,0x01,0x2C,0x2D}, 6);
    st_cmd_d(0xB4, (uint8_t[]){0x07}, 1);
    st_cmd_d(0xC0, (uint8_t[]){0xA2,0x02,0x84}, 3);
    st_cmd_d(0xC1, (uint8_t[]){0xC5}, 1);
    st_cmd_d(0xC2, (uint8_t[]){0x0A,0x00}, 2);
    st_cmd_d(0xC3, (uint8_t[]){0x8A,0x2A}, 2);
    st_cmd_d(0xC4, (uint8_t[]){0x8A,0xEE}, 2);
    st_cmd_d(0xC5, (uint8_t[]){0x0E}, 1);
    st_cmd(0x20);                                   /* INVOFF */
    st_cmd_d(0x36, (uint8_t[]){0xC0}, 1);
    st_cmd_d(0x3A, (uint8_t[]){0x05}, 1);           /* 16bpp */
    st_cmd_d(0x2A, (uint8_t[]){0x00,0x00,0x00,0x7F}, 4);
    st_cmd_d(0x2B, (uint8_t[]){0x00,0x00,0x00,0x9F}, 4);
    st_cmd_d(0xE0, (uint8_t[]){0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,
                               0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10}, 16);
    st_cmd_d(0xE1, (uint8_t[]){0x03,0x1d,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                               0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10}, 16);
    st_cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));
    st_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(100));
    st_cmd_d(0x36, (uint8_t[]){0x60}, 1);           /* MADCTL: MX|MV, RGB, rotation 3 */
}

/* ---- input ---- */
#define KQ_SIZE 64
static volatile uint16_t s_kq[KQ_SIZE];
static volatile uint32_t s_kq_w, s_kq_r;
static uint8_t s_btn_prev;

static void kq_push(int pressed, unsigned char key)
{
    if (!key) return;
    uint32_t n = (s_kq_w + 1) % KQ_SIZE;
    if (n != s_kq_r) {
        s_kq[s_kq_w] = (uint16_t)(((pressed ? 1 : 0) << 8) | key);
        s_kq_w = n;
    }
}

static unsigned char btn_key(uint8_t bit)
{
    switch (bit) {
    case BTN_UP:    return KEY_UPARROW;
    case BTN_DOWN:  return KEY_DOWNARROW;
    case BTN_LEFT:  return KEY_LEFTARROW;
    case BTN_RIGHT: return KEY_RIGHTARROW;
    case BTN_A:     return KEY_FIRE;
    case BTN_B:     return KEY_USE;
    case BTN_X:     return KEY_ENTER;
    case BTN_Y:     return KEY_ESCAPE;
    }
    return 0;
}

static void input_init(void)
{
    gpio_config_t o = {
        .pin_bit_mask = (1ULL << PIN_SR_CLK) | (1ULL << PIN_SR_CLE) | (1ULL << PIN_SR_LOAD),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&o);
    gpio_config_t i = { .pin_bit_mask = 1ULL << PIN_SR_DATA, .mode = GPIO_MODE_INPUT };
    gpio_config(&i);
    gpio_set_level(PIN_SR_LOAD, 1);
    gpio_set_level(PIN_SR_CLK, 0);
    gpio_set_level(PIN_SR_CLE, 1);
}

static uint8_t sr_read(void)
{
    gpio_set_level(PIN_SR_CLE, 1);
    gpio_set_level(PIN_SR_CLK, 1);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_SR_LOAD, 0);
    esp_rom_delay_us(2);
    gpio_set_level(PIN_SR_LOAD, 1);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_SR_CLE, 0);
    gpio_set_level(PIN_SR_CLK, 0);

    uint8_t v = 0;
    for (int b = 0; b < 8; b++) {
        if (gpio_get_level(PIN_SR_DATA)) v |= (1 << b);
        gpio_set_level(PIN_SR_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_SR_CLK, 0);
        esp_rom_delay_us(1);
    }
    gpio_set_level(PIN_SR_CLE, 1);
    return v;
}

/* ---- LEDs ---- */
static rmt_channel_handle_t s_rmt;
static rmt_encoder_handle_t s_rmt_enc;

static void leds_init(void)
{
    gpio_config_t en = { .pin_bit_mask = 1ULL << PIN_LED_EN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&en);
    gpio_set_level(PIN_LED_EN, 1);          /* power the WS2812 rail */

    rmt_tx_channel_config_t tx = {
        .gpio_num = PIN_LED_DATA,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&tx, &s_rmt) != ESP_OK) return;

    rmt_bytes_encoder_config_t enc = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 6, .level1 = 0, .duration1 = 6 },
        .flags.msb_first = 1,
    };
    rmt_new_bytes_encoder(&enc, &s_rmt_enc);
    rmt_enable(s_rmt);
}

static void leds_all(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rmt) return;
    uint8_t grb[NUM_LEDS * 3];
    for (int i = 0; i < NUM_LEDS; i++) {
        grb[i * 3 + 0] = g;
        grb[i * 3 + 1] = r;
        grb[i * 3 + 2] = b;
    }
    rmt_transmit_config_t tc = { .loop_count = 0 };
    rmt_transmit(s_rmt, s_rmt_enc, grb, sizeof(grb), &tc);
    rmt_tx_wait_all_done(s_rmt, portMAX_DELAY);
}

/* Drive the LEDs from the finished frame's overall colour cast:
 * DOOM tints the whole screen red when you're hit and gold on a pickup. */
static void leds_from_frame(uint32_t sr, uint32_t sg, uint32_t sb, uint32_t n)
{
    if (!n) return;
    int r = sr / n, g = sg / n, b = sb / n;
    int redness = r - (g + b) / 2;          /* damage flash */
    int goldness = (r + g) / 2 - b;         /* pickup / bonus flash */

    /* keep these tiny — the LEDs are right under your eyes */
    if (redness > 12) {
        int v = redness / 2; if (v > 20) v = 20;
        leds_all(v, 0, 0);
    } else if (goldness > 18 && r > 40 && g > 30) {
        int v = goldness / 3; if (v > 12) v = 12;
        leds_all(v, (v * 3) / 4, 0);
    } else {
        leds_all(0, 0, 0);                  /* idle: dark */
    }
}

static void poll_input(void)
{
    uint8_t now = sr_read();
    uint8_t changed = now ^ s_btn_prev;
    if (changed) {
        for (int b = 0; b < 8; b++) {
            uint8_t m = (uint8_t)(1 << b);
            if (changed & m) kq_push((now & m) ? 1 : 0, btn_key(m));
        }
        s_btn_prev = now;
    }
}

/* ---- DG hooks ---- */

void DG_Init(void)
{
    s_flush_done = xSemaphoreCreateBinary();
    xSemaphoreGive(s_flush_done);
    s_fb = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(s_fb);
    memset(s_fb, 0, LCD_W * LCD_H * 2);

    display_init();
    input_init();
    leds_init();
    leds_all(0, 0, 0);                 /* clear whatever the last firmware left */
    s_btn_prev = sr_read();

    gpio_set_level(PIN_BL, 1);
    ESP_LOGI(TAG, "display + input + leds up");
}

void DG_DrawFrame(void)
{
    const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;

    xSemaphoreTake(s_flush_done, portMAX_DELAY);   /* wait for previous flush */

    uint32_t sumR = 0, sumG = 0, sumB = 0;
    for (int y = 0; y < VIEW_H; y++) {
        const uint32_t *r0 = src + (size_t)(y * 2) * DOOMGENERIC_RESX;
        const uint32_t *r1 = r0 + DOOMGENERIC_RESX;
        uint16_t *d = s_fb + (size_t)(VIEW_Y0 + y) * LCD_W;
        for (int x = 0; x < VIEW_W; x++) {
            int sx = x * 2;
            uint32_t a = r0[sx], b = r0[sx + 1], c = r1[sx], e = r1[sx + 1];
            int rr = ((int)((a >> 16) & 0xff) + (int)((b >> 16) & 0xff)
                    + (int)((c >> 16) & 0xff) + (int)((e >> 16) & 0xff)) >> 2;
            int gg = ((int)((a >> 8) & 0xff) + (int)((b >> 8) & 0xff)
                    + (int)((c >> 8) & 0xff) + (int)((e >> 8) & 0xff)) >> 2;
            int bb = ((int)(a & 0xff) + (int)(b & 0xff)
                    + (int)(c & 0xff) + (int)(e & 0xff)) >> 2;
            sumR += rr; sumG += gg; sumB += bb;
            uint16_t v = (uint16_t)(((rr & 0xf8) << 8) | ((gg & 0xfc) << 3) | (bb >> 3));
            d[x] = (uint16_t)((v >> 8) | (v << 8));
        }
    }

    uint16_t x0 = LCD_X_OFF, x1 = LCD_X_OFF + LCD_W - 1;
    uint16_t y0 = LCD_Y_OFF, y1 = LCD_Y_OFF + LCD_H - 1;
    st_cmd_d(0x2A, (uint8_t[]){x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF}, 4);
    st_cmd_d(0x2B, (uint8_t[]){y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF}, 4);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, s_fb, LCD_W * LCD_H * 2);

    leds_from_frame(sumR, sumG, sumB, (uint32_t)VIEW_W * VIEW_H);
    poll_input();

    static uint32_t frames;
    static int64_t last;
    if (++frames == 60) {
        int64_t now = esp_timer_get_time();
        ESP_LOGI(TAG, "~%.1f fps", 60.0f * 1e6f / (float)(now - last));
        last = now;
        frames = 0;
    }
}

void DG_SleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (s_kq_r == s_kq_w) return 0;
    uint16_t d = s_kq[s_kq_r];
    s_kq_r = (s_kq_r + 1) % KQ_SIZE;
    *pressed = d >> 8;
    *key = d & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    ESP_LOGI(TAG, "%s", title ? title : "");
}

/* ---- entry point ---- */

static void doom_task(void *arg)
{
    char *argv[] = { "doom", "-iwad", "/spiffs/doom1.wad", NULL };
    doomgeneric_Create(3, argv);          /* returns after bootstrapping */
    ESP_LOGI(TAG, "entering game loop");
    for (;;) {
        doomgeneric_Tick();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "BCD-0o27 DOOM starting (IDF %s)", esp_get_idf_version());

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %u / %u bytes used", (unsigned)used, (unsigned)total);

    FILE *f = fopen("/spiffs/doom1.wad", "rb");
    if (!f) {
        ESP_LOGE(TAG, "no /spiffs/doom1.wad — flash the SPIFFS image first");
    } else {
        fseek(f, 0, SEEK_END);
        ESP_LOGI(TAG, "found doom1.wad, %ld bytes", ftell(f));
        fclose(f);
    }

    xTaskCreatePinnedToCore(doom_task, "doom", 24 * 1024, NULL, 5, NULL, 1);
}
