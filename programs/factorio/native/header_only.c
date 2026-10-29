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

// Triangulation.cpp's jc_voronoi is in jc_voronoi.cpp: the game's copy uses std::floor and std::ceil.
