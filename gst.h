#ifndef GST_H_
#define GST_H_
#include "blastem.h"

uint8_t save_gst(genesis_context * gen, char *fname, uint32_t m68k_pc);
uint32_t load_gst(genesis_context * gen, char * fname);

#endif //GST_H_
