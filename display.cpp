#include <stdio.h>
#include <cstring>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "display.hpp"
#include "hardware/sync.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"

extern "C" {
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"

#include "tmds_encode.h"
}

using namespace pico_stick;

namespace {
    void core1_main() {
        DisplayDriver* driver = (DisplayDriver*)multicore_fifo_pop_blocking();
        driver->run_core1();
    }
}

void DisplayDriver::run_core1() {
	dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (true) {
        sem_acquire_blocking(&dvi_start_sem);
        printf("Core 1 up\n");
        dvi_start(&dvi0);
        while (true) {
            const uint32_t line_counter = multicore_fifo_pop_blocking();
            if (line_counter & 0x80000000u) {
                sprites[line_counter & 0x7F].setup_patches(*this);
            }
            else {
                uint32_t *colourbuf = (uint32_t*)multicore_fifo_pop_blocking();
                if (!colourbuf) break;

                uint32_t *tmdsbuf = (uint32_t*)multicore_fifo_pop_blocking();
                prepare_scanline(line_counter, colourbuf, tmdsbuf);
            }
            multicore_fifo_push_blocking(0);
        }

        // dvi_stop() - needs implementing
    }
    __builtin_unreachable();
}

void DisplayDriver::init() {
    ram.init();

    gpio_init(PIN_HEARTBEAT);
    gpio_put(PIN_HEARTBEAT, 0);
    gpio_set_dir(PIN_HEARTBEAT, GPIO_OUT);

    gpio_init(PIN_VSYNC);
    gpio_put(PIN_VSYNC, 0);
    gpio_set_dir(PIN_VSYNC, GPIO_OUT);

    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
	sem_init(&dvi_start_sem, 0, 1);
	hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);

    patch_lock = spin_lock_instance(next_striped_spin_lock_num());

    // Claim DMA channels
    patch_write_channel = dma_claim_unused_channel(true);
    patch_control_channel = dma_claim_unused_channel(true);
    patch_chain_channel = dma_claim_unused_channel(true);

    // Setup write channel control word - transfer bytes from memory to memory
    dma_channel_config c = dma_channel_get_default_config(patch_write_channel);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_chain_to(&c, patch_chain_channel);
    uint32_t patch_write_channel_ctrl_word = c.ctrl;

    // Setup control channel - transfer into write channel control registers
    c = dma_channel_get_default_config(patch_control_channel);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_ring(&c, true, 4);

    dma_channel_configure(
        patch_control_channel, &c,
        &dma_hw->ch[patch_write_channel].read_addr,
        nullptr,
        4,
        false
    );

    // Setup chain channel - transfer into control channel read address
    c = dma_channel_get_default_config(patch_chain_channel);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);

    dma_channel_configure(
        patch_chain_channel, &c,
        &dma_hw->ch[patch_control_channel].al3_read_addr_trig,
        patch_transfer_control,
        1,
        false
    );

    for (int i = 0; i < MAX_FRAME_HEIGHT; ++i) {
        for (int j = 0; j < MAX_PATCHES_PER_LINE; ++j) {
            patches[i][j].data = nullptr;
            patches[i][j].ctrl = patch_write_channel_ctrl_word;
        }
    }
}

void DisplayDriver::run() {
	multicore_launch_core1(core1_main);
    multicore_fifo_push_blocking(uint32_t(this));

    printf("DVI Initialized\n");
    sem_release(&dvi_start_sem);

    constexpr int num_sprites = MAX_SPRITES;
#if 0
    int16_t x[num_sprites] = { 0, 100, 200, 300, 400 };
    int16_t y[num_sprites] = { 0, 200, 100, 300, 200 };
    int16_t xdir[num_sprites] = { 1, -1, 1, -1, 2 };
    int16_t ydir[num_sprites] = { 1, 1, -1, -1, 1 };
#else
    int32_t x[num_sprites];
    int32_t y[num_sprites];
    int32_t xdir[num_sprites];
    int32_t ydir[num_sprites];
    constexpr int sprite_move_shift = 7;

    for (int i = 0; i < num_sprites; ++i) {
        x[i] = (rand() % 640) << sprite_move_shift;
        y[i] = (rand() % 480) << sprite_move_shift;
        xdir[i] = (rand() % 61) - 30;
        ydir[i] = (rand() % 61) - 30;
    }
#endif

    uint heartbeat = 9;
    while (true) {
        if (++heartbeat >= 32) {
            heartbeat = 0;
            gpio_xor_mask(1u << PIN_HEARTBEAT);
        }

        uint32_t start_time = time_us_32();

        if (!frame_data.read_headers()) {
            // TODO!
            return;
        }
        //printf("%hdx%hd\n", frame_data.config.h_length, frame_data.config.v_length);

        frame_data.get_frame_table(frame_counter, frame_table);

        update_sprites();

        // Read first 2 lines
        line_counter = 0;
        read_two_lines(0);
        ram.wait_for_finish_blocking();
        line_counter = 2;

        printf("VSYNC %luus, late: %d\n", time_us_32() - start_time, dvi0.total_late_scanlines);

        main_loop();

        gpio_put(PIN_VSYNC, 0);

        // Grace period for slow RAM bank switch
        sleep_us(10);

        // Temp: Move our sprites around
        for (int i = 0; i < num_sprites; ++i) {
            x[i] += xdir[i];
            y[i] += ydir[i];
            if (x[i] < (-20 << sprite_move_shift) && xdir[i] < 0) xdir[i] = -xdir[i];
            if (x[i] > (640 << sprite_move_shift) && xdir[i] > 0) xdir[i] = -xdir[i];
            if (y[i] < (-20 << sprite_move_shift) && ydir[i] < 0) ydir[i] = -ydir[i];
            if (y[i] > (480 << sprite_move_shift) && ydir[i] > 0) ydir[i] = -ydir[i];
            if (i < 4)
                set_sprite(i, 4, x[i] >> sprite_move_shift, y[i] >> sprite_move_shift);
            else
                set_sprite(i, ((i + heartbeat) >> 3) & 3, x[i] >> sprite_move_shift, y[i] >> sprite_move_shift);
        }
    }
}

