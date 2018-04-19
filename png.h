#ifndef PNG_H_
#define PNG_H_

void save_png24(FILE *f, uint32_t *buffer, uint32_t width, uint32_t height, uint32_t pitch);
void save_png(FILE *f, uint32_t *buffer, uint32_t width, uint32_t height, uint32_t pitch);
uint32_t *load_png(uint8_t *buffer, uint32_t buf_size, uint32_t *width, uint32_t *height);

#endif //PNG_H_
