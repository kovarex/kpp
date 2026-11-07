// struct Emitter's members: functions' names and signatures; their declarations in Functions.hpp,
// part of the struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05
// (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::functionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy) -> const std::string&
{
  NameKey key{f, classTy, args.pairs};
  auto it = functionNames.find(key);
  if (it == functionNames.end())
    it = functionNames.emplace(std::move(key), computeFunctionName(f, args, classTy)).first;
  return it->second;
}

auto codegen::Emitter::withSpecialisationArgs(std::string q, Decl* d) -> std::string
{
  Decl* owner = global.owningClass(d);
  if (!owner || owner->kind != NodeKind::ClassDecl || !(owner->flags & DF_Specialization))
    return q;
  const NamePart& last = static_cast<ClassDecl*>(owner)->name.last();
  std::string spelled;
  for (Node* a : last.templateArgs)
  {
    const Ty* t = nullptr;
    switch (a->kind)
    {
      case NodeKind::NamedType: case NodeKind::PointerType: case NodeKind::LRefType: case NodeKind::RRefType:
      case NodeKind::ConstType: case NodeKind::ArrayType: case NodeKind::FunctionType: case NodeKind::BuiltinType:
      case NodeKind::DecltypeType: case NodeKind::AutoType: case NodeKind::MemberPointerType:
        t = global.typeOfAst(static_cast<Type*>(a), owner);
        break;
      default: break;
    }
    std::string one;
    if (t && t->isKnown())
      one = irString(t);
    else if (owner->file && owner->file->tokens && a->token < owner->file->tokens->size())
    {
      const Token& tok = (*owner->file->tokens)[a->token];
      one = std::string(owner->file->text.substr(tok.offset, tok.length));
    }
    spelled += (spelled.empty() ? "" : ", ") + one;
  }
  size_t pos = q.rfind("::");
  if (pos != std::string::npos)
    q.insert(pos, "<" + spelled + ">");
  return q;
}

