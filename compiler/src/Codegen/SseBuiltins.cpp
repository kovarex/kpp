// struct Emitter's members: SSE intrinsics and MSVC builtins; their declarations in
// SseBuiltins.hpp, part of the struct's body in Codegen/Emitter.hpp. A translation unit of its own
// since 2026-11-05 (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::sseTypeByName(const std::string& name) -> const Ty*
{
  if (name.ends_with("_ps")) return types.vectorType(types.floatType(32), 4);
  if (name.ends_with("_pd")) return types.vectorType(types.floatType(64), 2);
  if (name.ends_with("_si128")) return types.vectorType(types.intType(32), 4);
  if (name.ends_with("_epi32") || name.ends_with("_epu32")) return types.vectorType(types.intType(32), 4);
  if (name.ends_with("_epi64")) return types.vectorType(types.intType(64), 2);
  if (name.ends_with("_epi8")) return types.vectorType(types.intType(8), 16);
  return nullptr;
}

auto codegen::Emitter::sseArg(Expr* arg, const Ty* vecTy, Node* at) -> std::string
{
  Value v = rvalue(arg);
  const Ty* vt = v.ty ? types.unqualified(types.stripRef(v.ty)) : nullptr;
  if (vt && vt->kind == TyKind::Class)
  {
    StructInfo& info = structFor(vt, at);
    if (info.fieldDecls.empty())
      unsupported(at, "an SSE operation on " + types.toString(vt, &symbols) + ", a class without a vector member");
    std::string r = newTemp();
    fn += "  " + r + " = load " + llType(vecTy, at) + ", ptr " + gep(info.name, v.text, 0) + "\n"; // the __m128/__m128d/__m128i member
    return r;
  }
  if (!vt || vt->kind != TyKind::Vector)
    unsupported(at, "an SSE operation on " + (vt ? types.toString(vt, &symbols) : std::string("an operand without a type")) + ", which is not a vector");
  if (llType(vt, at) == llType(vecTy, at))
    return v.text;
  std::string r = newTemp();
  fn += "  " + r + " = bitcast " + llType(vt, at) + " " + v.text + " to " + llType(vecTy, at) + "\n";
  return r;
}

auto codegen::Emitter::sseScalar(Expr* arg, const Ty* scalarTy, Node* at) -> std::string
{
  Value v = rvalue(arg);
  return convert(v, scalarTy, at).text;
}

auto codegen::Emitter::sseIntrinsic(const std::string& name, const std::string& retTy, const std::string& paramTys) -> std::string
{
  std::string full = "@" + name;
  if (declaredNames.claim(full))
    decls += "declare " + retTy + " " + full + "(" + paramTys + ")\n";
  return full;
}

auto codegen::Emitter::sseBinOp(const std::string& op, const std::string& tyStr, const std::string& a, const std::string& b) -> std::string
{
  std::string r = newTemp();
  fn += "  " + r + " = " + op + " " + tyStr + " " + a + ", " + b + "\n";
  return r;
}

auto codegen::Emitter::sseCall(const std::string& intrinsic, const std::string& retTy, const std::string& args) -> std::string
{
  std::string r = newTemp();
  fn += "  " + r + " = call " + retTy + " " + intrinsic + "(" + args + ")\n";
  return r;
}

auto codegen::Emitter::sseResult(const std::string& reg, const Ty* vecTy, Node*) -> Value
{
  return {reg, vecTy, false};
}

auto codegen::Emitter::sseImmediate(Expr* e) -> std::optional<int64_t>
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (e && e->kind == NodeKind::CallExpr && static_cast<CallExpr*>(e)->callee->kind == NodeKind::NameExpr)
  {
    auto* call = static_cast<CallExpr*>(e);
    auto* callee = static_cast<NameExpr*>(call->callee);
    std::string name = callee->name.parts.size() == 1 ? sym(callee->name.parts[0].symbol) : std::string();
    if ((name == "_MM_SHUFFLE" && call->args.size() == 4) || (name == "_MM_SHUFFLE2" && call->args.size() == 2))
    {
      int64_t value = 0;
      for (Expr* a : call->args)
      {
        std::optional<int64_t> part = sseImmediate(a);
        if (!part)
          return std::nullopt;
        value = (value << (name == "_MM_SHUFFLE" ? 2 : 1)) | *part;
      }
      return value;
    }
  }
  return evalConst(e, 0);
}