void DisplayDriver::main_loop() {
    uint pixel_data_read_idx = 1;
    while (line_counter < frame_data.config.v_length + 2) {
        if (line_counter < frame_data.config.v_length) {
            // Read two lines into the buffers we just output
            read_two_lines(pixel_data_read_idx);
        }
        else {
            // We are done reading RAM, indicate RAM bank can be switched
            gpio_put(PIN_VSYNC, 1);

            // Use the patch write channel to clear the old patch data
            clear_patches();
        }

        // Flip the buffer index to the one read last time, which is now ready to output
        pixel_data_read_idx ^= 1;

        uint32_t *core0_tmds_buf, *core1_tmds_buf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &core1_tmds_buf);
        sio_hw->fifo_wr = line_counter;
        sio_hw->fifo_wr = uint32_t(pixel_data[pixel_data_read_idx]);
        sio_hw->fifo_wr = uint32_t(core1_tmds_buf);
        __sev();

        const uint32_t core1_line_length = line_lengths[pixel_data_read_idx * 2];
        uint32_t* core0_colour_buf = &pixel_data[pixel_data_read_idx][core1_line_length];

        queue_remove_blocking_u32(&dvi0.q_tmds_free, &core0_tmds_buf);
        prepare_scanline(line_counter + 1, core0_colour_buf, core0_tmds_buf);

        multicore_fifo_pop_blocking();
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &core1_tmds_buf);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &core0_tmds_buf);

        line_counter += 2;
    }
}

void DisplayDriver::set_sprite(int8_t i, int16_t idx, int16_t x, int16_t y) {
    sprites[i].set_sprite_table_idx(idx);
    sprites[i].set_sprite_pos(x, y);
}

void DisplayDriver::move_sprite(int8_t i, int16_t x, int16_t y) {
    sprites[i].set_sprite_pos(x, y);
}

void DisplayDriver::clear_sprite(int8_t i) {
    sprites[i].set_sprite_table_idx(-1);
}

void DisplayDriver::prepare_scanline(int line_number, uint32_t* pixel_data, uint32_t* tmds_buf) {
    tmds_encode_data_channel_fullres_16bpp(pixel_data, tmds_buf, frame_data.config.h_length, 4, 0);
    tmds_encode_data_channel_fullres_16bpp(pixel_data, tmds_buf + (frame_data.config.h_length >> 1), frame_data.config.h_length, 10, 5);
    tmds_encode_data_channel_fullres_16bpp(pixel_data, tmds_buf + frame_data.config.h_length, frame_data.config.h_length, 15, 11);
}    

void DisplayDriver::read_two_lines(uint idx) {
    uint32_t addresses[2];
    uint32_t* patch_ptr = patch_transfer_control;
    uint8_t* pixel_data_ptr = (uint8_t*)pixel_data[idx];
    for (int i = 0; i < 2; ++i) {
        FrameTableEntry& entry = frame_table[line_counter + i];
        addresses[i] = entry.line_address() + frame_data_address_offset;
        const uint32_t line_length = frame_data.config.h_length * get_pixel_data_len(entry.line_mode());
        line_lengths[idx * 2 + i] = line_length >> 2;

        for (int j = 0; j < MAX_PATCHES_PER_LINE; ++j) {
            auto* patch = &patches[line_counter + i][j];
            if (patch->data) {
                *patch_ptr++ = (uint32_t)patch;
            }
            else {
                break;
            }
        }

        pixel_data_ptr += line_length;
    }

    if (num_patches > 0) {
        while (dma_hw->ch[patch_chain_channel].read_addr < (uint32_t)&patch_transfer_control[num_patches + 1]);
    }
    num_patches = patch_ptr - patch_transfer_control;
    if (patch_ptr != patch_transfer_control) {
        *patch_ptr++ = 0;
        dma_hw->ch[patch_chain_channel].read_addr = (uint32_t)patch_transfer_control;
        ram.multi_read(addresses, &line_lengths[idx * 2], 2, pixel_data[idx], patch_chain_channel);
    }
    else {
        ram.multi_read(addresses, &line_lengths[idx * 2], 2, pixel_data[idx]);
    }
}

void DisplayDriver::clear_patches() {
    patch_transfer_control[0] = 0;
    patch_transfer_control[1] = 0;
    patch_transfer_control[2] = 0;
    patch_transfer_control[3] = patches[0][0].ctrl;

    dma_channel_config c = dma_channel_get_default_config(patch_write_channel);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_ring(&c, false, 4);

    dma_channel_configure(
        patch_write_channel, &c,
        patches,
        patch_transfer_control,
        MAX_FRAME_HEIGHT * MAX_PATCHES_PER_LINE * 4,
        true
    );

}

void DisplayDriver::update_sprites() {
    // Wait for patch clear to complete
    dma_channel_wait_for_finish_blocking(patch_write_channel);

    for (int i = 0; i < MAX_SPRITES; i += 2) {
        sprites[i].update_sprite(frame_data);
        multicore_fifo_push_blocking(0x80000000u + i);

        sprites[i+1].update_sprite(frame_data);
        sprites[i+1].setup_patches(*this);

        multicore_fifo_pop_blocking();
    }
}