auto codegen::Emitter::computeFunctionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy) -> std::string
{
  if (global.isExternC(f))
    return "@" + sym(f->name.last().symbol);
  // The global main (wmain): called by the generated @main (@wmain), which reports uncaught
  // exceptions. A namespace's main is a function like any other (tool::main), as in C++.
  if (f->name.parts.size() == 1 && !f->parent && (sym(f->name.parts[0].symbol) == "main" || sym(f->name.parts[0].symbol) == "wmain") && !global.owningClass(f))
    return "@kpp." + sym(f->name.parts[0].symbol);
  ArgsScope scope(*this, &args);
  std::string q;
  if (classTy && !classTy->args.empty())
  {
    // Class path with the instantiation's arguments, then the member's own name.
    std::string full = qualifiedName(f);
    std::string cls = qualifiedName(classTy->decl);
    q = irString(classTy);
    size_t pos = full.rfind("::");
    q += pos == std::string::npos ? "::" + full : full.substr(pos);
  }
  else
  {
    q = qualifiedName(f);
    if (f->name.parts.size() > 1 && !global.owningClass(f))
    {
      // `int M::twice(int) {}` outside its namespace: the name the declaration in M has.
      std::string prefix;
      for (size_t i = 0; i + 1 < f->name.parts.size(); ++i)
        if (f->name.parts[i].symbol != UINT32_MAX)
          prefix += sym(f->name.parts[i].symbol) + "::";
      size_t pos = q.rfind("::");
      std::string head = pos == std::string::npos ? "" : q.substr(0, pos + 2), tail = pos == std::string::npos ? q : q.substr(pos + 2);
      if (!prefix.empty() && head.find(prefix) == std::string::npos)
        q = head + prefix + tail;
    }
    q = withSpecialisationArgs(q, f);
    if (Decl* owner = global.owningClass(f); owner && owner->kind == NodeKind::ClassDecl && declaredInFunction(owner))
    {
      // A local class's member: the class as types print it (its function and place), with
      // the arguments of the function template instance it is in, then the member's own name;
      // two local classes named Visitor are two classes, and so is one in two instances.
      std::string instance;
      std::vector<std::string> printed;
      for (auto& [d, ty] : args.pairs)
        if (d && d->kind == NodeKind::TemplateParamDecl)
          printed.push_back(sym(static_cast<TemplateParamDecl*>(d)->name) + "=" + (ty ? irString(ty) : std::string("?")));
      std::sort(printed.begin(), printed.end());
      for (const std::string& p : printed)
        instance += (instance.empty() ? "<" : ", ") + p;
      if (!instance.empty())
        instance += ">";
      size_t pos = q.rfind("::");
      q = irString(classTy ? classTy : types.classType(owner, {})) + instance + (pos == std::string::npos ? "::" + q : q.substr(pos));
    }
  }
  if (!f->templateParams.empty())
  {
    q += "<";
    for (size_t i = 0; i < f->templateParams.size(); ++i)
    {
      const Ty* t = nullptr;
      for (auto& [d, ty] : args.pairs)
        if (d == f->templateParams[i])
          t = ty;
      q += (i ? ", " : "") + (t ? irString(t) : std::string("?"));
    }
    q += ">";
  }
  else if ((f->flags & DF_Specialization) && f->isTemplate)
  {
    // `template<> Vector<String> splitWords(StringView)`: the explicit arguments, or the return
    // type when the parameter appears only there, tell the specialisations of one template apart.
    std::string spelled;
    for (Node* a : f->name.last().templateArgs)
    {
      const Ty* t = uint8_t(a->kind) <= uint8_t(NodeKind::PackType) ? global.typeOfAst(static_cast<Type*>(a), f) : nullptr;
      spelled += (spelled.empty() ? "" : ", ") + (t && t->isKnown() ? irString(t) : std::string("?"));
    }
    if (spelled.empty())
      if (const Ty* ft = declType(f); ft && ft->kind == TyKind::Function && ft->inner)
        spelled = irString(ft->inner);
    q += "<" + spelled + ">";
  }
  const Ty* ft = declType(f);
  std::string params;
  for (size_t i = 0; ft && i < ft->args.size(); ++i)
    params += (i ? ", " : "") + irString(ft->args[i]);
  // A const method is a different function from its non-const overload, and one qualified && from
  // its overload qualified & (`which() &` beside `which() &&`; & and none never meet: [over.load] 2.3).
  // (a literal operator's name, `operator""_km`, has quotes, which an LLVM name escapes)
  for (size_t at = 0; (at = q.find('"', at)) != std::string::npos; at += 3)
    q.replace(at, 1, "\\22");
  return "@\"" + q + "(" + params + ")" + ((f->flags & DF_Const) && !(f->flags & DF_Static) ? " const" : "") + ((f->flags & DF_Volatile) && !(f->flags & DF_Static) ? " volatile" : "") +
         (f->refQualifier == FunctionDecl::RefQualifier::RValue && !(f->flags & DF_Static) ? " &&" : "") + "\"";
}