auto codegen::Emitter::sseBuiltin(NameExpr* n, CallExpr* c) -> std::optional<Value>
{
  if (n->name.parts.size() != 1)
    return std::nullopt;
  std::string name = sym(n->name.parts[0].symbol);

  // _MM_SHUFFLE(z, y, x, w) = ((z<<6)|(y<<4)|(x<<2)|w)
  if (name == "_MM_SHUFFLE" && c->args.size() == 4)
  {
    auto e = [&](int i) { return evalConst(c->args[i], 0); };
    auto z = e(0), y = e(1), x = e(2), w = e(3);
    if (z && y && x && w)
      return Value{std::to_string((*z << 6) | (*y << 4) | (*x << 2) | *w), types.intType(32)};
    return std::nullopt;
  }
  if (name == "_MM_SHUFFLE2" && c->args.size() == 2)
  {
    auto a = evalConst(c->args[0], 0), b = evalConst(c->args[1], 0);
    if (a && b)
      return Value{std::to_string((*a << 1) | *b), types.intType(32)};
    return std::nullopt;
  }
  // _MM_GET_ROUNDING_MODE() = (_mm_getcsr() & _MM_ROUND_MASK), a function-like macro
  if (name == "_MM_GET_ROUNDING_MODE" && c->args.empty())
  {
    std::optional<Value> csr = sseMore("_mm_getcsr", c);
    std::string r = newTemp();
    fn += "  " + r + " = and i32 " + csr->text + ", 24576\n"; // 0x6000
    return Value{r, types.intType(32, true)};
  }

  if (!name.starts_with("_mm_"))
    return std::nullopt;

  // Determine the vector type from the name suffix.
  const Ty* v4f = types.vectorType(types.floatType(32), 4);
  const Ty* v2d = types.vectorType(types.floatType(64), 2);
  const Ty* v4i = types.vectorType(types.intType(32), 4);
  const Ty* v2i64 = types.vectorType(types.intType(64), 2);
  const Ty* v16i8 = types.vectorType(types.intType(8), 16);

  auto ll = [&](const Ty* t) { return llType(t, c); };

  // --- undefined (zero args) ---
  if (name == "_mm_undefined_ps")  return sseResult("undef", v4f, c);
  if (name == "_mm_undefined_pd")  return sseResult("undef", v2d, c);
  if (name == "_mm_undefined_si128") return sseResult("undef", v4i, c);
  if (name == "_mm_setzero_si128") return sseResult("zeroinitializer", v4i, c);

  // --- arithmetic: binary float ---
  auto floatBinOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
  {
    if (c->args.size() < 2) return std::nullopt;
    std::string a = sseArg(c->args[0], vt, c);
    std::string b = sseArg(c->args[1], vt, c);
    return sseResult(sseBinOp(op, ll(vt), a, b), vt, c);
  };

  if (name == "_mm_add_ps")  return floatBinOp("fadd", v4f);
  if (name == "_mm_sub_ps")  return floatBinOp("fsub", v4f);
  if (name == "_mm_mul_ps")  return floatBinOp("fmul", v4f);
  if (name == "_mm_div_ps")  return floatBinOp("fdiv", v4f);
  if (name == "_mm_add_pd")  return floatBinOp("fadd", v2d);
  if (name == "_mm_sub_pd")  return floatBinOp("fsub", v2d);
  if (name == "_mm_mul_pd")  return floatBinOp("fmul", v2d);
  if (name == "_mm_div_pd")  return floatBinOp("fdiv", v2d);

  // --- arithmetic: binary integer ---
  auto intBinOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
  {
    if (c->args.size() < 2) return std::nullopt;
    std::string a = sseArg(c->args[0], vt, c);
    std::string b = sseArg(c->args[1], vt, c);
    return sseResult(sseBinOp(op, ll(vt), a, b), vt, c);
  };

  if (name == "_mm_add_epi32") return intBinOp("add", v4i);
  if (name == "_mm_sub_epi32") return intBinOp("sub", v4i);

  // --- bitwise: implemented as integer and/or/xor then bitcast back ---
  auto bitwiseOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
  {
    if (c->args.size() < 2) return std::nullopt;
    // Load as the native type, bitcast to i128 for the operation, bitcast back.
    std::string a = sseArg(c->args[0], vt, c);
    std::string b = sseArg(c->args[1], vt, c);
    // For float types, bitcast to <4 x i32> for the integer operation.
    bool needsCast = (vt == v4f || vt == v2d);
    const Ty* intVt = (vt == v2d) ? v2i64 : v4i;
    std::string ai = a, bi = b;
    if (needsCast)
    {
      ai = newTemp(); fn += "  " + ai + " = bitcast " + ll(vt) + " " + a + " to " + ll(intVt) + "\n";
      bi = newTemp(); fn += "  " + bi + " = bitcast " + ll(vt) + " " + b + " to " + ll(intVt) + "\n";
    }
    std::string ri = sseBinOp(op, ll(intVt), ai, bi);
    std::string res = ri;
    if (needsCast)
    {
      res = newTemp(); fn += "  " + res + " = bitcast " + ll(intVt) + " " + ri + " to " + ll(vt) + "\n";
    }
    return sseResult(res, vt, c);
  };

  if (name == "_mm_and_ps")    return bitwiseOp("and", v4f);
  if (name == "_mm_or_ps")     return bitwiseOp("or", v4f);
  if (name == "_mm_xor_ps")    return bitwiseOp("xor", v4f);
  if (name == "_mm_and_pd")    return bitwiseOp("and", v2d);
  if (name == "_mm_or_pd")     return bitwiseOp("or", v2d);
  if (name == "_mm_xor_pd")    return bitwiseOp("xor", v2d);
  if (name == "_mm_and_si128") return bitwiseOp("and", v4i);
  if (name == "_mm_or_si128")  return bitwiseOp("or", v4i);
  if (name == "_mm_xor_si128") return bitwiseOp("xor", v4i);

  // andnot: ~a & b
  auto andnotOp = [&](const Ty* vt) -> std::optional<Value>
  {
    if (c->args.size() < 2) return std::nullopt;
    std::string a = sseArg(c->args[0], vt, c);
    std::string b = sseArg(c->args[1], vt, c);
    bool needsCast = (vt == v4f || vt == v2d);
    const Ty* intVt = (vt == v2d) ? v2i64 : v4i;
    std::string ai = a, bi = b;
    if (needsCast)
    {
      ai = newTemp(); fn += "  " + ai + " = bitcast " + ll(vt) + " " + a + " to " + ll(intVt) + "\n";
      bi = newTemp(); fn += "  " + bi + " = bitcast " + ll(vt) + " " + b + " to " + ll(intVt) + "\n";
    }
    // NOT a
    std::string notA = newTemp();
    fn += "  " + notA + " = xor " + ll(intVt) + " " + ai + ", <";
    for (uint32_t i = 0; i < intVt->count; ++i) fn += (i ? ", " : "") + std::string(intVt->inner->bits == 64 ? "i64 -1" : "i32 -1");
    fn += ">\n";
    std::string ri = sseBinOp("and", ll(intVt), notA, bi);
    std::string res = ri;
    if (needsCast)
    {
      res = newTemp(); fn += "  " + res + " = bitcast " + ll(intVt) + " " + ri + " to " + ll(vt) + "\n";
    }
    return sseResult(res, vt, c);
  };

  if (name == "_mm_andnot_ps")    return andnotOp(v4f);
  if (name == "_mm_andnot_pd")    return andnotOp(v2d);
  if (name == "_mm_andnot_si128") return andnotOp(v4i);

  // --- float comparisons: fcmp, then select to produce all-ones or all-zeros mask ---
  auto cmpOp = [&](const std::string& pred, const Ty* vt) -> std::optional<Value>
  {
    if (c->args.size() < 2) return std::nullopt;
    std::string a = sseArg(c->args[0], vt, c);
    std::string b = sseArg(c->args[1], vt, c);
    std::string cmp = newTemp();
    std::string boolVt = "<" + std::to_string(vt->count) + " x i1>";
    fn += "  " + cmp + " = fcmp " + pred + " " + ll(vt) + " " + a + ", " + b + "\n";
    // Select: true → all-ones, false → all-zeros (as float bit pattern)
    const Ty* intVt = (vt == v2d) ? v2i64 : v4i;
    std::string allOnes = "<";
    for (uint32_t i = 0; i < intVt->count; ++i) allOnes += (i ? ", " : "") + std::string(intVt->inner->bits == 64 ? "i64 -1" : "i32 -1");
    allOnes += ">";
    std::string sel = newTemp();
    fn += "  " + sel + " = select " + boolVt + " " + cmp + ", " + ll(intVt) + " " + allOnes + ", " + ll(intVt) + " zeroinitializer\n";
    std::string res = newTemp();
    fn += "  " + res + " = bitcast " + ll(intVt) + " " + sel + " to " + ll(vt) + "\n";
    return sseResult(res, vt, c);
  };

  if (name == "_mm_cmpeq_ps")  return cmpOp("oeq", v4f);
  if (name == "_mm_cmpge_ps")  return cmpOp("oge", v4f);
  if (name == "_mm_cmpgt_ps")  return cmpOp("ogt", v4f);
  if (name == "_mm_cmple_ps")  return cmpOp("ole", v4f);
  if (name == "_mm_cmplt_ps")  return cmpOp("olt", v4f);
  if (name == "_mm_cmpneq_ps") return cmpOp("une", v4f);
  if (name == "_mm_cmpeq_pd")  return cmpOp("oeq", v2d);
  if (name == "_mm_cmpge_pd")  return cmpOp("oge", v2d);
  if (name == "_mm_cmpgt_pd")  return cmpOp("ogt", v2d);
  if (name == "_mm_cmple_pd")  return cmpOp("ole", v2d);
  if (name == "_mm_cmplt_pd")  return cmpOp("olt", v2d);
  if (name == "_mm_cmpneq_pd") return cmpOp("une", v2d);

  // --- integer comparisons ---
  auto icmpOp = [&](const std::string& pred, const Ty* vt) -> std::optional<Value>
  {
    if (c->args.size() < 2) return std::nullopt;
    std::string a = sseArg(c->args[0], vt, c);
    std::string b = sseArg(c->args[1], vt, c);
    std::string cmp = newTemp();
    std::string boolVt = "<" + std::to_string(vt->count) + " x i1>";
    fn += "  " + cmp + " = icmp " + pred + " " + ll(vt) + " " + a + ", " + b + "\n";
    std::string sel = newTemp();
    std::string allOnes = "<";
    for (uint32_t i = 0; i < vt->count; ++i) allOnes += (i ? ", " : "") + std::string(vt->inner->bits == 64 ? "i64 -1" : "i32 -1");
    allOnes += ">";
    fn += "  " + sel + " = select " + boolVt + " " + cmp + ", " + ll(vt) + " " + allOnes + ", " + ll(vt) + " zeroinitializer\n";
    return sseResult(sel, vt, c);
  };

  if (name == "_mm_cmpeq_epi32") return icmpOp("eq", v4i);
  if (name == "_mm_cmpgt_epi32") return icmpOp("sgt", v4i);
  if (name == "_mm_cmplt_epi32") return icmpOp("slt", v4i);

  // --- set (splat / build) ---
  if (name == "_mm_set1_ps" && c->args.size() >= 1)
  {
    std::string s = sseScalar(c->args[0], types.floatType(32), c);
    std::string v = newTemp();
    fn += "  " + v + " = insertelement " + ll(v4f) + " undef, float " + s + ", i32 0\n";
    std::string splat = newTemp();
    fn += "  " + splat + " = shufflevector " + ll(v4f) + " " + v + ", " + ll(v4f) + " undef, <4 x i32> zeroinitializer\n";
    return sseResult(splat, v4f, c);
  }
  if (name == "_mm_set1_pd" && c->args.size() >= 1)
  {
    std::string s = sseScalar(c->args[0], types.floatType(64), c);
    std::string v = newTemp();
    fn += "  " + v + " = insertelement " + ll(v2d) + " undef, double " + s + ", i32 0\n";
    std::string splat = newTemp();
    fn += "  " + splat + " = shufflevector " + ll(v2d) + " " + v + ", " + ll(v2d) + " undef, <2 x i32> zeroinitializer\n";
    return sseResult(splat, v2d, c);
  }
  if (name == "_mm_set1_epi32" && c->args.size() >= 1)
  {
    std::string s = sseScalar(c->args[0], types.intType(32), c);
    std::string v = newTemp();
    fn += "  " + v + " = insertelement " + ll(v4i) + " undef, i32 " + s + ", i32 0\n";
    std::string splat = newTemp();
    fn += "  " + splat + " = shufflevector " + ll(v4i) + " " + v + ", " + ll(v4i) + " undef, <4 x i32> zeroinitializer\n";
    return sseResult(splat, v4i, c);
  }
  // _mm_set_ps(d, c, b, a) builds <a, b, c, d> — note reversed order
  if (name == "_mm_set_ps" && c->args.size() >= 4)
  {
    std::string vec = "undef";
    for (int i = 3; i >= 0; --i)
    {
      std::string s = sseScalar(c->args[i], types.floatType(32), c);
      std::string next = newTemp();
      fn += "  " + next + " = insertelement " + ll(v4f) + " " + vec + ", float " + s + ", i32 " + std::to_string(3 - i) + "\n";
      vec = next;
    }
    return sseResult(vec, v4f, c);
  }
  if (name == "_mm_set_pd" && c->args.size() >= 2)
  {
    std::string s0 = sseScalar(c->args[1], types.floatType(64), c);
    std::string s1 = sseScalar(c->args[0], types.floatType(64), c);
    std::string v = newTemp();
    fn += "  " + v + " = insertelement " + ll(v2d) + " undef, double " + s0 + ", i32 0\n";
    std::string v2 = newTemp();
    fn += "  " + v2 + " = insertelement " + ll(v2d) + " " + v + ", double " + s1 + ", i32 1\n";
    return sseResult(v2, v2d, c);
  }
  if (name == "_mm_set_epi32" && c->args.size() >= 4)
  {
    std::string vec = "undef";
    for (int i = 3; i >= 0; --i)
    {
      std::string s = sseScalar(c->args[i], types.intType(32), c);
      std::string next = newTemp();
      fn += "  " + next + " = insertelement " + ll(v4i) + " " + vec + ", i32 " + s + ", i32 " + std::to_string(3 - i) + "\n";
      vec = next;
    }
    return sseResult(vec, v4i, c);
  }

  // --- load (unaligned) ---
  if (name == "_mm_loadu_ps" && c->args.size() >= 1)
  {
    std::string p = rvalue(c->args[0]).text;
    std::string r = newTemp();
    fn += "  " + r + " = load " + ll(v4f) + ", ptr " + p + ", align 1\n";
    return sseResult(r, v4f, c);
  }
  if (name == "_mm_loadu_pd" && c->args.size() >= 1)
  {
    std::string p = rvalue(c->args[0]).text;
    std::string r = newTemp();
    fn += "  " + r + " = load " + ll(v2d) + ", ptr " + p + ", align 1\n";
    return sseResult(r, v2d, c);
  }
  if (name == "_mm_loadu_si128" && c->args.size() >= 1)
  {
    std::string p = rvalue(c->args[0]).text;
    std::string r = newTemp();
    fn += "  " + r + " = load " + ll(v4i) + ", ptr " + p + ", align 1\n";
    return sseResult(r, v4i, c);
  }

  // --- store (unaligned) — returns void ---
  if (name == "_mm_storeu_ps" && c->args.size() >= 2)
  {
    std::string p = rvalue(c->args[0]).text;
    std::string v = sseArg(c->args[1], v4f, c);
    fn += "  store " + ll(v4f) + " " + v + ", ptr " + p + ", align 1\n";
    return Value{"", types.voidType(), false};
  }
  if (name == "_mm_storeu_pd" && c->args.size() >= 2)
  {
    std::string p = rvalue(c->args[0]).text;
    std::string v = sseArg(c->args[1], v2d, c);
    fn += "  store " + ll(v2d) + " " + v + ", ptr " + p + ", align 1\n";
    return Value{"", types.voidType(), false};
  }

  // --- cast (bitcast between vector types, no conversion) ---
  auto castOp = [&](const Ty* from, const Ty* to) -> std::optional<Value>
  {
    if (c->args.size() < 1) return std::nullopt;
    std::string a = sseArg(c->args[0], from, c);
    std::string r = newTemp();
    fn += "  " + r + " = bitcast " + ll(from) + " " + a + " to " + ll(to) + "\n";
    return sseResult(r, to, c);
  };

  if (name == "_mm_castps_si128") return castOp(v4f, v4i);
  if (name == "_mm_castsi128_ps") return castOp(v4i, v4f);
  if (name == "_mm_castps_pd")    return castOp(v4f, v2d);
  if (name == "_mm_castpd_ps")    return castOp(v2d, v4f);
  if (name == "_mm_castsi128_pd") return castOp(v4i, v2d);
  if (name == "_mm_castpd_si128") return castOp(v2d, v4i);

  // --- convert (type conversion) ---
  if (name == "_mm_cvtepi32_ps" && c->args.size() >= 1)
  {
    std::string a = sseArg(c->args[0], v4i, c);
    std::string r = newTemp();
    fn += "  " + r + " = sitofp " + ll(v4i) + " " + a + " to " + ll(v4f) + "\n";
    return sseResult(r, v4f, c);
  }
  if (name == "_mm_cvtps_epi32" && c->args.size() >= 1)
  {
    std::string a = sseArg(c->args[0], v4f, c);
    std::string intr = sseIntrinsic("llvm.x86.sse2.cvtps2dq", ll(v4i), ll(v4f));
    return sseResult(sseCall(intr, ll(v4i), ll(v4f) + " " + a), v4i, c);
  }
  if (name == "_mm_cvtps_pd" && c->args.size() >= 1)
  {
    // Low 2 floats → 2 doubles
    std::string a = sseArg(c->args[0], v4f, c);
    const Ty* v2f = types.vectorType(types.floatType(32), 2);
    std::string lo = newTemp();
    fn += "  " + lo + " = shufflevector " + ll(v4f) + " " + a + ", " + ll(v4f) + " undef, <2 x i32> <i32 0, i32 1>\n";
    std::string r = newTemp();
    fn += "  " + r + " = fpext " + ll(v2f) + " " + lo + " to " + ll(v2d) + "\n";
    return sseResult(r, v2d, c);
  }
  if (name == "_mm_cvtpd_ps" && c->args.size() >= 1)
  {
    std::string a = sseArg(c->args[0], v2d, c);
    const Ty* v2f = types.vectorType(types.floatType(32), 2);
    std::string lo = newTemp();
    fn += "  " + lo + " = fptrunc " + ll(v2d) + " " + a + " to " + ll(v2f) + "\n";
    std::string r = newTemp();
    fn += "  " + r + " = shufflevector " + ll(v2f) + " " + lo + ", " + ll(v2f) + " zeroinitializer, <4 x i32> <i32 0, i32 1, i32 2, i32 3>\n";
    return sseResult(r, v4f, c);
  }

  // --- min / max / sqrt / rcp / rsqrt ---
  auto unaryIntr = [&](const std::string& intr, const Ty* vt) -> std::optional<Value>
  {
    if (c->args.size() < 1) return std::nullopt;
    std::string a = sseArg(c->args[0], vt, c);
    std::string i = sseIntrinsic(intr, ll(vt), ll(vt));
    return sseResult(sseCall(i, ll(vt), ll(vt) + " " + a), vt, c);
  };
  auto binaryIntr = [&](const std::string& intr, const Ty* vt) -> std::optional<Value>
  {
    if (c->args.size() < 2) return std::nullopt;
    std::string a = sseArg(c->args[0], vt, c);
    std::string b = sseArg(c->args[1], vt, c);
    std::string i = sseIntrinsic(intr, ll(vt), ll(vt) + ", " + ll(vt));
    return sseResult(sseCall(i, ll(vt), ll(vt) + " " + a + ", " + ll(vt) + " " + b), vt, c);
  };

  if (name == "_mm_min_ps")  return binaryIntr("llvm.x86.sse.min.ps", v4f);
  if (name == "_mm_max_ps")  return binaryIntr("llvm.x86.sse.max.ps", v4f);
  if (name == "_mm_min_pd")  return binaryIntr("llvm.x86.sse2.min.pd", v2d);
  if (name == "_mm_max_pd")  return binaryIntr("llvm.x86.sse2.max.pd", v2d);
  if (name == "_mm_sqrt_ps") return unaryIntr("llvm.sqrt.v4f32", v4f);
  if (name == "_mm_sqrt_pd") return unaryIntr("llvm.sqrt.v2f64", v2d);
  if (name == "_mm_rcp_ps")  return unaryIntr("llvm.x86.sse.rcp.ps", v4f);
  if (name == "_mm_rsqrt_ps") return unaryIntr("llvm.x86.sse.rsqrt.ps", v4f);

  // --- movemask (returns i32) ---
  if (name == "_mm_movemask_ps" && c->args.size() >= 1)
  {
    std::string a = sseArg(c->args[0], v4f, c);
    std::string i = sseIntrinsic("llvm.x86.sse.movmsk.ps", "i32", ll(v4f));
    return Value{sseCall(i, "i32", ll(v4f) + " " + a), types.intType(32)};
  }
  if (name == "_mm_movemask_pd" && c->args.size() >= 1)
  {
    std::string a = sseArg(c->args[0], v2d, c);
    std::string i = sseIntrinsic("llvm.x86.sse2.movmsk.pd", "i32", ll(v2d));
    return Value{sseCall(i, "i32", ll(v2d) + " " + a), types.intType(32)};
  }
  if (name == "_mm_movemask_epi8" && c->args.size() >= 1)
  {
    std::string a = sseArg(c->args[0], v4i, c);
    std::string ab = newTemp();
    fn += "  " + ab + " = bitcast " + ll(v4i) + " " + a + " to " + ll(v16i8) + "\n";
    std::string i = sseIntrinsic("llvm.x86.sse2.pmovmskb.128", "i32", ll(v16i8));
    return Value{sseCall(i, "i32", ll(v16i8) + " " + ab), types.intType(32)};
  }

  // --- shuffle ---
  if (name == "_mm_shuffle_ps" && c->args.size() >= 3)
  {
    std::string a = sseArg(c->args[0], v4f, c);
    std::string b = sseArg(c->args[1], v4f, c);
    auto imm = sseImmediate(c->args[2]);
    if (!imm) return std::nullopt;
    int m = int(*imm);
    int i0 = m & 3, i1 = (m >> 2) & 3, i2 = ((m >> 4) & 3) + 4, i3 = ((m >> 6) & 3) + 4;
    std::string r = newTemp();
    fn += "  " + r + " = shufflevector " + ll(v4f) + " " + a + ", " + ll(v4f) + " " + b +
          ", <4 x i32> <i32 " + std::to_string(i0) + ", i32 " + std::to_string(i1) + ", i32 " + std::to_string(i2) + ", i32 " + std::to_string(i3) + ">\n";
    return sseResult(r, v4f, c);
  }
  if (name == "_mm_shuffle_pd" && c->args.size() >= 3)
  {
    std::string a = sseArg(c->args[0], v2d, c);
    std::string b = sseArg(c->args[1], v2d, c);
    auto imm = sseImmediate(c->args[2]);
    if (!imm) return std::nullopt;
    int m = int(*imm);
    int i0 = m & 1, i1 = ((m >> 1) & 1) + 2;
    std::string r = newTemp();
    fn += "  " + r + " = shufflevector " + ll(v2d) + " " + a + ", " + ll(v2d) + " " + b +
          ", <2 x i32> <i32 " + std::to_string(i0) + ", i32 " + std::to_string(i1) + ">\n";
    return sseResult(r, v2d, c);
  }
  if (name == "_mm_shuffle_epi32" && c->args.size() >= 2)
  {
    std::string a = sseArg(c->args[0], v4i, c);
    auto imm = sseImmediate(c->args[1]);
    if (!imm) return std::nullopt;
    int m = int(*imm);
    int i0 = m & 3, i1 = (m >> 2) & 3, i2 = (m >> 4) & 3, i3 = (m >> 6) & 3;
    std::string r = newTemp();
    fn += "  " + r + " = shufflevector " + ll(v4i) + " " + a + ", " + ll(v4i) + " undef, <4 x i32> <i32 " +
          std::to_string(i0) + ", i32 " + std::to_string(i1) + ", i32 " + std::to_string(i2) + ", i32 " + std::to_string(i3) + ">\n";
    return sseResult(r, v4i, c);
  }

  // --- unpack (interleave) ---
  auto unpackOp = [&](const Ty* vt, bool high) -> std::optional<Value>
  {
    if (c->args.size() < 2) return std::nullopt;
    std::string a = sseArg(c->args[0], vt, c);
    std::string b = sseArg(c->args[1], vt, c);
    uint32_t half = vt->count / 2;
    uint32_t start = high ? half : 0;
    std::string mask = "<" + std::to_string(vt->count) + " x i32> <";
    for (uint32_t i = 0; i < half; ++i)
    {
      if (i) mask += ", ";
      mask += "i32 " + std::to_string(start + i) + ", i32 " + std::to_string(vt->count + start + i);
    }
    mask += ">";
    std::string r = newTemp();
    fn += "  " + r + " = shufflevector " + ll(vt) + " " + a + ", " + ll(vt) + " " + b + ", " + mask + "\n";
    return sseResult(r, vt, c);
  };

  if (name == "_mm_unpacklo_ps")    return unpackOp(v4f, false);
  if (name == "_mm_unpackhi_ps")    return unpackOp(v4f, true);
  if (name == "_mm_unpacklo_pd")    return unpackOp(v2d, false);
  if (name == "_mm_unpackhi_pd")    return unpackOp(v2d, true);
  if (name == "_mm_unpacklo_epi32") return unpackOp(v4i, false);
  if (name == "_mm_unpackhi_epi32") return unpackOp(v4i, true);
  if (name == "_mm_unpacklo_epi64") return unpackOp(v2i64, false);
  if (name == "_mm_unpackhi_epi64") return unpackOp(v2i64, true);

  // --- shifts ---
  // Immediate shifts: slli, srli, srai
  // (The hardware's: a count past the width gives 0, or the sign. An IR shift of one is poison,
  // which an optimized build turned into anything.)
  auto shiftImmOp = [&](const std::string& intr, const Ty* vt) -> std::optional<Value>
  {
    if (c->args.size() < 2) return std::nullopt;
    std::string a = sseArg(c->args[0], vt, c);
    std::string imm = sseScalar(c->args[1], types.intType(32), c);
    std::string i = sseIntrinsic(intr, ll(vt), ll(vt) + ", i32");
    return sseResult(sseCall(i, ll(vt), ll(vt) + " " + a + ", i32 " + imm), vt, c);
  };

  if (name == "_mm_slli_epi32") return shiftImmOp("llvm.x86.sse2.pslli.d", v4i);
  if (name == "_mm_srli_epi32") return shiftImmOp("llvm.x86.sse2.psrli.d", v4i);
  if (name == "_mm_srai_epi32") return shiftImmOp("llvm.x86.sse2.psrai.d", v4i);

  // Variable shifts (by __m128i count, low 64 bits):
  auto shiftVarOp = [&](const std::string& intr, const Ty* vt) -> std::optional<Value>
  {
    if (c->args.size() < 2) return std::nullopt;
    std::string a = sseArg(c->args[0], vt, c);
    std::string b = sseArg(c->args[1], v4i, c);
    std::string i = sseIntrinsic(intr, ll(vt), ll(vt) + ", " + ll(v4i));
    return sseResult(sseCall(i, ll(vt), ll(vt) + " " + a + ", " + ll(v4i) + " " + b), vt, c);
  };

  if (name == "_mm_sll_epi32") return shiftVarOp("llvm.x86.sse2.psll.d", v4i);
  if (name == "_mm_srl_epi32") return shiftVarOp("llvm.x86.sse2.psrl.d", v4i);
  if (name == "_mm_sra_epi32") return shiftVarOp("llvm.x86.sse2.psra.d", v4i);

  // _mm_srli_si128: byte shift right (psrldq)
  if (name == "_mm_srli_si128" && c->args.size() >= 2)
  {
    std::string a = sseArg(c->args[0], v16i8, c);
    auto imm = sseImmediate(c->args[1]);
    if (!imm || *imm < 0 || *imm > 16) return std::nullopt;
    int shift = int(*imm);
    // Build shuffle mask: shift bytes right, fill with zeros
    std::string mask = "<16 x i32> <";
    for (int i = 0; i < 16; ++i)
    {
      if (i) mask += ", ";
      int src = i + shift;
      mask += "i32 " + std::to_string(src < 16 ? src : 16); // 16+ maps to the zero vector
    }
    mask += ">";
    std::string r = newTemp();
    fn += "  " + r + " = shufflevector " + ll(v16i8) + " " + a + ", " + ll(v16i8) + " zeroinitializer, " + mask + "\n";
    // Bitcast back to v4i
    std::string res = newTemp();
    fn += "  " + res + " = bitcast " + ll(v16i8) + " " + r + " to " + ll(v4i) + "\n";
    return sseResult(res, v4i, c);
  }

  // --- widening multiply ---
  if (name == "_mm_mul_epu32" && c->args.size() >= 2)
  {
    std::string a = sseArg(c->args[0], v4i, c);
    std::string b = sseArg(c->args[1], v4i, c);
    std::string i = sseIntrinsic("llvm.x86.sse2.pmulu.dq", ll(v2i64), ll(v4i) + ", " + ll(v4i));
    return sseResult(sseCall(i, ll(v2i64), ll(v4i) + " " + a + ", " + ll(v4i) + " " + b), v2i64, c);
  }

  return sseMore(name, c);
}

