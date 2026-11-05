// The driver: what a program links besides its objects: the C runtime's libraries, windows-gnu's
// and the Itanium model's runtimes; its declarations in Runtime.hpp (Driver.hpp). A translation
// unit of its own since 2026-11-05 (main.cpp split, stage 2), moved without a change.
#include "Driver.hpp"

namespace driver
{
std::string targetFlags(const Options& options)
{
  return options.target.msvc() ? std::string() : " --target=" + options.target.triple;
}

std::string gnuRuntime(const Options& options)
{
  static const char* def = "LIBRARY ucrtbase.dll\nEXPORTS\n__uncaught_exceptions\nstrtold\nwcstold\n_strtold_l\n";
  static const char* longDouble =
    "/* kpp's long double is a double, as MSVC's: MinGW-w64's long double functions on doubles. */\n"
    "#define F1(f) long double f##l(long double x) { return __builtin_##f(x); }\n"
    "#define F2(f) long double f##l(long double x, long double y) { return __builtin_##f(x, y); }\n"
    "F1(acos) F1(acosh) F1(asin) F1(asinh) F1(atan) F1(atanh) F1(cbrt) F1(ceil) F1(cos) F1(cosh) F1(erf) F1(erfc)\n"
    "F1(exp) F1(exp2) F1(expm1) F1(fabs) F1(floor) F1(lgamma) F1(log) F1(log10) F1(log1p) F1(log2) F1(logb)\n"
    "F1(nearbyint) F1(rint) F1(round) F1(sin) F1(sinh) F1(sqrt) F1(tan) F1(tanh) F1(tgamma) F1(trunc)\n"
    "F2(atan2) F2(copysign) F2(fdim) F2(fmax) F2(fmin) F2(fmod) F2(hypot) F2(nextafter) F2(pow) F2(remainder)\n"
    "long double fmal(long double x, long double y, long double z) { return __builtin_fma(x, y, z); }\n"
    "long double frexpl(long double x, int* e) { return __builtin_frexp(x, e); }\n"
    "long double ldexpl(long double x, int e) { return __builtin_ldexp(x, e); }\n"
    "long double modfl(long double x, long double* i) { double d; double r = __builtin_modf(x, &d); *i = d; return r; }\n"
    "long double nanl(const char* tag) { return __builtin_nan(tag); }\n"
    "long double nexttowardl(long double x, long double y) { return __builtin_nextafter(x, y); }\n"
    "long double remquol(long double x, long double y, int* q) { return __builtin_remquo(x, y, q); }\n"
    "long double scalblnl(long double x, long n) { return __builtin_scalbln(x, n); }\n"
    "long double scalbnl(long double x, int n) { return __builtin_scalbn(x, n); }\n"
    "int ilogbl(long double x) { return __builtin_ilogb(x); }\n"
    "long lrintl(long double x) { return __builtin_lrint(x); }\n"
    "long lroundl(long double x) { return __builtin_lround(x); }\n"
    "long long llrintl(long double x) { return __builtin_llrint(x); }\n"
    "long long llroundl(long double x) { return __builtin_llround(x); }\n"
    "long double _chgsignl(long double x) { return -x; }\n";
  std::filesystem::path tools = std::filesystem::path(clangPath()).parent_path();
  std::string lib = madeOnce("libkpp_ucrt", ".a", def, options, [&](const std::string& in, const std::string& out)
  {
    return "\"" + (tools / "llvm-dlltool.exe").string() + "\" -m i386:x86-64 -d \"" + in + "\" -l \"" + out + "\"";
  });
  std::string object = madeOnce("kpp_long_double", ".o", longDouble, options, [&](const std::string& in, const std::string& out)
  {
    return "\"" + clangPath() + "\"" + targetFlags(options) + " -c -O2 -mlong-double-64 -w -x c \"" + in + "\" -o \"" + out + "\"";
  });
  if (lib.empty() || object.empty())
    return "";
  return " \"" + lib + "\" \"" + object + "\"";
}

std::string madeOnce(const std::string& stem, const std::string& ext, const std::string& text, const Options& options,
                     const std::function<std::string(const std::string&, const std::string&)>& command)
{
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "kpp-imports";
  std::filesystem::create_directories(dir, ec);
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : text + command("", "")) // (the tools' path and flags too)
    hash = (hash ^ c) * 1099511628211ull;
  char hex[32];
  snprintf(hex, sizeof hex, "%016llx", (unsigned long long)hash);
  std::filesystem::path file = dir / (stem + "-" + hex + ext);
  if (std::filesystem::exists(file, ec))
    return file.string();
  std::string own = std::to_string(_getpid());
  std::filesystem::path input = dir / (stem + "-" + own + ".in"), output = dir / (stem + "-" + own + ext);
  if (FILE* f = fopen(input.string().c_str(), "wb"))
  {
    fputs(text.c_str(), f);
    fclose(f);
  }
  int rc = system(("\"" + command(input.string(), output.string()) + "\"").c_str());
  std::filesystem::remove(input, ec);
  if (rc != 0 || !std::filesystem::exists(output, ec))
  {
    fprintf(stderr, "error: could not make %s for %s (exit %d): %s\n", (stem + ext).c_str(), options.target.name.c_str(), rc,
            command(input.string(), output.string()).c_str());
    return "";
  }
  std::filesystem::rename(output, file, ec);
  return ec ? output.string() : file.string();
}

