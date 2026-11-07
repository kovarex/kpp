// struct Emitter's members: SSE intrinsics and MSVC builtins: the declarations, part of the
// struct's body, included in it in Codegen/Emitter.hpp; the definitions in SseBuiltins.cpp
// (codegen.cpp split, stage 2, 2026-11-05, moved without a change).
    // SSE intrinsics: _mm_* functions and _MM_SHUFFLE macros.
    // They are not declared; the codegen emits LLVM IR directly for each one.

    // The LLVM vector type for an SSE suffix: ps = <4 x float>, pd = <2 x double>,
    // si128/epi32/epu32/epi8 = <4 x i32> (or <2 x i64>, <16 x i8> as needed).
    const Ty* sseTypeByName(const std::string& name);

    // A SIMD vector argument as a register value of vecTy. A vector value is a register like a
    // scalar (bitcast when its lanes differ: an __m128i of <4 x i32> where <16 x i8> is wanted);
    // an SSE wrapper class (float32x4, float64x2, int32x4) gives its first field.
    std::string sseArg(Expr* arg, const Ty* vecTy, Node* at);

    // Evaluate a scalar argument (float, double, int32_t) as a register value.
    std::string sseScalar(Expr* arg, const Ty* scalarTy, Node* at);

    // Declare an LLVM intrinsic if not already declared, and return its name.
    std::string sseIntrinsic(const std::string& name, const std::string& retTy, const std::string& paramTys);

    // Emit a binary SSE operation: fadd, fsub, fmul, fdiv, add, sub for vectors.
    std::string sseBinOp(const std::string& op, const std::string& tyStr, const std::string& a, const std::string& b);

    // Emit an LLVM call.
    std::string sseCall(const std::string& intrinsic, const std::string& retTy, const std::string& args);

    // A vector result: its register, as any value of a scalar type is (it was a slot's address, which
    // a local or a parameter of type __m128 then stored as the vector itself).
    Value sseResult(const std::string& reg, const Ty* vecTy, Node*);

    // An SSE operation's immediate operand: a constant, or the macros `_MM_SHUFFLE(z, y, x, w)` and
    // `_MM_SHUFFLE2(x, y)`, which the import does not have (function-like) and evalConst cannot call.
    std::optional<int64_t> sseImmediate(Expr* e);

    // An SSE intrinsic's value (sseX86's), and an error for a target with no SSE (arm64): no
    // stand-in of another instruction set's, which a program of the target's says itself (the game's
    // HAS_SSE2 off).
    std::optional<Value> sseBuiltin(NameExpr* n, CallExpr* c);
    std::optional<Value> sseX86(NameExpr* n, CallExpr* c);

    // The SSE operations sseBuiltin does not have: on 8-, 16- and 64-bit lanes, on the lowest lane
    // only (_ss, _sd), packs, saturating and horizontal ones, the set, load and store forms and the
    // conversions. An integer result is an __m128i (<4 x i32>), its lanes bitcast back; nothing for
    // a name that is none of them.
    std::optional<Value> sseMore(const std::string& name, CallExpr* c);

    // MSVC's intrin.h (and stdlib.h) functions that are instructions: _BitScanReverse and
    // _BitScanForward (32 and 64 bits), __cpuid and __cpuidex, the byte swaps, the rotations and
    // __popcnt, as MSVC and clang-cl emit them. Nothing for other names.
    std::optional<Value> msvcBuiltin(NameExpr* n, CallExpr* c);

    // GCC's builtins that C code calls, which the import declares (clang declares them itself, so no
    // header does): __builtin_clz, __builtin_ctz and __builtin_popcount on unsigned int, unsigned
    // long (the target's C long) and unsigned long long, __builtin_bswap16, 32 and 64,
    // __builtin_expect, __builtin_prefetch and __builtin_alloca, as clang emits them. Nothing for
    // other names.
    std::optional<Value> gccBuiltin(NameExpr* n, CallExpr* c);