auto codegen::Emitter::sseMore(const std::string& name, CallExpr* c) -> std::optional<Value>
{
  const Ty* v4f = types.vectorType(types.floatType(32), 4);
  const Ty* v2d = types.vectorType(types.floatType(64), 2);
  const Ty* v4i = types.vectorType(types.intType(32), 4);
  const Ty* v2i64 = types.vectorType(types.intType(64), 2);
  const Ty* v8i16 = types.vectorType(types.intType(16), 8);
  const Ty* v16i8 = types.vectorType(types.intType(8), 16);
  const Ty* f32 = types.floatType(32);
  const Ty* f64 = types.floatType(64);
  auto ll = [&](const Ty* t) { return llType(t, c); };
  auto need = [&](size_t n)
  {
    if (c->args.size() < n)
      unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
  };
  auto arg = [&](size_t i, const Ty* vt) { return sseArg(c->args[i], vt, c); };
  auto asInt = [&](const std::string& r, const Ty* lanes) -> Value
  {
    if (lanes == v4i)
      return sseResult(r, v4i, c);
    std::string b = newTemp();
    fn += "  " + b + " = bitcast " + ll(lanes) + " " + r + " to " + ll(v4i) + "\n";
    return sseResult(b, v4i, c);
  };
  auto vecName = [&](const Ty* vt) { return "v" + std::to_string(vt->count) + (vt->inner->kind == TyKind::Float ? "f" : "i") + std::to_string(vt->inner->bits); };
  auto call2 = [&](const std::string& intrinsic, const Ty* ret, const Ty* in, const std::string& a, const std::string& b)
  {
    std::string i = sseIntrinsic(intrinsic, ll(ret), ll(in) + ", " + ll(in));
    return sseCall(i, ll(ret), ll(in) + " " + a + ", " + ll(in) + " " + b);
  };
  auto constArg = [&](size_t i) -> int64_t
  {
    std::optional<int64_t> v = sseImmediate(c->args[i]);
    if (!v)
      unsupported(c, name + " with an operand that is no constant");
    return *v;
  };
  auto laneScalar = [&](const Ty* vt) { return vt->inner; };
  auto build = [&](const Ty* vt, const std::vector<size_t>& order) // lane i from argument order[i]
  {
    std::string vec = "undef";
    for (size_t i = 0; i < order.size(); ++i)
    {
      std::string s = sseScalar(c->args[order[i]], laneScalar(vt), c);
      std::string next = newTemp();
      fn += "  " + next + " = insertelement " + ll(vt) + " " + vec + ", " + ll(laneScalar(vt)) + " " + s + ", i32 " + std::to_string(i) + "\n";
      vec = next;
    }
    return vec;
  };
  auto splat = [&](const Ty* vt)
  {
    std::string s = sseScalar(c->args[0], laneScalar(vt), c);
    std::string one = newTemp(), all = newTemp();
    fn += "  " + one + " = insertelement " + ll(vt) + " undef, " + ll(laneScalar(vt)) + " " + s + ", i32 0\n";
    fn += "  " + all + " = shufflevector " + ll(vt) + " " + one + ", " + ll(vt) + " undef, <" + std::to_string(vt->count) + " x i32> zeroinitializer\n";
    return all;
  };
  auto lowLane = [&](const Ty* vt, const std::string& scalar) // `scalar` in lane 0, the other lanes zero
  {
    std::string r = newTemp();
    fn += "  " + r + " = insertelement " + ll(vt) + " zeroinitializer, " + ll(laneScalar(vt)) + " " + scalar + ", i32 0\n";
    return r;
  };
  auto extract = [&](const Ty* vt, const std::string& v, int lane)
  {
    std::string r = newTemp();
    fn += "  " + r + " = extractelement " + ll(vt) + " " + v + ", i32 " + std::to_string(lane) + "\n";
    return r;
  };
  std::string suffix = name.substr(name.rfind('_') + 1);
  const Ty* lanes = suffix == "epi8" || suffix == "epu8" ? v16i8 : suffix == "epi16" || suffix == "epu16" ? v8i16
                  : suffix == "epi32" || suffix == "epu32" ? v4i : suffix == "epi64" || suffix == "epi64x" ? v2i64 : nullptr;
  bool unsignedLanes = suffix.size() > 2 && suffix[2] == 'u';

  // --- lane arithmetic (epi32 add/sub are sseBuiltin's) ---
  if ((name.starts_with("_mm_add_ep") || name.starts_with("_mm_sub_ep") || name == "_mm_mullo_epi16" || name == "_mm_mullo_epi32") && lanes)
  {
    need(2);
    std::string op = name.starts_with("_mm_add") ? "add" : name.starts_with("_mm_sub") ? "sub" : "mul";
    return asInt(sseBinOp(op, ll(lanes), arg(0, lanes), arg(1, lanes)), lanes);
  }
  if ((name.starts_with("_mm_adds_ep") || name.starts_with("_mm_subs_ep")) && lanes)
  {
    need(2);
    std::string op = std::string(unsignedLanes ? "u" : "s") + (name.starts_with("_mm_adds") ? "add" : "sub") + ".sat";
    return asInt(call2("llvm." + op + "." + vecName(lanes), lanes, lanes, arg(0, lanes), arg(1, lanes)), lanes);
  }
  if ((name.starts_with("_mm_min_ep") || name.starts_with("_mm_max_ep")) && lanes)
  {
    need(2);
    std::string op = std::string(unsignedLanes ? "u" : "s") + (name.starts_with("_mm_min") ? "min" : "max");
    return asInt(call2("llvm." + op + "." + vecName(lanes), lanes, lanes, arg(0, lanes), arg(1, lanes)), lanes);
  }
  if (name == "_mm_mulhi_epi16" || name == "_mm_mulhi_epu16")
  {
    need(2);
    return asInt(call2(name == "_mm_mulhi_epi16" ? "llvm.x86.sse2.pmulh.w" : "llvm.x86.sse2.pmulhu.w", v8i16, v8i16, arg(0, v8i16), arg(1, v8i16)), v8i16);
  }
  if (name == "_mm_madd_epi16")
  {
    need(2);
    return asInt(call2("llvm.x86.sse2.pmadd.wd", v4i, v8i16, arg(0, v8i16), arg(1, v8i16)), v4i);
  }
  if (name == "_mm_sad_epu8")
  {
    need(2);
    return asInt(call2("llvm.x86.sse2.psad.bw", v2i64, v16i8, arg(0, v16i8), arg(1, v16i8)), v2i64);
  }

  // --- packs ---
  if (name == "_mm_packs_epi32")
  {
    need(2);
    return asInt(call2("llvm.x86.sse2.packssdw.128", v8i16, v4i, arg(0, v4i), arg(1, v4i)), v8i16);
  }
  if (name == "_mm_packs_epi16" || name == "_mm_packus_epi16")
  {
    need(2);
    return asInt(call2(name == "_mm_packs_epi16" ? "llvm.x86.sse2.packsswb.128" : "llvm.x86.sse2.packuswb.128", v16i8, v8i16, arg(0, v8i16), arg(1, v8i16)), v16i8);
  }
  if (name == "_mm_packus_epi32")
  {
    need(2);
    return asInt(call2("llvm.x86.sse41.packusdw", v8i16, v4i, arg(0, v4i), arg(1, v4i)), v8i16);
  }

  // --- lane compares: all ones where true (epi32 ones are sseBuiltin's) ---
  if ((name.starts_with("_mm_cmpeq_ep") || name.starts_with("_mm_cmpgt_ep") || name.starts_with("_mm_cmplt_ep")) && lanes && lanes != v4i)
  {
    need(2);
    std::string pred = name.starts_with("_mm_cmpeq") ? "eq" : name.starts_with("_mm_cmpgt") ? "sgt" : "slt";
    std::string cmp = newTemp(), r = newTemp();
    fn += "  " + cmp + " = icmp " + pred + " " + ll(lanes) + " " + arg(0, lanes) + ", " + arg(1, lanes) + "\n";
    fn += "  " + r + " = sext <" + std::to_string(lanes->count) + " x i1> " + cmp + " to " + ll(lanes) + "\n";
    return asInt(r, lanes);
  }

  // --- shifts of 16- and 64-bit lanes by a count (the hardware's: a count past the width gives 0, or the sign) ---
  if ((name == "_mm_slli_epi16" || name == "_mm_srli_epi16" || name == "_mm_srai_epi16" || name == "_mm_slli_epi64" || name == "_mm_srli_epi64"))
  {
    need(2);
    std::string kind = name.substr(4, 4) == "slli" ? "pslli" : name.substr(4, 4) == "srli" ? "psrli" : "psrai";
    std::string intrinsic = "llvm.x86.sse2." + kind + (lanes == v8i16 ? ".w" : ".q");
    std::string i = sseIntrinsic(intrinsic, ll(lanes), ll(lanes) + ", i32");
    return asInt(sseCall(i, ll(lanes), ll(lanes) + " " + arg(0, lanes) + ", i32 " + sseScalar(c->args[1], types.intType(32), c)), lanes);
  }
  if (name == "_mm_slli_si128" || name == "_mm_bslli_si128")
  {
    need(2);
    int64_t shift = constArg(1);
    std::string mask = "<16 x i32> <";
    for (int i = 0; i < 16; ++i)
      mask += (i ? ", i32 " : "i32 ") + std::to_string(i - shift >= 0 && shift <= 15 ? 16 + (i - shift) : 0); // zero vector first, then the bytes moved up
    mask += ">";
    std::string r = newTemp();
    fn += "  " + r + " = shufflevector " + ll(v16i8) + " zeroinitializer, " + ll(v16i8) + " " + arg(0, v16i8) + ", " + mask + "\n";
    return asInt(r, v16i8);
  }

  // --- shuffles and unpacks on 8- and 16-bit lanes ---
  if (name == "_mm_shufflelo_epi16" || name == "_mm_shufflehi_epi16")
  {
    need(2);
    int64_t imm = constArg(1);
    bool high = name == "_mm_shufflehi_epi16";
    std::string mask = "<8 x i32> <";
    for (int i = 0; i < 8; ++i)
    {
      int lane = i;
      if (high == (i >= 4))
        lane = (high ? 4 : 0) + int((imm >> (2 * (i % 4))) & 3);
      mask += (i ? ", i32 " : "i32 ") + std::to_string(lane);
    }
    mask += ">";
    std::string r = newTemp();
    fn += "  " + r + " = shufflevector " + ll(v8i16) + " " + arg(0, v8i16) + ", " + ll(v8i16) + " undef, " + mask + "\n";
    return asInt(r, v8i16);
  }
  if ((name.starts_with("_mm_unpacklo_ep") || name.starts_with("_mm_unpackhi_ep")) && (lanes == v16i8 || lanes == v8i16))
  {
    need(2);
    bool high = name.starts_with("_mm_unpackhi");
    uint32_t n = lanes->count, start = high ? n / 2 : 0;
    std::string mask = "<" + std::to_string(n) + " x i32> <";
    for (uint32_t i = 0; i < n / 2; ++i)
      mask += (i ? ", i32 " : "i32 ") + std::to_string(start + i) + ", i32 " + std::to_string(n + start + i);
    mask += ">";
    std::string r = newTemp();
    fn += "  " + r + " = shufflevector " + ll(lanes) + " " + arg(0, lanes) + ", " + ll(lanes) + " " + arg(1, lanes) + ", " + mask + "\n";
    return asInt(r, lanes);
  }
  if (name == "_mm_insert_epi16")
  {
    need(3);
    std::string r = newTemp();
    fn += "  " + r + " = insertelement " + ll(v8i16) + " " + arg(0, v8i16) + ", i16 " + sseScalar(c->args[1], types.intType(16), c) + ", i32 " +
          std::to_string(constArg(2) & 7) + "\n";
    return asInt(r, v8i16);
  }
  if (name == "_mm_extract_epi16")
  {
    need(2);
    std::string lane = extract(v8i16, arg(0, v8i16), int(constArg(1) & 7)), r = newTemp();
    fn += "  " + r + " = zext i16 " + lane + " to i32\n";
    return Value{r, types.intType(32)};
  }

  // --- conversions ---
  if (name == "_mm_cvtsi128_si32")
  {
    need(1);
    return Value{extract(v4i, arg(0, v4i), 0), types.intType(32)};
  }
  if (name == "_mm_cvtsi128_si64" || name == "_mm_cvtsi128_si64x")
  {
    need(1);
    return Value{extract(v2i64, arg(0, v2i64), 0), types.intType(64)};
  }
  if (name == "_mm_cvtsi32_si128")
  {
    need(1);
    return asInt(lowLane(v4i, sseScalar(c->args[0], types.intType(32), c)), v4i);
  }
  if (name == "_mm_cvtsi64_si128" || name == "_mm_cvtsi64x_si128")
  {
    need(1);
    return asInt(lowLane(v2i64, sseScalar(c->args[0], types.intType(64), c)), v2i64);
  }
  if (name == "_mm_cvtss_f32")
  {
    need(1);
    return Value{extract(v4f, arg(0, v4f), 0), f32};
  }
  if (name == "_mm_cvtsd_f64")
  {
    need(1);
    return Value{extract(v2d, arg(0, v2d), 0), f64};
  }
  if (name == "_mm_cvtss_si32" || name == "_mm_cvt_ss2si" || name == "_mm_cvttss_si32" || name == "_mm_cvtt_ss2si" || name == "_mm_cvtss_si64" ||
      name == "_mm_cvttss_si64" || name == "_mm_cvtsd_si32" || name == "_mm_cvttsd_si32" || name == "_mm_cvtsd_si64" || name == "_mm_cvtsd_si64x" ||
      name == "_mm_cvttsd_si64" || name == "_mm_cvttsd_si64x")
  {
    // The lowest lane to an integer: rounded by the current rounding mode (to nearest even by
    // default), or truncated (cvtt); out of range the integer indefinite (INT_MIN), as the
    // instruction gives, never an fptosi's poison.
    need(1);
    bool single = name.find("ss") != std::string::npos, truncate = name.starts_with("_mm_cvtt"), wide = name.find("si64") != std::string::npos;
    const Ty* in = single ? v4f : v2d;
    std::string intrinsic = std::string(single ? "llvm.x86.sse.cvt" : "llvm.x86.sse2.cvt") + (truncate ? "t" : "") + (single ? "ss2si" : "sd2si") + (wide ? "64" : "");
    std::string iN = wide ? "i64" : "i32";
    return Value{sseCall(sseIntrinsic(intrinsic, iN, ll(in)), iN, ll(in) + " " + arg(0, in)), types.intType(wide ? 64 : 32)};
  }
  if (name == "_mm_cvttps_epi32" || name == "_mm_cvtpd_epi32" || name == "_mm_cvttpd_epi32")
  {
    need(1);
    const Ty* in = name == "_mm_cvttps_epi32" ? v4f : v2d;
    std::string intrinsic = name == "_mm_cvttps_epi32" ? "llvm.x86.sse2.cvttps2dq" : name == "_mm_cvtpd_epi32" ? "llvm.x86.sse2.cvtpd2dq" : "llvm.x86.sse2.cvttpd2dq";
    std::string i = sseIntrinsic(intrinsic, ll(v4i), ll(in));
    return asInt(sseCall(i, ll(v4i), ll(in) + " " + arg(0, in)), v4i);
  }
  if (name == "_mm_cvtepi32_pd")
  {
    need(1);
    std::string low = newTemp(), r = newTemp();
    fn += "  " + low + " = shufflevector " + ll(v4i) + " " + arg(0, v4i) + ", " + ll(v4i) + " undef, <2 x i32> <i32 0, i32 1>\n";
    fn += "  " + r + " = sitofp <2 x i32> " + low + " to " + ll(v2d) + "\n";
    return sseResult(r, v2d, c);
  }

  // --- set ---
  if (name == "_mm_setzero_ps")
    return sseResult("zeroinitializer", v4f, c);
  if (name == "_mm_setzero_pd")
    return sseResult("zeroinitializer", v2d, c);
  if ((name == "_mm_set1_epi8" || name == "_mm_set1_epi16" || name == "_mm_set1_epi64x") && lanes)
  {
    need(1);
    return asInt(splat(lanes), lanes);
  }
  if (name == "_mm_set1_ps1" || name == "_mm_set_ps1")
  {
    need(1);
    return sseResult(splat(v4f), v4f, c);
  }
  if (name == "_mm_set_ss" || name == "_mm_set_sd")
  {
    need(1);
    const Ty* vt = name == "_mm_set_ss" ? v4f : v2d;
    return sseResult(lowLane(vt, sseScalar(c->args[0], laneScalar(vt), c)), vt, c);
  }
  if (name == "_mm_setr_ps" || name == "_mm_setr_pd" || name == "_mm_setr_epi32" || name == "_mm_setr_epi16" || name == "_mm_setr_epi8" ||
      name == "_mm_set_epi16" || name == "_mm_set_epi8")
  {
    const Ty* vt = name == "_mm_setr_ps" ? v4f : name == "_mm_setr_pd" ? v2d : lanes;
    need(vt->count);
    std::vector<size_t> order(vt->count);
    for (size_t i = 0; i < order.size(); ++i)
      order[i] = name.starts_with("_mm_setr") ? i : order.size() - 1 - i; // _mm_set_ lists the highest lane first
    std::string v = build(vt, order);
    return vt->inner->kind == TyKind::Float ? sseResult(v, vt, c) : asInt(v, vt);
  }

  // --- loads and stores ---
  if (name == "_mm_load_ps" || name == "_mm_load_pd" || name == "_mm_load_si128")
  {
    need(1);
    const Ty* vt = name == "_mm_load_ps" ? v4f : name == "_mm_load_pd" ? v2d : v4i;
    std::string r = newTemp();
    fn += "  " + r + " = load " + ll(vt) + ", ptr " + rvalue(c->args[0]).text + ", align 16\n";
    return sseResult(r, vt, c);
  }
  if (name == "_mm_load_ss" || name == "_mm_load_sd" || name == "_mm_loadl_epi64" || name == "_mm_loadu_si32" || name == "_mm_loadu_si64")
  {
    need(1);
    const Ty* vt = name == "_mm_load_ss" ? v4f : name == "_mm_load_sd" ? v2d : name == "_mm_loadu_si32" ? v4i : v2i64;
    std::string s = newTemp();
    fn += "  " + s + " = load " + ll(laneScalar(vt)) + ", ptr " + rvalue(c->args[0]).text + ", align 1\n";
    std::string v = lowLane(vt, s);
    return vt->inner->kind == TyKind::Float ? sseResult(v, vt, c) : asInt(v, vt);
  }
  if (name == "_mm_store_ps" || name == "_mm_store_pd" || name == "_mm_store_si128" || name == "_mm_storeu_si128")
  {
    need(2);
    const Ty* vt = name == "_mm_store_ps" ? v4f : name == "_mm_store_pd" ? v2d : v4i;
    std::string p = rvalue(c->args[0]).text, v = arg(1, vt);
    fn += "  store " + ll(vt) + " " + v + ", ptr " + p + (name == "_mm_storeu_si128" ? ", align 1\n" : ", align 16\n");
    return Value{"", types.voidType(), false};
  }
  if (name == "_mm_store_ss" || name == "_mm_store_sd" || name == "_mm_storel_epi64" || name == "_mm_storeu_si32" || name == "_mm_storeu_si64")
  {
    need(2);
    const Ty* vt = name == "_mm_store_ss" ? v4f : name == "_mm_store_sd" ? v2d : name == "_mm_storeu_si32" ? v4i : v2i64;
    std::string p = rvalue(c->args[0]).text, lane = extract(vt, arg(1, vt), 0);
    fn += "  store " + ll(laneScalar(vt)) + " " + lane + ", ptr " + p + ", align 1\n";
    return Value{"", types.voidType(), false};
  }

  // --- the lowest lane only (the others from the first operand) ---
  if (name == "_mm_add_ss" || name == "_mm_sub_ss" || name == "_mm_mul_ss" || name == "_mm_div_ss" ||
      name == "_mm_add_sd" || name == "_mm_sub_sd" || name == "_mm_mul_sd" || name == "_mm_div_sd")
  {
    need(2);
    const Ty* vt = suffix == "ss" ? v4f : v2d;
    std::string a = arg(0, vt), b = arg(1, vt);
    std::string x = extract(vt, a, 0), y = extract(vt, b, 0);
    std::string op = name.substr(4, 3) == "add" ? "fadd" : name.substr(4, 3) == "sub" ? "fsub" : name.substr(4, 3) == "mul" ? "fmul" : "fdiv";
    std::string v = sseBinOp(op, ll(laneScalar(vt)), x, y), r = newTemp();
    fn += "  " + r + " = insertelement " + ll(vt) + " " + a + ", " + ll(laneScalar(vt)) + " " + v + ", i32 0\n";
    return sseResult(r, vt, c);
  }
  if (name == "_mm_max_ss" || name == "_mm_min_ss" || name == "_mm_max_sd" || name == "_mm_min_sd")
  {
    need(2);
    const Ty* vt = suffix == "ss" ? v4f : v2d;
    std::string intrinsic = std::string("llvm.x86.") + (suffix == "ss" ? "sse." : "sse2.") + name.substr(4, 3) + "." + suffix;
    return sseResult(call2(intrinsic, vt, vt, arg(0, vt), arg(1, vt)), vt, c);
  }
  if (name == "_mm_rcp_ss" || name == "_mm_rsqrt_ss")
  {
    need(1);
    std::string intrinsic = "llvm.x86.sse." + name.substr(4, name.size() - 7) + ".ss";
    std::string i = sseIntrinsic(intrinsic, ll(v4f), ll(v4f));
    return sseResult(sseCall(i, ll(v4f), ll(v4f) + " " + arg(0, v4f)), v4f, c);
  }

  // --- the control register and cache hints ---
  if (name == "_mm_getcsr")
  {
    std::string slot = "%v" + std::to_string(temp++), r = newTemp();
    allocas += "  " + slot + " = alloca i32\n";
    fn += "  call void " + sseIntrinsic("llvm.x86.sse.stmxcsr", "void", "ptr") + "(ptr " + slot + ")\n";
    fn += "  " + r + " = load i32, ptr " + slot + "\n";
    return Value{r, types.intType(32, true)};
  }
  if (name == "_mm_prefetch")
  {
    need(2);
    int64_t hint = constArg(1); // _MM_HINT_T0 3 ... _MM_HINT_NTA 0: the locality
    std::string i = sseIntrinsic("llvm.prefetch.p0", "void", "ptr, i32, i32, i32");
    fn += "  call void " + i + "(ptr " + convert(rvalue(c->args[0]), types.pointer(types.voidType()), c, true).text + ", i32 0, i32 " + // (its `const char*`)
          std::to_string(hint & 3) + ", i32 1)\n";
    return Value{"", types.voidType(), false};
  }
  return std::nullopt; // not an SSE operation the code generator has
}

