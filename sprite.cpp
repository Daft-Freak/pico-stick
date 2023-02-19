#include <cstring>
#include <cstdio>

#include "sprite.hpp"
#include "display.hpp"

using namespace pico_stick;

void Sprite::update_sprite(DisplayDriver& disp) {
    if (idx < 0) return;

    disp.frame_data.get_sprite_header(idx, &header);

    //printf("Setup sprite width %d, height %d\n", header.width, header.height);
    lines.resize(header.height);
    data.resize((header.height * header.width * get_pixel_data_len(header.sprite_mode()) + 3) >> 2);
    disp.frame_data.get_sprite(idx, header, lines.data(), data.data());

    for (int i = 0; i < header.height; ++i) {
        const int line_idx = y + i;
        auto& line = lines[i];
        
        uint8_t* pixel_data_ptr = (uint8_t*)disp.pixel_data[(line_idx >> 1) & 1];
        if (line_idx & 1) {
            pixel_data_ptr += get_pixel_data_len(disp.frame_table[line_idx].line_mode()) * disp.frame_data.config.h_length;
        }

        auto* patch = disp.patches[line_idx];
        for (int j = 0; patch->data && j < MAX_PATCHES_PER_LINE - 1; ++j) {
            ++patch;
        }

        int start = x + line.offset;
        int end = start + line.width;
        if (end <= 0) continue;
        if (start >= disp.frame_data.config.h_length) continue;
        if (start < 0) start = 0;
        if (end >= disp.frame_data.config.h_length) end = disp.frame_data.config.h_length;
        
        const int pixel_size = get_pixel_data_len(header.sprite_mode());
        start *= pixel_size;
        end *= pixel_size;

        patch->dest_ptr = pixel_data_ptr + start;
        patch->data = (uint8_t*)data.data() + line.data_start;
        patch->len = end - start;
    }
}
