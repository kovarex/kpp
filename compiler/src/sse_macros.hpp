#pragma once
// The SSE operations clang's intrinsics headers define as macros, since an operand must be a
// constant (_mm_shuffle_ps's mask): no C import declares them, the code generator builds each by its
// name (Codegen/SseBuiltins.cpp), and the walker types a call to one by the type its macro's cast
// gives, as C++ types it (`(__m128)__builtin_ia32_shufps(...)`), so that a call's other parts see it:
// an overload chosen by it, a template argument deduced from it, a class made of it by its
// constructor. These are the macros of clang 19's xmmintrin.h and emmintrin.h that the code
// generator builds; the walker leaves every `_mm_` and `_MM_` name undeclared without an error, and
// one the code generator does not build is its error.
#include <string_view>

#include "types.hpp"

// The result type of a call to the SSE macro named name, or null when it is none of them.
inline const Ty* sseMacroResult(std::string_view name, TypeTable& types)
{
  const Ty* m128 = types.vectorType(types.floatType(32), 4);  // __m128 (GlobalScope's builtin names)
  const Ty* m128d = types.vectorType(types.floatType(64), 2); // __m128d
  const Ty* m128i = types.vectorType(types.intType(32), 4);   // __m128i
  if (name == "_mm_shuffle_ps")
    return m128;
  if (name == "_mm_shuffle_pd")
    return m128d;
  if (name == "_mm_shuffle_epi32" || name == "_mm_shufflelo_epi16" || name == "_mm_shufflehi_epi16" || name == "_mm_slli_si128" ||
      name == "_mm_bslli_si128" || name == "_mm_srli_si128" || name == "_mm_insert_epi16")
    return m128i;
  if (name == "_mm_extract_epi16" || name == "_MM_SHUFFLE" || name == "_MM_SHUFFLE2")
    return types.intType(32);
  if (name == "_MM_GET_ROUNDING_MODE")
    return types.intType(32, true); // `_mm_getcsr() & _MM_ROUND_MASK`: unsigned int
  if (name == "_mm_prefetch")
    return types.voidType(); // __builtin_prefetch's
  return nullptr;
}