auto codegen::Emitter::msvcBuiltin(NameExpr* n, CallExpr* c) -> std::optional<Value>
{
  if (n->name.parts.size() != 1)
    return std::nullopt;
  std::string name = sym(n->name.parts[0].symbol);
  auto bitsOf = [&](std::initializer_list<std::pair<const char*, int>> names) {
    for (auto [known, bits] : names)
      if (name == known)
        return bits;
    return 0;
  };
  if (int bits = bitsOf({{"_byteswap_ushort", 16}, {"_byteswap_ulong", 32}, {"_byteswap_uint64", 64}, {"__popcnt16", 16}, {"__popcnt", 32}, {"__popcnt64", 64}}))
  {
    if (c->args.size() != 1)
      unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
    std::string iN = "i" + std::to_string(bits), op = name.starts_with("_byteswap") ? "llvm.bswap." + iN : "llvm.ctpop." + iN;
    std::string v = convert(rvalue(c->args[0]), types.intType(bits, true), c).text;
    return Value{sseCall(sseIntrinsic(op, iN, iN), iN, iN + " " + v), types.intType(bits, true)};
  }
  if (int bits = bitsOf({{"_rotl", 32}, {"_rotr", 32}, {"_lrotl", 32}, {"_lrotr", 32}, {"_rotl64", 64}, {"_rotr64", 64}, {"_rotl8", 8}, {"_rotr8", 8}, {"_rotl16", 16}, {"_rotr16", 16}}))
  {
    // The value rotated by the count modulo its width (the instruction's masking): a funnel
    // shift of the value with itself. The count is an int (sign-extended: -1 rotates the other
    // way), for the 8- and 16-bit forms an unsigned char.
    if (c->args.size() != 2)
      unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
    std::string iN = "i" + std::to_string(bits), op = std::string(name.find("rotl") != std::string::npos ? "llvm.fshl." : "llvm.fshr.") + iN;
    bool small = bits < 32;
    std::string v = convert(rvalue(c->args[0]), types.intType(bits, true), c).text;
    std::string count = convert(rvalue(c->args[1]), small ? types.intType(8, true) : types.intType(32), c).text;
    if (bits == 64 || bits == 16)
    {
      std::string wide = newTemp();
      fn += "  " + wide + " = " + (bits == 64 ? "sext i32 " : "zext i8 ") + count + " to " + iN + "\n";
      count = wide;
    }
    return Value{sseCall(sseIntrinsic(op, iN, iN + ", " + iN + ", " + iN), iN, iN + " " + v + ", " + iN + " " + v + ", " + iN + " " + count),
                 types.intType(bits, true)};
  }
  if (name == "_alloca" || name == "alloca")
  {
    // Stack memory for the rest of the function (StackWalker's buffers): an alloca where the
    // call is, aligned as malloc's (MSVC's _alloca gives 16).
    if (c->args.size() != 1)
      unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
    std::string size = convert(rvalue(c->args[0]), types.intType(64, true), c).text, r = newTemp();
    fn += "  " + r + " = alloca i8, i64 " + size + ", align 16\n";
    return Value{r, types.pointer(types.voidType()), false};
  }
  if (name == "_BitScanReverse" || name == "_BitScanForward" || name == "_BitScanReverse64" || name == "_BitScanForward64")
  {
    if (c->args.size() != 2)
      unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
    bool wide = name.ends_with("64"), reverse = name.starts_with("_BitScanReverse");
    std::string iN = wide ? "i64" : "i32";
    std::string index = convert(rvalue(c->args[0]), types.pointer(types.longType(true)), c).text; // (an unsigned long*)
    std::string mask = convert(rvalue(c->args[1]), types.intType(wide ? 64 : 32, true), c).text;
    std::string count = reverse ? "llvm.ctlz." + iN : "llvm.cttz." + iN;
    if (declaredNames.claim("@" + count))
      decls += "declare " + iN + " @" + count + "(" + iN + ", i1)\n";
    std::string zero = newTemp(), bits = newTemp(), at = newTemp(), at32 = newTemp(), found = newTemp(), r = newTemp();
    fn += "  " + zero + " = icmp eq " + iN + " " + mask + ", 0\n";
    fn += "  " + bits + " = call " + iN + " @" + count + "(" + iN + " " + mask + ", i1 false)\n";
    if (reverse)
      fn += "  " + at + " = sub " + iN + " " + (wide ? "63" : "31") + ", " + bits + "\n";
    else
      fn += "  " + at + " = add " + iN + " " + bits + ", 0\n";
    fn += "  " + at32 + " = " + (wide ? "trunc i64 " : "add i32 ") + at + (wide ? " to i32\n" : ", 0\n");
    // With mask 0 *index is left as it was (MSVC calls it undefined; the instruction leaves it).
    std::string old = newTemp(), kept = newTemp();
    fn += "  " + old + " = load i32, ptr " + index + "\n";
    fn += "  " + kept + " = select i1 " + zero + ", i32 " + old + ", i32 " + at32 + "\n";
    fn += "  store i32 " + kept + ", ptr " + index + "\n";
    fn += "  " + found + " = xor i1 " + zero + ", true\n";
    fn += "  " + r + " = zext i1 " + found + " to i8\n";
    return Value{r, types.intType(8, true)};
  }
  if (name == "__cpuid" || name == "__cpuidex")
  {
    if (c->args.size() != (name == "__cpuid" ? 2u : 3u))
      unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
    std::string info = convert(rvalue(c->args[0]), types.pointer(types.intType(32)), c).text;
    std::string leaf = convert(rvalue(c->args[1]), types.intType(32), c).text;
    std::string sub = name == "__cpuidex" ? convert(rvalue(c->args[2]), types.intType(32), c).text : "0";
    std::string regs = newTemp();
    fn += "  " + regs + " = call { i32, i32, i32, i32 } asm sideeffect \"cpuid\", \"={ax},={bx},={cx},={dx},{ax},{cx},~{dirflag},~{fpsr},~{flags}\"(i32 " +
          leaf + ", i32 " + sub + ")\n";
    for (int i = 0; i < 4; ++i)
    {
      std::string v = newTemp(), slot = newTemp();
      fn += "  " + v + " = extractvalue { i32, i32, i32, i32 } " + regs + ", " + std::to_string(i) + "\n";
      fn += "  " + slot + " = getelementptr inbounds i32, ptr " + info + ", i64 " + std::to_string(i) + "\n";
      fn += "  store i32 " + v + ", ptr " + slot + "\n";
    }
    return Value{"", types.voidType(), false};
  }
  return std::nullopt;
}

