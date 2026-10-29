/* stb_dxt as the converted game imports it (Graphics/Util/DXTCompression): the library's
   declarations, and the internals the game's own compressor calls, which stb_dxt.h has as static
   functions and tables: native/header_only.c builds the library and gives them these C names, and
   the converter writes them for the internals' names. */
#pragma once
#include <stb_image/stb_dxt.h>

void kpp_stb__InitDXT(void);
void kpp_stb__EvalColors(unsigned char* color, unsigned short c0, unsigned short c1);
void kpp_stb__DitherBlock(unsigned char* dest, unsigned char* block);
unsigned int kpp_stb__MatchColorsBlock(unsigned char* block, unsigned char* color, int dither);
void kpp_stb__OptimizeColorsBlock(unsigned char* block, unsigned short* pmax16, unsigned short* pmin16);
int kpp_stb__RefineBlock(unsigned char* block, unsigned short* pmax16, unsigned short* pmin16, unsigned int mask);
void kpp_stb__CompressAlphaBlock(unsigned char* dest, unsigned char* src, int stride);
/* stb__OMatch5 and stb__OMatch6, which kpp_stb__InitDXT fills. */
extern unsigned char (*kpp_stb__OMatch5)[2];
extern unsigned char (*kpp_stb__OMatch6)[2];
