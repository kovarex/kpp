/* stb_image's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): built
   once with the C++ library (stb.lib, stb_image.cpp) and once converted with it (both with the C
   zlib's zlib.lib, which stb_image_write deflates through), it must print the same bytes. For every
   image the list file names (the core PNGs and JPEG of data\) it prints:
   - stbi_info_from_memory and stbi_is_16_bit_from_memory;
   - the pixels decoded with their own channel count and with 4, flipped on load or not, the 16-bit
     and float loads of every 5th, as a hash (or the failure reason);
   - the same of a copy truncated at a pseudo-random place;
   - for every 3rd image, its pixels written as PNG (compression levels, forced filters, a stride,
     flipped on write), TGA with and without RLE, BMP and JPEG at three qualities, each through a
     write function as its length and hash, and the PNG read back. (Not HDR: this
     stb_image_write's stbi_write_hdr_core reads row i at `data + comp*x*i*x`, past the image from
     the second row on; the game writes no HDR.)

     stb_image LISTFILE > output.txt (harness.cmd stb_image builds and compares both) */
#include <stb_image/stb_image.hpp>
#include <stb_image/stb_image_write.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb.lib compiles stb_image_write as C++, where the header's plain `extern int` variables have C++'s
   names; this driver, in C, uses C's. The linker takes the one for the other, as kpp-native's
   header_only.c did for the game before the swap (the conversion drops the pragmas: the converted
   library defines the variables under C's names). */
#pragma comment(linker, "/alternatename:stbi_write_png_compression_level=?stbi_write_png_compression_level@@3HA")
#pragma comment(linker, "/alternatename:stbi_write_force_png_filter=?stbi_write_force_png_filter@@3HA")
#pragma comment(linker, "/alternatename:stbi_write_tga_with_rle=?stbi_write_tga_with_rle@@3HA")

static unsigned long long state = 0x9E3779B97F4A7C15ull;
static unsigned int nextRandom(void)
{
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (unsigned int)(state >> 16);
}

static unsigned long long fnv1a(const void* data, size_t size)
{
  const unsigned char* bytes = (const unsigned char*)data;
  unsigned long long h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i)
  {
    h ^= bytes[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

/* What a writer gave its write function: the bytes, kept. */
typedef struct
{
  unsigned char* data;
  size_t size, capacity;
} Written;

static void collect(void* context, void* data, int size)
{
  Written* w = (Written*)context;
  if (w->size + size > w->capacity)
  {
    w->capacity = (w->size + size) * 2;
    w->data = (unsigned char*)realloc(w->data, w->capacity);
  }
  memcpy(w->data + w->size, data, size);
  w->size += size;
}

static void report(const char* what, int rc, Written* w)
{
  printf("  %s: %d, %zu bytes %016llx\n", what, rc, w->size, fnv1a(w->data, w->size));
  w->size = 0;
}

/* One load from memory, as a line: its size and channels and the pixels' hash, or why it failed. */
static void load(const char* what, const unsigned char* data, int size, int channels, int flip)
{
  int x = 0, y = 0, comp = 0;
  stbi_set_flip_vertically_on_load(flip);
  unsigned char* pixels = stbi_load_from_memory(data, size, &x, &y, &comp, channels);
  if (!pixels)
    printf("  %s %d%s: failed \"%s\"\n", what, channels, flip ? " flipped" : "", stbi_failure_reason());
  else
    printf("  %s %d%s: %dx%d %d %016llx\n", what, channels, flip ? " flipped" : "", x, y, comp,
           fnv1a(pixels, (size_t)x * y * (channels ? channels : comp)));
  stbi_image_free(pixels);
  stbi_set_flip_vertically_on_load(0);
}

static void writeAll(const unsigned char* data, int size, int index)
{
  int x, y, comp;
  unsigned char* pixels = stbi_load_from_memory(data, size, &x, &y, &comp, 0);
  if (!pixels)
    return;
  Written w = {0};
  static const int levels[] = {1, 8, 3};
  stbi_write_png_compression_level = levels[index % 3];
  stbi_write_force_png_filter = -1;
  report("png", stbi_write_png_to_func(collect, &w, x, y, comp, pixels, x * comp), &w);
  stbi_write_force_png_filter = index % 6;
  report("png filtered", stbi_write_png_to_func(collect, &w, x, y, comp, pixels, x * comp), &w);
  stbi_write_force_png_filter = -1;
  if (y > 1)
    report("png stride", stbi_write_png_to_func(collect, &w, x, y / 2, comp, pixels, x * comp * 2), &w);
  stbi_flip_vertically_on_write(1);
  report("png flipped", stbi_write_png_to_func(collect, &w, x, y, comp, pixels, x * comp), &w);
  stbi_flip_vertically_on_write(0);

  /* the PNG read back */
  stbi_write_png_compression_level = 8;
  stbi_write_png_to_func(collect, &w, x, y, comp, pixels, x * comp);
  int bx, by, bcomp;
  unsigned char* back = stbi_load_from_memory(w.data, (int)w.size, &bx, &by, &bcomp, comp);
  printf("  png back: %dx%d %d %s\n", bx, by, bcomp, back && bx == x && by == y && memcmp(back, pixels, (size_t)x * y * comp) == 0 ? "same" : "DIFFERENT");
  stbi_image_free(back);
  w.size = 0;

  stbi_write_tga_with_rle = 1;
  report("tga rle", stbi_write_tga_to_func(collect, &w, x, y, comp, pixels), &w);
  stbi_write_tga_with_rle = 0;
  report("tga", stbi_write_tga_to_func(collect, &w, x, y, comp, pixels), &w);
  stbi_write_tga_with_rle = 1;
  report("bmp", stbi_write_bmp_to_func(collect, &w, x, y, comp, pixels), &w);
  static const int qualities[] = {50, 90, 100};
  for (int q = 0; q < 3; ++q)
  {
    char what[16];
    snprintf(what, sizeof(what), "jpg %d", qualities[q]);
    report(what, stbi_write_jpg_to_func(collect, &w, x, y, comp, pixels, qualities[q]), &w);
  }
  stbi_image_free(pixels);
  free(w.data);
}

static void image(const char* path, int index)
{
  FILE* f = fopen(path, "rb");
  if (!f)
  {
    printf("%s: cannot open\n", path);
    return;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char* data = (unsigned char*)malloc(size ? size : 1);
  fread(data, 1, size, f);
  fclose(f);

  int x = 0, y = 0, comp = 0;
  int info = stbi_info_from_memory(data, (int)size, &x, &y, &comp);
  printf("%s: %ld bytes, info %d %dx%d %d, 16-bit %d\n", path, size, info, x, y, comp, stbi_is_16_bit_from_memory(data, (int)size));
  load("load", data, (int)size, 0, 0);
  load("load", data, (int)size, 4, index % 2);
  if (index % 5 == 0)
  {
    stbi_us* wide = stbi_load_16_from_memory(data, (int)size, &x, &y, &comp, 0);
    printf("  16: %s %016llx\n", wide ? "ok" : stbi_failure_reason(), wide ? fnv1a(wide, (size_t)x * y * comp * 2) : 0);
    stbi_image_free(wide);
    float* floats = stbi_loadf_from_memory(data, (int)size, &x, &y, &comp, 3);
    printf("  float: %s %016llx\n", floats ? "ok" : stbi_failure_reason(), floats ? fnv1a(floats, (size_t)x * y * 3 * sizeof(float)) : 0);
    stbi_image_free(floats);
  }

  /* damaged copies */
  if (size > 8)
  {
    unsigned char* copy = (unsigned char*)malloc(size);
    memcpy(copy, data, size);
    int cut = 8 + (int)(nextRandom() % (unsigned)(size - 8));
    load("truncated", copy, cut, 4, 0);
    /* (No copy with a byte changed: this stb_image's zlib reads zeros past the end of its data
       instead of failing, so a changed byte in a PNG's IDAT can decode for hours, in C as in kpp:
       left-arrow.png's 387 bytes.) */
    free(copy);
  }
  if (index % 3 == 0)
    writeAll(data, (int)size, index);
  free(data);
}

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    printf("usage: stb_image LISTFILE\n");
    return 2;
  }
  FILE* list = fopen(argv[1], "r");
  if (!list)
  {
    printf("cannot open %s\n", argv[1]);
    return 1;
  }
  char line[4096];
  int index = 0;
  while (fgets(line, sizeof(line), list))
  {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
    if (n > 0)
      image(line, index++);
  }
  fclose(list);
  return 0;
}
