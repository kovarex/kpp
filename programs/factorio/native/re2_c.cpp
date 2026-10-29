// The thin C interface the converted game calls RE2 through (design.md: C++ libraries get a thin C
// wrapper): an RE2 object behind a pointer, a match with its submatches as offsets into the text,
// Replace and GlobalReplace, and RE2's own parsers of submatches into numbers. The Kpp side is the
// RE2 class in overrides/src/Util/RegexUtil.kpp. Built by build.cmd into factorio-native.lib,
// linked with the game's re2.lib.
#include <re2/re2.h>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C"
{
  // A compiled pattern (latin1: RE2::Latin1, else UTF-8); kppRe2Ok says whether it compiled.
  void* kppRe2New(const char* pattern, size_t length, int latin1)
  {
    RE2::Options options;
    if (latin1)
      options.set_encoding(RE2::Options::EncodingLatin1);
    return new RE2(re2::StringPiece(pattern, length), options);
  }
  void kppRe2Delete(void* re) { delete static_cast<RE2*>(re); }
  int kppRe2Ok(const void* re) { return static_cast<const RE2*>(re)->ok() ? 1 : 0; }
  int kppRe2GroupCount(const void* re) { return static_cast<const RE2*>(re)->NumberOfCapturingGroups(); }

  // RE2::Match of text[start, end) (anchor: 0 unanchored, 1 at the start, 2 both ends). On a match,
  // offsets[2i] and offsets[2i + 1] are where submatch i (0: the whole match) begins and ends in
  // text, both -1 for a group that took no part, for i < count (at most 32).
  int kppRe2Match(const void* re, const char* text, size_t length, size_t start, size_t end, int anchor, long long* offsets, int count)
  {
    re2::StringPiece submatches[32];
    if (count > 32)
      count = 32;
    if (!static_cast<const RE2*>(re)->Match(re2::StringPiece(text, length), start, end, static_cast<RE2::Anchor>(anchor), submatches, count))
      return 0;
    for (int i = 0; i < count; ++i)
      if (submatches[i].data() == nullptr)
      {
        offsets[2 * i] = -1;
        offsets[2 * i + 1] = -1;
      }
      else
      {
        offsets[2 * i] = submatches[i].data() - text;
        offsets[2 * i + 1] = offsets[2 * i] + (long long)submatches[i].size();
      }
    return 1;
  }

  // RE2::Replace (the first match) or RE2::GlobalReplace (every match) of re in text by rewrite;
  // the result, malloc'ed (kppRe2Free), in *out with its length in *outLength. Returns the number
  // of replacements.
  int kppRe2Replace(const void* re, const char* text, size_t length, const char* rewrite, size_t rewriteLength, int global, char** out,
                    size_t* outLength)
  {
    std::string s(text, length);
    re2::StringPiece r(rewrite, rewriteLength);
    const RE2& pattern = *static_cast<const RE2*>(re);
    int n = global ? RE2::GlobalReplace(&s, pattern, r) : (RE2::Replace(&s, pattern, r) ? 1 : 0);
    *out = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(*out, s.data(), s.size());
    (*out)[s.size()] = 0;
    *outLength = s.size();
    return n;
  }
  void kppRe2Free(void* p) { std::free(p); }

  // RE2::Arg's parsers, as FullMatch applies them to a submatch: integers in base 10 without
  // leading spaces, in range of their type; floating point with leading spaces allowed. 1 when
  // the text parses.
  int kppRe2ParseShort(const char* s, size_t n, short* out) { return RE2::Arg(out).Parse(s, n) ? 1 : 0; }
  int kppRe2ParseUnsignedShort(const char* s, size_t n, unsigned short* out) { return RE2::Arg(out).Parse(s, n) ? 1 : 0; }
  int kppRe2ParseInt(const char* s, size_t n, int* out) { return RE2::Arg(out).Parse(s, n) ? 1 : 0; }
  int kppRe2ParseUnsignedInt(const char* s, size_t n, unsigned int* out) { return RE2::Arg(out).Parse(s, n) ? 1 : 0; }
  int kppRe2ParseLongLong(const char* s, size_t n, long long* out) { return RE2::Arg(out).Parse(s, n) ? 1 : 0; }
  int kppRe2ParseUnsignedLongLong(const char* s, size_t n, unsigned long long* out) { return RE2::Arg(out).Parse(s, n) ? 1 : 0; }
  int kppRe2ParseFloat(const char* s, size_t n, float* out) { return RE2::Arg(out).Parse(s, n) ? 1 : 0; }
  int kppRe2ParseDouble(const char* s, size_t n, double* out) { return RE2::Arg(out).Parse(s, n) ? 1 : 0; }
}