std::string itaniumRuntime(const Options& options)
{
  static const char* personality = R"(/* kpp's personality (main.cpp's itaniumRuntime). */
#include <stdint.h>
typedef struct Header { uint64_t cls; void* cleanup; uint64_t private1, private2; } Header;
extern const uint8_t* _Unwind_GetLanguageSpecificData(void* context);
extern uintptr_t _Unwind_GetRegionStart(void* context);
extern uintptr_t _Unwind_GetIPInfo(void* context, int* beforeInstruction);
extern void _Unwind_SetGR(void* context, int index, uintptr_t value);
extern void _Unwind_SetIP(void* context, uintptr_t value);
extern long write(int fd, const void* bytes, unsigned long count);
extern void abort(void);
extern int64_t kpp_eh_offset(const void* thrown, const void* target) __attribute__((weak));
extern void kpp_eh_terminate(Header* exception, int32_t why) __attribute__((weak));

static void badTable(void)
{
static const char text[] = "kpp: an exception table its personality does not read (main.cpp's itaniumRuntime)\n";
write(2, text, sizeof text - 1);
abort();
}

/* Ends the program from the search, the throw site on the stack: kpp_eh_terminate's report (why: 2 a
 noexcept function, 3 a call its function's table does not list), or, in a program that has no
 kpp_eh_terminate (no throw of its own: another language's exception), a line and abort. */
static void terminate(Header* exception, int32_t why)
{
static const char text[] = "kpp: an exception of another language ended the program (main.cpp's itaniumRuntime)\n";
if (kpp_eh_terminate)
  kpp_eh_terminate(exception, why);
write(2, text, sizeof text - 1);
abort();
}

static uint64_t uleb(const uint8_t** p)
{
uint64_t result = 0;
unsigned shift = 0;
uint8_t byte;
do
{
  byte = *(*p)++;
  result |= (uint64_t)(byte & 0x7f) << shift;
  shift += 7;
} while (byte & 0x80);
return result;
}

static int64_t sleb(const uint8_t** p)
{
uint64_t result = 0;
unsigned shift = 0;
uint8_t byte;
do
{
  byte = *(*p)++;
  result |= (uint64_t)(byte & 0x7f) << shift;
  shift += 7;
} while (byte & 0x80);
if (shift < 64 && (byte & 0x40))
  result |= ~(uint64_t)0 << shift;
return (int64_t)result;
}

/* The type table's entry `index` (1 and up, counted back from its base): udata4 (absolute: a static
 non-PIC link, FreeBSD's), absptr, or indirect pcrel sdata4 (PIC: glibc's). A zero is null, catch
 (...)'s, in every encoding: pc-relative too, where it is no offset. */
static const void* typeEntry(const uint8_t* base, uint8_t encoding, int64_t index)
{
if (encoding == 0x03)
  return (const void*)(uintptr_t)*(const uint32_t*)(base - index * 4);
if (encoding == 0x00)
  return *(const void* const*)(base - index * 8);
if (encoding == 0x9b)
{
  const uint8_t* at = base - index * 4;
  int32_t offset = *(const int32_t*)at;
  return offset ? *(const void* const*)(at + offset) : 0;
}
badTable();
return 0;
}

int __kpp_personality_v0(int version, int actions, uint64_t exceptionClass, Header* exception, void* context)
{
if (version != 1)
  return 3; /* _URC_FATAL_PHASE1_ERROR */
const uint8_t* lsda = _Unwind_GetLanguageSpecificData(context);
if (!lsda)
  return 8; /* _URC_CONTINUE_UNWIND */
int before = 0;
uintptr_t ip = _Unwind_GetIPInfo(context, &before);
if (!before)
  --ip;
uintptr_t start = _Unwind_GetRegionStart(context);
const uint8_t* p = lsda;
if (*p++ != 0xff) /* @LPStart omitted: the landing pads are relative to the function */
  badTable();
uint8_t typeEncoding = *p++;
const uint8_t* typeBase = 0;
if (typeEncoding != 0xff)
{
  uint64_t offset = uleb(&p);
  typeBase = p + offset;
}
if (*p++ != 0x01) /* call sites in uleb128 */
  badTable();
uint64_t length = uleb(&p);
const uint8_t* end = p + length;
const uint8_t* actionTable = end;
uint64_t pad = 0, action = 0;
int listed = 0;
while (p < end)
{
  uint64_t siteStart = uleb(&p), siteLength = uleb(&p), sitePad = uleb(&p), siteAction = uleb(&p);
  if (ip < start + siteStart)
    break;
  if (ip < start + siteStart + siteLength)
  {
    listed = 1;
    pad = sitePad;
    action = siteAction;
    break;
  }
}
/* A call the table does not list may not throw: its frame's cleanups would be skipped, silently.
   C++ terminates there; so does kpp, in the search. A listed call without a landing pad has
   nothing to do in this frame. */
if (!listed)
{
  if (actions & 1)
    terminate(exception, 3);
  return 8;
}
if (!pad)
  return 8;
int kpp = exceptionClass == 0x004b5050004b5050ull; /* "KPP\0KPP\0" */
int64_t selector = 0;
int cleanup = action == 0;
if (action)
{
  const uint8_t* record = actionTable + action - 1;
  for (;;)
  {
    int64_t filter = sleb(&record);
    const uint8_t* nextAt = record;
    int64_t next = sleb(&record);
    if (filter > 0)
    {
      /* A catch clause: null catches everything (catch (...)), a descriptor the thrown class or one
         of its bases. */
      const void* type = typeEntry(typeBase, typeEncoding, filter);
      if (kpp && (!type || kpp_eh_offset(*(const void* const*)((const uint8_t*)exception + 40), type) >= 0))
      {
        selector = filter;
        break;
      }
    }
    else if (filter < 0)
    {
      /* An exception specification: kpp writes only the empty one, a noexcept function's, which no
         exception passes. */
      if (actions & 1)
        terminate(exception, 2);
      selector = filter;
      break;
    }
    else
      cleanup = 1;
    if (!next)
      break;
    record = nextAt + next;
  }
}
if (actions & 1) /* _UA_SEARCH_PHASE */
  return selector > 0 ? 6 : 8; /* _URC_HANDLER_FOUND, _URC_CONTINUE_UNWIND */
if (!(actions & 4)) /* not the handler's frame: its landing pad runs for its cleanups alone */
{
  if (!cleanup)
    return 8;
  selector = 0;
}
_Unwind_SetGR(context, 0, (uintptr_t)exception);
_Unwind_SetGR(context, 1, (uintptr_t)selector);
_Unwind_SetIP(context, start + pad);
return 7; /* _URC_INSTALL_CONTEXT */
}
)";
  std::string object = madeOnce("kpp_personality", ".o", personality, options, [&](const std::string& in, const std::string& out)
  {
    return "\"" + clangPath() + "\"" + targetFlags(options) + " -c -O2 -fPIC -ffunction-sections -fno-stack-protector -w -x c \"" + in + "\" -o \"" + out + "\"";
  });
  return object.empty() ? std::string() : " \"" + object + "\"";
}
}
