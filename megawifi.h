#ifndef MEGAWIFI_H_
#define MEGAWIFI_H_

void *megawifi_write_w(uint32_t address, void *context, uint16_t value);
void *megawifi_write_b(uint32_t address, void *context, uint8_t value);
uint16_t megawifi_read_w(uint32_t address, void *context);
uint8_t megawifi_read_b(uint32_t address, void *context);
//Where the module keeps its configuration and flash: prefix.mwcfg and prefix.mwflash.
//Without one they go in the game's save directory, if there is one.
void megawifi_set_storage_prefix(const char *prefix);

#endif //MEGAWIFI_H_