auto codegen::Emitter::displayFunctionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy) -> std::string
{
  ArgsScope scope(*this, &args);
  auto spell = [&](const Ty* t) { return t ? types.toString(t, &symbols) : std::string("?"); };
  std::string q;
  Decl* owner = global.owningClass(f);
  if (global.isExternC(f))
    q = sym(f->name.last().symbol);
  else if (owner && owner->kind == NodeKind::ClassDecl)
  {
    // A member: its class as types print it (an instance's arguments, a local class's place),
    // then the member's own name.
    std::string full = qualifiedName(f);
    size_t pos = full.rfind("::");
    q = spell(classTy ? classTy : types.classType(owner, {})) + (pos == std::string::npos ? "::" + full : full.substr(pos));
  }
  else
  {
    q = qualifiedName(f);
    if (f->name.parts.size() > 1)
    {
      // `int M::twice(int) {}` outside its namespace: the name the declaration in M has.
      std::string prefix;
      for (size_t i = 0; i + 1 < f->name.parts.size(); ++i)
        if (f->name.parts[i].symbol != UINT32_MAX)
          prefix += sym(f->name.parts[i].symbol) + "::";
      size_t pos = q.rfind("::");
      std::string head = pos == std::string::npos ? "" : q.substr(0, pos + 2), tail = pos == std::string::npos ? q : q.substr(pos + 2);
      if (!prefix.empty() && head.find(prefix) == std::string::npos)
        q = head + prefix + tail;
    }
    q = withSpecialisationArgs(q, f);
  }
  if (!f->templateParams.empty())
  {
    q += "<";
    for (size_t i = 0; i < f->templateParams.size(); ++i)
    {
      const Ty* t = nullptr;
      for (auto& [d, ty] : args.pairs)
        if (d == f->templateParams[i])
          t = ty;
      q += (i ? ", " : "") + spell(t);
    }
    q += ">";
  }
  else if ((f->flags & DF_Specialization) && f->isTemplate)
  {
    // (an explicit specialisation's arguments, as computeFunctionName tells them apart)
    std::string spelled;
    for (Node* a : f->name.last().templateArgs)
    {
      const Ty* t = uint8_t(a->kind) <= uint8_t(NodeKind::PackType) ? global.typeOfAst(static_cast<Type*>(a), f) : nullptr;
      spelled += (spelled.empty() ? "" : ", ") + (t && t->isKnown() ? spell(t) : std::string("?"));
    }
    if (spelled.empty())
      if (const Ty* ft = declType(f); ft && ft->kind == TyKind::Function && ft->inner)
        spelled = spell(ft->inner);
    q += "<" + spelled + ">";
  }
  const Ty* ft = declType(f);
  std::string params;
  for (size_t i = 0; ft && i < ft->args.size(); ++i)
    params += (i ? ", " : "") + spell(ft->args[i]);
  return q + "(" + params + ")" + ((f->flags & DF_Const) && !(f->flags & DF_Static) ? " const" : "") + ((f->flags & DF_Volatile) && !(f->flags & DF_Static) ? " volatile" : "") +
         (f->refQualifier == FunctionDecl::RefQualifier::RValue && !(f->flags & DF_Static) ? " &&" : "");
}

auto codegen::Emitter::lambdaSignatureOf(const Ty* callTy, Node* at) -> Signature
{
  Signature s;
  const Ty* ret = callTy->inner ? callTy->inner : types.voidType();
  if (!ret->isKnown())
    ret = types.pointer(types.voidType());
  s.hasThis = true;
  s.paramTypes.push_back("ptr");
  s.paramTys.push_back(nullptr);
  if (returnsInMemory(ret))
  {
    s.hasSret = true;
    s.paramTypes.push_back("ptr");
    s.paramTys.push_back(nullptr);
  }
  for (const Ty* pt : callTy->args)
  {
    s.paramTypes.push_back(isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, at));
    s.paramTys.push_back(pt);
  }
  s.returnType = s.hasSret ? "void" : ret->isReference() ? "ptr" : llType(ret, at);
  return s;
}

auto codegen::Emitter::declarationText(const Signature& s, const std::string& name) -> std::string
{
  std::string text = "declare" + std::string(s.resultExtension) + " " + s.returnType + " " + name + "(";
  for (size_t i = 0; i < s.paramTypes.size(); ++i)
    text += (i ? ", " : "") + s.paramTypes[i];
  if (s.variadic)
    text += s.paramTypes.empty() ? "..." : ", ...";
  return text + ")";
}

auto codegen::Emitter::recordRequestDeclaration(const std::string& name, std::string declaration) -> void
{
  std::lock_guard lock(sh.definitionsMutex);
  sh.requestDeclarations.emplace(name, std::move(declaration));
}
