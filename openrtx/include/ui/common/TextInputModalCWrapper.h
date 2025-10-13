#ifdef __cplusplus
extern "C" {
#endif

typedef const char* (*textinput_getter_t)(void);
typedef void (*textinput_setter_t)(const char*);

void m17_modal_create(const char* label, char* buffer, size_t maxLength,
                      textinput_getter_t getter, textinput_setter_t setter);
void m17_modal_draw(void);
void m17_modal_destroy(void);

#ifdef __cplusplus
}
#endif