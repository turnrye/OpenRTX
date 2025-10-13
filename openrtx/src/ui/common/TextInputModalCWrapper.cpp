#include "ui/common/TextInputModal.hpp"
#include "ui/common/TextInputModalCWrapper.h"

extern "C" {

static TextInputModal* m17_modal = nullptr;

void m17_modal_create(const char* label, char* buffer, size_t maxLength,
                      textinput_getter_t getter, textinput_setter_t setter) {
    if (m17_modal) delete m17_modal;
    m17_modal = new TextInputModal(
        label,
        [getter]() { return std::string(getter ? getter() : ""); },
        nullptr,
        [setter, buffer]() { if (setter) setter(buffer); },
        maxLength,
        true
    );
}

void m17_modal_draw() {
    if (m17_modal) m17_modal->draw();
}

void m17_modal_destroy() {
    if (m17_modal) { delete m17_modal; m17_modal = nullptr; }
}

}