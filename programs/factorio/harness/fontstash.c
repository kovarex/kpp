/* fontstash's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): built
   once with the C++ library (fontstash.lib, fontstash.cpp) and once converted with it, it must print
   the same bytes. For every font the list file names (data\core\fonts) it makes two stashes, one
   plain and one with the game's SDF settings (FontStashFactorio.cpp), with the first font as their
   fallback, and prints:
   - the vertical metrics and line bounds at several sizes;
   - texts in several scripts drawn at sizes, spacings, blurs and alignments: the advance, the text
     bounds, and what the render callbacks got (each atlas update's pixels and each draw's vertices,
     texture coordinates and colours, as hashes);
   - the quads of a text iterator, and fonsTextWrapSize at a width;
   - the atlas when done, its size and the hash of its pixels, the atlas grown when it is full (as the
     game does), and the errors the stash reported.

     fontstash LISTFILE > output.txt (harness.cmd fontstash builds and compares both) */
/* fontstash.h names FONSfont before its typedef, which C++ allows (the game includes it from C++):
   in C the typedef comes first. */
typedef struct FONSfont FONSfont;
#include <fontstash/fontstash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long fnv1a(unsigned long long h, const void* data, size_t size)
{
  const unsigned char* bytes = (const unsigned char*)data;
  for (size_t i = 0; i < size; ++i)
  {
    h ^= bytes[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

/* What the render callbacks got since the last report. */
typedef struct
{
  int width, height;
  int updates, draws, vertices;
  unsigned long long updateHash, drawHash;
  FONScontext* stash;
} Renderer;

static int renderCreate(void* user, int width, int height)
{
  Renderer* r = (Renderer*)user;
  r->width = width;
  r->height = height;
  printf("  create %dx%d\n", width, height);
  return 1;
}

static int renderResize(void* user, int width, int height)
{
  Renderer* r = (Renderer*)user;
  r->width = width;
  r->height = height;
  printf("  resize %dx%d\n", width, height);
  return 1;
}

static void renderUpdate(void* user, int* rect, const unsigned char* data)
{
  Renderer* r = (Renderer*)user;
  ++r->updates;
  r->updateHash = fnv1a(r->updateHash, rect, 4 * sizeof(int));
  for (int y = rect[1]; y < rect[3]; ++y)
    r->updateHash = fnv1a(r->updateHash, data + (size_t)y * r->width + rect[0], (size_t)(rect[2] - rect[0]));
}

static void renderDraw(void* user, const float* verts, const float* tcoords, const unsigned int* colors, int nverts)
{
  Renderer* r = (Renderer*)user;
  ++r->draws;
  r->vertices += nverts;
  r->drawHash = fnv1a(r->drawHash, verts, (size_t)nverts * 2 * sizeof(float));
  r->drawHash = fnv1a(r->drawHash, tcoords, (size_t)nverts * 2 * sizeof(float));
  r->drawHash = fnv1a(r->drawHash, colors, (size_t)nverts * sizeof(unsigned int));
}

static void renderDelete(void* user)
{
  (void)user;
  printf("  delete\n");
}

/* As FontStashFactorio.cpp: a full atlas grows to twice its height, up to 4096. */
static void onError(void* user, int error, int value)
{
  Renderer* r = (Renderer*)user;
  printf("  error %d %d\n", error, value);
  if (error == FONS_ATLAS_FULL)
  {
    int w, h;
    fonsGetAtlasSize(r->stash, &w, &h);
    if (h < 4096)
      printf("  expand %d\n", fonsExpandAtlas(r->stash, w, h * 2));
  }
}

static void report(Renderer* r, const char* what, float advance, const float* bounds, float measured)
{
  printf("  %s: advance %a, bounds %a %a %a %a (%a), %d updates %016llx, %d draws %d vertices %016llx\n", what, advance, bounds[0], bounds[1],
         bounds[2], bounds[3], measured, r->updates, r->updateHash, r->draws, r->vertices, r->drawHash);
  r->updates = r->draws = r->vertices = 0;
  r->updateHash = r->drawHash = 0xCBF29CE484222325ull;
}

static int wrapAt(void* data, float width)
{
  return width > *(float*)data;
}

static const char* texts[] = {
  "Iron gear wheel",
  "The quick brown fox jumps over the lazy dog. 0123456789 +-*/=()[]{}",
  "P\xC5\x99\xC3\xADli\xC5\xA1 \xC5\xBElu\xC5\xA5ou\xC4\x8Dk\xC3\xBD k\xC5\xAF\xC5\x88 \xC3\xBAp\xC4\x9Bl \xC4\x8F\xC3\xA1" "belsk\xC3\xA9 \xC3\xB3" "dy",
  "\xD0\xA1\xD1\x8A\xD0\xB5\xD1\x88\xD1\x8C \xD0\xB5\xD1\x89\xD1\x91 \xD1\x8D\xD1\x82\xD0\xB8\xD1\x85 \xD0\xBC\xD1\x8F\xD0\xB3\xD0\xBA\xD0\xB8\xD1\x85",
  "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88 \xE4\xB8\xAD\xE6\x96\x87",
  "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9 \xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4 \xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC",
  "",
};

static void exercise(FONScontext* stash, Renderer* r, int font, int sdf)
{
  static const float sizes[] = {12.0f, 18.5f, 32.0f, 64.0f};
  static const int aligns[] = {FONS_ALIGN_LEFT | FONS_ALIGN_BASELINE, FONS_ALIGN_CENTER | FONS_ALIGN_MIDDLE, FONS_ALIGN_RIGHT | FONS_ALIGN_TOP,
                               FONS_ALIGN_LEFT | FONS_ALIGN_BOTTOM};
  fonsClearState(stash);
  fonsSetFont(stash, font);
  for (int s = 0; s < 4; ++s)
  {
    fonsSetSize(stash, sizes[s]);
    float ascender, descender, lineh, miny, maxy;
    fonsVertMetrics(stash, &ascender, &descender, &lineh);
    fonsLineBounds(stash, 10.0f, &miny, &maxy);
    printf("  size %a: metrics %a %a %a, line %a %a\n", sizes[s], ascender, descender, lineh, miny, maxy);
    for (int t = 0; t < (int)(sizeof(texts) / sizeof(texts[0])); ++t)
    {
      fonsPushState(stash);
      fonsSetAlign(stash, aligns[(s + t) % 4]);
      fonsSetSpacing(stash, (float)((s + t) % 3) - 0.5f);
      fonsSetColor(stash, 0xFF000000u | (unsigned)(t * 0x1F3D5B));
      if (!sdf)
        fonsSetBlur(stash, (float)((s * 3 + t) % 4));
      float bounds[4] = {0};
      float measured = fonsTextBounds(stash, 3.0f, 40.0f, texts[t], NULL, bounds);
      float advance = fonsDrawText(stash, 3.0f, 40.0f, texts[t], NULL);
      char what[32];
      snprintf(what, sizeof(what), "text %d", t);
      report(r, what, advance, bounds, measured);
      fonsPopState(stash);
    }
    FONStextIter iter;
    FONSquad quad;
    unsigned long long quads = 0xCBF29CE484222325ull;
    int count = 0;
    fonsTextIterInit(stash, &iter, 1.0f, 2.0f, texts[1], NULL);
    while (fonsTextIterNext(stash, &iter, &quad))
    {
      quads = fnv1a(quads, &quad, sizeof(quad));
      ++count;
    }
    float limit = sizes[s] * 6.0f;
    printf("  iterator: %d quads %016llx, wrap at %a: %d\n", count, quads, limit, fonsTextWrapSize(stash, texts[1], NULL, wrapAt, &limit));
  }
  int dirty[4];
  int valid = fonsValidateTexture(stash, dirty);
  int w, h;
  const unsigned char* atlas = fonsGetTextureData(stash, &w, &h);
  printf("  atlas %dx%d %016llx, dirty %d\n", w, h, fnv1a(0xCBF29CE484222325ull, atlas, (size_t)w * h), valid);
}

static unsigned char* readFile(const char* path, int* size)
{
  FILE* f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char* data = (unsigned char*)malloc(n ? n : 1);
  fread(data, 1, n, f);
  fclose(f);
  *size = (int)n;
  return data;
}

static void font(const char* path, const unsigned char* fallback, int fallbackSize)
{
  int size;
  unsigned char* data = readFile(path, &size);
  if (!data)
  {
    printf("%s: cannot open\n", path);
    return;
  }
  printf("%s: %d bytes\n", path, size);
  for (int sdf = 0; sdf < 2; ++sdf)
  {
    Renderer r;
    memset(&r, 0, sizeof(r));
    r.updateHash = r.drawHash = 0xCBF29CE484222325ull;
    FONSparams params;
    memset(&params, 0, sizeof(params));
    params.width = 512;
    params.height = 256;
    params.flags = FONS_ZERO_TOPLEFT;
    params.userPtr = &r;
    params.renderCreate = renderCreate;
    params.renderResize = renderResize;
    params.renderUpdate = renderUpdate;
    params.renderDraw = renderDraw;
    params.renderDelete = renderDelete;
    FONScontext* stash = fonsCreateInternal(&params);
    r.stash = stash;
    fonsSetErrorCallback(stash, onError, &r);
    int id, base;
    if (sdf)
    {
      FONSsdfSettings settings;
      memset(&settings, 0, sizeof(settings));
      settings.sdfEnabled = 1;
      settings.onedgeValue = 127;
      settings.padding = 5;
      settings.pixelDistScale = (float)settings.onedgeValue / (float)settings.padding;
      id = fonsAddFontSdfMem(stash, "font", data, size, 0, settings);
      base = fonsAddFontSdfMem(stash, "fallback", fallback, fallbackSize, 0, settings);
    }
    else
    {
      id = fonsAddFontMem(stash, "font", data, size, 0);
      base = fonsAddFontMem(stash, "fallback", fallback, fallbackSize, 0);
    }
    printf(" %s: font %d, fallback %d, by name %d, added %d\n", sdf ? "sdf" : "plain", id, base, fonsGetFontByName(stash, "fallback"),
           id >= 0 && base >= 0 ? fonsAddFallbackFont(stash, id, base) : -1);
    if (id >= 0)
      exercise(stash, &r, id, sdf);
    fonsDeleteInternal(stash);
  }
  free(data);
}

// The interface's constants, as the converted library's scope has them (design.md 16): value, size
// and signedness, the same in the C and the kpp build.
static void constant(const char* name, long long value, size_t size, int isSigned)
{
  printf("%s = %lld, %u bytes, %s\n", name, value, (unsigned)size, isSigned ? "signed" : "unsigned");
}

// An enumerator's value alone: kpp's sizeof of an enumerator is pending (language/sizeof_enumerator).
static void enumerator(const char* name, long long value)
{
  printf("%s = %lld\n", name, value);
}

static void interface(void)
{
  constant("FONS_INVALID", (long long)FONS_INVALID, sizeof(FONS_INVALID), FONS_INVALID * 0 - 1 < 0);
  constant("FONS_SCRATCH_BUF_SIZE", (long long)FONS_SCRATCH_BUF_SIZE, sizeof(FONS_SCRATCH_BUF_SIZE), FONS_SCRATCH_BUF_SIZE * 0 - 1 < 0);
  constant("FONS_HASH_LUT_SIZE", (long long)FONS_HASH_LUT_SIZE, sizeof(FONS_HASH_LUT_SIZE), FONS_HASH_LUT_SIZE * 0 - 1 < 0);
  constant("FONS_INIT_FONTS", (long long)FONS_INIT_FONTS, sizeof(FONS_INIT_FONTS), FONS_INIT_FONTS * 0 - 1 < 0);
  constant("FONS_INIT_GLYPHS", (long long)FONS_INIT_GLYPHS, sizeof(FONS_INIT_GLYPHS), FONS_INIT_GLYPHS * 0 - 1 < 0);
  constant("FONS_INIT_ATLAS_NODES", (long long)FONS_INIT_ATLAS_NODES, sizeof(FONS_INIT_ATLAS_NODES), FONS_INIT_ATLAS_NODES * 0 - 1 < 0);
  constant("FONS_VERTEX_COUNT", (long long)FONS_VERTEX_COUNT, sizeof(FONS_VERTEX_COUNT), FONS_VERTEX_COUNT * 0 - 1 < 0);
  constant("FONS_MAX_STATES", (long long)FONS_MAX_STATES, sizeof(FONS_MAX_STATES), FONS_MAX_STATES * 0 - 1 < 0);
  constant("FONS_MAX_FALLBACKS", (long long)FONS_MAX_FALLBACKS, sizeof(FONS_MAX_FALLBACKS), FONS_MAX_FALLBACKS * 0 - 1 < 0);
  enumerator("FONS_ZERO_TOPLEFT", (long long)FONS_ZERO_TOPLEFT);
  enumerator("FONS_ZERO_BOTTOMLEFT", (long long)FONS_ZERO_BOTTOMLEFT);
  enumerator("FONS_ALIGN_LEFT", (long long)FONS_ALIGN_LEFT);
  enumerator("FONS_ALIGN_CENTER", (long long)FONS_ALIGN_CENTER);
  enumerator("FONS_ALIGN_RIGHT", (long long)FONS_ALIGN_RIGHT);
  enumerator("FONS_ALIGN_TOP", (long long)FONS_ALIGN_TOP);
  enumerator("FONS_ALIGN_MIDDLE", (long long)FONS_ALIGN_MIDDLE);
  enumerator("FONS_ALIGN_BOTTOM", (long long)FONS_ALIGN_BOTTOM);
  enumerator("FONS_ALIGN_BASELINE", (long long)FONS_ALIGN_BASELINE);
  enumerator("FONS_ATLAS_FULL", (long long)FONS_ATLAS_FULL);
  enumerator("FONS_SCRATCH_FULL", (long long)FONS_SCRATCH_FULL);
  enumerator("FONS_STATES_OVERFLOW", (long long)FONS_STATES_OVERFLOW);
  enumerator("FONS_STATES_UNDERFLOW", (long long)FONS_STATES_UNDERFLOW);
}

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    printf("usage: fontstash LISTFILE\n");
    return 2;
  }
  interface();
  FILE* list = fopen(argv[1], "r");
  if (!list)
  {
    printf("cannot open %s\n", argv[1]);
    return 1;
  }
  char line[4096];
  unsigned char* fallback = NULL;
  int fallbackSize = 0;
  while (fgets(line, sizeof(line), list))
  {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
    if (n == 0)
      continue;
    /* the first font is every stash's fallback */
    if (!fallback)
      fallback = readFile(line, &fallbackSize);
    if (fallback)
      font(line, fallback, fallbackSize);
  }
  fclose(list);
  free(fallback);
  return 0;
}
