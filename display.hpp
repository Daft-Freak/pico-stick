#pragma once

#include <map>

#include "pico/sem.h"
#include "aps6404.hpp"
extern "C"
{
#include "dvi.h"
#include "dvi_timing.h"
}
#include "constants.hpp"
#include "frame_decode.hpp"
#include "sprite.hpp"

class DisplayDriver
{
public:
    DisplayDriver(PIO pio = pio1);

    // May only be called before init
    bool set_res(pico_stick::Resolution res);
    pico_stick::Resolution get_res() const { return current_res; }

    void init();

    // Runs the display.  This never returns and also starts processing on core1.
    // The resolution and hence timing are specified in the PSRAM
    // so this sets up/resets the DVI appropriately as they change.
    void run();

    // Setup a sprite with data and position
    void set_sprite(int8_t i, int16_t table_idx, pico_stick::BlendMode mode, int16_t x, int16_t y);

    // Move an existing sprite
    void move_sprite(int8_t i, int16_t x, int16_t y);

    // Disbale a sprite
    void clear_sprite(int8_t i);

    void set_frame_data_address_offset(int idx, int offset) {
        next_frame_data_address_offset[idx] = offset;
    }

    // Override the value of frame_counter, used to switch frames if the frame divider is 0
    void set_frame_counter(int val) {
        frame_counter = val;
    }

    // Called internally by run().
    void run_core1();

    pimoroni::APS6404 &get_ram() { return ram; }
    uint32_t get_clock_khz() { return dvi0.timing->bit_clk_khz; }

    // Diagnostic data - all times in us.
    struct Diags {
        uint32_t scanline_total_prep_time[2] = {0, 0};
        uint32_t scanline_max_prep_time[2] = {0, 0};
        uint32_t scanline_max_sprites[2] = {0, 0};
        uint32_t vsync_time = 0;
        uint32_t peak_scanline_time = 0;
        uint32_t total_late_scanlines = 0;
        uint32_t available_total_scanline_time = 0;
        uint32_t available_time_per_scanline = 0;
        uint32_t available_vsync_time = 0;
    };
    const Diags& get_diags() const { return diags; }
    void clear_peak_scanline_time() { diags.peak_scanline_time = 0; }
    void clear_late_scanlines();

    // Set this callback to get diags info each frame before it is cleared
    void (*diags_callback)(const Diags&) = nullptr;

    // Defaults to QPI.  If use_spi true then RAM set back to SPI mode for VSYNC.
    void set_spi_mode(bool use_spi) { spi_mode = use_spi; }

    void enable_heartbeat(bool enable) { heartbet_led = enable; }

private:
    friend class Sprite;

    enum ScanlineMode {
        DOUBLE_PIXELS = 1,
        PALETTE = 2,
        RGB888 = 4,
    };

    void main_loop();
    void prepare_scanline_core0(int line_number, uint32_t *pixel_data, uint32_t *tmds_buf, int scanline_mode);
    void prepare_scanline_core1(int line_number, uint32_t *pixel_data, uint32_t *tmds_buf, int scanline_mode);
    void read_two_lines(uint idx);
    void setup_palette();
    void clear_patches();
    void update_sprites();

    FrameDecode frame_data;
    pico_stick::Resolution current_res;

    pimoroni::APS6404 ram;
    struct dvi_inst dvi0;
    struct semaphore dvi_start_sem;

    uint8_t last_bank = 2;
    int frames_to_next_count = 0;
    int frame_counter = 0;
    int line_counter = 0;

    int frame_data_address_offset[NUM_SCROLL_OFFSETS] = {0};
    int next_frame_data_address_offset[NUM_SCROLL_OFFSETS] = {0};

    // Must be as long as the greatest supported frame height.
    pico_stick::FrameTableEntry* frame_table;

    // Patches that require blending, done by CPU
    Sprite::BlendPatch patches[MAX_FRAME_HEIGHT][MAX_PATCHES_PER_LINE];

    // Must be long enough to accept two lines plus one padding word at maximum data length and maximum width
    uint32_t pixel_data[NUM_LINE_BUFFERS / 2][((MAX_FRAME_WIDTH + 1) * 3) / 2];
    uint32_t* pixel_ptr[NUM_LINE_BUFFERS];
    uint32_t line_lengths[2];
    int8_t line_mode[NUM_LINE_BUFFERS];

    Sprite sprites[MAX_SPRITES];

    // Palette TMDS symbol look up tables
    uint32_t tmds_palette_luts[PALETTE_SIZE * PALETTE_SIZE * 12];
    uint32_t* tmds_15bpp_lut = &tmds_palette_luts[PALETTE_SIZE * PALETTE_SIZE * 2];

    // Pixel doubling TMDS LUT
    uint32_t tmds_doubled_palette_lut[PALETTE_SIZE * 3];

    // TMDS buffers.  Better to have them here than rely on dynamic allocation
    uint32_t tmds_buffers[NUM_TMDS_BUFFERS * 3 * MAX_FRAME_WIDTH / DVI_SYMBOLS_PER_WORD];

    Diags diags;

    // Whether the RAM should be in SPI mode for the app processor
    bool spi_mode = false;

    // Whether to output heartbeat LED
    bool heartbet_led = true;
};