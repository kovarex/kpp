// The header-only C libraries whose implementations the game compiles inside its own .cpp files,
// with the configuration those files give them. The converted .kpp files import the headers, which
// declares the functions; the implementations are built here, into factorio-native.lib.
#include <math.h>
#include <stddef.h>

// BitmapUtil.cpp: stb_image_resize, allocating through the game's stbirCustomMalloc and
// stbirCustomFree (a temporary buffer when there is a context), which the converted BitmapUtil
// exports to C (overrides/src/Graphics/BitmapUtil.kpp).
void* kppStbirMalloc(size_t size, void* context);
void kppStbirFree(void* pointer, void* context);
#define STBIR_MALLOC(size, context) kppStbirMalloc(size, context)
#define STBIR_FREE(pointer, context) kppStbirFree(pointer, context)
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image/stb_image_resize.h>

// BitmapUtil.cpp: the signed distance field generator.
#define SDF_IMPLEMENTATION
#include <sdf/sdf.h>

// TextureProcessor.cpp: stb_rect_pack. The game makes it static to that file (STBRP_STATIC); here
// it is extern, for the converted TextureProcessor.
#define STB_RECT_PACK_IMPLEMENTATION
#include <imgui/stb_rect_pack.h>

// DXTCompression.hpp: stb_dxt, whose static internals the game's own SSE compressor calls (it
// includes the implementation into its header): those it calls get C names here, which
// include/kpp_stb_dxt.h declares and the converter writes for them.
#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include <stb_image/stb_dxt.h>
void kpp_stb__InitDXT(void) { stb__InitDXT(); }
void kpp_stb__EvalColors(unsigned char* color, unsigned short c0, unsigned short c1) { stb__EvalColors(color, c0, c1); }
void kpp_stb__DitherBlock(unsigned char* dest, unsigned char* block) { stb__DitherBlock(dest, block); }
unsigned int kpp_stb__MatchColorsBlock(unsigned char* block, unsigned char* color, int dither) { return stb__MatchColorsBlock(block, color, dither); }
void kpp_stb__OptimizeColorsBlock(unsigned char* block, unsigned short* pmax16, unsigned short* pmin16)
{
  stb__OptimizeColorsBlock(block, pmax16, pmin16);
}
int kpp_stb__RefineBlock(unsigned char* block, unsigned short* pmax16, unsigned short* pmin16, unsigned int mask)
{
  return stb__RefineBlock(block, pmax16, pmin16, mask);
}
void kpp_stb__CompressAlphaBlock(unsigned char* dest, unsigned char* src, int stride) { stb__CompressAlphaBlock(dest, src, stride); }
unsigned char (*kpp_stb__OMatch5)[2] = stb__OMatch5;
unsigned char (*kpp_stb__OMatch6)[2] = stb__OMatch6;

// Triangulation.cpp's jc_voronoi is in jc_voronoi.cpp: the game's copy uses std::floor and std::ceil.
