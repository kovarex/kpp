// struct Emitter's members: names and IR types; their declarations in EmitterNames.hpp, part of the
// struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05 (codegen.cpp
// split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::qualifiedName(Decl* d) -> const std::string&
{
  auto it = qualifiedNames.find(d);
  if (it == qualifiedNames.end())
    it = qualifiedNames.emplace(d, global.qualifiedName(d)).first;
  return it->second;
}

auto codegen::Emitter::declaredInFunction(const Decl* d) -> bool
{
  for (const Decl* p = d ? d->parent : nullptr; p; p = p->parent)
    if (p->kind == NodeKind::FunctionDecl)
      return true;
  return false;
}

auto codegen::Emitter::llType(const Ty* t, Node* at) -> std::string
{
  if (!t)
    unsupported(at, "an expression without a type");
  switch (t->kind)
  {
    case TyKind::Void: return "void";
    case TyKind::Bool: return "i1";
    case TyKind::Char: case TyKind::Int: return "i" + std::to_string(t->bits);
    case TyKind::Float: return t->bits == 32 ? "float" : "double";
    case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Function:
      return "ptr";
    // A function (`{ ptr, i32 }` with the adjustment of `this` where the class has a base at an
    // offset: memberFunctionPointerAdjusts), or a data member's offset within its class (an i32 in
    // MSVC's ABI, an i64 in Itanium's: memberOffsetType).
    case TyKind::MemberPointer:
      return t->inner && t->inner->kind == TyKind::Function ? (memberFunctionPointerAdjusts(t, at) ? "{ ptr, i32 }" : "ptr") : memberOffsetType();
    case TyKind::Lambda: return t->decl ? closureStructName(t) : "ptr";
    case TyKind::Array:
      return "[" + std::to_string(t->count) + " x " + llType(t->inner, at) + "]";
    case TyKind::Vector:
      return "<" + std::to_string(t->count) + " x " + llType(t->inner, at) + ">";
    case TyKind::Class: return structFor(t, at).name;
    case TyKind::Enum: return llType(enumUnderlying(t->decl), at);
    default:
      unsupported(at, "the type " + types.toString(t, &symbols) + (t->kind == TyKind::Pack ? " (a parameter pack)" : t->kind == TyKind::Param ? " (a template parameter)" : "") +
                        (at ? std::string(" of ") + nodeKindName(at->kind) : std::string()));
  }
}

auto codegen::Emitter::enumUnderlying(Decl* d) -> const Ty*
{
  Decl* def = global.definitionOf(d);
  auto* e = static_cast<EnumDecl*>(def ? def : d);
  if (e->underlying)
    if (const Ty* u = global.typeOfAst(e->underlying, e))
      if (u->isKnown())
        return u;
  return types.intType(32);
}

auto codegen::Emitter::storeZero(const Ty* t, const std::string& addr, Node* at) -> void
{
  // An empty class has no state to clear, and as a base it may have no bytes of its own (laid
  // out with no size: its byte would be the next member's, or past the object).
  if (isClass(t) && structFor(t, at).empty)
    return;
  std::string ty = llType(t, at);
  if (ty[0] == '%' || ty[0] == '[' || ty.compare(0, 2, "<{") == 0)
  {
    if (declaredNames.claim("@llvm.memset.p0.i64"))
      decls += "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n";
    fn += "  call void @llvm.memset.p0.i64(ptr " + addr + ", i8 0, i64 ptrtoint (ptr getelementptr (" + ty + ", ptr null, i32 1) to i64), i1 false)\n";
    return;
  }
  fn += "  store " + ty + " zeroinitializer, ptr " + addr + "\n";
}

auto codegen::Emitter::copyBytes(const std::string& dst, const std::string& src, const Ty* t, Node* at) -> void
{
  std::string ty = llType(t, at);
  if (ty[0] == '%' || ty[0] == '[' || ty.compare(0, 2, "<{") == 0)
  {
    if (declaredNames.claim("@llvm.memcpy.p0.p0.i64"))
      decls += "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n";
    fn += "  call void @llvm.memcpy.p0.p0.i64(ptr " + dst + ", ptr " + src + ", i64 ptrtoint (ptr getelementptr (" + ty + ", ptr null, i32 1) to i64), i1 false)\n";
    return;
  }
  std::string v = newTemp();
  fn += "  " + v + " = load " + ty + ", ptr " + src + "\n";
  fn += "  store " + ty + " " + v + ", ptr " + dst + "\n";
}
