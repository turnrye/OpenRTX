#include "ui/common/TextInputModal.hpp"

void TextInputModal::draw() {
    uint16_t rect_width = CONFIG_SCREEN_WIDTH - (layout.horizontal_pad * 2);
    uint16_t rect_height = (CONFIG_SCREEN_HEIGHT - (layout.top_h + layout.bottom_h))/2;
    point_t rect_origin = {(CONFIG_SCREEN_WIDTH - rect_width) / 2,
                            (CONFIG_SCREEN_HEIGHT - rect_height) / 2};
    gfx_drawRect(rect_origin, rect_width, rect_height, color_white, false);
    
    std::string value = getValue();

    gfx_printLine(1, 1, layout.top_h, CONFIG_SCREEN_HEIGHT - layout.bottom_h,
                    layout.horizontal_pad, layout.input_font,
                    TEXT_ALIGN_CENTER, color_white, value.c_str()); // ui_state->new_callsign);
}