auto codegen::Emitter::gccBuiltin(NameExpr* n, CallExpr* c) -> std::optional<Value>
{
  if (n->name.parts.size() != 1)
    return std::nullopt;
  std::string name = sym(n->name.parts[0].symbol);
  auto arguments = [&](size_t least, size_t most)
  {
    if (c->args.size() < least || c->args.size() > most)
      unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
  };
  // cpuid.h's __get_cpuid_max, __get_cpuid and __get_cpuid_count, inline functions whose bodies are
  // inline asm: the cpuid instruction as MSVC's __cpuid is (msvcBuiltin, the subleaf 0 where none is
  // given), without branches. __get_cpuid answers a leaf only when the highest leaf of its range (0
  // or 0x80000000) is at least it; otherwise it gives 0 and the four values stay as they were.
  if (name == "__get_cpuid_max" || name == "__get_cpuid" || name == "__get_cpuid_count")
  {
    bool highestOnly = name == "__get_cpuid_max", withSubleaf = name == "__get_cpuid_count";
    size_t count = highestOnly ? 2 : withSubleaf ? 6 : 5;
    arguments(count, count);
    const Ty* u32 = types.intType(32, true);
    auto cpuid = [&](const std::string& leaf, const std::string& subleaf)
    {
      std::string regs = newTemp();
      fn += "  " + regs + " = call { i32, i32, i32, i32 } asm sideeffect \"cpuid\", \"={ax},={bx},={cx},={dx},{ax},{cx},~{dirflag},~{fpsr},~{flags}\"(i32 " +
            leaf + ", i32 " + subleaf + ")\n";
      std::vector<std::string> out;
      for (int i = 0; i < 4; ++i)
      {
        out.push_back(newTemp());
        fn += "  " + out.back() + " = extractvalue { i32, i32, i32, i32 } " + regs + ", " + std::to_string(i) + "\n";
      }
      return out;
    };
    std::string leaf = convert(rvalue(c->args[0]), u32, c).text;
    if (highestOnly)
    {
      // The highest leaf of the range in eax, and ebx (the vendor's first four letters for leaf 0) in
      // *sig unless sig is null (stored into a slot of its own then).
      std::string sig = convert(rvalue(c->args[1]), types.pointer(u32), c).text;
      std::vector<std::string> regs = cpuid(leaf, "0");
      std::string slot = "%v" + std::to_string(temp++), isNull = newTemp(), to = newTemp();
      allocas += "  " + slot + " = alloca i32\n";
      fn += "  " + isNull + " = icmp eq ptr " + sig + ", null\n";
      fn += "  " + to + " = select i1 " + isNull + ", ptr " + slot + ", ptr " + sig + "\n";
      fn += "  store i32 " + regs[1] + ", ptr " + to + "\n";
      return Value{regs[0], u32};
    }
    std::string subleaf = withSubleaf ? convert(rvalue(c->args[1]), u32, c).text : "0";
    std::vector<std::string> outs;
    for (size_t i = withSubleaf ? 2 : 1; i < c->args.size(); ++i)
      outs.push_back(convert(rvalue(c->args[i]), types.pointer(u32), c).text);
    std::string range = newTemp(), none = newTemp(), below = newTemp(), refused = newTemp();
    fn += "  " + range + " = and i32 " + leaf + ", -2147483648\n";
    std::vector<std::string> highest = cpuid(range, "0");
    fn += "  " + none + " = icmp eq i32 " + highest[0] + ", 0\n";
    fn += "  " + below + " = icmp ult i32 " + highest[0] + ", " + leaf + "\n";
    fn += "  " + refused + " = or i1 " + none + ", " + below + "\n";
    std::vector<std::string> regs = cpuid(leaf, subleaf);
    for (int i = 0; i < 4; ++i)
    {
      std::string old = newTemp(), kept = newTemp();
      fn += "  " + old + " = load i32, ptr " + outs[size_t(i)] + "\n";
      fn += "  " + kept + " = select i1 " + refused + ", i32 " + old + ", i32 " + regs[size_t(i)] + "\n";
      fn += "  store i32 " + kept + ", ptr " + outs[size_t(i)] + "\n";
    }
    std::string answered = newTemp(), r = newTemp();
    fn += "  " + answered + " = xor i1 " + refused + ", true\n";
    fn += "  " + r + " = zext i1 " + answered + " to i32\n";
    return Value{r, types.intType(32)};
  }
  if (!name.starts_with("__builtin_"))
    return std::nullopt;
  std::string base = name.substr(10);
  // clz, ctz and popcount: the operand is an unsigned int, an unsigned long (the target's C long: 32
  // bits on Windows, 64 on LP64 targets) or an unsigned long long by the suffix, the count an int.
  // A zero's clz and ctz are its width (llvm.ctlz and llvm.cttz with is_zero_poison false; GCC
  // leaves them undefined).
  for (const char* prefix : {"clz", "ctz", "popcount"})
  {
    std::string p = prefix;
    if (base.rfind(p, 0) != 0)
      continue;
    std::string suffix = base.substr(p.size());
    const Ty* operand = suffix.empty() ? types.intType(32, true) : suffix == "l" ? types.longType(true) : suffix == "ll" ? types.intType(64, true) : nullptr;
    if (!operand)
      return std::nullopt;
    arguments(1, 1);
    std::string iN = "i" + std::to_string(operand->bits);
    std::string v = convert(rvalue(c->args[0]), operand, c).text;
    std::string op = p == "clz" ? "llvm.ctlz." : p == "ctz" ? "llvm.cttz." : "llvm.ctpop.";
    std::string r = p == "popcount" ? sseCall(sseIntrinsic(op + iN, iN, iN), iN, iN + " " + v)
                                    : sseCall(sseIntrinsic(op + iN, iN, iN + ", i1"), iN, iN + " " + v + ", i1 false");
    if (operand->bits != 32)
    {
      std::string narrow = newTemp();
      fn += "  " + narrow + " = trunc " + iN + " " + r + " to i32\n";
      r = narrow;
    }
    return Value{r, types.intType(32)};
  }
  if (base == "bswap16" || base == "bswap32" || base == "bswap64")
  {
    arguments(1, 1);
    int bits = std::stoi(base.substr(5));
    std::string iN = "i" + std::to_string(bits);
    std::string v = convert(rvalue(c->args[0]), types.intType(uint8_t(bits), true), c).text;
    return Value{sseCall(sseIntrinsic("llvm.bswap." + iN, iN, iN), iN, iN + " " + v), types.intType(uint8_t(bits), true)};
  }
  if (base == "expect")
  {
    // The first argument's value (the second, a constant, only says which value is likely).
    arguments(2, 2);
    Value v = convert(rvalue(c->args[0]), types.longType(), c);
    rvalue(c->args[1]);
    return Value{v.text, types.longType()};
  }
  if (base == "prefetch")
  {
    // The address, then a read (0) or a write (1) and the locality 0 to 3, both constants (0 and 3
    // when left out), as llvm.prefetch takes them.
    arguments(1, 3);
    auto constant = [&](size_t i, int64_t otherwise) -> int64_t
    {
      if (i >= c->args.size())
        return otherwise;
      std::optional<int64_t> k = evalConst(c->args[i], 0);
      if (!k)
        unsupported(c, name + " with an argument that is no constant");
      return *k;
    };
    int64_t write = constant(1, 0), locality = constant(2, 3);
    std::string address = convert(rvalue(c->args[0]), types.pointer(types.voidType()), c, true).text;
    fn += "  call void " + sseIntrinsic("llvm.prefetch.p0", "void", "ptr, i32, i32, i32") + "(ptr " + address + ", i32 " + std::to_string(write & 1) +
          ", i32 " + std::to_string(locality & 3) + ", i32 1)\n";
    return Value{"", types.voidType(), false};
  }
  if (base == "alloca")
  {
    // Stack memory for the rest of the function, as _alloca's (aligned to 16 bytes, as clang's).
    arguments(1, 1);
    std::string size = convert(rvalue(c->args[0]), types.intType(64, true), c).text, r = newTemp();
    fn += "  " + r + " = alloca i8, i64 " + size + ", align 16\n";
    return Value{r, types.pointer(types.voidType()), false};
  }
  return std::nullopt;
}
