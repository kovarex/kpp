// GlobalScope::Impl::Walker, phase 2: the Walker, which types and checks bodies (its class in Walker.hpp): a
// translation unit of its own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"
#include "sse_macros.hpp"
#include <cmath> // std::isnan, isinf, fabs (MSVC's headers brought it in by chance; libstdc++'s do not)

auto GlobalScope::Impl::Walker::probedCast(const Ty* t, const Ty* operand) -> const Ty*
{
  if (!probing || !t || !operand || !t->isKnown() || !operand->isKnown() || types.isDependent(t) || types.isDependent(operand))
    return t;
  const Ty* from = types.unqualified(types.stripRef(operand));
  const Ty* to = types.unqualified(types.stripRef(t));
  if (from->kind != TyKind::Class || to->kind == TyKind::Class || to->kind == TyKind::Void || from->isBraceList())
    return t;
  return impl.convertsByOperator(from, to, 0, 0, true) ? t : unknown(Why::Unsupported);
}

auto GlobalScope::Impl::Walker::forConstexprElements(ForConstexprStmt* f, std::vector<const Ty*>& out) -> bool
{
  auto* v = static_cast<TemplateParamDecl*>(f->var);
  Node* list = f->list;
  if (list && list->kind == NodeKind::NamedType && isTypesList(static_cast<NamedType*>(list)))
  {
    for (Node* arg : static_cast<NamedType*>(list)->name.parts[0].templateArgs)
    {
      const Ty* t = arg && uint8_t(arg->kind) <= uint8_t(NodeKind::PackType) ? typeOf(static_cast<Type*>(arg)) : nullptr;
      if (!t || !t->isKnown() || types.isDependent(t))
        return false;
      out.push_back(t);
    }
    return v->isType;
  }
  if (list && list->kind == NodeKind::BraceInitExpr)
  {
    const Ty* elementTy = v->valueType ? typeOf(v->valueType) : nullptr;
    if (v->isType || !elementTy || !elementTy->isKnown() || types.isDependent(elementTy) || elementTy->kind == TyKind::Auto)
      return false;
    for (Expr* e : static_cast<BraceInitExpr*>(list)->args)
    {
      std::optional<int64_t> c = impl.constEval(e, ctx());
      if (!c)
        return false;
      out.push_back(types.valueType(elementTy, *c));
    }
    return true;
  }
  bool alternatives = list && list->kind == NodeKind::CallExpr && static_cast<CallExpr*>(list)->callee &&
                      static_cast<CallExpr*>(list)->callee->kind == NodeKind::NameExpr &&
                      symbols.name(static_cast<NameExpr*>(static_cast<CallExpr*>(list)->callee)->name.last().symbol) == "alternatives";
  Expr* subject = reflectionSubject(list);
  if (!subject || subject->kind != NodeKind::NameExpr || v->isType)
    return false;
  NamedType named{};
  named.kind = NodeKind::NamedType;
  named.token = subject->token;
  named.name = static_cast<NameExpr*>(subject)->name;
  const Ty* cls = typeOf(&named);
  cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
  if (!cls || cls->kind != TyKind::Class || types.isDependent(cls))
    return false;
  Subst s = cls->args.empty() ? Subst{} : impl.substFor(cls);
  if (alternatives)
  {
    ClassDecl* u = typedUnionOf(cls);
    if (!u)
      return false;
    for (Decl* alt : u->alternatives)
      out.push_back(types.member(impl.declaredTypeOf(alt, s.pairs.empty() ? nullptr : &s, 0), alt, cls));
    return true;
  }
  ClassDecl* def = impl.classDefinition(cls);
  if (!def || !def->bases.empty())
    return false;
  for (Decl* d : membersRead(def))
    if (d->kind == NodeKind::VariableDecl && !(d->flags & (DF_Static | DF_Typedef)) && !static_cast<VariableDecl*>(d)->name.empty())
      out.push_back(types.member(impl.declaredTypeOf(d, s.pairs.empty() ? nullptr : &s, 0), d, cls));
  return true;
}

auto GlobalScope::Impl::Walker::decltypeOf(Expr* e, const Ty* t) -> const Ty*
{
  if (Impl::namesTupleBinding(e) && t)
    return types.stripRef(t); // `decltype(auto) x = a;` of a tuple-like binding a: what get gives, no reference
  if (!e || !t || !t->isKnown() || e->kind == NodeKind::NameExpr || e->kind == NodeKind::MemberExpr)
    return t;
  if (t->kind == TyKind::RRef && isLvalueExpression(e))
    return types.lref(t->inner); // (`(x.r)` of an `int&& r`: what a name says is an lvalue)
  return t->isReference() || !isLvalueExpression(e) ? t : types.lref(t);
}

auto GlobalScope::Impl::Walker::typedUnionOf(const Ty* t) -> ClassDecl*
{
  if (!t || t->kind != TyKind::Class || !t->decl || t->decl->kind != NodeKind::ClassDecl)
    return nullptr;
  auto* c = static_cast<ClassDecl*>(t->decl);
  return c->isTypedUnion && c->isDefinition ? c : nullptr;
}

auto GlobalScope::Impl::Walker::isTypesList(const NamedType* n) const -> bool
{
  return n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && symbols.name(n->name.parts[0].symbol) == "types";
}

auto GlobalScope::Impl::Walker::reflectionSubject(Node* list) const -> Expr*
{
  if (!list || list->kind != NodeKind::CallExpr)
    return nullptr;
  auto* c = static_cast<CallExpr*>(list);
  if (!c->callee || c->callee->kind != NodeKind::NameExpr || c->args.size() != 1)
    return nullptr;
  const QualifiedName& name = static_cast<NameExpr*>(c->callee)->name;
  if (name.parts.size() != 1 || name.parts[0].hasTemplateArgs)
    return nullptr;
  std::string_view which = symbols.name(name.parts[0].symbol);
  return which == "members" || which == "alternatives" ? c->args[0] : nullptr;
}

auto GlobalScope::Impl::Walker::alternativeNamed(ClassDecl* u, Expr* index) -> EnumeratorDecl*
{
  if (!index || index->kind != NodeKind::NameExpr || !u->tagEnum)
    return nullptr;
  auto* n = static_cast<NameExpr*>(index);
  if (n->name.parts.size() != 1 || n->name.parts[0].hasTemplateArgs ||
      (n->resolved && n->resolved->kind != NodeKind::EnumeratorDecl))
    return nullptr;
  if (n->resolved)
    return nullptr; // already an enumerator: the qualified spelling, or one in scope
  for (Decl* d : static_cast<EnumDecl*>(u->tagEnum)->enumerators)
    if (d->kind == NodeKind::EnumeratorDecl && static_cast<EnumeratorDecl*>(d)->name == n->name.parts[0].symbol)
      return static_cast<EnumeratorDecl*>(d);
  return nullptr;
}

auto GlobalScope::Impl::Walker::alternativeOf(const Ty* descriptor, size_t& index) -> ClassDecl*
{
  if (!descriptor || descriptor->kind != TyKind::Member || descriptor->args.empty())
    return nullptr;
  ClassDecl* u = typedUnionOf(descriptor->args[0]);
  for (size_t i = 0; u && i < u->alternatives.size(); ++i)
    if (u->alternatives[i] == descriptor->decl)
    {
      index = i;
      return u;
    }
  return nullptr;
}

GlobalScope::Impl::Walker::Walker(Impl& impl, ResolveStats& stats, const std::vector<Token>& tokens, std::string_view text) : impl(impl), types(impl.types), stats(stats), symbols(impl.symbols), tokens(tokens), text(text), scope(&impl.root), tokenScope(&tokens, text)
{
}

auto GlobalScope::Impl::Walker::spelling(uint32_t token) const -> std::string_view
{
  const Token& t = tokens[std::min<size_t>(token, tokens.size() - 1)];
  return text.substr(t.offset, t.length);
}

auto GlobalScope::Impl::Walker::spelling(const LiteralExpr* l) const -> std::string_view
{
  if (!l->file || !l->file->tokens || l->token >= l->file->tokens->size())
    return spelling(l->token);
  const Token& t = (*l->file->tokens)[l->token];
  return l->file->text.substr(t.offset, t.length);
}

auto GlobalScope::Impl::Walker::countUnresolved(const QualifiedName& name, uint32_t token) -> void
{
  uint32_t first = name.parts[0].symbol;
  if (first == impl.symStd && name.parts.size() > 1)
  {
    ++stats.qualifiedUnresolvedStd;
    return;
  }
  uint32_t key = name.last().symbol;
  if (key == UINT32_MAX)
    return;
  if (name.parts.size() == 1 && isAllCaps(symbols.name(key)))
  {
    ++stats.unresolvedMacroLike;
    return;
  }
  ++stats.unresolvedSymbols[key];
  stats.unresolvedFirst.emplace(key, ResolveStats::Where{stats.fileIndex, token});
}

auto GlobalScope::Impl::Walker::checkNotLibrary(const QualifiedName& name, uint32_t token) -> void
{
  if (name.empty() || !file || probing || activeSubst)
    return;
  Lookup l = impl.lookupQualified(name, scope, &locals, 0);
  if (l.scope && l.scope->library && l.decl && l.decl->kind == NodeKind::NamespaceDecl)
  {
    std::string library(symbols.name(l.scope->name));
    impl.reportCheckerError(file, token, "'" + library + "' is a library: its names are written " + library + "::name, never brought in by a using-directive or a namespace alias (design.md 16)");
  }
}

auto GlobalScope::Impl::Walker::checkNotLibrary(const Type* type, uint32_t token) -> void
{
  if (type && type->kind == NodeKind::NamedType)
    checkNotLibrary(static_cast<const NamedType*>(type)->name, token);
}

auto GlobalScope::Impl::Walker::scopedEnumeratorNamed(uint32_t symbol) -> std::string
{
  // The scoped enums the scopes around declare, each scope's entries read under its shards' locks (an error's
  // message only: never on a lookup's path).
  std::string found;
  for (Scope* s = scope; s && found.empty(); s = s->parent)
    for (uint32_t i = 0; i < s->shardCount && found.empty(); ++i)
    {
      std::lock_guard lock(s->shards[i].mutex);
      for (const auto& [entrySymbol, entries] : s->shards[i].map)
        for (const Entry& e : entries)
          if (e.decl && e.decl->kind == NodeKind::EnumDecl && static_cast<EnumDecl*>(e.decl)->isClass && e.decl->ownScope &&
              e.decl->ownScope->find(symbol) && (found.empty() || symbols.name(entrySymbol) < found)) // (the first by spelling)
            found = symbols.name(entrySymbol);
    }
  return found;
}

auto GlobalScope::Impl::Walker::reportUndeclared(const QualifiedName& name, uint32_t token, const NameExpr* at, bool dependentCall) -> void
{
  if ((at && at == undeclaredLater) || !file || file->isImported || probing || name.parts.empty() || name.parts[0].symbol == UINT32_MAX)
    return;
  // `util::LIMIT` of an unscoped library util, whose names are bare (design.md 16).
  if (name.parts.size() > 1 && !name.global && !name.typeQualifier && impl.unscopedLibraries.count(name.parts[0].symbol) &&
      impl.lookupUnqualified(name.parts[0].symbol, scope, &locals, 0, true).found == Found::No)
  {
    std::string library(symbols.name(name.parts[0].symbol));
    std::string message = "'" + library + "' is an unscoped library: its names are written bare, never " + library + "::name (design.md 16)";
    if (activeSubst)
      reportInInstance(token, message);
    else if (!inGenericTemplate())
      impl.reportCheckerError(file, token, message);
    return;
  }
  if (name.parts.size() > 1 && !name.typeQualifier)
  {
    // `filesystem::recursive_directory_iterator`: a name its namespace does not declare (G's stdlib
    // comparison, 2026-11-05: codegen's "a range of unknown type" was all that was said). Only a
    // qualifier that is a namespace: a class's members are for the member lookup (bases that depend
    // on a template, an instance's specialization) to judge.
    QualifiedName qualifier = name;
    qualifier.parts.count -= 1;
    Lookup q = impl.lookupQualified(qualifier, scope, &locals, 0, activeSubst);
    if (q.found == Found::No || !q.decl || q.decl->kind != NodeKind::NamespaceDecl || q.ambiguousWith)
      return;
    std::string spelledName;
    for (size_t i = 0; i < name.parts.size(); ++i)
      spelledName += (i ? "::" : name.global ? "::" : "") + std::string(name.parts[i].symbol == UINT32_MAX ? "?" : symbols.name(name.parts[i].symbol));
    std::string message = "'" + spelledName + "' is not declared";
    if (activeSubst)
      reportInInstance(token, message);
    else if (!(inGenericTemplate() && (dependentCall || inClassWithParameterBase())))
      impl.reportCheckerError(file, token, message);
    return;
  }
  if (name.parts.size() != 1)
    return; // (the C import's inline bodies name the C compiler's builtins: codegen's to judge, if emitted)
  // What the code generator implements by name, macros of the C headers: va_start and its kin,
  // the SSE intrinsics and their _MM_ helpers (one it does not implement is its error).
  std::string_view spelled = symbols.name(name.parts[0].symbol);
  if (spelled == "va_start" || spelled == "va_end" || spelled == "va_copy" || spelled == "va_arg" || spelled.starts_with("_mm_") ||
      spelled.starts_with("_MM_"))
    return;
  std::string message = "'" + std::string(spelled) + "' is not declared";
  if (Lookup again = impl.lookupUnqualified(name.parts[0].symbol, scope, &locals, 0); again.absent)
  {
    // A false `when` declaration's name (design.md 19), which stopped the lookup.
    std::string condition = conditionText(again.absent);
    message = "'" + std::string(spelled) + "' exists only when " + condition + " holds: use it inside `if constexpr (" + condition + ")` or a `when` on it";
  }
  else if (const ParsedFile* owner = impl.filePrivateTo(name.parts[0].symbol, file))
    message = "'" + std::string(spelled) + "' is private to " + owner->path;
  else if (std::string scoped = scopedEnumeratorNamed(name.parts[0].symbol); !scoped.empty())
    message += " (an enumerator of a scoped enum is named through its enum, in a case label too): write `" + scoped + "::" + std::string(spelled) +
               "`, not `" + std::string(spelled) + "`";
  else
    for (Scope* library : impl.libraryScopes)
      if (library->find(name.parts[0].symbol))
      {
        message += "; did you mean " + std::string(symbols.name(library->name)) + "::" + std::string(spelled) + "?";
        break;
      }
  if (activeSubst)
  {
    reportInInstance(token, message);
    return;
  }
  if (inGenericTemplate() && (dependentCall || inClassWithParameterBase()))
    return;
  impl.reportCheckerError(file, token, message);
}

// `check(a, b)` in a test is the statement (design.md 8.7), never a call of a `check` that a call
// there would find: read as a check and a message, the call would be a silent wrong answer. A call
// finds a name in the scopes around it, in an instance's dependent bases (which only the instance
// knows) and in the namespaces of its arguments.
auto GlobalScope::Impl::Walker::checkTestStatementName(const AssertStmt* a) -> void
{
  if (!a->testKind || !file || file->isImported || probing)
    return;
  const char* name = a->testKind == 1 ? "check" : a->testKind == 2 ? "require" : "fail";
  uint32_t symbol = impl.symbols.intern(name);
  Lookup found = impl.lookupUnqualified(symbol, scope, &locals, 0);
  if (found.found == Found::No && activeSubst && thisType && thisType->kind == TyKind::Pointer)
    impl.lookupThroughBaseInstances(types.unqualified(thisType->inner), symbol, found, 0);
  if (found.found == Found::No)
  {
    // The statement's arguments: cond, or `cond || __kppMessage(arguments...)`; fail's message alone.
    std::vector<const Expr*> arguments;
    auto addMessage = [&](const Expr* e)
    {
      if (e && e->kind == NodeKind::CallExpr)
        for (const Expr* argument : static_cast<const CallExpr*>(e)->args)
          arguments.push_back(argument);
    };
    if (a->testKind == 3)
      addMessage(a->cond);
    else if (a->cond->kind == NodeKind::BinaryExpr && P(static_cast<const BinaryExpr*>(a->cond)->op) == P::OrOr)
    {
      arguments.push_back(static_cast<const BinaryExpr*>(a->cond)->lhs);
      addMessage(static_cast<const BinaryExpr*>(a->cond)->rhs);
    }
    else
      arguments.push_back(a->cond);
    std::vector<Scope*> namespaces;
    for (const Expr* argument : arguments)
      if (argument && argument->ty && argument->ty->isKnown() && !types.isDependent(argument->ty))
        impl.associatedNamespaces(types.unqualified(types.stripRef(argument->ty)), namespaces);
    for (Scope* in : namespaces)
      if (found.found == Found::No)
        impl.lookupInScope(in, symbol, found, 0);
  }
  if (found.found == Found::No)
    return;
  std::string message = "'" + std::string(name) + "' in a test is the statement, and a declaration of that name is visible here: rename the declaration (design.md 8.7)";
  if (activeSubst)
    reportInInstance(a->token, message);
  else
    impl.reportCheckerError(file, a->token, message);
}

auto GlobalScope::Impl::Walker::reportInInstance(uint32_t token, std::string message) -> void
{
  if (!bodyOf || undecidedBranches > 0)
    return;
  // (not where the instance was asked for: the first request to claim it, which varies with the
  // threads' order, and the error is to be the same at every thread count)
  std::string instance = instanceDescription();
  if (!instance.empty())
    message += " (in " + instance + ")";
  std::lock_guard lock(impl.checkerErrorsMutex);
  impl.instanceNameErrors[bodyOf].push_back(CheckerError{file, token, message});
}

auto GlobalScope::Impl::Walker::reportWarning(uint32_t token, const std::string& message) -> void
{
  if (undecidedBranches > 0)
    return;
  if (activeSubst && bodyOf)
  {
    std::lock_guard lock(impl.checkerErrorsMutex);
    impl.instanceWarnings[bodyOf].push_back(CheckerError{file, token, message});
    return;
  }
  impl.reportCheckerWarning(file, token, message);
}

auto GlobalScope::Impl::Walker::checkConstModification(uint32_t token, const Ty* object, const char* what, const Expr* operand) -> void
{
  if (!object || !object->isConst || !object->isKnown())
    return;
  while (operand && operand->kind == NodeKind::ParenExpr)
    operand = static_cast<const ParenExpr*>(operand)->inner;
  if (operand && operand->kind == NodeKind::NameExpr)
    if (Decl* d = static_cast<const NameExpr*>(operand)->resolved;
        d && std::find(constCopyCaptures.begin(), constCopyCaptures.end(), d) != constCopyCaptures.end())
    {
      reportCheck(token, std::string(what) + " " + std::string(symbols.name(static_cast<const NameExpr*>(operand)->name.last().symbol)) +
                           ", a capture by copy of a lambda that is not mutable (its copies are const)");
      return;
    }
  reportCheck(token, std::string(what) + " a const value (" + types.toString(object, &impl.symbols) + ")");
}

auto GlobalScope::Impl::Walker::checkConstDropped(uint32_t token, const std::string& what, const Ty* declared, const Ty* given) -> void
{
  if (probing || !declared || !declared->isReference() || !declared->inner || Impl::isConstObject(declared->inner) || !given ||
      !given->isReference() || !given->inner || !given->isKnown() || types.isDependent(given) || types.isDependent(declared) ||
      Impl::mentionsUnknown(given) || Impl::mentionsUnknown(declared) || !Impl::isConstObject(given->inner))
    return;
  const Ty* object = impl.unqualifiedDeep(given->inner);
  const Ty* referred = impl.unqualifiedDeep(declared->inner);
  if (object != referred && !(object->kind == TyKind::Class && referred->kind == TyKind::Class && impl.isBaseClass(referred, object)))
    return; // (an object of another type: a conversion's, never the object itself)
  reportCheck(token, what + " (" + types.toString(declared, &impl.symbols) + ") is bound to a const object (" + types.toString(given->inner, &impl.symbols) +
                         "): a non-const reference binds no const object, only a const reference does ([dcl.init.ref] 5)");
}

auto GlobalScope::Impl::Walker::checkTemporaryBound(uint32_t token, const std::string& what, const Ty* declared, const Ty* given) -> void
{
  if (probing || !declared || declared->kind != TyKind::LRef || !declared->inner || Impl::isConstObject(declared->inner) || !given ||
      !given->isKnown() || given->kind == TyKind::LRef || given->kind == TyKind::Pack || given->isBraceList() || types.isDependent(given) ||
      types.isDependent(declared) || Impl::mentionsUnknown(given) || Impl::mentionsUnknown(declared))
    return;
  if (given->kind == TyKind::RRef && Impl::isConstObject(given->inner))
    return; // (a const object's: checkConstDropped's error)
  if (types.stripRef(given)->kind == TyKind::Void)
    return; // (`return c.bump();` of a void call: no value at all, codegen's error)
  if (declared->inner->kind == TyKind::Function && types.stripRef(given)->kind == TyKind::Function)
    return; // (a function is an lvalue; which one and its noexcept: scalarReferenceTarget)
  if (impl.conversionRank(given, declared) < Impl::NotViable)
    return; // (a class whose conversion operator gives an lvalue of the type, a function)
  reportCheck(token, what + " (" + types.toString(declared, &impl.symbols) + ") is bound to a value (" +
                         types.toString(types.unqualified(types.stripRef(given)), &impl.symbols) +
                         "), no lvalue: a non-const reference binds an object, never a temporary ([dcl.init.ref] 5)");
}

auto GlobalScope::Impl::Walker::checkAssignable(uint32_t token, const char* what, const Expr* operand, const Ty* type) -> void
{
  if (probing || !operand || !type || !type->isKnown() || types.isDependent(type) || Impl::mentionsUnknown(type) || type->kind == TyKind::LRef ||
      isLvalueExpression(operand))
    return;
  const Ty* s = types.unqualified(types.stripRef(type));
  if (!(s->isArithmetic() || s->kind == TyKind::Enum || s->kind == TyKind::Pointer || s->kind == TyKind::MemberPointer))
    return; // (a class's operator, a function of its own, takes what it takes)
  reportCheck(token, std::string(what) + " is no lvalue (a value of " + types.toString(s, &impl.symbols) +
                         "): a built-in assignment, increment or decrement changes an object, never a value ([expr.ass] 1, [expr.pre.incr] 1)");
}

auto GlobalScope::Impl::Walker::checkCastConst(NamedCastExpr* c, const Ty* from, const Ty* to) -> void
{
  const char* cast = c->castKind == Kw::static_cast_        ? "static_cast"
                     : c->castKind == Kw::reinterpret_cast_ ? "reinterpret_cast"
                     : c->castKind == Kw::dynamic_cast_     ? "dynamic_cast"
                                                            : nullptr;
  if (probing || !cast || !from || !to || !from->isKnown() || !to->isKnown() || types.isDependent(from) || types.isDependent(to) ||
      Impl::mentionsUnknown(from) || Impl::mentionsUnknown(to))
    return;
  // [expr.static.cast]: a conversion's inverse, and noexcept only goes by a conversion, never comes.
  auto functionIn = [&](const Ty* t)
  {
    t = types.unqualified(types.stripRef(t));
    return t->kind == TyKind::Pointer || t->kind == TyKind::MemberPointer ? types.unqualified(t->inner) : t;
  };
  if (c->castKind == Kw::static_cast_ && impl.losesNoexcept(functionIn(to), functionIn(from)))
    reportCheck(c->token, "a static_cast does not make a function noexcept: " + types.toString(from, &impl.symbols) + " to " + types.toString(to, &impl.symbols));
  // What the operand's const is on: the object a reference cast refers to, what a pointer (or an
  // array, decayed) points at.
  const Ty* source = nullptr;
  const Ty* target = nullptr;
  if (to->isReference())
  {
    source = types.stripRef(from);
    target = to->inner;
  }
  else if (const Ty* f = types.unqualified(types.stripRef(from)); to->kind == TyKind::Pointer && (f->kind == TyKind::Pointer || f->kind == TyKind::Array))
  {
    source = f->inner;
    target = to->inner;
  }
  if (!source || !target || !Impl::isConstObject(source) || Impl::isConstObject(target))
    return;
  reportCheck(c->token, std::string(cast) + "<" + types.toString(to, &impl.symbols) + "> casts away the const of " + types.toString(source, &impl.symbols) +
                            ": only const_cast does ([expr.static.cast] 1, [expr.reinterpret.cast] 2, [expr.dynamic.cast] 2)");
}

auto GlobalScope::Impl::Walker::reportCheck(uint32_t token, const std::string& message) -> void
{
  if (!file || file->isImported || probing)
    return;
  if (activeSubst)
    reportInInstance(token, message);
  else if (!inGenericTemplate())
    impl.reportCheckerError(file, token, message);
}

auto GlobalScope::Impl::Walker::nonConstOnConstObject(FunctionDecl* f, const Ty* objectTy) -> bool
{
  const Ty* o = objectTy ? types.stripRef(objectTy) : nullptr;
  return f && o && o->isConst && o->isKnown() && o->kind == TyKind::Class && !(f->flags & (DF_Const | DF_Static | DF_Constructor | DF_Destructor));
}

auto GlobalScope::Impl::Walker::checkConstObjectCall(FunctionDecl* f, const Ty* objectTy, uint32_t token) -> void
{
  if (!nonConstOnConstObject(f, objectTy))
    return;
  uint32_t symbol = impl.functionSymbol(f->name);
  reportCheck(token, "the member function " + std::string(symbol == UINT32_MAX ? "?" : impl.symbols.name(symbol)) +
                         " is not const, and its object is a " + types.toString(types.stripRef(objectTy), &impl.symbols));
}

auto GlobalScope::Impl::Walker::checkRefQualifierMix(FunctionDecl* f) -> void
{
  if (activeSubst || probing || !file || file->isImported || !f->parent || f->parent->kind != NodeKind::ClassDecl || f->name.empty() || (f->flags & DF_Static))
    return;
  const Ty* ft = nullptr;
  for (Decl* m : static_cast<ClassDecl*>(f->parent)->members)
  {
    if (m == f)
      return; // (reported at the later one)
    if (m->kind != NodeKind::FunctionDecl)
      continue;
    auto* g = static_cast<FunctionDecl*>(m);
    if ((g->refQualifier == FunctionDecl::RefQualifier::None) == (f->refQualifier == FunctionDecl::RefQualifier::None) || g->name.empty() ||
        g->name.last().symbol != f->name.last().symbol || g->params.size() != f->params.size() || g->templateParams.size() != f->templateParams.size() ||
        (g->flags & (DF_Const | DF_Static)) != (f->flags & DF_Const) || !f->templateParams.empty())
      continue;
    if (!ft)
      ft = impl.declaredTypeOf(f, nullptr, 0);
    const Ty* gt = impl.declaredTypeOf(g, nullptr, 0);
    if (!ft || !gt || ft->kind != TyKind::Function || gt->kind != TyKind::Function || ft->args.size() != gt->args.size() || ft->variadic != gt->variadic ||
        !std::equal(ft->args.begin(), ft->args.end(), gt->args.begin()))
      continue;
    impl.reportCheckerError(file, f->token, "the member function " + impl.namePartText(f->name.last()) +
                                              " has a ref qualifier and another overload of the same parameters has none");
    return;
  }
}

auto GlobalScope::Impl::Walker::checkRefQualifiedCall(FunctionDecl* f, const Ty* objectTy, uint32_t token) -> void
{
  if (!f || !objectTy || (f->flags & DF_Static) || f->refQualifier == FunctionDecl::RefQualifier::None)
    return;
  bool rvalueOnly = f->refQualifier == FunctionDecl::RefQualifier::RValue;
  if (rvalueOnly ? objectTy->kind != TyKind::LRef : objectTy->kind != TyKind::RRef || ((f->flags & DF_Const) && !(f->flags & DF_Volatile)))
    return;
  uint32_t symbol = impl.functionSymbol(f->name);
  reportCheck(token, "the member function " + std::string(symbol == UINT32_MAX ? "?" : impl.symbols.name(symbol)) + " is declared " +
                       (rvalueOnly ? "&& (for an rvalue object), and its object is an lvalue" : "& (for an lvalue object), and its object is an rvalue"));
}

auto GlobalScope::Impl::Walker::checkConstObjectOverloads(Decl* primary, const std::vector<Entry>* overloads, const std::vector<const Ty*>& argTypes, const Subst* classSubst,
                                                         const Ty* objectTy, const NamePart* namePart, Span<Expr*> callArgs, uint32_t token) -> void
{
  const Ty* o = objectTy ? types.stripRef(objectTy) : nullptr;
  if (probing || !o || !o->isConst || o->kind != TyKind::Class || !o->isKnown())
    return;
  bool ambiguous = false;
  if (FunctionDecl* g = pickOverload(primary, overloads, argTypes, classSubst, types.unqualified(o), ambiguous, namePart, callArgs, nullptr); g && !ambiguous)
    checkConstObjectCall(g, objectTy, token);
}

auto GlobalScope::Impl::Walker::reportNoMatchingMember(Decl* primary, const std::vector<Entry>* overloads, const Ty* objectTy, uint32_t symbol,
                                                     const std::vector<const Ty*>& argTypes, uint32_t token) -> void
{
  // (only where every type is known: an unknown one is reported where it comes from, and a dependent
  // one is the instances' to judge; and only among plain functions: a function template's deduction
  // that pickOverload does not make, a pack's or an abbreviated template's, codegen may still make)
  auto plain = [](const Decl* d) { return d && d->kind == NodeKind::FunctionDecl && d->templateParams.empty() && !d->isTemplate; };
  if (!plain(primary))
    return;
  if (overloads)
    for (const Entry& e : *overloads)
      if (e.decl->kind == NodeKind::FunctionDecl && !plain(e.decl))
        return;
  const Ty* o = objectTy ? types.unqualified(types.stripRef(objectTy)) : nullptr;
  if (!o || o->kind != TyKind::Class || !o->isKnown() || types.isDependent(o) || Impl::mentionsUnknown(o))
    return;
  std::string list;
  for (const Ty* a : argTypes)
  {
    if (!a || !a->isKnown() || types.isDependent(a) || Impl::mentionsUnknown(a))
      return;
    list += (list.empty() ? "" : ", ") + types.toString(types.stripRef(a), &impl.symbols);
  }
  reportCheck(token, "no overload of " + types.toString(o, &impl.symbols) + "::" + std::string(impl.symbols.name(symbol)) + " takes (" + list + ")");
}

// A function's body against its declarations (design.md 8.7). `test` is decided by the first
// declaration: a body may not add it. A body outside a test class must repeat it: only with `test`
// does a build without tests skip it, and only then may it use the check statements.
auto GlobalScope::Impl::Walker::checkTestDeclaration(FunctionDecl* f) -> void
{
  if (!f->body || activeSubst || probing || !file || file->isImported || f->name.empty())
    return;
  RecordInto none(nullptr); // (as checkDefinedOnce)
  uint32_t symbol = impl.functionSymbol(f->name);
  Scope* home = impl.homeScope(f);
  Lookup look;
  if (symbol == UINT32_MAX || !home || !impl.lookupInScope(home, symbol, look, 0, false))
    return;
  auto compare = [&](Decl* d)
  {
    if (!d || d == f || d->kind != NodeKind::FunctionDecl)
      return;
    auto* declared = static_cast<FunctionDecl*>(d);
    // (a class template's member: the body has the class's parameters, definitionOf's comparison)
    bool member = home->kind == ScopeKind::Class && f->refQualifier == declared->refQualifier && (f->flags & (DF_Const | DF_Volatile)) == (declared->flags & (DF_Const | DF_Volatile)) &&
                  impl.typeAs(f, declared) == impl.matchedType(declared);
    if (declared->body || (!member && !impl.sameFunction(f, declared)))
      return;
    std::string name = impl.namePartText(f->name.last());
    if (f->testCode && !declared->testCode)
      impl.reportCheckerError(file, f->token, "the function " + name + " is test here and its first declaration is not: write test there (design.md 8.7)");
    else if (!f->testCode && declared->testCode && home->kind == ScopeKind::Class)
      impl.reportCheckerError(file, f->token, "the member function " + name + " of a test class is defined outside it: write test before its definition (design.md 8.7)");
  };
  if (look.overloads)
    for (const Entry& e : *look.overloads)
      compare(e.decl);
  else
    compare(look.decl);
}

auto GlobalScope::Impl::Walker::checkDefinedOnce(FunctionDecl* f) -> void
{
  // (an instance's own member, `template<> int Converter<double>::kind() const`, is no second
  // definition of the template's: isMemberSpecialization)
  if (!f->body || activeSubst || probing || !file || file->isImported || !f->templateParams.empty() || f->condition ||
      f->name.empty() || Impl::isFunctionSpecialization(f) || Impl::isMemberSpecialization(f) || inGenericTemplate())
    return;
  // (not recorded as what f's walk reads: a compile server's build would walk f again for every
  // overload added to its name, and its codegen then emitted f twice. So an edit that adds a
  // definition before an unchanged one is not reported by the server's build: clang rejects the
  // two definitions, a build from scratch reports them.)
  RecordInto none(nullptr);
  uint32_t symbol = impl.functionSymbol(f->name);
  Scope* home = impl.homeScope(f);
  Lookup look;
  if (symbol == UINT32_MAX || !home || !impl.lookupInScope(home, symbol, look, 0, false) || !look.overloads)
    return;
  auto before = [](const FunctionDecl* a, const FunctionDecl* b) // a's definition comes first
  {
    uint32_t fa = a->file ? a->file->index : 0, fb = b->file ? b->file->index : 0;
    return fa != fb ? fa < fb : a->token < b->token;
  };
  const FunctionDecl* first = nullptr;
  for (const Entry& e : *look.overloads)
  {
    if (!e.decl || e.decl == f || e.decl->kind != NodeKind::FunctionDecl)
      continue;
    auto* g = static_cast<FunctionDecl*>(e.decl);
    // [except.spec] 4: every declaration of a function says the same (an instance's conditions are
    // compared where they are known, none of a template's here).
    if (!g->body && !impl.isCFunction(f) && f->templateParams.empty() && !inGenericTemplate() && impl.sameFunction(f, g) &&
        impl.isNoexcept(f, activeSubst) && impl.isNoexcept(g, activeSubst) && impl.isNoexcept(f, activeSubst) != impl.isNoexcept(g, activeSubst))
      impl.reportCheckerError(file, f->token, "the function " + impl.namePartText(f->name.last()) + " says otherwise of noexcept than its declaration at " + Impl::locationOf(g->file, g->token));
    if (g->body && g->templateParams.empty() && !g->condition && !Impl::isFunctionSpecialization(g) && !Impl::isMemberSpecialization(g) &&
        before(g, f) && (!first || before(g, first)) && !impl.replacedAllocation(g) && !impl.replacedAllocation(f) && impl.sameFunction(f, g))
      first = g;
  }
  if (!first)
    return;
  // (in the same file: its line, which was the text after the place's last ':', the column with
  // --diagnostic-columns and not a line in Visual Studio's form)
  std::string where = Impl::locationOf(first->file, first->token);
  if (first->file == file && first->file->tokens && first->token < first->file->tokens->size())
  {
    size_t offset = std::min<size_t>((*first->file->tokens)[first->token].offset, first->file->text.size());
    where = "line " + std::to_string(1 + std::count(first->file->text.begin(), first->file->text.begin() + offset, '\n'));
  }
  impl.reportCheckerError(file, f->token, "the function " + impl.namePartText(f->name.last()) + " is defined twice (first at " + where + ")");
}

auto GlobalScope::Impl::Walker::checkArrayBounds(VariableDecl* v) -> void
{
  if (probing || !file || file->isImported || (v->flags & DF_Typedef) || (inGenericTemplate() && !activeSubst) || undecidedBranches > 0)
    return;
  for (Type* t = v->type; t;)
  {
    if (t->kind == NodeKind::ConstType)
    {
      t = static_cast<ConstType*>(t)->inner;
      continue;
    }
    if (t->kind != NodeKind::ArrayType)
      break;
    auto* a = static_cast<ArrayType*>(t);
    if (a->size)
    {
      Impl::constWhy().clear();
      std::optional<int64_t> n = impl.constEval(a->size, ctx());
      std::string name = v->name.empty() ? std::string("?") : impl.namePartText(v->name.last());
      if (!n)
      {
        std::string why = Impl::constWhy();
        impl.reportCheckerError(file, a->size->token, "the bound of the array " + name + " is no constant" + (why.empty() ? std::string() : ": " + why));
      }
      else if (*n < 0)
        impl.reportCheckerError(file, a->size->token, "the bound of the array " + name + " is negative (" + std::to_string(*n) + ")");
    }
    t = a->element;
  }
}

auto GlobalScope::Impl::Walker::checkElidedBound(VariableDecl* v, const Ty* declared) -> void
{
  if (probing || !file || file->isImported || !v->init || v->init->kind != NodeKind::BraceInitExpr || !declared || !declared->isKnown() ||
      (inGenericTemplate() && !activeSubst))
    return;
  Type* written = v->type;
  while (written && written->kind == NodeKind::ConstType)
    written = static_cast<ConstType*>(written)->inner;
  const Ty* t = types.unqualified(declared);
  if (!written || written->kind != NodeKind::ArrayType || static_cast<ArrayType*>(written)->size || t->kind != TyKind::Array || !t->inner)
    return;
  if (const Ty* et = types.unqualified(t->inner); et->kind != TyKind::Array && et->kind != TyKind::Class)
    return; // (elements whose braces no value elides: an element for each value, or a string's characters)
  Span<Expr*> values = static_cast<BraceInitExpr*>(v->init)->args;
  size_t count = impl.elementsFromValues(t->inner, values, [&](Expr* x) { return x->ty; }, activeSubst);
  if (count != t->count)
  {
    std::string name = v->name.empty() ? std::string("?") : impl.namePartText(v->name.last());
    impl.reportCheckerError(file, v->token, "the array " + name + " of unknown bound takes " + std::to_string(count) + " elements from its " +
                                                std::to_string(values.size()) + " values, whose braces are elided, but its type was counted with " +
                                                std::to_string(t->count) + " before they were typed: write its bound");
  }
}

auto GlobalScope::Impl::Walker::checkDiscarded(Expr* e) -> void
{
  if (probing || !file || file->isImported || retypingElements)
    return;
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (!e || e->kind != NodeKind::CallExpr)
    return;
  auto* c = static_cast<CallExpr*>(e);
  Decl* callee = c->callee && c->callee->kind == NodeKind::NameExpr     ? static_cast<NameExpr*>(c->callee)->resolved
                 : c->callee && c->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(c->callee)->resolved
                                                                        : nullptr;
  if (!callee || callee->kind != NodeKind::FunctionDecl)
    return;
  auto* f = static_cast<FunctionDecl*>(callee);
  std::string name = f->name.empty() ? std::string("?") : impl.namePartText(f->name.last());
  if (f->attributes & ATTR_NoDiscard)
  {
    reportWarning(c->token, "the result of '" + name + "', which is [[nodiscard]], is discarded");
    return;
  }
  const Ty* t = c->ty && !c->ty->isReference() ? types.unqualified(c->ty) : nullptr;
  if (t && (t->kind == TyKind::Class || t->kind == TyKind::Enum) && t->decl && (t->decl->attributes & ATTR_NoDiscard))
    reportWarning(c->token, "the result of '" + name + "', a " + types.toString(t, &symbols) + ", which is [[nodiscard]], is discarded");
}

auto GlobalScope::Impl::Walker::noteDeprecatedCall(FunctionDecl* f, Node* at) -> void
{
  if (!f || !(f->attributes & ATTR_Deprecated) || probing || !file || file->isImported || retypingElements || !at)
    return;
  std::string why;
  if (f->file && f->file->tokens)
    for (const auto& [d, token] : f->file->deprecations)
      if (d == f && token < f->file->tokens->size())
      {
        const Token& t = (*f->file->tokens)[token];
        if (size_t(t.offset) + t.length <= f->file->text.size())
          why = std::string(f->file->text.substr(t.offset, t.length));
        if (why.size() >= 2 && why.front() == '"' && why.back() == '"')
          why = why.substr(1, why.size() - 2); // (the literal's text, as C++ compilers quote it)
      }
  std::string name = f->name.empty() ? std::string("?") : impl.namePartText(f->name.last());
  reportWarning(at->token, "'" + name + "' is deprecated" + (why.empty() ? std::string() : ": " + why));
}

auto GlobalScope::Impl::Walker::inClassWithParameterBase() -> bool
{
  for (Scope* s = scope; s; s = s->parent)
    if (s->kind == ScopeKind::Class && !s->baseTypes.empty())
    {
      impl.ensureBases(s, 0);
      if (s->baseScopes.size() < s->baseTypes.size())
        return true;
    }
  return false;
}

auto GlobalScope::Impl::Walker::unknown(Why why) -> const Ty*
{
  if (stats.collectWhy && current && file)
  {
    uint32_t symbol = UINT32_MAX;
    switch (current->kind)
    {
      case NodeKind::NameExpr:
        if (!static_cast<NameExpr*>(current)->name.empty())
          symbol = static_cast<NameExpr*>(current)->name.last().symbol;
        break;
      case NodeKind::MemberExpr:
        if (!static_cast<MemberExpr*>(current)->member.empty())
          symbol = static_cast<MemberExpr*>(current)->member.last().symbol;
        break;
      case NodeKind::CallExpr:
        if (Expr* callee = static_cast<CallExpr*>(current)->callee)
        {
          if (callee->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(callee)->name.empty())
            symbol = static_cast<NameExpr*>(callee)->name.last().symbol;
          else if (callee->kind == NodeKind::MemberExpr && !static_cast<MemberExpr*>(callee)->member.empty())
            symbol = static_cast<MemberExpr*>(callee)->member.last().symbol;
        }
        break;
      case NodeKind::BinaryExpr:
        if (!static_cast<BinaryExpr*>(current)->isKeyword && static_cast<BinaryExpr*>(current)->op < uint32_t(P::Count))
          symbol = impl.opSymbols[static_cast<BinaryExpr*>(current)->op];
        break;
      case NodeKind::UnaryExpr:
        if (!static_cast<UnaryExpr*>(current)->isKeyword && static_cast<UnaryExpr*>(current)->op < uint32_t(P::Count))
          symbol = impl.opSymbols[static_cast<UnaryExpr*>(current)->op];
        break;
      default:
        break;
    }
    uint64_t key = uint64_t(why) << 56 | uint64_t(activeSubst ? 1 : 0) << 55 | uint64_t(uint8_t(current->kind)) << 32 | symbol;
    auto [it, inserted] = stats.why.emplace(key, ResolveStats::WhySite{file, current->token, 0});
    ++it->second.count;
  }
  switch (why)
  {
    case Why::Name: ++stats.unknownName; break;
    case Why::Base: ++stats.unknownBase; break;
    case Why::Member: ++stats.unknownMember; break;
    case Why::Overload: ++stats.unknownOverload; break;
    case Why::Deduction: ++stats.unknownDeduction; break;
    case Why::AutoReturn: ++stats.unknownAutoReturn; break;
    case Why::Unsupported: ++stats.unknownUnsupported; break;
    case Why::Operator: ++stats.unknownOperator; break;
    case Why::Auto: ++stats.unknownAuto; break;
    default: break;
  }
  return types.unknown();
}

auto GlobalScope::Impl::Walker::settledCalleeType(Expr* e, const Ty* t) -> const Ty*
{
  if (t->kind != TyKind::Function || !t->inner || !impl.containsAuto(t->inner) ||
      (e->kind != NodeKind::NameExpr && e->kind != NodeKind::MemberExpr))
    return t;
  Decl* d = e->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(e)->resolved : static_cast<MemberExpr*>(e)->resolved;
  if (!d || d->kind != NodeKind::FunctionDecl || !d->templateParams.empty() || !static_cast<FunctionDecl*>(d)->body)
    return t;
  for (Decl* p = d->parent; p; p = p->parent)
    if ((p->kind == NodeKind::ClassDecl || p->kind == NodeKind::FunctionDecl) && !p->templateParams.empty())
      return t; // in a template: its instantiation's arguments decide
  // Through the function's one instance, never through what its own file's walk cached: that is
  // there or not by which file was walked first, and the instance then walked or not.
  const Ty* r = instantiateFunction(static_cast<FunctionDecl*>(d), Subst{}, true, nullptr);
  return r && r->isKnown() && !impl.containsAuto(r) ? types.functionLike(t, r, t->args) : t;
}

auto GlobalScope::Impl::Walker::record(Expr* e, const Ty* t) -> const Ty*
{
  if (!t)
    t = types.unknown();
  t = settledCalleeType(e, t);
  e->ty = t;
  if (!t->isKnown())
    ++stats.exprsUnknown;
  else if (types.isDependent(t))
    ++stats.exprsDependent;
  else
    ++stats.exprsTyped;
  if (!impl.whyAt.empty() && file)
    traceAt(e, t);
  return t;
}

auto GlobalScope::Impl::Walker::traceAt(Expr* e, const Ty* t) -> void
{
  const std::string& path = file->path;
  auto inFile = [&](const std::string& want) { return path.size() >= want.size() && path.compare(path.size() - want.size(), want.size(), want) == 0; };
  bool anyInFile = false;
  for (const auto& [want, wantLine] : impl.whyAt)
    anyInFile = anyInFile || inFile(want);
  if (!anyInFile)
    return;
  const Token& tk = tokens[std::min<size_t>(e->token, tokens.size() - 1)];
  uint32_t line = 1 + uint32_t(std::count(text.begin(), text.begin() + std::min<size_t>(tk.offset, text.size()), '\n'));
  bool wanted = false;
  for (const auto& [want, wantLine] : impl.whyAt)
    wanted = wanted || (wantLine == line && inFile(want));
  if (!wanted)
    return;
  // An operator's or a destructor's name part has no symbol of its own (`operator-(a, b)`).
  auto partName = [&](const NamePart& part)
  {
    if (part.special == 2 && part.opToken < uint32_t(P::Count))
      return std::string(symbols.name(impl.opSymbols[part.opToken]));
    if (part.special == 3)
      return std::string("operator <conversion>");
    std::string name = part.symbol == UINT32_MAX ? "?" : std::string(symbols.name(part.symbol));
    return part.special == 1 ? "~" + name : name;
  };
  std::string what = nodeKindName(e->kind);
  if (e->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(e)->name.empty())
    what += " " + partName(static_cast<NameExpr*>(e)->name.last());
  else if (e->kind == NodeKind::MemberExpr && !static_cast<MemberExpr*>(e)->member.empty())
    what += " ." + partName(static_cast<MemberExpr*>(e)->member.last());
  std::string inst;
  if (activeSubst)
    for (const Subst::Pair& p : activeSubst->pairs)
      inst += (inst.empty() ? " [" : ", ") + types.toString(p.type, &symbols);
  fprintf(stderr, "why-at %s:%u %s at '%.*s' -> %s%s%s\n", path.c_str(), line, what.c_str(), int(std::min<uint32_t>(tk.length, 40)),
          text.data() + tk.offset, types.toString(t, &symbols).c_str(), inst.c_str(), inst.empty() ? "" : "]");
}

auto GlobalScope::Impl::Walker::ctx() const -> TypeContext
{
  TypeContext c{scope, &locals, activeSubst, &tokens, text};
  c.elementPacks = elementPacks;
  c.elementIndex = elementIndex;
  return c;
}

auto GlobalScope::Impl::Walker::argsConcrete(TypeTable& types, const Ty* t) -> bool
{
  if (!t || t->kind != TyKind::Class || t->args.empty())
    return false;
  for (const Ty* a : t->args)
    if (!a->isKnown() || types.isDependent(a))
      return false;
  return true;
}

auto GlobalScope::Impl::Walker::checkMemberBody(Walker& sub, FunctionDecl* f, const Subst& classSubst) -> void
{
  // Out-of-line members carry their own template parameter list; map it positionally.
  Subst merged = classSubst;
  Decl* cls = sub.scope->owner;
  if (cls && f->templateParams.size() == cls->templateParams.size() && f->parent != cls)
    for (size_t i = 0; i < f->templateParams.size(); ++i)
      if (const Ty* t = classSubst.find(cls->templateParams[i]))
        merged.pairs.push_back({f->templateParams[i], t});
  bool producer;
  Impl::InstanceRecord* inst = impl.claimInstance(f, merged, sub.thisType, producer);
  if (!producer)
    return;
  RecordInto into(&inst->deps); // what typing the instance reads is its own
  FunctionDecl* copy = impl.copyInstance(f, sub.scope, merged);
  inst->body = copy;
  // Walked by a walker of its own, as sub would walk it, so that its statistics are its own: a
  // compile server checks an instance again when its template's body changes (step 4).
  ResolveStats mine;
  mine.fileIndex = sub.stats.fileIndex;
  mine.collectWhy = sub.stats.collectWhy;
  Walker member(impl, mine, sub.tokens, sub.text);
  member.file = sub.file;
  FileOfLookups walkedFile(sub.file);
  member.scope = sub.scope;
  member.thisType = sub.thisType;
  member.activeSubst = &merged;
  member.instantiationDepth = sub.instantiationDepth;
  member.requestFile = sub.requestFile;
  member.requestToken = sub.requestToken;
  member.current = sub.current;
  member.locals.items = sub.locals.items;
  if (f->parent != cls)
    for (Decl* p : f->templateParams)
      member.locals.push(static_cast<TemplateParamDecl*>(p)->name, p);
  if (recordingDependencies)
  {
    auto r = std::make_unique<Impl::InstanceRecord::Recheck>();
    r->member = true;
    r->source = f;
    r->scope = member.scope;
    r->thisType = member.thisType;
    r->subst = merged;
    r->locals = member.locals.items;
    r->file = member.file;
    r->depth = member.instantiationDepth;
    r->requestFile = member.requestFile;
    r->requestToken = member.requestToken;
    r->fileIndex = mine.fileIndex;
    r->collectWhy = mine.collectWhy;
    inst->recheck = std::move(r);
  }
  member.returnFrames.push_back(nullptr);
  member.walkDecl(copy);
  const Ty* deduced = member.returnFrames.back();
  member.returnFrames.pop_back();
  if (deduced)
    inst->deducedReturn = impl.deducedShape(copy->returnType && !copy->trailingReturn ? member.typeOf(copy->returnType) : nullptr, deduced);
  mine.instExprsTyped += mine.exprsTyped;
  mine.instExprsDependent += mine.exprsDependent;
  mine.instExprsUnknown += mine.exprsUnknown;
  impl.addInstanceStats(mine, inst); // counted once, whoever asked (Impl::instanceStatsTotal)
  ++impl.instBodies;
  inst->counted |= 1;
  inst->state.store(2, std::memory_order_release);
  inst->state.notify_all();
}

auto GlobalScope::Impl::Walker::instantiateClass(const Ty* classTy) -> void
{
  if (instantiationDepth > 16 || !argsConcrete(types, classTy) || !classTy->decl || !classTy->decl->ownScope)
    return;
  for (const Ty* a : classTy->args)
    if (impl.holdsText(a) && current && classTy->decl->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(classTy->decl)->name.empty())
    {
      // `Vector<Text>`: its instance would keep the Text past the call (design.md 14).
      reportTextError(current->token, impl.isTextClosure(a) ? "a lambda capturing a Text cannot be stored (design.md 14)"
                                                            : textStored("an argument of " + std::string(symbols.name(static_cast<ClassDecl*>(classTy->decl)->name.last().symbol))));
      break;
    }
  if (!classTy->decl->isTemplate || classTy->decl->templateParams.empty())
    return;
  bool producer;
  Impl::Instantiation* inst = impl.claimClass(classTy, producer);
  if (!producer)
    return; // Done or being done by someone else; nothing here needs its result.
  RecordInto into(&inst->deps); // what checking the instance reads is its own
  Scope* cls = classTy->decl->ownScope;
  if (impl.queueMemberBodies.load(std::memory_order_relaxed))
  {
    // While the files are resolved: the instance's member bodies a task for whichever worker
    // is free (Impl::MemberTask), not a weight on the thread that happened to ask first.
    const ParsedFile* askedIn = current && file ? file : nullptr;
    uint32_t askedAt = askedIn ? current->token : UINT32_MAX;
    impl.queueMemberBody({classTy, nullptr, askedIn, askedAt, instantiationDepth});
    inst->state.store(2, std::memory_order_release);
    inst->state.notify_all();
    return;
  }
  auto start = std::chrono::steady_clock::now();
  Subst subst = impl.substFor(classTy);
  subCheck(classTy->decl, subst, cls, types.pointer(classTy), [&](Walker& sub)
  {
    auto* c = static_cast<ClassDecl*>(classTy->decl);
    for (Decl* m : membersRead(c))
      if (m->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(m)->body && m->templateParams.empty())
        checkMemberBody(sub, static_cast<FunctionDecl*>(m), subst);
    for (FunctionDecl* f : cls->outOfLineMembers)
      if (f->body && (!Impl::isMemberSpecialization(f) || impl.memberSpecializationIsFor(f, classTy)))
        checkMemberBody(sub, f, subst); // `template<> R Button<RecipeID>::f()`: for its instantiation only
  });
  impl.instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  inst->state.store(2, std::memory_order_release);
  inst->state.notify_all();
}

auto GlobalScope::Impl::Walker::instantiateFunction(FunctionDecl* f, const Subst& subst, bool needReturn, const Subst* classSubst, const Ty* objectType, Impl::InstanceRecord** record) -> const Ty*
{
  for (Decl* p : f->templateParams)
    if (!subst.find(p) || types.isDependent(subst.find(p)) || !subst.find(p)->isKnown())
      return nullptr;
  // A member function template declared in its class and defined after it (`template<bool B>
  // auto* Net::maybeGet(int) const {...}`): the definition's body, its own template parameters
  // taking the declaration's arguments in order; the instance stays the declaration's.
  FunctionDecl* def = f;
  if (!f->body && needReturn && f->returnType && !f->trailingReturn && Impl::astMentionsAuto(f->returnType))
    if (Decl* cls = impl.owningClassOf(f); cls && cls->ownScope && !f->templateParams.empty())
    {
      uint32_t symbol = impl.functionSymbol(f->name);
      const Ty* ft = impl.matchedType(f);
      // (The declaration's `auto` return against whatever the definition's type has deduced:
      // the parameters and the method's constness decide.)
      auto sameParameters = [&](const Ty* a, const Ty* b)
      {
        if (!a || !b || a->kind != TyKind::Function || b->kind != TyKind::Function || a->args.size() != b->args.size() || a->variadic != b->variadic)
          return false;
        for (size_t i = 0; i < a->args.size(); ++i)
          if (a->args[i] != b->args[i])
            return false;
        return true;
      };
      for (FunctionDecl* candidate : cls->ownScope->outOfLineMembers)
        if (candidate->body && impl.functionSymbol(candidate->name) == symbol && !Impl::isMemberSpecialization(candidate) &&
            candidate->templateParams.size() == f->templateParams.size() && (candidate->flags & (DF_Const | DF_Volatile)) == (f->flags & (DF_Const | DF_Volatile)) &&
            candidate->refQualifier == f->refQualifier)
          if (const Ty* ct = impl.typeAs(candidate, f); ct == ft || (ft && ft->kind == TyKind::Function && impl.containsAuto(ft->inner) && sameParameters(ct, ft)))
          {
            def = candidate;
            break;
          }
    }
  if (!def->body || instantiationDepth > 16)
    return nullptr;
  if (classSubst)
    for (const Subst::Pair& pair : classSubst->pairs)
      if (!pair.type->isKnown() || types.isDependent(pair.type))
        return nullptr;
  Subst full = subst;
  if (classSubst)
    for (const Subst::Pair& pair : classSubst->pairs)
      if (!full.find(pair.param))
        full.pairs.push_back(pair);
  Subst defFull = full;
  if (def != f)
    for (size_t i = 0; i < f->templateParams.size(); ++i)
      defFull.pairs.push_back({def->templateParams[i], subst.find(f->templateParams[i])});
  Scope* bodyScope = def->name.parts.size() > 1 ? semanticScopeFor(def->name) : impl.lexicalScopeOf(def);
  const Ty* bodyThis = nullptr;
  if (objectType)
    bodyThis = types.pointer(types.unqualified(types.stripRef(objectType)));
  else if (Decl* cls = enclosingClassDecl(bodyScope))
    bodyThis = types.pointer(impl.substitute(impl.selfType(cls), full));
  ++impl.instRequests;
  bool producer;
  Impl::InstanceRecord* inst = impl.claimInstance(f, full, bodyThis, producer);
  if (record)
    *record = inst;
  if (!producer)
  {
    if (!needReturn)
      return nullptr;
    if (inst->state.load(std::memory_order_acquire) != 2)
    {
      // Queued and not started (by this thread or another): its result is needed now, so
      // this walk takes the check over (never waits for a task no worker may have reached).
      if (inst->queued.exchange(0) == 1)
        return runFunctionInstance(inst, *inst->task);
      if (inst->owner.load() == std::this_thread::get_id())
        return nullptr; // Recursive auto function.
      ++impl.instWaits;
      for (int seen = inst->state.load(std::memory_order_acquire); seen != 2; seen = inst->state.load(std::memory_order_acquire))
        inst->state.wait(seen, std::memory_order_acquire); // blocks (C++20 atomic wait) instead of spinning on yield
    }
    return inst->deducedReturn;
  }
  Impl::FunctionInstanceTask task{f, def, full, defFull, bodyScope, bodyThis, current && file ? file : nullptr,
                                  current && file ? current->token : UINT32_MAX, instantiationDepth};
  if (!needReturn && impl.queueMemberBodies.load(std::memory_order_relaxed))
  {
    // While the files are resolved: a check nobody waits for is a task for whichever worker
    // is free (docs/compile-speed.md, step 3), not a weight on the file that asked first.
    inst->task = std::make_unique<Impl::FunctionInstanceTask>(std::move(task));
    inst->queued.store(1);
    impl.queueMemberBody({nullptr, nullptr, nullptr, 0, 0, inst});
    return nullptr;
  }
  return runFunctionInstance(inst, task);
}

auto GlobalScope::Impl::Walker::runFunctionInstance(Impl::InstanceRecord* inst, const Impl::FunctionInstanceTask& t) -> const Ty*
{
  RecordInto into(&inst->deps); // what typing the instance reads is its own
  if (recordingDependencies)
  {
    inst->recheck = std::make_unique<Impl::InstanceRecord::Recheck>();
    inst->recheck->source = t.def;
    inst->recheck->task = std::make_unique<Impl::FunctionInstanceTask>(t);
  }
  inst->owner.store(std::this_thread::get_id());
  // At the depth of the walk that asked for it, whoever runs it (the depth limit decides what
  // is checked, so it must not depend on which walk took a queued check over).
  int savedDepth = std::exchange(instantiationDepth, t.depth);
  struct DepthRestore { int& d; int saved; ~DepthRestore() { d = saved; } } depthRestore{instantiationDepth, savedDepth};
  if (!t.f->templateParams.empty())
    ++impl.instFunctions, inst->counted |= 2; // a function template's (a class template's member claimed here or by its class's check counts as a body either way)
  auto start = std::chrono::steady_clock::now();
  // The instantiation's own copy of f's nodes (its definition's), typed here and kept as its body.
  // One with no arguments at all is a function that is no template, typed before the walk of
  // its file reached it (for its `auto` return): its lambdas are the types that walk gives
  // them, which the code generator emits (`chunkLineByLineScan(...)`'s TransformView).
  FunctionDecl* copy = impl.copyInstance(t.def, t.bodyScope, t.full, t.full.pairs.empty());
  inst->body = copy;
  const Ty* deduced = nullptr;
  const Ty* pattern = nullptr; // the written `auto&`, `const auto*`...: the deduced type takes its shape
  subCheck(t.def, t.defFull, t.bodyScope, t.bodyThis, [&](Walker& sub)
  {
    if (t.requestFile)
    {
      sub.requestFile = t.requestFile;
      sub.requestToken = t.requestToken;
    }
    sub.returnFrames.push_back(nullptr);
    sub.walkDecl(copy);
    deduced = sub.returnFrames.back();
    sub.returnFrames.pop_back();
    if (copy->returnType && !copy->trailingReturn)
      pattern = sub.typeOf(copy->returnType);
    ++impl.instBodies;
    inst->counted |= 1;
  }, inst);
  inst->deducedReturn = deduced ? impl.deducedShape(pattern, deduced) : nullptr;
  impl.instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  inst->state.store(2, std::memory_order_release);
  inst->state.notify_all();
  return inst->deducedReturn;
}

auto GlobalScope::Impl::Walker::reportAmbiguity(const Lookup& l, const QualifiedName& name, uint32_t token) -> void
{
  if (l.ambiguousIn && !name.empty())
  {
    reportBaseAmbiguity(l, name.last().symbol, token);
    return;
  }
  if (!l.ambiguousWith || !l.decl || !file || name.parts.size() != 1)
    return;
  std::string spelled(impl.symbols.name(name.parts[0].symbol));
  impl.reportCheckerError(file, token, "'" + spelled + "' is ambiguous: the one declared at " + impl.locationOf(l.decl->file, l.decl->token) + " and the one at " +
                                           impl.locationOf(l.ambiguousWith->file, l.ambiguousWith->token) + " (a using-directive puts both at one level)");
}

auto GlobalScope::Impl::Walker::reportBaseAmbiguity(const Lookup& l, uint32_t symbol, uint32_t token) -> void
{
  if (!l.ambiguousIn || !l.foundIn || !file || probing || inGenericTemplate() || symbol == UINT32_MAX)
    return;
  auto className = [&](Scope* s)
  {
    if (s && s->owner && s->owner->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(s->owner)->name.empty())
      return std::string(impl.symbols.name(static_cast<ClassDecl*>(s->owner)->name.last().symbol));
    return std::string("?");
  };
  impl.reportCheckerError(file, token, "the member " + std::string(impl.symbols.name(symbol)) + " is ambiguous: both bases " + className(l.foundIn) +
                                           " and " + className(l.ambiguousIn) + " have one");
}

auto GlobalScope::Impl::Walker::resolveTypeName(NamedType* t) -> void
{
  if (t->name.empty())
    return;
  Lookup l = impl.lookupTypeName(t, scope, &locals, 0, activeSubst);
  reportAmbiguity(l, t->name, t->token);
  if (l.found == Found::Local && l.decl && l.decl->when && t->name.parts.size() == 1)
  {
    // A local alias, class or enumeration a `when` declares (design.md 19): the one of the name
    // that exists here (convertType takes it from resolved), or the error that none does.
    bool undecided = false;
    Decl* named = whenUse(t->name.parts[0].symbol, t->token, l.decl, undecided);
    if (!named)
    {
      t->resolved = nullptr;
      return;
    }
    l.decl = named;
    l.scope = named->ownScope;
  }
  // A name an earlier walk found among its function's local aliases, classes and enumerations, walked
  // again where those are not in scope (a copy typed on its own: typeOfSignatureExpression's probe,
  // which deduces a call's template arguments at compile time), names that declaration still, not
  // what the scope declares of the name: gcd's `using C = GcdType<M, N>;`, in `unsignedMagnitude(
  // static_cast<C>(m))`, deduced a program's class C (D, 2026-11-06).
  if (l.found != Found::Local && t->resolved && t->name.parts.size() == 1 && t->resolved->parent && t->resolved->parent->kind == NodeKind::FunctionDecl &&
      (t->resolved->kind == NodeKind::UsingAliasDecl || t->resolved->kind == NodeKind::ClassDecl || t->resolved->kind == NodeKind::EnumDecl ||
       (t->resolved->kind == NodeKind::VariableDecl && (t->resolved->flags & DF_Typedef))))
  {
    l.found = Found::Local;
    l.decl = t->resolved;
    l.scope = t->resolved->ownScope;
  }
  t->resolved = l.decl;
  if (l.found == Found::No)
  {
    if (t->name.parts.size() == 1 && impl.builtinByName(t->name.parts[0].symbol))
      ++stats.typesBuiltinByName;
    else if (t->name.parts.size() == 1 && t->name.parts[0].hasTemplateArgs && impl.isTypeTraitName(t->name.parts[0].symbol))
      ++stats.typesResolved; // RemoveConst<T> and friends
    else
    {
      ++stats.typesUnresolved;
      countUnresolved(t->name, t->token);
      // A type name nothing declares is an error where it is written, as an undeclared name is
      // (not `class X` in a template argument, which declares X; not a value trait, isConst<T>,
      // written where a type may be; not in a template's own walk: its instances say).
      const NamePart& first = t->name.parts[0];
      if (!t->elaborated && !(t->name.parts.size() == 1 && first.hasTemplateArgs && impl.isBoolTraitName(first.symbol)) && !inGenericTemplate())
        reportUndeclared(t->name, t->token);
    }
  }
  else if (l.found == Found::Dependent)
    ++stats.typesDependent;
  else
    ++stats.typesResolved;
  checkTemplateArgumentCount(t, l);
  checkClassConstraints(t, l);
  checkSpecializationTie(t, l);
  for (const NamePart& part : t->name.parts)
    for (Node* arg : part.templateArgs)
      walkNode(arg);
}

auto GlobalScope::Impl::Walker::checkSpecializationTie(NamedType* t, const Lookup& l) -> void
{
  const NamePart& last = t->name.last();
  if (!last.hasTemplateArgs || !l.decl || !l.overloads || !file || inGenericTemplate())
    return;
  Decl* primary = impl.primaryOf(*l.overloads);
  if (!primary || primary->kind != NodeKind::ClassDecl || !(primary->attributes & ATTR_Specialized))
    return;
  std::vector<const Ty*> args;
  for (size_t i = 0; i < last.templateArgs.size(); ++i)
  {
    const Ty* a = impl.templateArgument(last.templateArgs[i], impl.parameterFor(primary, i), ctx(), 0);
    if (!a || !a->isKnown() || types.isDependent(a) || Impl::mentionsUnknown(a))
      return;
    args.push_back(a);
  }
  std::vector<ClassDecl*> tied;
  impl.matchSpecialization(l, args, 0, &tied);
  if (tied.size() < 2)
    return;
  std::string between;
  for (ClassDecl* c : tied)
  {
    std::vector<const Ty*> patterns;
    std::string written = impl.debugName(last.symbol) + "<";
    if (impl.specializationPatterns(c, primary, patterns, 0))
      for (size_t i = 0; i < patterns.size(); ++i)
        written += (i ? ", " : "") + types.toString(patterns[i], &symbols);
    between += (between.empty() ? "" : " and ") + written + "> (" + Impl::locationOf(c->file, c->token) + ")";
  }
  const Ty* named = types.classType(primary, Span<const Ty*>{args.data(), uint32_t(args.size())});
  impl.reportCheckerError(file, t->token, "the partial specializations of " + types.toString(named, &symbols) + " are ambiguous: " + between);
}

auto GlobalScope::Impl::Walker::checkClassConstraints(NamedType* t, const Lookup& l) -> void
{
  const NamePart& last = t->name.last();
  if (!last.hasTemplateArgs || !l.decl || !file || inGenericTemplate())
    return;
  Decl* def = l.decl->kind == NodeKind::ClassDecl && l.scope && l.scope->owner && l.scope->kind == ScopeKind::Class ? l.scope->owner : l.decl;
  if (def->kind == NodeKind::ClassDecl && l.overloads)
    if (Decl* primary = impl.primaryOf(*l.overloads))
      def = primary;
  if (def->kind != NodeKind::ClassDecl)
    return;
  bool constrained = false;
  for (Decl* p : def->templateParams)
    constrained = constrained || impl.typeConstraintOf(static_cast<TemplateParamDecl*>(p), def);
  if (!constrained)
    return;
  const Ty* ty = typeOf(t);
  if (!ty || ty->kind != TyKind::Class || types.isDependent(ty) || Impl::mentionsUnknown(ty))
    return;
  Subst s = impl.substFor(ty);
  for (Decl* p : def->templateParams)
  {
    auto* tp = static_cast<TemplateParamDecl*>(p);
    VariableDecl* concept_ = impl.typeConstraintOf(tp, def);
    if (!concept_)
      continue;
    std::optional<bool> holds = impl.typeConstraintHolds(tp, def, s, 0);
    const Ty* arg = s.find(tp);
    if (holds && *holds)
      continue;
    if (!holds && (!arg || !arg->isKnown() || types.isDependent(arg) || Impl::mentionsUnknown(arg)))
      continue;
    std::string param(symbols.name(tp->name));
    std::string named = types.toString(ty, &symbols);
    std::string concept_name = concept_->name.empty() ? std::string("its concept") : impl.debugName(concept_->name.last().symbol);
    impl.reportCheckerError(file, t->token, holds ? named + ": " + param + " = " + types.toString(arg, &symbols) + " does not satisfy " + concept_name
                                                  : named + ": the constraint " + concept_name + " on " + param + " cannot be evaluated for " +
                                                      types.toString(arg, &symbols));
  }
}

auto GlobalScope::Impl::Walker::checkTemplateArgumentCount(NamedType* t, const Lookup& l) -> void
{
  const NamePart& last = t->name.last();
  if (!last.hasTemplateArgs || !l.decl || !file)
    return;
  Decl* def = l.decl->kind == NodeKind::ClassDecl && l.scope && l.scope->owner && l.scope->kind == ScopeKind::Class ? l.scope->owner : l.decl;
  if (def->kind == NodeKind::ClassDecl && l.overloads)
    if (Decl* primary = impl.primaryOf(*l.overloads))
      def = primary; // the primary's parameters, not a partial specialisation's (`FallbackValueHolder<T, true>`)
  if (def->kind != NodeKind::ClassDecl && def->kind != NodeKind::UsingAliasDecl)
    return;
  Span<Decl*> params = def->kind == NodeKind::ClassDecl ? impl.effectiveParams(def) : def->templateParams;
  if (params.empty())
    return; // not a template: other errors say so
  if (static_cast<TemplateParamDecl*>(params[params.size() - 1])->isPack)
    return;
  size_t given = 0;
  for (Node* arg : last.templateArgs)
  {
    if (arg && arg->kind == NodeKind::PackType)
      return; // `X<Args...>`: as many as the pack has, told once it is known
    ++given;
  }
  if (given <= params.size())
    return;
  std::string name = impl.debugName(last.symbol);
  impl.reportCheckerError(file, t->token, name + " takes " + std::to_string(params.size()) + " template argument" +
                                              (params.size() == 1 ? "" : "s") + ", " + std::to_string(given) + " given");
}

auto GlobalScope::Impl::Walker::hiddenObjectType(VariableDecl* v) -> const Ty*
{
  const Ty* t = impl.declaredTypeOf(v, activeSubst, 0);
  if (t && impl.containsAuto(t))
  {
    if (const Ty* deduced = locals.deducedTypeOf(v))
      t = deduced; // the object as this walk deduced it (also a range-for variable's)
    else if (v->init && v->init->ty)
      t = impl.deduceVariable(v, t, v->init->ty, v->init);
    else if (const Ty* cached = v->cachedType.load(std::memory_order_acquire); cached && !impl.containsAuto(cached))
      t = cached;
    else
      t = nullptr;
  }
  return t && t->isKnown() ? t : nullptr;
}

auto GlobalScope::Impl::Walker::bindingType(VariableDecl* v, uint32_t symbol) -> const Ty*
{
  size_t index = v->structuredBindings.size();
  for (size_t i = 0; i < v->structuredBindings.size(); ++i)
    if (static_cast<NameExpr*>(v->structuredBindings[i])->name.last().symbol == symbol)
      index = i;
  if (index == v->structuredBindings.size())
    return nullptr;
  const Ty* t = hiddenObjectType(v);
  if (!t)
    return unknown(Why::Auto);
  bool isConst = types.stripRef(t)->isConst;
  const Ty* object = types.unqualified(types.stripRef(t));
  if (object->kind == TyKind::Array)
    return isConst ? types.qualified(object->inner, true, false) : object->inner;
  if (object->kind == TyKind::Param)
    return object; // the element of a range that depends on a template parameter
  if (object->kind != TyKind::Class)
    return unknown(Why::Unsupported);
  // The class declaring the members: the object's, or the base all of them are in.
  const Ty* holder = impl.bindingMembersClass(object);
  Scope* cs = holder ? impl.classScopeOf(holder) : nullptr;
  if (!cs || !cs->owner || cs->owner->kind != NodeKind::ClassDecl)
    return unknown(Why::Unsupported);
  size_t seen = 0;
  for (Decl* m : membersRead(static_cast<ClassDecl*>(cs->owner)))
  {
    // (an unnamed bit field is no member: `unsigned : 0;` before `double y;` gave y's binding the
    // type unsigned, and codegen read y's double through it)
    if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)) || static_cast<VariableDecl*>(m)->name.empty())
      continue;
    if (seen++ == index)
    {
      Subst s = holder->args.empty() ? Subst{} : impl.substFor(holder);
      const Ty* mt = impl.declaredTypeOf(m, holder->args.empty() ? activeSubst : &s, 0);
      return mt && isConst ? types.qualified(mt, true, false) : mt;
    }
  }
  return unknown(Why::Unsupported);
}

auto GlobalScope::Impl::Walker::typeOfResolvedName(const Lookup& l, const NamePart& lastPart, Expr* e) -> const Ty*
{
  Decl* d = l.decl;
  if (!d)
    return unknown(Why::Name);
  switch (d->kind)
  {
    case NodeKind::VariableDecl:
    {
      if (auto* v = static_cast<VariableDecl*>(d); !v->structuredBindings.empty())
        if (const Ty* bt = bindingType(v, lastPart.symbol))
          return bt;
      if (lastPart.hasTemplateArgs && Impl::conceptOf(d))
        return types.boolType(); // `has_load<T, Input>` as a value: a concept-id is a bool
      if (lastPart.hasTemplateArgs && !d->templateParams.empty())
        if (const Ty* t = impl.variableTemplateType(static_cast<VariableDecl*>(d), lastPart, ctx()))
          return t; // `two_v<int>`: T is int here
      if (l.found == Found::Local)
        if (const Ty* walked = locals.deducedTypeOf(d); walked && !impl.containsAuto(walked))
          return walked; // the type this walk gave the local (a class of the function's own, under an instance's arguments)
      const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
      if (impl.containsAuto(t))
      {
        // A local whose initializer was typed already has its deduced type cached by the
        // walker; otherwise (members/globals in other files) deduce from the initializer now.
        auto* v = static_cast<VariableDecl*>(d);
        if (const Ty* deduced = locals.deducedTypeOf(v))
          return deduced; // a local (or range-for variable) as this walk deduced it
        // (Only a known initializer type: an unknown one is there or not by whether the
        // variable's own walk came first, and the use was then counted as auto's or not.)
        if (v->init && v->init->ty && v->init->ty->isKnown() && v->init->kind != NodeKind::BraceInitExpr)
          return impl.deduceVariable(v, t, v->init->ty, v->init);
        return unknown(Why::Auto);
      }
      return t;
    }
    case NodeKind::ParamDecl:
    {
      if (l.found == Found::Local)
        if (const Ty* walked = locals.deducedTypeOf(d))
          return walked; // a lambda's parameter as this walk typed it
      const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
      // A function parameter pack in the pattern of one element's expansion: that element.
      if (static_cast<ParamDecl*>(d)->isPack && elementPacks && !elementPacks->empty() && t && t->kind == TyKind::Pack && elementIndex < t->args.size())
        return t->args[elementIndex];
      return impl.containsAuto(t) ? unknown(Why::Auto) : t;
    }
    case NodeKind::EnumeratorDecl:
      return impl.declaredTypeOf(d, activeSubst, 0); // (of this instance's enum in a class template)
    case NodeKind::FunctionDecl:
    {
      // `getOrCreate<Expression, const NoiseExpression&>` as a value: the function template
      // with its explicit arguments (a trailing pack takes the rest), once they bind all of
      // its parameters.
      auto* f = static_cast<FunctionDecl*>(d);
      if (lastPart.hasTemplateArgs && !f->templateParams.empty() && (!l.overloads || l.overloads->size() <= 1))
      {
        Subst s;
        bindExplicitArgs(f, lastPart, Span<Expr*>{}, s);
        bool bound = true;
        for (Decl* p : f->templateParams)
          bound = bound && s.find(p);
        if (bound)
        {
          if (activeSubst)
            for (const Subst::Pair& p : activeSubst->pairs)
              if (!s.find(p.param))
                s.pairs.push_back(p); // the enclosing instance's (a member of a class template)
          return impl.namedFunctionType(f, impl.declaredTypeOf(f, &s, 0), &s);
        }
      }
      return impl.namedFunctionType(f, impl.declaredTypeOf(d, activeSubst, 0), activeSubst);
    }
    case NodeKind::ClassDecl:
    case NodeKind::EnumDecl:
    case NodeKind::UsingAliasDecl:
    {
      // A type used as an expression: constructor call `Foo(x)`, `Foo{}` or `Enum(x)`.
      TypeContext c = ctx();
      // `const_iterator(*this)` in a member of ParticleSurface : InfiniteRectangle<ParticleChunk*>:
      // the base's nested class with the base's arguments.
      Subst bs;
      if (e && e->kind == NodeKind::NameExpr && static_cast<NameExpr*>(e)->name.parts.size() == 1)
        bs = impl.baseSubstForUnqualified(l, c, 0);
      if (!bs.empty())
      {
        if (activeSubst)
          for (const Subst::Pair& p : activeSubst->pairs)
            bs.pairs.push_back(p);
        c.subst = &bs;
      }
      return impl.typeOfTypeDecl(d, l, lastPart, c, 0);
    }
    case NodeKind::TemplateParamDecl:
    {
      const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
      return t && t->kind == TyKind::Value && t->inner ? t->inner : t; // `M == Mode::Fast`: the value's type
    }
    case NodeKind::NamespaceDecl:
      return unknown(Why::Unsupported);
    default:
      (void)e;
      return unknown(Why::Unsupported);
  }
}

auto GlobalScope::Impl::Walker::dependentNameType(Decl* d) -> const Ty*
{
  const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
  const Ty* element = Impl::elementOf(d, t, ctx());
  if (element != t && element && element->kind == TyKind::Value && d->kind == NodeKind::TemplateParamDecl &&
      !static_cast<TemplateParamDecl*>(d)->isType && element->inner)
    return element->inner; // a constant's element, as declaredTypeOf gives a constant parameter's type
  return element;
}

auto GlobalScope::Impl::Walker::resolveName(NameExpr* e) -> const Ty*
{
  if (e->name.empty())
  {
    // The hidden object of `auto [a, b] = e;`, which a tuple-like binding's get<i>(object)
    // names (tupleBindings).
    if (e->resolved && e->resolved->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(e->resolved)->structuredBindings.empty())
      if (const Ty* t = hiddenObjectType(static_cast<VariableDecl*>(e->resolved)))
        return t;
    return unknown(Why::Unsupported);
  }
  walkType(e->name.typeQualifier); // `decltype(this->deviation)::CoordinateType::FractionalBits`
  if (e->name.parts.size() == 1 && e->name.parts[0].hasTemplateArgs && impl.isBoolTraitName(e->name.parts[0].symbol))
  {
    // A builtin trait: a bool the code generator evaluates once the arguments are known.
    for (Node* arg : e->name.parts[0].templateArgs)
      walkNode(arg);
    e->resolved = nullptr;
    ++stats.namesGlobal;
    return types.boolType();
  }
  // Before the lookup, which converts them: `decltype((i))` reads what its operand's name resolved to
  // (`Name<decltype((i))>::value` was Name<int>, silently).
  for (const NamePart& part : e->name.parts)
    for (Node* arg : part.templateArgs)
      walkNode(arg);
  Lookup l = impl.lookupQualified(e->name, scope, &locals, 0, activeSubst);
  if (l.found == Found::No && e->name.parts.size() > 1)
    l = memberOfQualifierInstance(e);
  Impl::preferValue(l); // (`&stat`: the function, not the struct)
  reportAmbiguity(l, e->name, e->token);
  // `using Colors::shade;` in a namespace: the name is the variable it names (functions stay the
  // using-declaration's overload set, which a call takes through withNamespaceUsings).
  if (l.decl && l.decl->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(l.decl)->isEnum && !l.ambiguousWith && l.foundIn &&
      (l.foundIn->kind == ScopeKind::Namespace || l.foundIn->kind == ScopeKind::Class)) // (`using A::x;` in a class: A's field)
  {
    Lookup target = impl.lookupQualified(static_cast<UsingNameDecl*>(l.decl)->name, impl.lexicalScopeOf(l.decl), nullptr, 0);
    if (target.decl && target.decl->kind == NodeKind::VariableDecl)
      l = target;
  }
  e->resolved = l.decl;
  // A template-id naming a variable: `println(ll < u, " ", sc >> 1)` reads `ll<u, " ", sc>` and then
  // `> 1` (design.md 22: the token after the `>` decides), which is an error, never the variable with
  // its arguments dropped (until 2026-11-05 println printed `false` for `true -28`).
  if (!e->name.parts.empty() && e->name.last().hasTemplateArgs && l.decl &&
      (l.decl->kind == NodeKind::ParamDecl || (l.decl->kind == NodeKind::VariableDecl && l.decl->templateParams.empty() && !(l.decl->flags & DF_Typedef))) &&
      file && !file->isImported && !probing)
  {
    impl.reportCheckerError(file, e->token, "'" + std::string(symbols.name(e->name.last().symbol)) +
                                                "' is a variable, not a template: `a < b, c > d` and `a < b, c >> d` read as a template-id when the token after the `>` "
                                                "would follow one (design.md 22): write the comparison in parentheses, `(a < b)`");
    return unknown(Why::Name);
  }
  if (!whenLookup(l, e))
    return types.unknown(); // (design.md 19: reported, or the instances' to tell)
  noteImplicitMember(e, l);
  switch (l.found)
  {
    case Found::No:
      if (e->name.parts.size() == 1)
        if (const Ty* b = impl.builtinByName(e->name.parts[0].symbol))
          return b; // `uint32_t(x)`
      ++stats.namesUnresolved;
      countUnresolved(e->name, e->token);
      reportUndeclared(e->name, e->token, e);
      return unknown(Why::Name);
    case Found::Local: ++stats.namesLocal; break;
    case Found::Member: ++stats.namesMember; break;
    case Found::Global: ++stats.namesGlobal; break;
    case Found::Dependent: ++stats.namesDependent; break;
  }
  if (l.found == Found::Dependent)
    return l.decl ? dependentNameType(l.decl) : types.unknown();
  // Members of the current class template keep their dependent types; members reached
  // through a qualified name on an instantiation get substituted.
  if ((e->name.parts.size() > 1 || e->name.typeQualifier) && l.found == Found::Member && l.foundIn && l.foundIn->owner &&
      (!l.foundIn->owner->templateParams.empty() ||
       (l.foundIn->owner->kind == NodeKind::EnumDecl && !Impl::effectiveParams(l.foundIn->owner).empty())))
  {
    // Qualifier like `Vector<int>::npos`: substitute from the qualifier type when it is an
    // instantiation; `decltype(check<T>(nullptr))::value` from the class the decltype names.
    QualifiedName qualifier = e->name;
    qualifier.parts.count -= 1;
    NamedType tmp{};
    tmp.kind = NodeKind::NamedType;
    tmp.name = qualifier;
    tmp.token = e->token;
    if (!qualifier.parts.empty() && qualifier.last().hasTemplateArgs) // `S<Box<int>, int>::v`: its specialization's tie too
      checkSpecializationTie(&tmp, impl.lookupQualified(qualifier, scope, &locals, 0, activeSubst));
    const Ty* q = qualifier.parts.empty() ? typeOf(e->name.typeQualifier) : typeOf(&tmp);
    q = q ? types.unqualified(types.stripRef(q)) : types.unknown();
    // `IsSmall::value` with `struct IsSmall : Constant<bool, true> {}`: a member of a template
    // base takes that base instantiation's arguments.
    if (q->kind == TyKind::Class && (!q->args.empty() || (q->decl && q->decl->ownScope != l.foundIn)))
      if (Subst s = impl.memberSubst(q, l.foundIn, 0); !s.pairs.empty())
        return memberThroughThis(l, impl.namedFunctionType(l.decl, impl.declaredTypeOf(l.decl, &s, 0), &s));
    // `Fast::Enum::True` with an enum declared in a class template: that instance's enumerator.
    if (q->kind == TyKind::Enum)
      if (Subst s = impl.substFor(q); !s.pairs.empty())
        return impl.declaredTypeOf(l.decl, &s, 0);
  }
  return memberThroughThis(l, typeOfResolvedName(l, e->name.last(), e));
}

auto GlobalScope::Impl::Walker::reachThroughBases(Scope* cls, Scope* target, int depth) -> int
{
  if (cls == target)
    return 1;
  if (depth > 16 || cls->baseTypes.empty() || !cls->owner)
    return 0;
  LocalStack params;
  for (Decl* p : cls->templateParams)
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  Impl::pushEnclosingTemplateParams(cls->owner->parent, params);
  int reached = 0;
  for (Type* b : cls->baseTypes)
  {
    Scope* bs = impl.scopeOfType(b, cls->parent, &params, depth + 1);
    if (!bs || bs == cls)
      continue;
    int via = reachThroughBases(bs, target, depth + 1);
    if (!via)
      continue;
    const Ty* bt = impl.convertType(b, TypeContext{cls->parent, &params, nullptr}, depth + 1);
    if (via == 1 && !(bt && types.isDependent(bt)))
      return 1;
    reached = 2;
  }
  return reached;
}

auto GlobalScope::Impl::Walker::noteImplicitMember(const NameExpr* n, const Lookup& l) -> void
{
  if (!thisWarningsOn() || l.found != Found::Member || n->name.parts.size() != 1 || n->name.global || n->name.typeQualifier || !file ||
      file->isImported || probing || declaredTypes > 0 || !l.foundIn || l.foundIn->kind != ScopeKind::Class || !l.foundIn->owner)
    return;
  Decl* d = l.decl;
  if (d && d->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(d)->isEnum) // (`using Base::f;`, then `f()`)
    d = impl.lookupQualified(static_cast<UsingNameDecl*>(d)->name, impl.lexicalScopeOf(d), nullptr, 0).decl;
  if (!d || (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::FunctionDecl) || (d->flags & (DF_Typedef | DF_Constructor | DF_Destructor)))
    return; // (types, enumerators and nested classes need no `this.`: design.md 23c)
  // (`operator=(other)`: the operator's symbol; a destructor's or a conversion's name has none to give)
  const NamePart& part = n->name.parts[0];
  uint32_t symbol = !part.special ? part.symbol : part.special == 2 && part.opToken < uint32_t(P::Count) ? impl.opSymbols[part.opToken] : UINT32_MAX;
  if (symbol == UINT32_MAX)
    return;
  std::string name(symbols.name(symbol));
  // As written (`operator ==`): the operator's tokens, `()` and `[]` two of them.
  std::string old = !part.special ? name
                                  : Impl::spelledTokens(file, n->token, n->token + (P(part.opToken) == P::LParen || P(part.opToken) == P::LBracket ? 2 : 1));
  if (old.empty())
    return;
  bool isStatic = (d->flags & DF_Static) != 0;
  std::string message = Impl::locationOf(file, n->token) + ": warning: " + name + " is a " +
                        (isStatic ? "static member" : d->kind == NodeKind::FunctionDecl ? "member function" : "data member");
  for (Scope* s = scope; s; s = s->parent)
    if (s->kind == ScopeKind::Class)
      if (int via = reachThroughBases(s, l.foundIn, 0))
      {
        // What the scopes around the class have of the name: what a bare name means once it is never a
        // member (Q10), and what C++ takes for a member of a dependent base, which it looks up where the
        // template is written ([temp.dep] 3).
        bool around = impl.lookupUnqualified(symbol, s->parent, nullptr, 0).found != Found::No;
        if (via == 2)
          message += around ? " (found in a dependent base, which C++ does not search: it takes the " + name + " around the class)"
                            : " (found in a dependent base, which C++ does not search: an error there)";
        else if (around)
          message += " (a bare " + name + " will be the one around the class)";
        break;
      }
  // The fix last, the form an editor's quick fix reads: a static is no value of the object (design.md 23c).
  noteThisWarning(message + ": write `" + (isStatic ? "This::" : "this.") + old + "`, not `" + old + "`");
}

auto GlobalScope::Impl::Walker::memberThroughThis(const Lookup& l, const Ty* t) -> const Ty*
{
  // In a const member function a data member is const, unless mutable; in a volatile one volatile,
  // a mutable one too ([dcl.stc] 10 lifts const alone).
  if (!t || !t->isKnown() || t->isReference() || l.found != Found::Member || !l.decl || l.decl->kind != NodeKind::VariableDecl ||
      (l.decl->flags & (DF_Static | DF_Typedef)) || !l.foundIn || l.foundIn->kind != ScopeKind::Class || !thisType ||
      thisType->kind != TyKind::Pointer || !thisType->inner)
    return t;
  bool addConst = thisType->inner->isConst && !(l.decl->flags & DF_Mutable) && !Impl::isConstObject(t);
  bool addVolatile = thisType->inner->isVolatile && !t->isVolatile;
  if (!addConst && !addVolatile)
    return t;
  Scope* self = impl.classScopeOf(types.unqualified(thisType->inner));
  if (!self || !reachesClassScope(self, l.foundIn, 0))
    return t;
  return types.qualified(t, t->isConst || addConst, t->isVolatile || addVolatile);
}

auto GlobalScope::Impl::Walker::memberOfQualifierInstance(NameExpr* e) -> Lookup
{
  QualifiedName qualifier = e->name;
  qualifier.parts.count -= 1;
  NamedType written{};
  written.kind = NodeKind::NamedType;
  written.token = e->token;
  written.name = qualifier;
  const Ty* q = typeOf(&written);
  q = q ? types.unqualified(types.stripRef(q)) : nullptr;
  if (!q || q->kind != TyKind::Class || types.isDependent(q))
    return {};
  MemberResult r = memberOf(q, e->name.last().symbol, nullptr, e->token, false);
  if (!r.decl || !r.decl->parent || r.decl->parent->kind != NodeKind::ClassDecl || !r.decl->parent->ownScope)
    return {};
  Lookup l;
  l.found = Found::Member;
  l.decl = r.decl;
  l.overloads = r.overloads;
  l.foundIn = r.decl->parent->ownScope;
  return l;
}

auto GlobalScope::Impl::Walker::objectForArrow(const Ty* t, int depth) -> const Ty*
{
  t = types.stripRef(t);
  if (!t || depth > 4)
    return t;
  if (t->kind == TyKind::Pointer)
    return t->inner;
  if (t->kind == TyKind::Class)
  {
    MemberResult arrow = memberOf(t, impl.opSymbols[uint32_t(P::Arrow)], nullptr, 0, false);
    if (arrow.overloads && arrow.overloads->size() > 1)
    {
      // `const T* operator->() const` and `T* operator->()`: the one the object's constness picks.
      Decl* pick = nullptr;
      for (const Entry& entry : *arrow.overloads)
        if (entry.decl && entry.decl->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(entry.decl)->params.empty() &&
            (!pick || ((entry.decl->flags & DF_Const) != 0) == t->isConst))
          pick = entry.decl;
      if (pick)
      {
        arrow.decl = pick;
        arrow.type = impl.declaredTypeOf(pick, arrow.subst.pairs.empty() ? nullptr : &arrow.subst, 0);
      }
    }
    // `auto operator->() { return this->idIterator.operator->(); }`: what the body returns.
    if (arrow.decl && arrow.decl->kind == NodeKind::FunctionDecl &&
        (arrow.why == Why::Auto || (arrow.type && arrow.type->kind == TyKind::Function && impl.containsAuto(arrow.type->inner))))
      return objectForArrow(returnTypeOfCall(static_cast<FunctionDecl*>(arrow.decl), arrow.subst.pairs.empty() ? nullptr : &arrow.subst,
                                             Span<Expr*>{}, nullptr),
                            depth + 1);
    if (arrow.type && arrow.type->kind == TyKind::Function)
      return objectForArrow(arrow.type->inner, depth + 1);
    return types.unknown();
  }
  return t;
}

auto GlobalScope::Impl::Walker::instantiationOfBase(const Ty* t, Scope* cls, Scope* in, int depth) -> const Ty*
{
  return impl.instantiationOfBase(t, cls, in, depth);
}

auto GlobalScope::Impl::Walker::memberOfInstantiatedBases(const Ty* t, Scope* cls, uint32_t symbol, uint32_t token) -> std::optional<MemberResult>
{
  if (!cls->owner || instantiatedBaseDepth > 16)
    return std::nullopt;
  Subst own = impl.substFor(t);
  LocalStack params;
  for (Decl* p : cls->templateParams)
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  ParsedFile* file = cls->owner->file;
  TypeContext ctx{cls->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  // Every base is searched: a name two bases declare (members of two different classes) is
  // ambiguous, as C++'s member lookup says ([class.member.lookup] 6), never the first base's.
  std::optional<MemberResult> result;
  const Ty* resultBase = nullptr;
  ++instantiatedBaseDepth;
  for (Type* b : cls->baseTypes)
    for (const Ty* bt : impl.baseInstances(b, ctx, 1))
    {
      // (a closure base too: its call operator, impl.closureCall)
      if (!bt || (bt->kind != TyKind::Class && bt->kind != TyKind::Lambda) || !bt->decl || types.isDependent(bt))
        continue;
      MemberResult r = memberOf(bt, symbol, nullptr, token, false);
      if (!r.decl)
        continue;
      if (!result)
      {
        result = std::move(r);
        resultBase = bt;
      }
      else if (r.decl->parent != result->decl->parent && file && !probing && !inGenericTemplate())
        impl.reportCheckerError(file, token,
                                "the member " + std::string(impl.symbols.name(symbol)) + " is ambiguous: both bases " +
                                    types.toString(resultBase, &impl.symbols) + " and " + types.toString(bt, &impl.symbols) + " have one");
    }
  --instantiatedBaseDepth;
  return result;
}

auto GlobalScope::Impl::Walker::withUsingOverloads(const Lookup& l, const Ty* t, Scope* cls, Subst& baseArgs) -> std::shared_ptr<std::vector<Entry>>
{
  auto isUsing = [](Decl* d) { return d && d->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(d)->isEnum; };
  if (!l.overloads && isUsing(l.decl) && l.foundIn && l.foundIn->kind == ScopeKind::Class)
  {
    // The using-declaration alone of its name: an overload set of one.
    std::vector<Entry> alone{Entry{l.decl, nullptr}};
    Lookup one = l;
    one.overloads = &alone;
    return withUsingOverloads(one, t, cls, baseArgs);
  }
  if (!l.overloads || l.overloads->empty() || !l.foundIn || l.foundIn->kind != ScopeKind::Class)
    return nullptr;
  // A using-declaration whose qualifier is a template parameter of the class (`using X::operator();`,
  // `using Ts::operator()...;`): the members of the instance's argument, or of each element of
  // its pack, which a lookup of the qualified name without the instance's arguments did not reach.
  Subst instanceArgs = t ? impl.substFor(t) : Subst{};
  auto parameterTypes = [&](UsingNameDecl* u, std::vector<const Ty*>& out)
  {
    if (u->name.parts.size() != 2 || u->name.parts[0].hasTemplateArgs || u->name.global)
      return false;
    for (const Subst::Pair& p : instanceArgs.pairs)
      if (p.param && p.param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(p.param)->name == u->name.parts[0].symbol && p.type)
      {
        if (p.type->kind == TyKind::Pack)
          out.insert(out.end(), p.type->args.begin(), p.type->args.end());
        else
          out.push_back(p.type);
        return true;
      }
    return false;
  };
  if (std::none_of(l.overloads->begin(), l.overloads->end(), [&](const Entry& e) { return isUsing(e.decl); }))
    return nullptr;
  auto merged = std::make_shared<std::vector<Entry>>();
  for (const Entry& e : *l.overloads)
    if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
      merged->push_back(e);
  size_t ownCount = merged->size();
  // The class's own function hides the base's of the same parameter types and qualifiers
  // (C++ [namespace.udecl] 15): ArtilleryTurret's `getInventory(uint8) override` beside `using
  // super::getInventory;` is one candidate. Only where both are plain functions whose parameter
  // types name no template parameter.
  auto hidden = [&](FunctionDecl* base)
  {
    const uint32_t quals = DF_Const | DF_Volatile | DF_RefQual | DF_Static | DF_Variadic;
    if (!base->templateParams.empty())
      return false;
    const Ty* bt = impl.declaredTypeOf(base, nullptr, 0);
    if (!bt || bt->kind != TyKind::Function || types.isDependent(bt))
      return false;
    for (size_t i = 0; i < ownCount; ++i)
    {
      auto* own = static_cast<FunctionDecl*>((*merged)[i].decl);
      if (!own->templateParams.empty() || (own->flags & quals) != (base->flags & quals) || own->refQualifier != base->refQualifier || own->params.size() != base->params.size())
        continue;
      const Ty* ot = impl.declaredTypeOf(own, nullptr, 0);
      if (!ot || ot->kind != TyKind::Function || ot->args.size() != bt->args.size())
        continue;
      bool same = true;
      for (size_t a = 0; a < ot->args.size() && same; ++a)
        same = ot->args[a] == bt->args[a];
      if (same)
        return true;
    }
    return false;
  };
  for (const Entry& e : *l.overloads)
  {
    if (!isUsing(e.decl))
      continue;
    std::vector<const Ty*> bases;
    if (parameterTypes(static_cast<UsingNameDecl*>(e.decl), bases))
    {
      uint32_t symbol = impl.functionSymbol(static_cast<UsingNameDecl*>(e.decl)->name);
      for (const Ty* base : bases)
      {
        MemberResult m = memberOf(base, symbol, nullptr, e.decl->token, false);
        if (m.overloads)
          for (const Entry& te : *m.overloads)
            if (te.decl && te.decl->kind == NodeKind::FunctionDecl && !hidden(static_cast<FunctionDecl*>(te.decl)))
              merged->push_back(te);
        for (const Subst::Pair& p : m.subst.pairs)
          if (!baseArgs.find(p.param))
            baseArgs.pairs.push_back(p);
      }
      continue;
    }
    Lookup target = impl.lookupQualified(static_cast<UsingNameDecl*>(e.decl)->name, l.foundIn, nullptr, 0);
    if (target.overloads)
    {
      for (const Entry& te : *target.overloads)
        if (te.decl && te.decl->kind == NodeKind::FunctionDecl && !hidden(static_cast<FunctionDecl*>(te.decl)))
          merged->push_back(te);
    }
    else if (target.decl && target.decl->kind == NodeKind::FunctionDecl && !hidden(static_cast<FunctionDecl*>(target.decl)))
      merged->push_back(Entry{target.decl, nullptr});
    if (t && cls && target.foundIn && target.foundIn != l.foundIn && target.foundIn->kind == ScopeKind::Class)
      if (const Ty* base = instantiationOfBase(t, cls, target.foundIn, 0))
        for (const Subst::Pair& p : impl.substFor(base).pairs)
          if (!baseArgs.find(p.param))
            baseArgs.pairs.push_back(p);
  }
  if (merged->empty())
    return nullptr; // (nothing the using-declarations name: the lookup's own answer)
  return merged;
}

auto GlobalScope::Impl::Walker::memberOf(const Ty* objectTy, uint32_t symbol, const QualifiedName* name, uint32_t token, bool countMissing) -> MemberResult
{
  MemberResult r;
  const Ty* t = types.stripRef(objectTy);
  if (!t || !t->isKnown())
  {
    r.why = Why::Base;
    return r;
  }
  if (t->kind == TyKind::Param || (t->kind == TyKind::Class && types.isDependent(t) && !t->decl))
  {
    r.type = types.param(t->decl);
    return r;
  }
  if (t->kind == TyKind::DependentMember)
  {
    // `typename T::X` object: known once T is.
    r.type = types.param(nullptr);
    return r;
  }
  // A closure type's one member, its call operator (impl.closureCall): what `using
  // Ts::operator()...;` names in a class derived from closures, and what a lookup through such a
  // base finds.
  if (t->kind == TyKind::Lambda && symbol == impl.opSymbols[uint32_t(P::LParen)] && (!name || name->parts.size() <= 1))
    if (ClosureCall* call = impl.closureCall(types.unqualified(t)))
    {
      r.decl = &call->decl;
      r.overloads = &call->entries;
      r.type = call->signature;
      return r;
    }
  if (t->kind != TyKind::Class && t->kind != TyKind::Enum)
  {
    r.why = Why::Base;
    return r;
  }
  Scope* cls = t->decl ? t->decl->ownScope : nullptr;
  if (!cls)
  {
    // Only a forward declaration was seen.
    Entry e{t->decl, nullptr};
    cls = impl.scopeOfEntry(e, 0);
    if (!cls)
    {
      r.why = Why::Base;
      return r;
    }
  }
  Lookup l;
  bool found = true;
  const Ty* foundInBase = nullptr; // the instantiation of the base l was found in, once computed
  if (name && name->parts.size() > 1)
  {
    // `obj.Base::member`: Base as the object's class sees it, else as the expression's context
    // does (`prototype->Super::addValues(result)` with Super a template parameter).
    l = impl.lookupQualified(*name, cls, nullptr, 0);
    if (l.found == Found::No || !l.decl)
    {
      QualifiedName qualifier = *name;
      qualifier.parts.count -= 1;
      NamedType written{};
      written.kind = NodeKind::NamedType;
      written.token = token;
      written.name = qualifier;
      const Ty* q = typeOf(&written);
      if (q && q->isKnown() && types.isDependent(q))
      {
        r.type = types.param(nullptr); // known once Super is
        return r;
      }
      if (Scope* qs = q && q->kind == TyKind::Class ? impl.classScopeOf(q) : nullptr)
      {
        Lookup inQualifier;
        if (impl.lookupInScope(qs, symbol, inQualifier, 0))
          l = inQualifier;
      }
    }
    else if (l.foundIn && l.foundIn != cls && l.foundIn->kind == ScopeKind::Class && !types.isDependent(t))
    {
      // `m.Leaf<char>::value` with the bases Leaf<int> and Leaf<char>: the member of the class the
      // qualifier names, of which every instance of Leaf shares the scope it was found in. (Until
      // 2026-11-07 the first base of that scope's: an int read from the char's byte.)
      QualifiedName qualifier = *name;
      qualifier.parts.count -= 1;
      NamedType written{};
      written.kind = NodeKind::NamedType;
      written.token = token;
      written.name = qualifier;
      const Ty* q = typeOf(&written);
      q = q ? types.unqualified(types.stripRef(q)) : nullptr;
      const Ty* object = types.unqualified(t);
      if (q && q->kind == TyKind::Class && q->isKnown() && !types.isDependent(q) && (q == object || impl.derivesFrom(object, q)))
        if (Scope* qs = impl.classScopeOf(q))
          foundInBase = qs == l.foundIn ? q : instantiationOfBase(q, qs, l.foundIn, 0);
    }
  }
  else
  {
    found = impl.lookupInScope(cls, symbol, l, 0);
    reportBaseAmbiguity(l, symbol, token);
    // A class template's bases are linked to its scope once, without its arguments, so a base
    // the arguments choose (`struct X : BoolConstant<B>`, `Conditional<c, Value<T>, T>`) is
    // missing there or is some other class: an instantiation's members come from its own bases.
    if (!t->args.empty() && !types.isDependent(t) && !cls->baseTypes.empty())
    {
      bool viaOtherClass = false;
      if (found && l.foundIn && l.foundIn != cls && l.foundIn->kind == ScopeKind::Class)
      {
        foundInBase = instantiationOfBase(t, cls, l.foundIn, 0);
        viaOtherClass = !foundInBase;
      }
      if ((!found && !l.absent) || viaOtherClass) // (a false `when` member's name stops it: design.md 19)
        if (std::optional<MemberResult> viaBase = memberOfInstantiatedBases(t, cls, symbol, token))
          return *viaBase;
    }
    else if (!found && !l.absent && !types.isDependent(t) && !cls->baseTypes.empty())
    {
      // A plain class deriving from such an instantiation (`struct Item : H<Item, true>`,
      // whose Hook takes a sibling nested class as its base by the arguments): the member
      // through the bases as types.
      if (std::optional<MemberResult> viaBase = memberOfInstantiatedBases(t, cls, symbol, token))
        return *viaBase;
    }
  }
  if (!found && l.absent && symbols.name(symbol) != "operator=") // (`operator=`: the implicit one, as when it is not declared at all)
  {
    // A member a false `when` dropped (design.md 19): used where its condition is not known to hold.
    if (file && !file->isImported && !probing && token)
    {
      std::string condition = conditionText(l.absent);
      impl.reportCheckerError(file, token, "'" + std::string(symbols.name(symbol)) + "' exists only when " + condition +
                                               " holds: use it inside `if constexpr (" + condition + ")` or a `when` on it");
    }
    r.type = types.unknown();
    return r;
  }
  if (!found)
  {
    if (types.isDependent(t))
    {
      r.type = types.unknown();
      r.why = Why::None; // Member of a dependent base: cannot know yet.
      if (countMissing)
        ++stats.membersDependent;
      return r;
    }
    if (hasUnknownBase(cls, 0))
    {
      // A base class outside the corpus (std::vector...) may declare it.
      r.why = Why::Base;
      if (countMissing)
      {
        ++stats.membersUnknownBase;
        ++stats.unknownBaseMembers[symbol];
      }
      return r;
    }
    r.why = Why::Member;
    // `v.index()`, `v.get<1>()` on a typed union (E's report): std::variant's members, which it has
    // not; it is asked with `v == U::Name` or v.tag() and read with `v[Name]` (design.md 8.5).
    if (countMissing && !probing && typedUnionOf(types.unqualified(t)))
    {
      std::string_view called = symbols.name(symbol);
      if (called == "index" || called == "get" || called == "getIf" || called == "holdsAlternative" || called == "emplace" || called == "valueless_by_exception")
      {
        std::string u = types.toString(types.unqualified(t), &impl.symbols);
        reportCheck(token, std::string(called) + " is no member of the typed union " + u + ": a `union enum` is asked with `v == " + u +
                               "::Name` (or v.tag()) and read with `v[Name]` (design.md 8.5)");
      }
    }
    if (countMissing)
    {
      ++stats.membersNotFound;
      ++stats.missingMembers[symbol];
      stats.missingMembersFirst.emplace(symbol, ResolveStats::Where{stats.fileIndex, token});
      // (`a.operator=(b)` names the implicit assignment, which no class declares)
      if (!probing && !Impl::mentionsUnknown(t) && symbols.name(symbol) != "operator=")
      {
        bool afterOperator = token + 1 < tokens.size() && (tokens[token].is(P::Dot) || tokens[token].is(P::Arrow)); // (the error marks the name)
        reportCheck(afterOperator ? token + 1 : token, types.toString(types.unqualified(t), &impl.symbols) + " has no member '" + std::string(symbols.name(symbol)) + "'");
      }
    }
    return r;
  }
  Subst usingArgs;
  std::shared_ptr<std::vector<Entry>> merged = withUsingOverloads(l, t, cls, usingArgs);
  if (merged)
  {
    l.decl = merged->front().decl;
    l.overloads = merged.get();
  }
  if (l.decl && l.decl->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(l.decl)->isEnum)
  {
    // `using Base::f;` in the class: f means the base's overloads, as members of that base
    // (`using super::front;` with `using super = CompactDeque<T>;`: front's T is CompactDeque's).
    Lookup target = impl.lookupQualified(static_cast<UsingNameDecl*>(l.decl)->name, l.foundIn ? l.foundIn : cls, nullptr, 0);
    if (target.found != Found::No && target.decl)
    {
      l.decl = target.decl;
      l.overloads = target.overloads;
      if (target.foundIn && target.foundIn->kind == ScopeKind::Class)
      {
        l.foundIn = target.foundIn;
        foundInBase = nullptr;
      }
    }
  }
  if (l.found == Found::No || !l.decl)
  {
    r.why = Why::Member;
    return r;
  }
  Impl::preferValue(l); // (`p.Reading(3)` beside a nested class Reading: the member function)
  r.decl = l.decl;
  r.overloads = l.overloads;
  r.merged = merged;
  const Ty* owner = t;
  if (l.foundIn && l.foundIn != cls && l.foundIn->kind == ScopeKind::Class)
    if (const Ty* base = foundInBase ? foundInBase : instantiationOfBase(t, cls, l.foundIn, 0))
      owner = base; // `v.x` with x declared in the base Vector2<T, V>: that base's instantiation binds T
  r.subst = impl.substFor(owner);
  if (!r.subst.empty())
    instantiateClass(types.unqualified(owner));
  for (const Subst::Pair& p : usingArgs.pairs)
    if (!r.subst.find(p.param))
      r.subst.pairs.push_back(p);
  if (l.found == Found::Dependent)
  {
    r.type = types.param(l.decl);
    return r;
  }
  // Several `when` data members of the name in a class template (design.md 19, an #ifdef/#else pair: `when
  // (isTriviallyCopyable<T>) T copy; when (!isTriviallyCopyable<T>) short copy;`): the instance's is the one whose
  // condition holds under its arguments, as its layout has it (codegen's whenMemberHolds). None, or two, is an error.
  if (l.decl->kind == NodeKind::VariableDecl && l.decl->when && l.decl->whenKnown < 0 && l.overloads && l.overloads->size() > 1 &&
      !types.isDependent(owner) && !r.subst.empty())
  {
    int holding = 0;
    bool undecided = false;
    if (Decl* chosen = impl.whenMemberOf(l.decl, r.subst, holding, undecided))
    {
      r.decl = chosen;
      r.overloads = nullptr;
    }
    else if (!undecided)
    {
      if (file && !file->isImported && !probing && token)
        impl.reportCheckerError(file, token, "'" + std::string(symbols.name(symbol)) + "' " +
                                                 (holding == 0 ? "does not exist in " + types.toString(owner, &symbols) + ": none of its `when` conditions holds there"
                                                               : "is declared twice in " + types.toString(owner, &symbols) + ": the `when` conditions of " +
                                                                   std::to_string(holding) + " of its declarations hold there") +
                                                 " (design.md 19)");
      r.type = types.unknown();
      return r;
    }
  }
  const Ty* declared = impl.declaredTypeOf(r.decl, &r.subst, 0);
  if (impl.containsAuto(declared))
  {
    auto* v = r.decl->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(r.decl) : nullptr;
    declared = v && v->init && v->init->ty ? impl.deduceVariable(v, declared, v->init->ty, v->init) : types.unknown();
    if (!declared->isKnown())
      r.why = Why::Auto;
  }
  r.type = declared;
  return r;
}

auto GlobalScope::Impl::Walker::hasUnknownBase(Scope* cls, int depth) -> bool
{
  if (!cls || depth > 16 || cls->baseTypes.empty())
    return false;
  impl.ensureBases(cls, 0);
  if (cls->baseScopes.size() < cls->baseTypes.size())
    return true;
  for (Scope* b : cls->baseScopes)
    if (hasUnknownBase(b, depth + 1))
      return true;
  return false;
}

auto GlobalScope::Impl::Walker::typeOfMember(MemberExpr* m) -> const Ty*
{
  checkPostfixAfterParenthesisedType(m->base, m->token);
  const Ty* base = walkExpr(m->base);
  if (m->dynamicMember)
  {
    // `object.[m]`: the data member the descriptor m names (reflection, design.md 8.3).
    const Ty* d = types.stripRef(walkExpr(m->dynamicMember));
    const Ty* object = m->op == P::Arrow || m->op == P::SafeArrow ? objectForArrow(base) : base;
    if (!d || !d->isKnown())
      return unknown(Why::Base);
    if (d->kind != TyKind::Member || !d->decl)
    {
      m->resolved = nullptr;
      ++stats.membersDependent;
      return types.isDependent(d) ? d : unknown(Why::Unsupported); // in a template body: dependent until the loop binds m
    }
    m->resolved = d->decl;
    ++stats.membersBound;
    const Ty* t = d->inner ? d->inner : types.unknown();
    const Ty* obj = object ? types.stripRef(object) : nullptr;
    if (obj && obj->isConst && t->isKnown() && !t->isReference())
      t = types.qualified(t, true, false);
    return t;
  }
  for (const NamePart& part : m->member.parts)
    for (Node* arg : part.templateArgs)
      walkNode(arg);
  if (m->member.empty())
    return unknown(Why::Unsupported);
  const NamePart& last = m->member.last();
  uint32_t symbol = last.special == 2 && last.opToken < uint32_t(P::Count) ? impl.opSymbols[last.opToken] : last.symbol;
  if (symbol == UINT32_MAX)
  {
    if (last.special == 1)
    {
      // obj.~Foo()
      ++stats.membersBound;
      return types.function(types.voidType(), {}, false);
    }
    return unknown(Why::Unsupported);
  }
  const Ty* object = base;
  if (m->op == P::Arrow || m->op == P::SafeArrow)
    object = objectForArrow(base);
  if (!object || !object->isKnown())
  {
    ++stats.membersUnknownBase;
    ++stats.unknownBaseMembers[symbol];
    return unknown(Why::Base);
  }
  if (const Ty* descriptor = types.stripRef(object); descriptor->kind == TyKind::Member)
  {
    // A reflection descriptor (`for constexpr (auto m : members(T))`): m.name, m.offset, m.type
    // (written as a type: `a.type value;`, read as decltype(a.type)); an alternative's
    // (`alternatives(U)`) also a.tag, its U::Tag value.
    m->resolved = nullptr;
    ++stats.membersBound;
    std::string_view which = symbols.name(symbol);
    if (which == "name")
      return types.pointer(types.qualified(types.charType(), true, false));
    if (which == "type")
      return descriptor->inner ? descriptor->inner : types.unknown();
    size_t index = 0;
    if (ClassDecl* u = alternativeOf(descriptor, index))
      return which == "tag" && u->tagEnum ? impl.tagTypeOf(u, descriptor->args.empty() ? nullptr : descriptor->args[0]) : unknown(Why::Member);
    if (which == "offset")
      return types.sizeType();
    return unknown(Why::Member);
  }
  MemberResult r = memberOf(object, symbol, &m->member, m->token);
  m->resolved = r.decl;
  if (r.decl)
  {
    // (A plain member function of a known object is bound whatever its type says yet: one whose
    // `auto` result is not deduced yet (another thread deducing it) read as dependent, so the count
    // depended on the threads' timing: `getScreenPositionFunction()`, the headline at -j 8 and 32.)
    bool plainFunction = r.decl->kind == NodeKind::FunctionDecl && r.decl->templateParams.empty() && !types.isDependent(types.stripRef(object));
    if (r.type && types.isDependent(r.type) && !plainFunction)
      ++stats.membersDependent;
    else
      ++stats.membersBound;
  }
  else if (r.why == Why::Base)
    ++stats.membersUnknownBase;
  if (r.why != Why::None)
    return unknown(r.why);
  if (r.type && r.type->kind == TyKind::Function && last.special == 0)
  {
    // `obj.method` used as a value is only meaningful in a call, which handles overloads.
  }
  // A data member of a const object is const (`rectangle.data.begin()` with `const Rect&
  // rectangle` calls the const begin()), unless it is mutable, static or a reference; an array
  // member's elements are (`this->cells[0] = 5;` in a const member function was no error).
  if (r.type && r.decl && r.decl->kind == NodeKind::VariableDecl && !(r.decl->flags & (DF_Static | DF_Mutable | DF_Typedef)) &&
      !r.type->isReference() && r.type->isKnown() && !Impl::isConstObject(r.type))
    if (const Ty* obj = types.stripRef(object); obj && obj->isConst)
      return types.qualified(r.type, true, r.type->isVolatile || obj->isVolatile);
  // (of a volatile one volatile, the same way, a mutable one too, and read and written as one)
  if (r.type && r.decl && r.decl->kind == NodeKind::VariableDecl && !(r.decl->flags & (DF_Static | DF_Typedef)) && !r.type->isReference() && r.type->isKnown())
    if (const Ty* obj = types.stripRef(object); obj && obj->isVolatile)
      return types.qualified(r.type, r.type->isConst, true);
  return r.type ? r.type : types.unknown();
}

auto GlobalScope::Impl::Walker::reachesClassScope(Scope* cls, Scope* target, int depth) -> bool
{
  if (cls == target)
    return true;
  if (depth > 16 || cls->kind != ScopeKind::Class || cls->baseTypes.empty())
    return false;
  impl.ensureBases(cls, depth);
  for (Scope* b : cls->baseScopes)
    if (reachesClassScope(b, target, depth + 1))
      return true;
  return false;
}

auto GlobalScope::Impl::Walker::reportAmbiguousCall(CallExpr* c, const NamePart& name, const std::vector<const Ty*>& argTypes, const std::vector<FunctionDecl*>& tied, const Subst* classSubst) -> void
{
  if (!Impl::ambiguousCallsAreErrors || tied.size() < 2 || inGenericTemplate() || probing)
    return;
  for (const Ty* a : argTypes)
    if (!a || !a->isKnown() || types.isDependent(a) || Impl::mentionsUnknown(a))
      return;
  std::vector<std::string> candidates;
  for (FunctionDecl* f : tied)
  {
    const Ty* ft = impl.declaredTypeOf(f, classSubst, 0);
    // (a function template's type names its own parameters: `int32(T)` beside `int32(T&)` is
    // as ambiguous, C++'s partial ordering finding neither more specialised)
    if (!ft || (types.isDependent(ft) && f->templateParams.empty()))
      return;
    candidates.push_back(types.toString(ft, &impl.symbols) + " (" + Impl::locationOf(f->file, f->token) + ")");
  }
  std::sort(candidates.begin(), candidates.end());
  std::string message = "the call of " + impl.namePartText(name) + " with (";
  for (size_t i = 0; i < argTypes.size(); ++i)
    message += (i ? ", " : "") + types.toString(argTypes[i], &impl.symbols);
  message += ") is ambiguous between " + candidates[0];
  for (size_t i = 1; i < candidates.size(); ++i)
    message += (i + 1 == candidates.size() ? " and " : ", ") + candidates[i];
  impl.reportCheckerError(file, c->token, message);
}

auto GlobalScope::Impl::Walker::usualDeallocation(const Ty* t, uint32_t token, bool& sized, bool& declared, bool array) -> FunctionDecl*
{
  sized = false;
  declared = false;
  MemberResult r = memberOf(t, array ? impl.symOperatorDeleteArray : impl.symOperatorDelete, nullptr, token, false);
  if (!r.decl || r.decl->kind != NodeKind::FunctionDecl)
    return nullptr;
  declared = true;
  std::vector<FunctionDecl*> candidates;
  if (r.overloads)
  {
    for (const Entry& c : *r.overloads)
      if (c.decl && c.decl->kind == NodeKind::FunctionDecl)
        candidates.push_back(static_cast<FunctionDecl*>(c.decl));
  }
  else
    candidates.push_back(static_cast<FunctionDecl*>(r.decl));
  FunctionDecl* withSize = nullptr;
  for (FunctionDecl* f : candidates)
  {
    if (!f->templateParams.empty())
      continue;
    if (f->params.size() == 1)
      return f; // (of the class's own, the one without a size, [expr.delete] 10)
    if (f->params.size() == 2 && !withSize)
      if (const Ty* second = impl.declaredTypeOf(f->params[1], &r.subst, 0); second && types.unqualified(types.stripRef(second)) == types.sizeType())
        withSize = f;
  }
  sized = withSize != nullptr;
  return withSize;
}

auto GlobalScope::Impl::Walker::chooseAllocation(NewExpr* n, const Ty* t, bool array) -> void
{
  n->allocator = nullptr;
  n->deallocator = nullptr;
  n->sizedDeallocation = false;
  MemberResult r;
  if (t && t->kind == TyKind::Class && !n->global)
    r = memberOf(t, array ? impl.symOperatorNewArray : impl.symOperatorNew, nullptr, n->token, false);
  if (!r.decl || r.decl->kind != NodeKind::FunctionDecl)
  {
    chooseGlobalAllocation(n, t, array);
    return;
  }
  std::vector<const Ty*> argTypes{types.sizeType()};
  for (const Ty* a : argTypesOf(n->placement))
    argTypes.push_back(a);
  bool ambiguous = false;
  FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypes, &r.subst, nullptr, ambiguous);
  if (!f || ambiguous)
  {
    std::string with;
    for (const Ty* a : argTypes)
      with += (with.empty() ? "" : ", ") + types.toString(types.unqualified(types.stripRef(a)), &symbols);
    std::string name = array ? "operator new[]" : "operator new";
    reportHere(n->token, std::string(array ? "the new[] of " : "the new of a ") + types.toString(t, &symbols) +
                             (ambiguous ? " is ambiguous: more than one of its class's " + name + " takes (" : ": none of its class's " + name + " takes (") + with + ")" +
                             (ambiguous ? "" : " (`::new` allocates with the global one)"));
    return;
  }
  if (!f->templateParams.empty())
  {
    reportHere(n->token, "a class's own operator new that is a template is not supported yet");
    return;
  }
  n->allocator = f;
  // (sizedDeallocation: the class's usual operator delete takes the size; an array's then has a
  // cookie, whatever operator new[] made it, for delete[] to know its size)
  bool declared = false;
  FunctionDecl* usual = usualDeallocation(t, n->token, n->sizedDeallocation, declared, array);
  if (n->placement.empty())
  {
    n->deallocator = usual;
    return;
  }
  // A placement form's: the operator delete whose parameters after the first are the operator
  // new's, none when the class declares no such one (the memory is then not given back, as in C++).
  MemberResult d = memberOf(t, array ? impl.symOperatorDeleteArray : impl.symOperatorDelete, nullptr, n->token, false);
  if (!d.decl || d.decl->kind != NodeKind::FunctionDecl)
    return;
  std::vector<FunctionDecl*> candidates;
  if (d.overloads)
  {
    for (const Entry& c : *d.overloads)
      if (c.decl && c.decl->kind == NodeKind::FunctionDecl)
        candidates.push_back(static_cast<FunctionDecl*>(c.decl));
  }
  else
    candidates.push_back(static_cast<FunctionDecl*>(d.decl));
  for (FunctionDecl* g : candidates)
  {
    if (g->params.size() != f->params.size() || !g->templateParams.empty())
      continue;
    bool same = true;
    for (size_t i = 1; i < f->params.size() && same; ++i)
      same = impl.declaredTypeOf(f->params[i], &r.subst, 0) == impl.declaredTypeOf(g->params[i], &d.subst, 0);
    if (same)
    {
      n->deallocator = g;
      return;
    }
  }
}

auto GlobalScope::Impl::Walker::chooseGlobalAllocation(NewExpr* n, const Ty* t, bool array) -> void
{
  if (!t || !t->isKnown() || types.isDependent(t) || !impl.programAllocationFunctions())
    return; // (malloc, or the placement at an address)
  Lookup l;
  if (!impl.lookupInScope(&impl.root, array ? impl.symOperatorNewArray : impl.symOperatorNew, l, 0))
    return;
  std::vector<const Ty*> argTypes{types.sizeType()};
  for (const Ty* a : argTypesOf(n->placement))
    argTypes.push_back(a);
  bool ambiguous = false;
  FunctionDecl* f = pickOverload(l.decl, l.overloads, argTypes, nullptr, nullptr, ambiguous);
  if (!f || ambiguous)
  {
    const Ty* place = n->placement.size() == 1 ? types.unqualified(types.stripRef(argTypes[1])) : nullptr;
    if (!ambiguous && place && (place->kind == TyKind::Pointer || place->kind == TyKind::Array))
      return; // `new (p) T`: the library's placement at an address
    std::string with;
    for (const Ty* a : argTypes)
      with += (with.empty() ? "" : ", ") + types.toString(types.unqualified(types.stripRef(a)), &symbols);
    reportHere(n->token, std::string(ambiguous ? "more than one" : "no") + " global " + (array ? "operator new[]" : "operator new") + " takes (" + with + ")");
    return;
  }
  if (!f->templateParams.empty())
  {
    reportHere(n->token, "a global operator new that is a template is not supported yet");
    return;
  }
  n->allocator = f;
  if (n->placement.empty())
    return; // (the usual global operator delete gives it back: codegen's globalDeallocation)
  // A placement form's: the global operator delete whose parameters after the first are its, none
  // when there is no such one (the memory is then not given back, as in C++).
  Lookup d;
  if (!impl.lookupInScope(&impl.root, array ? impl.symOperatorDeleteArray : impl.symOperatorDelete, d, 0))
    return;
  std::vector<Decl*> all;
  if (d.overloads)
    for (const Entry& c : *d.overloads)
      all.push_back(c.decl);
  else
    all.push_back(d.decl);
  for (Decl* c : all)
  {
    if (!c || c->kind != NodeKind::FunctionDecl || impl.replacedAllocation(static_cast<FunctionDecl*>(c)))
      continue;
    auto* g = static_cast<FunctionDecl*>(c);
    if (g->params.size() != f->params.size() || !g->templateParams.empty())
      continue;
    bool same = true;
    for (size_t i = 1; i < f->params.size() && same; ++i)
      same = impl.declaredTypeOf(f->params[i], nullptr, 0) == impl.declaredTypeOf(g->params[i], nullptr, 0);
    if (same)
    {
      n->deallocator = g;
      return;
    }
  }
}

auto GlobalScope::Impl::Walker::pickOverload(Decl* primary, const std::vector<Entry>* overloads, const std::vector<const Ty*>& argTypes, const Subst* classSubst, const Ty* objectTy, bool& ambiguous, const NamePart* namePart, Span<Expr*> callArgs, std::vector<FunctionDecl*>* tied) -> FunctionDecl*
{
  std::vector<FunctionDecl*> candidates;
  if (overloads)
  {
    for (const Entry& e : *overloads)
      if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
        candidates.push_back(static_cast<FunctionDecl*>(e.decl));
  }
  else if (primary && primary->kind == NodeKind::FunctionDecl)
    candidates.push_back(static_cast<FunctionDecl*>(primary));
  // The library's allocation function a program replaced is no candidate: the program's is it.
  if (candidates.size() > 1)
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](FunctionDecl* c) { return impl.replacedAllocation(c); }), candidates.end());
  // The arguments that are the literal 0, a null pointer for a pointer parameter.
  std::vector<bool> nullConstants;
  bool anyNull = false;
  for (Expr* a : callArgs)
  {
    Expr* x = a;
    while (x && x->kind == NodeKind::ParenExpr)
      x = static_cast<ParenExpr*>(x)->inner;
    bool zero = false;
    if (x && x->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(x)->literalKind == TokenKind::Number)
    {
      std::string_view text = spelling(static_cast<LiteralExpr*>(x));
      zero = isZeroLiteral(text);
    }
    nullConstants.push_back(zero);
    anyNull = anyNull || zero;
  }
  const std::vector<bool>* nulls = anyNull ? &nullConstants : nullptr;
  // The arguments that are bit-fields promoting by their width (`unsigned u3 : 3` to int): their
  // conversion to that type ranks as a promotion ([conv.prom] 5, [over.ics.scs]).
  std::vector<const Ty*> promotions;
  bool anyPromotion = false;
  for (size_t i = 0; i < callArgs.size(); ++i)
  {
    const Ty* promoted = i < argTypes.size() ? impl.bitFieldPromotion(callArgs[i], argTypes[i], ctx().subst) : nullptr;
    promotions.push_back(promoted);
    anyPromotion = anyPromotion || promoted;
  }
  const std::vector<const Ty*>* bitFields = anyPromotion ? &promotions : nullptr;
  if (!namePart && !callSets)
    return impl.chooseAmong(candidates, argTypes, classSubst, objectTy, ambiguous, nullptr, nulls, tied, bitFields);
  // The explicit template arguments (`load<bool>()`): a candidate with fewer parameters than
  // the list names (`typeIndex<T, Tail...>` with Tail known), and no pack to take the rest,
  // is not one, nor one whose parameter is of another kind than its argument. An overload set
  // among the arguments: only a candidate that takes one of its functions (takesCallSets).
  std::function<bool(FunctionDecl*, Subst&)> bind = [&](FunctionDecl* f, Subst& s)
  {
    if (!takesCallSets(f, s))
      return false;
    if (!namePart || !namePart->hasTemplateArgs)
      return true;
    if (namePart->templateArgs.empty()) // `f<>(1)`: a template, its arguments all deduced; never a plain f
      return !f->templateParams.empty() || (f->flags & DF_Constructor);
    size_t n = f->templateParams.size();
    size_t before = s.pairs.size();
    size_t given = bindExplicitArgs(f, *namePart, callArgs, s);
    if (given > n && !(n && static_cast<TemplateParamDecl*>(f->templateParams[n - 1])->isPack))
      return false;
    for (size_t i = before; i < s.pairs.size(); ++i)
      if (!argumentKindFits(s.pairs[i].type, static_cast<TemplateParamDecl*>(s.pairs[i].param)))
        return false; // `create<Abs>` names no `template<VoronoiType TYPE> create`
    return true;
  };
  return impl.chooseAmong(candidates, argTypes, classSubst, objectTy, ambiguous, &bind, nulls, tied, bitFields);
}

auto GlobalScope::Impl::Walker::packParamIn(Node* n, int depth) -> Decl*
{
  if (!n || depth > 32)
    return nullptr;
  switch (n->kind)
  {
    case NodeKind::NameExpr:
    {
      // Before the walk resolved the name (an instance's copy, typed the first time), the
      // parameter it names among the locals.
      auto* name = static_cast<NameExpr*>(n);
      Decl* d = name->resolved;
      if (!d && name->name.parts.size() == 1 && !name->name.parts[0].hasTemplateArgs)
        d = locals.find(name->name.parts[0].symbol);
      return d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->isPack ? d : nullptr;
    }
    case NodeKind::ParenExpr: return packParamIn(static_cast<ParenExpr*>(n)->inner, depth + 1);
    case NodeKind::UnaryExpr: return packParamIn(static_cast<UnaryExpr*>(n)->operand, depth + 1);
    case NodeKind::CastExpr: return packParamIn(static_cast<CastExpr*>(n)->operand, depth + 1);
    case NodeKind::NamedCastExpr: return packParamIn(static_cast<NamedCastExpr*>(n)->operand, depth + 1);
    case NodeKind::MemberExpr: return packParamIn(static_cast<MemberExpr*>(n)->base, depth + 1);
    case NodeKind::BinaryExpr: // `v * 2` (`[...w = v * 2]`, `f(v + 1)...`)
      if (Decl* d = packParamIn(static_cast<BinaryExpr*>(n)->lhs, depth + 1))
        return d;
      return packParamIn(static_cast<BinaryExpr*>(n)->rhs, depth + 1);
    case NodeKind::CallExpr:
    {
      auto* c = static_cast<CallExpr*>(n);
      if (Decl* d = packParamIn(c->callee, depth + 1))
        return d;
      for (Expr* a : c->args)
        if (Decl* d = packParamIn(a, depth + 1))
          return d;
      return nullptr;
    }
    default: return nullptr;
  }
}

auto GlobalScope::Impl::Walker::isEmptyExpansion(Expr* a) -> bool
{
  if (!a || a->kind != NodeKind::UnaryExpr || static_cast<UnaryExpr*>(a)->isKeyword ||
      P(static_cast<UnaryExpr*>(a)->op) != P::Ellipsis || !static_cast<UnaryExpr*>(a)->postfix)
    return false;
  Decl* pd = packParamIn(static_cast<UnaryExpr*>(a)->operand);
  if (!pd)
    return false;
  const Ty* pt = impl.declaredTypeOf(pd, activeSubst, 0);
  return pt && pt->kind == TyKind::Pack && pt->args.empty();
}

auto GlobalScope::Impl::Walker::argTypesOf(const Span<Expr*>& args) -> std::vector<const Ty*>
{
  bool walked = walkedArgs && args.data == walkedArgs && args.size() == walkedArgTypes.size();
  std::vector<const Ty*> out;
  for (size_t i = 0; i < args.size(); ++i)
  {
    Expr* a = args[i];
    if (isEmptyExpansion(a))
      continue;
    const Ty* t = walked ? walkedArgTypes[i] : a ? a->ty : nullptr;
    if (a && t && t->kind == TyKind::Pack)
    {
      // Each element as its pattern is one: `args...` of a pack of parameters gives lvalues (named),
      // by value or forwarding, so that a forwarding reference given one deduces an lvalue
      // reference and forward<> copies it (`Args... args` given a String deduced String, and
      // makeUnique moved from it: templates/by_value_pack_element_forwards_as_lvalue).
      Expr* pattern = a;
      if (a->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(a)->isKeyword && static_cast<UnaryExpr*>(a)->postfix &&
          P(static_cast<UnaryExpr*>(a)->op) == P::Ellipsis && static_cast<UnaryExpr*>(a)->operand)
        pattern = static_cast<UnaryExpr*>(a)->operand;
      while (pattern->kind == NodeKind::ParenExpr && static_cast<ParenExpr*>(pattern)->inner)
        pattern = static_cast<ParenExpr*>(pattern)->inner;
      for (const Ty* e : t->args)
        out.push_back(argumentType(pattern, e, types));
    }
    else if (a && a->kind == NodeKind::BraceInitExpr)
      out.push_back(braceListType(a, impl.initializerListDecl(), types));
    else
      out.push_back(a ? argumentType(a, t, types) : nullptr);
  }
  return out;
}

auto GlobalScope::Impl::Walker::bindExplicitArgs(Span<Decl*> templateParams, const NamePart& namePart, Span<Expr*> args, Subst& s, bool emptyPackWithoutArgs) -> size_t
{
  size_t n = templateParams.size(), named = 0;
  std::vector<const Ty*> given;
  // A parameter pack takes the written arguments from its position on, wherever it is
  // (`template<typename... Ts, typename InputT, typename... Args> dummyLoad(InputT&, Args&&...)`
  // called `dummyLoad<char, int>(input)`): the parameters after it are deduced.
  size_t firstPack = n;
  for (size_t i = 0; i < n; ++i)
    if (static_cast<TemplateParamDecl*>(templateParams[i])->isPack)
    {
      firstPack = i;
      break;
    }
  for (Node* written : namePart.templateArgs)
  {
    Decl* param = n ? templateParams[std::min(std::min(given.size(), n - 1), firstPack)] : nullptr;
    const Ty* t = explicitArgument(written, param);
    bool expansion = written && written->kind == NodeKind::PackType;
    if (expansion && t && t->kind == TyKind::Pack)
    {
      given.insert(given.end(), t->args.begin(), t->args.end());
      named += t->args.size();
    }
    else
    {
      given.push_back(t);
      named += !expansion;
    }
  }
  if (n == 0)
    return named;
  for (size_t i = 0; i < given.size() && i < n; ++i)
  {
    auto* param = static_cast<TemplateParamDecl*>(templateParams[i]);
    if (param->isPack)
    {
      std::vector<const Ty*> rest(given.begin() + i, given.end());
      s.pairs.push_back({param, types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
      return named;
    }
    s.pairs.push_back({param, given[i]});
  }
  if (namePart.hasTemplateArgs && args.empty() && emptyPackWithoutArgs)
  {
    auto* last = static_cast<TemplateParamDecl*>(templateParams[n - 1]);
    if (last->isPack && !s.find(last))
      s.pairs.push_back({last, types.pack({})}); // `count<int>()`: nothing left for Args
  }
  return named;
}

auto GlobalScope::Impl::Walker::bindExplicitArgs(FunctionDecl* f, const NamePart& namePart, Span<Expr*> args, Subst& s, bool emptyPackWithoutArgs) -> size_t
{
  return bindExplicitArgs(f->templateParams, namePart, args, s, emptyPackWithoutArgs);
}

auto GlobalScope::Impl::Walker::argumentKindFits(const Ty* t, const TemplateParamDecl* p) -> bool
{
  if (!t || !t->isKnown() || !p->innerParams.empty())
    return true; // template template parameters take templates: not checked here
  if (t->kind == TyKind::Pack)
  {
    for (const Ty* e : t->args)
      if (!argumentKindFits(e, p))
        return false;
    return true;
  }
  bool constantParam = t->kind == TyKind::Param && t->decl && !static_cast<TemplateParamDecl*>(t->decl)->isType;
  if (p->isType)
    return t->kind != TyKind::Value && !constantParam;
  return t->kind == TyKind::Value || constantParam || t->kind == TyKind::DependentMember; // `T::value` may be either
}

auto GlobalScope::Impl::Walker::explicitArgument(Node* written, Decl* param) -> const Ty*
{
  const Ty* t = impl.templateArgument(written, param, ctx(), 0);
  bool packParam = param && param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(param)->isPack;
  if (t && t->kind == TyKind::Pack && t->args.size() == 1 && !packParam && !(written && written->kind == NodeKind::PackType))
    return t->args[0];
  return t;
}

auto GlobalScope::Impl::Walker::recordTemplateArgs(CallExpr* call, FunctionDecl* f, const Subst& s) -> void
{
  const Ty* found[8] = {};
  uint8_t n = 0;
  for (Decl* p : f->templateParams)
  {
    const Ty* t = s.find(p);
    if (!t || n >= 8)
      break;
    found[n++] = t;
  }
  for (uint8_t i = 0; i < n; ++i)
    call->templateArgs[i] = found[i];
  call->templateArgCount = n;
}

auto GlobalScope::Impl::Walker::returnTypeOfCall(FunctionDecl* f, const Subst* classSubst, const Span<Expr*>& args, const NamePart* namePart, CallExpr* call, const std::vector<const Ty*>* operands) -> const Ty*
{
  auto argTypes = [&] { return operands ? *operands : argTypesOf(args); };
  noteDeprecatedCall(f, call);
  if (f->body == nullptr && f->returnType == nullptr && !(f->flags & (DF_Constructor | DF_Destructor | DF_Conversion)))
    return unknown(Why::Unsupported);
  bool autoReturn = false;
  if (f->returnType && Impl::astMentionsAuto(f->returnType) && !f->trailingReturn)
    autoReturn = true; // `auto`, `auto&`, `const auto*`: the body decides
  if (autoReturn)
  {
    if (!f->templateParams.empty())
    {
      Subst s = classSubst ? *classSubst : Subst{};
      const Ty* fnTy = impl.declaredTypeOf(f, classSubst, 0);
      if (namePart)
        bindExplicitArgs(f, *namePart, args, s);
      impl.deduceCall(f, fnTy, argTypes(), s);
      impl.fillDefaultTemplateArgs(f, s);
      if (call)
        recordTemplateArgs(call, f, s);
      if (const Ty* r = instantiateFunction(f, s, true, classSubst))
        return r;
      return unknown(Why::AutoReturn);
    }
    // The body's deduction through the function's one instance (checked once, whoever asks
    // first), never through what its own file's walk cached: that was there or not by which
    // file was walked first, and the instance walked or not with it.
    if (const Ty* r = instantiateFunction(f, Subst{}, true, classSubst))
      return r;
    return unknown(Why::AutoReturn);
  }
  Subst subst = classSubst ? *classSubst : Subst{};
  // The callee's type for deduction: the enclosing instantiation's arguments apply, but never
  // to the callee's own parameters (a template calling itself, `sum(rest...)` inside
  // `sum<int, int, int>`, deduces them afresh from the arguments).
  Subst outer;
  const Subst* forType = classSubst ? classSubst : activeSubst;
  if (!classSubst && activeSubst && !f->templateParams.empty())
  {
    for (const Subst::Pair& p : activeSubst->pairs)
    {
      bool own = false;
      for (Decl* tp : f->templateParams)
        own = own || tp == p.param;
      if (!own)
        outer.pairs.push_back(p);
    }
    forType = outer.pairs.empty() ? nullptr : &outer;
  }
  const Ty* fnTy = impl.declaredTypeOf(f, forType, 0);
  if (!f->templateParams.empty())
  {
    // Explicit template arguments first, then deduction from the arguments.
    if (namePart)
      bindExplicitArgs(f, *namePart, args, subst);
    impl.deduceCall(f, fnTy, argTypes(), subst);
    impl.fillDefaultTemplateArgs(f, subst);
    if (call)
      recordTemplateArgs(call, f, subst);
    {
      static bool debugCalls = getenv("KPP_DEBUG_CALLS") != nullptr;
      if (debugCalls)
      {
        std::string line = std::string(call ? "call " : "call(no expr) ") + impl.debugName(f->name.last().symbol) + ":";
        for (size_t i = 0; call && i < call->templateArgCount; ++i)
          line += " " + types.toString(call->templateArgs[i], &impl.symbols);
        line += " | args:";
        for (const Ty* a : argTypes())
          line += " " + types.toString(a, &impl.symbols);
        line += " | explicit:";
        if (namePart)
          for (size_t i = 0; i < namePart->templateArgs.size() && i < f->templateParams.size(); ++i)
            line += " " + types.toString(impl.templateArgument(namePart->templateArgs[i], f->templateParams[i], ctx(), 0), &impl.symbols);
        line += " | subst:";
        if (activeSubst)
          for (auto& [d, ty] : activeSubst->pairs)
            line += " " + impl.debugName(static_cast<TemplateParamDecl*>(d)->name) + "=" + types.toString(ty, &impl.symbols);
        fprintf(stderr, "%s\n", line.c_str());
      }
    }
    const Ty* result = impl.substitute(fnTy->inner, subst);
    Type* written = f->trailingReturn ? f->trailingReturn : f->returnType;
    LocalStack own;
    Impl::pushEnclosingTemplateParams(f, own);
    if ((f->trailingReturn && Impl::astMentionsDecltype(f->trailingReturn)) || impl.astNeedsArguments(written, impl.lexicalScopeOf(f), &own))
    {
      // `-> Future<decltype(callable())>`, `typename Conditional<(sizeof(A) < sizeof(B)), B,
      // A>::Type`: typed again with the deduced arguments, since the generic type substituted
      // keeps neither what calling the closure gives nor which specialisation the condition picks.
      Subst all = subst;
      if (forType)
        for (const Subst::Pair& p : forType->pairs)
          if (!all.find(p.param))
            all.pairs.push_back(p);
      if (const Ty* full = impl.declaredTypeOf(f, &all, 0); full && full->kind == TyKind::Function && full->inner->isKnown())
        result = full->inner;
    }
    if (impl.dependsOn(result, f->templateParams))
      return unknown(Why::Deduction);
    if (!types.isDependent(result))
      instantiateFunction(f, subst, false, classSubst);
    return result;
  }
  return fnTy->inner;
}

auto GlobalScope::Impl::Walker::addArgumentDependentCandidates(Lookup& l, uint32_t symbol, const std::vector<const Ty*>& argTypes, Subst& friendArgs, uint32_t token) -> void
{
  std::vector<Scope*> namespaces;
  for (const Ty* a : argTypes)
    impl.associatedNamespaces(a, namespaces);
  for (Scope* ns : namespaces)
  {
    if (ns == l.foundIn)
      continue;
    Lookup found;
    if (!impl.lookupInScope(ns, symbol, found, 0, false) || !found.decl || found.decl->kind != NodeKind::FunctionDecl || found.foundIn != ns)
      continue;
    l = l.found == Found::No ? found : impl.mergeOverloads(l, found);
  }
  std::string conflict;
  Lookup friends = impl.hiddenFriendLookup(symbol, argTypes, friendArgs, &conflict);
  if (!conflict.empty())
    reportHere(token, conflict);
  if (friends.decl)
    l = l.found == Found::No ? friends : impl.mergeOverloads(l, friends);
}

auto GlobalScope::Impl::Walker::reportHere(uint32_t token, const std::string& message) -> void
{
  if (!file || file->isImported || probing)
    return;
  if (activeSubst)
    reportInInstance(token, message);
  else if (!inGenericTemplate())
    impl.reportCheckerError(file, token, message);
}

auto GlobalScope::Impl::Walker::onlyPlainCandidate(Decl* d, const std::vector<Entry>* overloads) -> bool
{
  if (!d || d->kind != NodeKind::FunctionDecl || !d->templateParams.empty())
    return false;
  size_t functions = 0;
  if (overloads)
    for (const Entry& e : *overloads)
      functions += e.decl->kind == NodeKind::FunctionDecl;
  return functions <= 1;
}

auto GlobalScope::Impl::Walker::namesFunctionsWithoutTarget(Expr* e) -> bool
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (e && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->isKeyword && !static_cast<UnaryExpr*>(e)->postfix &&
      P(static_cast<UnaryExpr*>(e)->op) == P::Amp)
    e = static_cast<UnaryExpr*>(e)->operand;
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (e && e->kind == NodeKind::MemberExpr)
  {
    // `t.f` not called: a non-static member function is no expression unless called
    // ([expr.ref]); static ones as a name of them.
    auto* m = static_cast<MemberExpr*>(e);
    if (!m->resolved || m->resolved->kind != NodeKind::FunctionDecl || m->member.empty())
      return false;
    if (!(m->resolved->flags & DF_Static))
      return true;
    if (m->member.last().hasTemplateArgs)
      return false;
    const Ty* object = m->base && m->base->ty ? types.unqualified(types.stripRef(m->base->ty)) : nullptr;
    if (object && m->op == P::Arrow)
      object = object->kind == TyKind::Pointer && object->inner ? types.unqualified(object->inner) : nullptr;
    if (!object || object->kind != TyKind::Class)
      return false;
    MemberResult r = memberOf(object, m->member.last().symbol, nullptr, m->token, false);
    return r.decl && r.decl->kind == NodeKind::FunctionDecl && !onlyPlainCandidate(r.decl, r.overloads);
  }
  if (!e || e->kind != NodeKind::NameExpr)
    return false;
  auto* n = static_cast<NameExpr*>(e);
  if (!n->resolved || n->resolved->kind != NodeKind::FunctionDecl || n->name.empty() || n->name.last().hasTemplateArgs)
    return false;
  Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
  if (l.found == Found::No && n->name.parts.size() > 1)
    l = memberOfQualifierInstance(n);
  return l.decl && l.decl->kind == NodeKind::FunctionDecl && !onlyPlainCandidate(l.decl, l.overloads);
}

auto GlobalScope::Impl::Walker::setHasFunctionOf(const OverloadSetArgument& set, const Ty* target) -> bool
{
  Subst qs = set.name->name.parts.size() > 1 ? impl.qualifierSubst(set.name->name, ctx(), 0, set.set.foundIn) : Subst{};
  for (const Entry& e : *set.set.overloads)
    if (e.decl && e.decl->kind == NodeKind::FunctionDecl && e.decl->templateParams.empty())
      if (const Ty* t = impl.namedFunctionType(e.decl, impl.declaredTypeOf(e.decl, qs.empty() ? activeSubst : &qs, 0), qs.empty() ? activeSubst : &qs);
          t == target || impl.losesNoexcept(t, target))
        return true;
  return false;
}

auto GlobalScope::Impl::Walker::takesCallSets(FunctionDecl* f, Subst& s) -> bool
{
  if (!callSets)
    return true;
  const Ty* ft = impl.declaredTypeOf(f, s.pairs.empty() ? nullptr : &s, 0);
  if (!ft || ft->kind != TyKind::Function)
    return true;
  for (const OverloadSetArgument& set : *callSets)
  {
    if (set.index >= ft->args.size() || !set.set.overloads)
      continue;
    const Ty* pt = types.unqualified(types.stripRef(ft->args[set.index]));
    const Ty* target = pt && (pt->kind == TyKind::Pointer || pt->kind == TyKind::MemberPointer) ? pt->inner : pt;
    if (!target || target->kind != TyKind::Function || types.isDependent(target))
      continue;
    if (!setHasFunctionOf(set, target))
      return false;
  }
  return true;
}

auto GlobalScope::Impl::Walker::overloadSetArgument(Expr* a, size_t index, OverloadSetArgument& out) -> bool
{
  while (a && a->kind == NodeKind::ParenExpr)
    a = static_cast<ParenExpr*>(a)->inner;
  Expr* address = nullptr;
  if (a && a->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(a)->isKeyword && !static_cast<UnaryExpr*>(a)->postfix &&
      P(static_cast<UnaryExpr*>(a)->op) == P::Amp)
  {
    address = a;
    a = static_cast<UnaryExpr*>(a)->operand;
  }
  if (!a || a->kind != NodeKind::NameExpr)
    return false;
  auto* n = static_cast<NameExpr*>(a);
  if (!n->resolved || n->resolved->kind != NodeKind::FunctionDecl || n->name.empty())
    return false;
  Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
  if (l.found == Found::Global)
    l = impl.withNamespaceUsings(l, 0);
  size_t functions = 0;
  if (l.overloads)
    for (const Entry& e : *l.overloads)
      functions += e.decl && e.decl->kind == NodeKind::FunctionDecl && e.decl->templateParams.empty();
  if (functions < 2)
    return false;
  out = OverloadSetArgument{index, n, address, l};
  return true;
}

auto GlobalScope::Impl::Walker::bindTemplateAddress(Expr* a, const Ty* target) -> void
{
  Expr* named = a;
  while (named && named->kind == NodeKind::ParenExpr)
    named = static_cast<ParenExpr*>(named)->inner;
  Expr* address = nullptr;
  if (named && named->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(named)->isKeyword && P(static_cast<UnaryExpr*>(named)->op) == P::Amp)
  {
    address = named;
    named = static_cast<UnaryExpr*>(named)->operand;
  }
  if (!named || named->kind != NodeKind::NameExpr || !target)
    return;
  auto* n = static_cast<NameExpr*>(named);
  if (!n->resolved || n->resolved->kind != NodeKind::FunctionDecl || n->name.empty())
    return;
  auto* f = static_cast<FunctionDecl*>(n->resolved);
  if (f->templateParams.empty() && n->name.last().hasTemplateArgs)
  {
    // `constructExpression<BitwiseNot>` beside a constructExpression(double) that is no
    // template: the written arguments name the template (the only one of the name).
    Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
    FunctionDecl* only = nullptr;
    int templates = 0;
    if (l.overloads)
      for (const Entry& entry : *l.overloads)
        if (entry.decl && entry.decl->kind == NodeKind::FunctionDecl && !entry.decl->templateParams.empty())
        {
          only = static_cast<FunctionDecl*>(entry.decl);
          ++templates;
        }
    if (templates != 1)
      return;
    f = only;
  }
  if (f->templateParams.empty())
  {
    // `AreaFn f = Shape::area;` beside an area(int): of the name's functions, the one of the pointer's type, as for
    // an argument (bindOverloadSets).
    if (OverloadSetArgument set; !n->name.last().hasTemplateArgs && overloadSetArgument(a, 0, set))
      bindOverloadSet(set, target);
    return;
  }
  const Ty* pt = types.unqualified(types.stripRef(target));
  const Ty* want = pt && pt->kind == TyKind::Pointer ? pt->inner : pt;
  if (!want || want->kind != TyKind::Function || !want->isKnown() || types.isDependent(want))
    return;
  Subst s;
  bindExplicitArgs(f, n->name.last(), Span<Expr*>{}, s, false);
  if (activeSubst)
    for (const Subst::Pair& p : activeSubst->pairs)
      if (!s.find(p.param))
        s.pairs.push_back(p); // the enclosing instance's (a member of a class template)
  const Ty* fnTy = impl.declaredTypeOf(f, &s, 0);
  std::vector<const Ty*> argTypes(want->args.begin(), want->args.end());
  impl.deduceCall(f, fnTy, argTypes, s);
  for (Decl* p : f->templateParams)
    if (!s.find(p))
    {
      if (!static_cast<TemplateParamDecl*>(p)->isPack)
        return; // not deduced: left as typed
      s.pairs.push_back({p, types.pack({})});
    }
  const Ty* instance = impl.declaredTypeOf(f, &s, 0);
  if (!instance || instance->kind != TyKind::Function || !instance->isKnown() || types.isDependent(instance))
    return;
  n->resolved = f;
  record(n, instance);
  if (address)
    record(address, types.pointer(instance));
}

auto GlobalScope::Impl::Walker::bindTemplateAddresses(Expr* e, const Ty* target, int depth) -> void
{
  if (!e || !target || depth > 8)
    return;
  const Ty* t = types.unqualified(types.stripRef(target));
  if (t->kind == TyKind::Function)
    t = types.pointer(t); // `int (&r)(double) = twice;` chooses as a pointer does
  if (e->kind != NodeKind::BraceInitExpr)
  {
    bindTemplateAddress(e, t);
    return;
  }
  auto* list = static_cast<BraceInitExpr*>(e);
  if (!list->designators.empty())
    return;
  if (t->kind == TyKind::Array && t->inner)
  {
    for (Expr* a : list->args)
      bindTemplateAddresses(a, t->inner, depth + 1);
    return;
  }
  if (t->kind != TyKind::Class || !t->isKnown() || types.isDependent(t))
    return;
  if (impl.isInitializerList(t))
  {
    if (!t->args.empty())
      for (Expr* a : list->args)
        bindTemplateAddresses(a, t->args[0], depth + 1);
    return;
  }
  Scope* s = impl.classScopeOf(t);
  if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
    return;
  auto* cls = static_cast<ClassDecl*>(s->owner);
  Subst subst = impl.substFor(t);
  // A class with constructors: the one constructor (copy and move ones aside) whose parameters
  // the list's elements can fill, its parameter types (`OperationTestCase(const char* name,
  // ExpressionBuilder expressionBuilder, ...)`); several or none decide nothing here.
  FunctionDecl* taker = nullptr;
  int takers = 0;
  bool constructors = false;
  for (Decl* m : membersRead(cls))
  {
    if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || (m->flags & DF_Delete))
      continue;
    constructors = true;
    auto* f = static_cast<FunctionDecl*>(m);
    size_t required = 0;
    for (Decl* p : f->params)
      required += !static_cast<ParamDecl*>(p)->defaultValue && !static_cast<ParamDecl*>(p)->isPack;
    if (f->params.size() < list->args.size() || required > list->args.size() || !f->templateParams.empty())
      continue;
    if (f->params.size() == 1)
      if (const Ty* only = impl.declaredTypeOf(f->params[0], subst.empty() ? nullptr : &subst, 0);
          only && types.unqualified(types.stripRef(only)) == t)
        continue; // the copy or move constructor
    taker = f;
    ++takers;
  }
  if (constructors)
  {
    if (takers != 1)
      return;
    for (size_t i = 0; i < list->args.size(); ++i)
      bindTemplateAddresses(list->args[i], impl.declaredTypeOf(taker->params[i], subst.empty() ? nullptr : &subst, 0), depth + 1);
    return;
  }
  if (!cls->bases.empty())
    return;
  size_t i = 0;
  for (Decl* m : membersRead(cls))
  {
    if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef | DF_Friend)))
      continue;
    if (i >= list->args.size())
      break;
    bindTemplateAddresses(list->args[i++], impl.declaredTypeOf(m, subst.empty() ? nullptr : &subst, 0), depth + 1);
  }
}

auto GlobalScope::Impl::Walker::bindOverloadSet(const OverloadSetArgument& s, const Ty* pt) -> void
{
  pt = pt ? types.unqualified(types.stripRef(pt)) : nullptr;
  const Ty* target = pt && (pt->kind == TyKind::Pointer || pt->kind == TyKind::MemberPointer) ? pt->inner : pt;
  if (!target || target->kind != TyKind::Function || types.isDependent(target))
    return;
  Subst qs = s.name->name.parts.size() > 1 ? impl.qualifierSubst(s.name->name, ctx(), 0, s.set.foundIn) : Subst{};
  FunctionDecl* chosen = nullptr;
  const Ty* chosenTy = nullptr;
  bool several = false;
  for (const Entry& e : *s.set.overloads)
  {
    if (!e.decl || e.decl->kind != NodeKind::FunctionDecl || !e.decl->templateParams.empty())
      continue;
    const Ty* ot = impl.namedFunctionType(e.decl, impl.declaredTypeOf(e.decl, qs.empty() ? activeSubst : &qs, 0), qs.empty() ? activeSubst : &qs);
    if (ot != target && !impl.losesNoexcept(ot, target))
      continue;
    several = several || chosen;
    chosen = static_cast<FunctionDecl*>(e.decl);
    chosenTy = ot;
  }
  if (!chosen || several || chosen == s.name->resolved)
    return;
  s.name->resolved = chosen;
  record(s.name, chosenTy);
  if (s.address && s.address->ty)
  {
    const Ty* was = s.address->ty;
    if (was->kind == TyKind::MemberPointer && !was->args.empty())
      record(s.address, types.memberPointer(chosenTy, was->args[0]));
    else if (was->kind == TyKind::Pointer)
      record(s.address, types.pointer(chosenTy));
  }
}

auto GlobalScope::Impl::Walker::bindOverloadSets(const std::vector<OverloadSetArgument>& sets, const Ty* fnTy) -> void
{
  if (!fnTy || fnTy->kind != TyKind::Function)
    return;
  for (const OverloadSetArgument& s : sets)
    if (s.index < fnTy->args.size())
      bindOverloadSet(s, fnTy->args[s.index]);
}

auto GlobalScope::Impl::Walker::checkClassToScalar(const Ty* from, const Ty* to, uint32_t token, bool ambiguousOnly) -> void
{
  if (probing || activeSubst || !file || !from || !to || !from->isKnown() || !to->isKnown() || types.isDependent(from) ||
      types.isDependent(to))
    return;
  const Ty* f = types.unqualified(types.stripRef(from));
  const Ty* t = types.unqualified(types.stripRef(to));
  if (f->kind != TyKind::Class || f->isBraceList() || Impl::lambdaOf(f) || Impl::mentionsUnknown(f) || (f->decl && (f->decl->attributes & ATTR_Draft)))
    return;
  if (!t->isArithmetic() && t->kind != TyKind::Enum && t->kind != TyKind::Pointer)
    return;
  // The one rule codegen converts by (design.md 24, chooseConversion): an error exactly where it
  // finds no operator.
  bool ambiguous = false;
  if (impl.chooseConversion(f, t, GlobalScope::ConversionKind::ToType, false, nullptr, nullptr, false, &ambiguous))
    return; // (an explicit operator bool converts a condition, never an argument or a copy-initialization)
  if (ambiguous)
  {
    // Two conversion operators equally good (`operator int` and `operator double` to a long): C++'s
    // ambiguous conversion, here rather than codegen's "a class where a scalar is expected".
    impl.reportCheckerError(file, token, "the conversion of " + types.toString(f, &symbols) + " to " + types.toString(t, &symbols) +
                                           " is ambiguous: more than one of its conversion operators converts to it equally well");
    return;
  }
  if (ambiguousOnly)
    return;
  impl.reportCheckerError(file, token, "a class (" + types.toString(f, &symbols) + ") does not convert to " + types.toString(t, &symbols) +
                                         ": it has no implicit conversion operator to a scalar");
}

auto GlobalScope::Impl::Walker::checkParenthesizedArray(Span<Expr*> args, const Ty* array, uint32_t token) -> void
{
  if (probing || !file || file->isImported || !array || array->kind != TyKind::Array || !array->isKnown() || types.isDependent(array) || inGenericTemplate())
    return;
  for (Expr* a : args)
    if (!a || (a->kind == NodeKind::UnaryExpr && static_cast<UnaryExpr*>(a)->postfix && P(static_cast<UnaryExpr*>(a)->op) == P::Ellipsis))
      return; // (a pack's elements: counted where they are expanded)
  const Ty* element = array->inner;
  std::string elementName = types.toString(element, &symbols);
  if (array->count != 0 && args.size() > array->count)
  {
    reportCheck(args[array->count]->token, std::to_string(args.size()) + " values in parentheses for an array of " + std::to_string(array->count) + " " + elementName);
    return;
  }
  for (Expr* a : args)
  {
    if (a->kind == NodeKind::BraceInitExpr || !a->ty || !a->ty->isKnown() || types.isDependent(a->ty) || Impl::mentionsUnknown(a->ty))
      continue; // (an element's own braces, as in a list; what the walk could not type is said where it is)
    const Ty* from = argumentType(a, a->ty, types);
    if (impl.conversionRank(from, element, 0) >= NotViable)
      reportCheck(a->token, "an array's element of type " + elementName + " is initialized from a value of type " + types.toString(types.stripRef(from), &symbols) +
                                ", which does not convert to it (parentheses elide no braces: each value is one element)");
  }
  if (array->count > args.size())
    if (std::optional<bool> fills = impl.defaultConstructible(element); fills && !*fills)
      reportCheck(token, "the array's elements after the " + std::to_string(args.size()) + " values in parentheses are value-initialized, and " + elementName +
                             " has no default constructor");
}

auto GlobalScope::Impl::Walker::checkConstruction(const Ty* t, Span<Expr*> args, uint32_t token) -> void
{
  if (probing || activeSubst || !file || !t || !t->isKnown() || types.isDependent(t))
    return;
  const Ty* c = types.unqualified(types.stripRef(t));
  if (c->kind != TyKind::Class || c->isBraceList() || Impl::lambdaOf(c) || Impl::mentionsUnknown(c) || (c->decl && (c->decl->attributes & ATTR_Draft)))
    return;
  ClassDecl* def = impl.classDefinition(c);
  if (!def || def->isTypedUnion || args.empty())
    return; // (no arguments: the default constructor, or an aggregate's value-initialization)
  std::vector<const Ty*> argTypes;
  for (Expr* a : args)
  {
    if (!a || a->kind == NodeKind::BraceInitExpr || !a->ty || !a->ty->isKnown() || types.isDependent(a->ty) || Impl::mentionsUnknown(a->ty))
      return; // (a brace list, a pack expansion's element, an argument not typed: left to codegen, as before)
    if (a->kind == NodeKind::UnaryExpr && static_cast<UnaryExpr*>(a)->postfix && P(static_cast<UnaryExpr*>(a)->op) == P::Ellipsis)
      return;
    argTypes.push_back(argumentType(a, a->ty, types));
  }
  bool constructors = false;
  for (Decl* m : membersRead(def))
    constructors = constructors || (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor));
  std::vector<const Ty*> inherited;
  impl.inheritedConstructorSources(def, c, inherited, 0);
  if (!constructors && inherited.empty())
    return; // an aggregate: its members take the arguments
  if (argTypes.size() == 1)
  {
    // A copy or move from the class or a class derived from it: its implicit copy or move
    // constructor takes it where it declares none.
    const Ty* a = types.unqualified(types.stripRef(argTypes[0]));
    if (a == c || (a->kind == TyKind::Class && impl.isBaseClass(c, a)))
      return;
    // From a class with a conversion operator, own or a base's (an explicit one too, in a direct
    // initialization): it may make the T that the copy or move constructor takes (`SignalID(connector)`
    // through SignalIDConnector's `operator const SignalID&()`). Not checked here yet: left to codegen.
    std::function<bool(const Ty*, int)> converts = [&](const Ty* k, int level) -> bool
    {
      ClassDecl* kd = k && k->kind == TyKind::Class ? impl.classDefinition(k) : nullptr;
      if (!kd || level > 8)
        return false;
      for (Decl* m : membersRead(kd))
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Conversion))
          return true;
      std::vector<const Ty*> bases;
      if (impl.directBases(k, kd, bases))
        for (const Ty* b : bases)
          if (converts(b, level + 1))
            return true;
      return false;
    };
    if (converts(a, 0))
      return;
  }
  bool ambiguous = false;
  FunctionDecl* chosen = impl.chooseConstructor(c, argTypes, nullptr, nullptr, false, &ambiguous);
  if (chosen && !ambiguous)
    return;
  std::string list;
  for (const Ty* a : argTypes)
    list += (list.empty() ? "" : ", ") + types.toString(a, &symbols);
  impl.reportCheckerError(file, token, ambiguous ? "the construction of " + types.toString(c, &symbols) + " from (" + list + ") is ambiguous: more than one constructor takes it as well"
                                                 : "no constructor of " + types.toString(c, &symbols) + " takes (" + list + ")");
}

auto GlobalScope::Impl::Walker::checkExplicitCopy(const Ty* from, const Ty* to, uint32_t token) -> void
{
  if (probing || activeSubst || !file || !from || !to || !from->isKnown() || !to->isKnown() || types.isDependent(from) || types.isDependent(to) ||
      Impl::mentionsUnknown(from) || Impl::mentionsUnknown(to))
    return;
  const Ty* t = types.unqualified(types.stripRef(to));
  const Ty* f = types.unqualified(types.stripRef(from));
  if (t->kind != TyKind::Class || f == t || f->isBraceList() || f->kind == TyKind::Pack || (t->decl && (t->decl->attributes & ATTR_Draft)) ||
      (to->kind == TyKind::LRef && !to->inner->isConst) || (f->kind == TyKind::Class && impl.isBaseClass(t, f)))
    return; // (a non-const lvalue reference binds no converted temporary: another error)
  if (impl.conversionRank(from, t, 0) < NotViable)
    return; // an implicit conversion
  bool ambiguous = false;
  FunctionDecl* ctor = impl.chooseConstructor(t, {from}, nullptr, nullptr, false, &ambiguous);
  Subst s = impl.substFor(t);
  // `Outer2 o = 5;` with `Outer2(Inner)` and `Inner(int)`: the constructor takes the value only
  // through a conversion of its own, a second user-defined conversion, which no implicit
  // conversion has ([over.best.ics] 4; codegen made both).
  // (Not of a class with a constructor template, which may take the value itself, one conversion:
  // Optional's `Optional(U&& v)` given a typed union's alternative for an Optional<Union>.)
  bool constructorTemplate = false;
  if (ClassDecl* def = impl.classDefinition(t))
    for (Decl* m : membersRead(def))
      constructorTemplate = constructorTemplate || (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !m->templateParams.empty());
  if (ctor && !ambiguous && !constructorTemplate && ctor->templateParams.empty() && !ctor->params.empty() && !impl.isExplicit(ctor, s.empty() ? nullptr : &s))
  {
    const Ty* pt = impl.declaredTypeOf(ctor->params[0], s.empty() ? nullptr : &s, 0);
    if (int rank = pt ? impl.conversionRank(from, pt, 0) : NotViable; rank >= 4 && rank < NotViable)
    {
      std::string name = types.toString(t, &symbols), via = types.toString(types.unqualified(types.stripRef(pt)), &symbols);
      reportHere(token, "copy-initialising " + name + " from " + types.toString(f, &symbols) + " takes two user-defined conversions, to " + via +
                          " and then to " + name + ", where an implicit conversion has one at most ([over.best.ics] 4): " + name + "(...) says which");
      return;
    }
  }
  // (A constructor template's `explicit(cond)` is decided with its own arguments deduced: not here.)
  if (!ctor || ambiguous || (ctor->explicitCondition && !ctor->templateParams.empty()) || !impl.isExplicit(ctor, s.empty() ? nullptr : &s))
    return;
  std::string name = types.toString(t, &symbols);
  reportHere(token, "copy-initialising " + name + " from " + types.toString(f, &symbols) +
                      " by its explicit constructor, which only a direct-initialisation takes (" + name + "(...))");
}

auto GlobalScope::Impl::Walker::checkExplicitListCopy(BraceInitExpr* b, const Ty* to) -> void
{
  if (probing || activeSubst || !file || !b || !b->designators.empty() || !to || !to->isKnown() || types.isDependent(to) || Impl::mentionsUnknown(to))
    return;
  const Ty* t = types.unqualified(types.stripRef(to));
  if (t->kind != TyKind::Class || impl.isInitializerList(t))
    return; // (an aggregate has no constructor chosen below)
  std::vector<const Ty*> argTypes;
  for (Expr* a : b->args)
  {
    if (!a || !a->ty || !a->ty->isKnown() || types.isDependent(a->ty) || Impl::mentionsUnknown(a->ty) || a->kind == NodeKind::BraceInitExpr)
      return;
    argTypes.push_back(argumentType(a, a->ty, types));
  }
  if (argTypes.size() == 1 && types.unqualified(types.stripRef(argTypes[0])) == t)
    return; // a copy or move of an object of the class itself
  Subst s = impl.substFor(t);
  // [over.match.list]'s first phase: an InitializerList constructor the elements convert to is the
  // one chosen (`Vector<int> v = {9};`), not one taking the elements as its arguments.
  if (ClassDecl* def = impl.classDefinition(t))
    for (Decl* m : membersRead(def))
    {
      auto* f = m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) ? static_cast<FunctionDecl*>(m) : nullptr;
      const Ty* first = f && !f->params.empty() ? impl.declaredTypeOf(f->params[0], s.empty() ? nullptr : &s, 0) : nullptr;
      const Ty* list = first ? types.unqualified(types.stripRef(first)) : nullptr;
      if (!list || !impl.isInitializerList(list) || list->args.empty() || !impl.takesArgumentCount(f, 1))
        continue;
      bool converts = true;
      for (const Ty* a : argTypes)
        converts = converts && impl.conversionRank(a, list->args[0], 0) < NotViable;
      if (converts)
        return;
    }
  bool ambiguous = false;
  FunctionDecl* ctor = impl.chooseConstructor(t, argTypes, nullptr, nullptr, false, &ambiguous);
  if (!ctor || ambiguous || (ctor->explicitCondition && !ctor->templateParams.empty()) || !impl.isExplicit(ctor, s.empty() ? nullptr : &s))
    return;
  std::string name = types.toString(t, &symbols);
  reportHere(b->token, "copy-list-initialising " + name + " by its explicit constructor, which only a direct-initialisation takes (" + name + "{...})");
}

auto GlobalScope::Impl::Walker::surelyNotConstant(const Expr* e, int depth) -> bool
{
  if (!e || depth > 16)
    return false;
  switch (e->kind)
  {
    case NodeKind::NameExpr:
    {
      Decl* d = static_cast<const NameExpr*>(e)->resolved;
      if (!d)
        return false;
      if (d->kind == NodeKind::ParamDecl)
        return true;
      if (d->kind == NodeKind::VariableDecl && !(d->flags & (DF_Constexpr | DF_Typedef)))
      {
        const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
        return t && t->isKnown() && !t->isReference() && !Impl::isConstObject(t);
      }
      return false;
    }
    case NodeKind::MemberExpr:
    {
      Decl* d = static_cast<const MemberExpr*>(e)->resolved;
      return d && d->kind == NodeKind::VariableDecl && !(d->flags & (DF_Static | DF_Constexpr | DF_Typedef));
    }
    case NodeKind::CallExpr:
    {
      const Expr* callee = static_cast<const CallExpr*>(e)->callee;
      Decl* d = callee && callee->kind == NodeKind::NameExpr ? static_cast<const NameExpr*>(callee)->resolved
                : callee && callee->kind == NodeKind::MemberExpr ? static_cast<const MemberExpr*>(callee)->resolved
                : nullptr;
      return d && d->kind == NodeKind::FunctionDecl && !(d->flags & (DF_Constexpr | DF_Consteval));
    }
    case NodeKind::ParenExpr: return surelyNotConstant(static_cast<const ParenExpr*>(e)->inner, depth + 1);
    case NodeKind::UnaryExpr: return surelyNotConstant(static_cast<const UnaryExpr*>(e)->operand, depth + 1);
    case NodeKind::BinaryExpr:
      return surelyNotConstant(static_cast<const BinaryExpr*>(e)->lhs, depth + 1) || surelyNotConstant(static_cast<const BinaryExpr*>(e)->rhs, depth + 1);
    case NodeKind::CastExpr: return surelyNotConstant(static_cast<const CastExpr*>(e)->operand, depth + 1);
    case NodeKind::NamedCastExpr: return surelyNotConstant(static_cast<const NamedCastExpr*>(e)->operand, depth + 1);
    case NodeKind::IndexExpr: return surelyNotConstant(static_cast<const IndexExpr*>(e)->base, depth + 1);
    default: return false;
  }
}

auto GlobalScope::Impl::Walker::narrows(Expr* e, const Ty* from, const Ty* to, std::string& why) -> bool
{
  const Ty* f = from ? types.unqualified(types.stripRef(from)) : nullptr;
  const Ty* t = to ? types.unqualified(types.stripRef(to)) : nullptr;
  if (!e || !f || !t || !f->isKnown() || !t->isKnown() || types.isDependent(f) || types.isDependent(t) || Impl::mentionsUnknown(f) ||
      Impl::mentionsUnknown(t))
    return false;
  if (f->kind == TyKind::Enum)
  {
    if (!f->decl || static_cast<EnumDecl*>(f->decl)->isClass)
      return false; // (a scoped enumeration converts to no integer: another check's)
    const Ty* u = impl.declaredUnderlying(f);
    f = u && u->isKnown() && u->kind != TyKind::Enum ? types.unqualified(u) : types.intType(32);
  }
  auto integral = [](const Ty* x) { return x->kind == TyKind::Int || x->kind == TyKind::Char || x->kind == TyKind::Bool; };
  std::string source = types.toString(f, &impl.symbols), target = types.toString(t, &impl.symbols);
  if ((f->kind == TyKind::Pointer || f->kind == TyKind::MemberPointer) && t->kind == TyKind::Bool)
  {
    why = source + " to bool";
    return true;
  }
  if (f->kind == TyKind::Float && integral(t))
  {
    why = source + " to " + target;
    return true;
  }
  // Whether integer type t holds every value of integer type f.
  auto holdsAll = [&](const Ty* x, const Ty* of) {
    if (of->kind == TyKind::Bool)
      return true;
    if (x->kind == TyKind::Bool)
      return false;
    if (of->isUnsigned == x->isUnsigned)
      return x->bits >= of->bits;
    return of->isUnsigned && x->bits > of->bits;
  };
  bool floatNarrower = f->kind == TyKind::Float && t->kind == TyKind::Float && t->bits < f->bits;
  bool intToFloat = integral(f) && t->kind == TyKind::Float;
  bool intToInt = integral(f) && integral(t) && !holdsAll(t, f);
  if (!floatNarrower && !intToFloat && !intToInt)
    return false;
  // The exception: a constant the target holds.
  std::string savedWhy = Impl::constWhy();
  std::optional<ConstValue> v = probesSharedNodes() ? std::nullopt : impl.constValue(e, ctx());
  Impl::constWhy() = savedWhy;
  if (!v || !v->isScalar())
  {
    if (!surelyNotConstant(e))
      return false;
    why = source + ", which is no constant, to " + target;
    return true;
  }
  bool fits = true;
  std::string shown;
  if (v->isFloat())
  {
    double x = v->f;
    shown = std::to_string(x);
    if (t->kind == TyKind::Float && t->bits == 32)
      fits = std::isnan(x) || std::isinf(x) || std::fabs(x) <= double(std::numeric_limits<float>::max());
  }
  else
  {
    // The integer as its type has it: a 64-bit unsigned one's bits are its value.
    bool big = f->isUnsigned && f->bits == 64 && v->i < 0;
    uint64_t u = uint64_t(v->i);
    shown = big ? std::to_string(u) : std::to_string(v->i);
    if (t->kind == TyKind::Float)
    {
      double asFloat = t->bits == 32 ? double(float(big ? double(u) : double(v->i))) : (big ? double(u) : double(v->i));
      fits = big ? asFloat < 18446744073709551616.0 && uint64_t(asFloat) == u : asFloat >= -9223372036854775808.0 && asFloat < 9223372036854775808.0 && int64_t(asFloat) == v->i;
    }
    else if (t->kind == TyKind::Bool)
      fits = !big && (v->i == 0 || v->i == 1);
    else if (t->isUnsigned)
      fits = big ? t->bits == 64 : v->i >= 0 && (t->bits >= 64 || uint64_t(v->i) < (uint64_t(1) << t->bits));
    else
      fits = !big && (t->bits >= 64 || (v->i >= -(int64_t(1) << (t->bits - 1)) && v->i < (int64_t(1) << (t->bits - 1))));
  }
  if (fits)
    return false;
  why = shown + " (" + source + ") to " + target + ", which cannot hold it";
  return true;
}

auto GlobalScope::Impl::Walker::checkListNarrowing(Span<Expr*> elements, const Ty* to, int depth) -> void
{
  if (probing || !file || depth > 8 || !to || !to->isKnown() || types.isDependent(to) || Impl::mentionsUnknown(to))
    return;
  const Ty* t = types.unqualified(types.stripRef(to));
  // One element initializing what: a nested list the same way, an expression checked.
  auto element = [&](Expr* e, const Ty* target) {
    if (!e || !target)
      return;
    if (e->kind == NodeKind::BraceInitExpr)
    {
      if (static_cast<BraceInitExpr*>(e)->designators.empty() && !static_cast<BraceInitExpr*>(e)->text)
        checkListNarrowing(static_cast<BraceInitExpr*>(e)->args, target, depth + 1);
      return;
    }
    if (!e->ty || !e->ty->isKnown() || e->ty->kind == TyKind::Pack || types.isDependent(e->ty) || Impl::mentionsUnknown(e->ty))
      return;
    std::string why;
    if (narrows(e, e->ty, target, why))
      reportCheck(e->token, "a brace initialization narrows " + why + " ([dcl.init.list] 7): C++ refuses it, a cast says it is meant");
  };
  // Whether an element that is no list initializes a whole element of type target (else brace
  // elision spreads it over target's parts: the targets are not told here).
  auto whole = [&](Expr* e, const Ty* target) {
    const Ty* u = types.unqualified(types.stripRef(target));
    if (!e || e->kind == NodeKind::BraceInitExpr || (u->kind != TyKind::Class && u->kind != TyKind::Array))
      return true;
    const Ty* given = e->ty ? types.unqualified(types.stripRef(e->ty)) : nullptr;
    return given && (given == u || (given->kind == TyKind::Class && u->kind == TyKind::Class && impl.isBaseClass(u, given)));
  };
  if (t->isArithmetic() || t->kind == TyKind::Enum || t->kind == TyKind::Pointer)
  {
    if (elements.size() == 1)
      element(elements[0], t);
    return;
  }
  if (t->kind == TyKind::Array && t->inner)
  {
    for (Expr* e : elements)
      if (!whole(e, t->inner))
        return;
    for (Expr* e : elements)
      element(e, t->inner);
    return;
  }
  if (t->kind != TyKind::Class || t->isBraceList() || Impl::lambdaOf(t))
    return;
  if (impl.isInitializerList(t))
  {
    if (!t->args.empty())
      for (Expr* e : elements)
        element(e, t->args[0]);
    return;
  }
  ClassDecl* def = impl.classDefinition(t);
  if (!def || def->isTypedUnion || (def->attributes & ATTR_Draft))
    return;
  Subst s = impl.substFor(t);
  const Subst* sp = s.pairs.empty() ? nullptr : &s;
  bool constructors = false;
  for (Decl* m : membersRead(def))
    constructors = constructors || (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor));
  if (!constructors)
  {
    // An aggregate: its members in order (one with bases: its bases first, not told here).
    if (!def->bases.empty())
      return;
    std::vector<const Ty*> members;
    for (Decl* m : membersRead(def))
      if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)) && !static_cast<VariableDecl*>(m)->name.empty())
        members.push_back(impl.declaredTypeOf(m, sp, 0));
    if (elements.size() > members.size())
      return;
    for (size_t i = 0; i < elements.size(); ++i)
      if (!members[i] || !whole(elements[i], members[i]))
        return;
    for (size_t i = 0; i < elements.size(); ++i)
      element(elements[i], members[i]);
    return;
  }
  // A class with constructors: its InitializerList constructor when the elements convert to its
  // element type ([over.match.list] 1), else the constructor chosen for the elements as arguments.
  std::vector<const Ty*> argTypes;
  for (Expr* a : elements)
  {
    if (!a || a->kind == NodeKind::BraceInitExpr || !a->ty || !a->ty->isKnown() || types.isDependent(a->ty) || Impl::mentionsUnknown(a->ty) ||
        a->ty->kind == TyKind::Pack)
      return;
    argTypes.push_back(argumentType(a, a->ty, types));
  }
  if (argTypes.size() == 1 && types.unqualified(types.stripRef(argTypes[0])) == t)
    return; // a copy or a move
  for (Decl* m : membersRead(def))
  {
    auto* f = m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) ? static_cast<FunctionDecl*>(m) : nullptr;
    const Ty* first = f && !f->params.empty() ? impl.declaredTypeOf(f->params[0], sp, 0) : nullptr;
    const Ty* list = first ? types.unqualified(types.stripRef(first)) : nullptr;
    if (!list || !impl.isInitializerList(list) || list->args.empty() || !impl.takesArgumentCount(f, 1))
      continue;
    bool converts = true;
    for (const Ty* a : argTypes)
      converts = converts && impl.conversionRank(a, list->args[0], 0) < NotViable;
    if (converts)
    {
      for (Expr* e : elements)
        element(e, list->args[0]);
      return;
    }
  }
  bool ambiguous = false;
  FunctionDecl* ctor = impl.chooseConstructor(t, argTypes, nullptr, nullptr, false, &ambiguous);
  if (!ctor || ambiguous || !ctor->templateParams.empty())
    return;
  for (size_t i = 0; i < elements.size() && i < ctor->params.size(); ++i)
    if (auto* p = static_cast<ParamDecl*>(ctor->params[i]); p && !p->isPack)
      element(elements[i], impl.declaredTypeOf(p, sp, 0));
}

auto GlobalScope::Impl::Walker::checkExplicitArguments(FunctionDecl* f, const Ty* fnTy, const Span<Expr*>& args) -> void
{
  if (probing || !f || !f->templateParams.empty() || !fnTy || fnTy->kind != TyKind::Function || types.isDependent(fnTy))
    return;
  std::vector<const Ty*> argTypes = argTypesOf(args);
  if (argTypes.size() != args.size())
    return;
  for (size_t i = 0; i < args.size() && i < fnTy->args.size(); ++i)
    if (args[i] && argTypes[i] && argTypes[i]->kind != TyKind::Pack)
    {
      if (args[i]->kind == NodeKind::BraceInitExpr)
      {
        checkExplicitListCopy(static_cast<BraceInitExpr*>(args[i]), fnTy->args[i]);
        if (static_cast<BraceInitExpr*>(args[i])->designators.empty() && !static_cast<BraceInitExpr*>(args[i])->text)
          checkListNarrowing(static_cast<BraceInitExpr*>(args[i])->args, fnTy->args[i]); // `take({1.5})` for an int
      }
      else
        checkExplicitCopy(argTypes[i], fnTy->args[i], args[i]->token);
    }
}

auto GlobalScope::Impl::Walker::checkScalarArguments(FunctionDecl* f, const Ty* fnTy, const Span<Expr*>& args, bool ambiguousOnly) -> void
{
  if (probing || !f || !f->templateParams.empty() || !fnTy || fnTy->kind != TyKind::Function || types.isDependent(fnTy))
    return;
  std::vector<const Ty*> argTypes = argTypesOf(args);
  if (argTypes.size() != args.size())
    return;
  for (size_t i = 0; i < args.size() && i < fnTy->args.size(); ++i)
    if (args[i] && args[i]->kind != NodeKind::BraceInitExpr && argTypes[i] && argTypes[i]->kind != TyKind::Pack)
      checkClassToScalar(argTypes[i], fnTy->args[i], args[i]->token, ambiguousOnly);
}

auto GlobalScope::Impl::Walker::checkReferenceArguments(FunctionDecl* f, const Ty* fnTy, const Span<Expr*>& args, const std::string& callee) -> void
{
  if (!f || !f->templateParams.empty() || !fnTy || fnTy->kind != TyKind::Function)
    return;
  checkReferenceBindings(callee.empty() ? std::string(symbols.name(f->name.last().symbol)) : callee, f->params, fnTy->args, args);
}

auto GlobalScope::Impl::Walker::checkReferenceBindings(const std::string& callee, Span<Decl*> params, Span<const Ty*> paramTypes, const Span<Expr*>& args) -> void
{
  if (probing || inGenericTemplate())
    return;
  for (const Ty* t : paramTypes)
    if (!t || types.isDependent(t))
      return;
  std::vector<const Ty*> argTypes = argTypesOf(args);
  if (argTypes.size() != args.size())
    return;
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i)
  {
    const Ty* param = paramTypes[i];
    const Ty* given = argTypes[i];
    bool expansion = args[i] && args[i]->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(args[i])->isKeyword &&
                     static_cast<UnaryExpr*>(args[i])->postfix && P(static_cast<UnaryExpr*>(args[i])->op) == P::Ellipsis;
    if (!args[i] || expansion || !param || !param->isReference() || !param->inner || (param->kind == TyKind::LRef && Impl::isConstObject(param->inner)) || !given ||
        !given->isKnown() || given->kind == TyKind::Pack || types.isDependent(given) || Impl::mentionsUnknown(given))
      continue;
    if (impl.conversionRank(given, param) < Impl::NotViable)
      continue;
    const Ty* a = types.unqualified(types.stripRef(given));
    const Ty* pt = types.unqualified(param->inner);
    bool related = a == pt || (a->kind == TyKind::Class && pt->kind == TyKind::Class && impl.isBaseClass(pt, a));
    auto* p = i < params.size() ? static_cast<ParamDecl*>(params[i]) : nullptr;
    std::string name = p && p->name != UINT32_MAX ? std::string(symbols.name(p->name)) : "#" + std::to_string(i + 1);
    std::string argument = "the argument for " + name + " (" + types.toString(param, &symbols) + ") of " + callee;
    // `take(move(s))` with a const s: an rvalue of a const object, which a T&& does not bind.
    if (param->kind == TyKind::RRef && given->kind == TyKind::RRef && related && Impl::isConstObject(types.stripRef(given)) && !Impl::isConstObject(param->inner))
    {
      reportHere(args[i]->token, argument + " is a const object (" + types.toString(types.stripRef(given), &symbols) +
                                     "): an rvalue reference to a non-const object binds no const one ([dcl.init.ref] 5)");
      continue;
    }
    // (A T&& given an lvalue of another type converts it to a temporary, which it binds: what is
    // not viable there is another conversion's, the code generator's error.)
    if (param->kind == TyKind::RRef && (given->kind != TyKind::LRef || a->kind == TyKind::Function || !related))
      continue;
    if (param->kind == TyKind::RRef)
      reportHere(args[i]->token, argument + " is an lvalue of " + types.toString(types.stripRef(given), &symbols) +
                                     ": an rvalue reference binds a temporary or what move() gives, never an lvalue of its type or of a class "
                                     "derived from it ([dcl.init.ref] 5.4.4)");
    else
      reportHere(args[i]->token, argument + " is no lvalue of its type (it is " +
                                     (given->isBraceList() ? std::string("a brace list") : types.toString(types.stripRef(given), &symbols)) +
                                     "): a non-const reference binds an lvalue of its type, or of a class derived from it, never a temporary "
                                     "([dcl.init.ref] 5)");
  }
}

auto GlobalScope::Impl::Walker::bindOverloadSetsToConstructor(const std::vector<OverloadSetArgument>& sets, const Ty* t, Span<Expr*> args) -> void
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (!t || t->kind != TyKind::Class || !t->isKnown() || types.isDependent(t))
    return;
  Scope* s = impl.classScopeOf(t);
  if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
    return;
  std::vector<FunctionDecl*> constructors;
  for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
      constructors.push_back(static_cast<FunctionDecl*>(m));
  Subst subst = impl.substFor(t);
  if (constructors.empty())
  {
    // `NamedFunction{Expression::computeConstant}` of an aggregate: its data members take the
    // arguments in order (a class with bases is left: they would come first).
    auto* cls = static_cast<ClassDecl*>(s->owner);
    if (!cls->bases.empty())
      return;
    std::vector<const Ty*> members;
    for (Decl* m : membersRead(cls))
      if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
        members.push_back(impl.declaredTypeOf(m, subst.empty() ? nullptr : &subst, 0));
    bindOverloadSets(sets, types.function(types.voidType(), Span<const Ty*>{members.data(), uint32_t(members.size())}, false));
    return;
  }
  bool ambiguous = false;
  std::vector<const Ty*> argTypes = argTypesOf(args);
  for (const OverloadSetArgument& set : sets)
    if (set.index < argTypes.size())
      argTypes[set.index] = types.unknown(); // the overload set decides nothing until the constructor is chosen
  if (FunctionDecl* ctor = impl.chooseAmong(constructors, argTypes, subst.empty() ? nullptr : &subst, nullptr, ambiguous); ctor && !ambiguous)
    bindOverloadSets(sets, impl.declaredTypeOf(ctor, subst.empty() ? nullptr : &subst, 0));
}

auto GlobalScope::Impl::Walker::bindOverloadSetsOfConstruction(const Ty* t, Span<Expr*> args) -> void
{
  std::vector<OverloadSetArgument> sets;
  for (size_t i = 0; i < args.size(); ++i)
    if (OverloadSetArgument s; overloadSetArgument(args[i], i, s))
      sets.push_back(s);
  if (!sets.empty())
    bindOverloadSetsToConstructor(sets, t, args);
}

auto GlobalScope::Impl::Walker::parenthesisedTypeCallee(Expr* e) -> NameExpr*
{
  if (!e || e->kind != NodeKind::CallExpr)
    return nullptr;
  auto* c = static_cast<CallExpr*>(e);
  if (!c->callee || c->callee->kind != NodeKind::ParenExpr || c->args.size() != 1)
    return nullptr;
  Expr* inner = static_cast<ParenExpr*>(c->callee)->inner;
  if (!inner || inner->kind != NodeKind::NameExpr || static_cast<NameExpr*>(inner)->name.empty())
    return nullptr;
  auto* n = static_cast<NameExpr*>(inner);
  Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
  if (l.found == Found::No)
    return n->name.parts.size() == 1 && !n->name.parts[0].hasTemplateArgs && impl.builtinByName(n->name.parts[0].symbol) ? n : nullptr;
  Decl* d = l.decl;
  bool type = d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                    (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)) ||
                    (d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isType));
  return type ? n : nullptr;
}

auto GlobalScope::Impl::Walker::checkPostfixAfterParenthesisedType(Expr* base, uint32_t token) -> void
{
  if (file && parenthesisedTypeCallee(base))
    impl.reportCheckerError(file, token, "`(T)(x)` followed by `.`, `->`, `[` or `(` is a cast of the whole postfix expression in C++; "
                                         "write `T(x)` or `(T)((x)...)` to say which");
}

auto GlobalScope::Impl::Walker::ownsNodes() const -> bool
{
  if (probing)
    return false;
  if (activeSubst)
    return true;
  for (Decl* d = bodyOf; d; d = d->parent)
    if (!d->templateParams.empty())
      return false;
  for (Scope* s = scope; s; s = s->parent)
    if (s->owner && !s->owner->templateParams.empty())
      return false;
  return true;
}

auto GlobalScope::Impl::Walker::markTextCapture(LambdaExpr* l, size_t mark) -> void
{
  if (!impl.textDecl() || !l->closure)
    return;
  auto outsideHoldsText = [&](Decl* d)
  {
    for (size_t i = 0; i < mark && i < locals.items.size(); ++i)
      if (locals.items[i].decl == d)
      {
        const Ty* t = locals.items[i].type ? locals.items[i].type : impl.declaredTypeOf(d, activeSubst, 0);
        return impl.holdsText(t);
      }
    return false;
  };
  bool holds = false;
  for (const LambdaCapture& cap : l->captures)
    if (!cap.decl && cap.symbol != UINT32_MAX)
      for (size_t i = std::min(mark, locals.items.size()); i-- > 0;)
        if (locals.items[i].symbol == cap.symbol)
        {
          holds = holds || outsideHoldsText(locals.items[i].decl);
          break;
        }
  if (!holds && l->body)
    forEachNode(l->body, [&](Node* n)
    {
      if (!holds && n->kind == NodeKind::NameExpr)
        if (Decl* d = static_cast<NameExpr*>(n)->resolved; d && (d->kind == NodeKind::ParamDecl || d->kind == NodeKind::VariableDecl))
          holds = outsideHoldsText(d);
    });
  if (holds)
    impl.markTextClosure(l->closure);
}

auto GlobalScope::Impl::Walker::reportTextError(uint32_t token, const std::string& message) -> void
{
  if (probing || !file)
    return;
  if (!file->isLibrary)
    impl.reportCheckerError(file, token, message);
  else if (activeSubst && requestFile && !requestFile->isLibrary)
    impl.reportCheckerError(requestFile, requestToken, message);
  // (else an instance the library asked for, which the program's own request reported)
}

auto GlobalScope::Impl::Walker::checkClosureTo(const Expr* value, const Ty* to, uint32_t token) -> void
{
  if (!value || !value->ty || !to || !impl.isTextClosure(value->ty) || inGenericTemplate())
    return;
  const Ty* target = types.unqualified(types.stripRef(to));
  if (target->kind == TyKind::Param || types.isDependent(target) || target == types.unqualified(types.stripRef(value->ty)))
    return;
  reportTextError(token, "a lambda capturing a Text cannot be stored (design.md 14)");
}

auto GlobalScope::Impl::Walker::textStored(const std::string& as) -> std::string
{
  return "a Text is valid only during the call that gives it, so it cannot be " + as + " (design.md 14)";
}

auto GlobalScope::Impl::Walker::checkOverrideNoexcept(FunctionDecl* f) -> void
{
  // [except.spec] 4: an overrider allows no exception its overridden function does not, whether
  // it says override or not. (A destructor's, implicit too: checkDestructorNoexcept.)
  if (probing || retypingElements || !file || file->isImported || (f->flags & (DF_Static | DF_Constructor | DF_Destructor)) || !f->templateParams.empty())
    return;
  Decl* cls = impl.owningClassOf(f);
  if (!cls || cls->kind != NodeKind::ClassDecl)
    return;
  const Ty* self = impl.selfType(cls);
  if (activeSubst)
    self = impl.substitute(self, *activeSubst);
  else if (inGenericTemplate())
    return;
  if (!self || !self->isKnown() || types.isDependent(self) || impl.isNoexcept(f, activeSubst) != std::optional<bool>(false))
    return;
  if (FunctionDecl* base = impl.overriddenNoexcept(self, f, impl.overrideKey(f, self), 0))
    impl.reportCheckerError(file, f->token, "this function overrides the noexcept function at " + impl.locationOf(base->file, base->token) + " and is not noexcept itself");
}

auto GlobalScope::Impl::Walker::checkDestructorNoexcept(ClassDecl* c) -> void
{
  // A class's own, every destructor of it having one exception specification; an instance's is checked
  // when the instance is laid out, as naming it does not instantiate it (collectVtables).
  if (probing || retypingElements || !file || file->isImported || !c->isDefinition || c->isClosure || c->isTypedUnion || c->bases.empty() || activeSubst || inGenericTemplate())
    return;
  const Ty* self = impl.selfType(c);
  const Ty* base = self && self->isKnown() && !types.isDependent(self) ? impl.destructorLoosens(self) : nullptr;
  if (!base)
    return;
  uint32_t at = c->token;
  for (Decl* m : membersRead(c))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor))
      at = m->token;
  impl.reportCheckerError(file, at, Impl::destructorLoosensText(types.toString(self, &impl.symbols), types.toString(base, &impl.symbols)));
}

auto GlobalScope::Impl::Walker::checkDestructorRedeclared(FunctionDecl* f) -> void
{
  // `S::~S() noexcept(false) {}` of `~S();`: [except.spec] 4, by what each says or, with no specifier, the
  // implicit one's. A template's are compared by nothing here: clang neither.
  if (!(f->flags & DF_Destructor) || f->name.parts.size() < 2 || probing || retypingElements || !file || file->isImported || activeSubst || inGenericTemplate())
    return;
  Decl* cls = impl.owningClassOf(f);
  if (!cls || cls->kind != NodeKind::ClassDecl)
    return;
  const Ty* self = impl.selfType(cls);
  if (!self || !self->isKnown() || types.isDependent(self))
    return;
  auto says = [&](FunctionDecl* d) -> std::optional<bool>
  {
    if (d->flags & DF_Noexcept)
      return impl.isNoexcept(d, nullptr);
    std::optional<bool> parts = impl.destructorPartsMayThrow(self);
    return parts ? std::optional<bool>(!*parts) : std::nullopt;
  };
  for (Decl* m : membersRead(static_cast<ClassDecl*>(cls)))
    if (auto* d = static_cast<FunctionDecl*>(m); m != f && m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) && says(d) && says(f) && says(d) != says(f))
      impl.reportCheckerError(file, f->token, "this destructor says otherwise of noexcept than its declaration at " + Impl::locationOf(d->file, d->token));
}

auto GlobalScope::Impl::Walker::checkCaseEnds(Stmt* body) -> void
{
  // The first step of no implicit fallthrough (design.md): what a case runs on into the next by is written, so
  // that the flip to an implicit break changes the meaning of nothing. Whether its end is reached, by the
  // statements alone, as the converter says it when it writes `fallthrough;`.
  if (activeSubst || probing || !file || file->isImported || !body || body->kind != NodeKind::BlockStmt)
    return;
  std::function<bool(Stmt*)> closes = [&](Stmt* s)
  {
    if (!s)
      return false;
    switch (s->kind)
    {
      case NodeKind::ReturnStmt: case NodeKind::BreakStmt: case NodeKind::ContinueStmt: case NodeKind::GotoStmt: case NodeKind::FallthroughStmt:
        return true;
      case NodeKind::ExprStmt:
        return static_cast<ExprStmt*>(s)->expr && static_cast<ExprStmt*>(s)->expr->kind == NodeKind::ThrowExpr;
      case NodeKind::BlockStmt:
      {
        auto* b = static_cast<BlockStmt*>(s);
        return !b->stmts.empty() && closes(b->stmts[b->stmts.size() - 1]);
      }
      case NodeKind::IfStmt:
        return static_cast<IfStmt*>(s)->else_ && closes(static_cast<IfStmt*>(s)->then) && closes(static_cast<IfStmt*>(s)->else_);
      default:
        return false;
    }
  };
  auto reportAt = [&](uint32_t token)
  {
    impl.reportCheckerError(file, token, "the case before this one runs on into it: end that case with `fallthrough;` to say so, or with `break;` (design.md, \"No implicit fallthrough\")");
  };
  Stmt* last = nullptr; // the last statement since a label
  std::function<void(Span<Stmt*>)> section = [&](Span<Stmt*> stmts)
  {
    for (Stmt* s : stmts)
    {
      if (!s || s->kind == NodeKind::EmptyStmt)
        continue;
      if (s->kind == NodeKind::CaseStmt || s->kind == NodeKind::DefaultStmt)
      {
        if (last && !closes(last))
          reportAt(s->token);
        last = nullptr;
      }
      else if (s->kind == NodeKind::ForConstexprStmt)
      {
        // `for constexpr (...) case T::kind: ...`: each element's statements are written once.
        Stmt* each = static_cast<ForConstexprStmt*>(s)->body;
        if (each && each->kind == NodeKind::BlockStmt)
          section(static_cast<BlockStmt*>(each)->stmts);
        else if (each)
          last = each->kind == NodeKind::CaseStmt || each->kind == NodeKind::DefaultStmt ? nullptr : each;
      }
      else
        last = s;
    }
  };
  section(static_cast<BlockStmt*>(body)->stmts);
}

auto GlobalScope::Impl::Walker::checkOverride(FunctionDecl* f) -> void
{
  checkOverrideNoexcept(f);
  checkDestructorRedeclared(f);
  // (factorio: a stream deriving from OutStream kept `writeBytes(const char*, int) override` when the
  // library's sizes became unsigned, compiled, and writing to it called the base's pure function)
  // Required `override` (design.md 23b): one that overrides says so, `override` or `final` (Q6); a destructor
  // need not, a polymorphic class's being virtual (Q5), and an out-of-line definition cannot: the word is
  // its class's declaration's. A class template's are its instances' to tell, each by its arguments (Q7,
  // GlobalScope::checkInstanceStaticAsserts, which names the instance).
  bool unmarked = f->name.parts.size() == 1 && !(f->flags & (DF_Override | DF_Final | DF_Static | DF_Constructor | DF_Destructor));
  if ((!(f->flags & DF_Override) && !unmarked) || probing || retypingElements || !file || file->isImported || activeSubst || inGenericTemplate())
    return;
  Decl* cls = impl.owningClassOf(f);
  if (!cls || cls->kind != NodeKind::ClassDecl)
    return;
  const Ty* self = impl.selfType(cls);
  if (!self || !self->isKnown() || types.isDependent(self))
    return;
  std::optional<bool> overrides = impl.overridesVirtual(self, impl.overrideKey(f, self), 0);
  if (!overrides || *overrides != unmarked)
    return;
  const NamePart& n = f->name.last();
  std::string name = (f->flags & DF_Destructor) ? std::string("the destructor") : n.special ? std::string("this operator") : impl.debugName(n.symbol);
  uint32_t overrideToken = UINT32_MAX;
  std::string old = Impl::spelledTokens(file, f->token, Impl::declaratorEnd(file, f->token, &overrideToken));
  if (unmarked)
    impl.reportCheckerError(file, f->token, name + " overrides a virtual function: write `" + old + " override`, not `" + old + "`");
  else
    impl.reportCheckerError(file, overrideToken != UINT32_MAX ? overrideToken : f->token, name + " says override and overrides no virtual function: remove `override`");
}

auto GlobalScope::Impl::Walker::checkAbstractObject(VariableDecl* v, const Ty* declared) -> void
{
  // (factorio: a stream whose writeBytes no longer overrode the pure one was made, and writing to it
  // called the pure function)
  if (!declared || probing || retypingElements || !file || file->isImported || (v->flags & (DF_Extern | DF_Typedef)))
    return;
  const Ty* t = types.unqualified(declared);
  while (t->kind == TyKind::Array)
    t = types.unqualified(t->inner);
  if (t->kind != TyKind::Class || !t->isKnown() || types.isDependent(t) || impl.containsAuto(t) || Impl::mentionsUnknown(t))
    return;
  std::optional<bool> abstract;
  if (bodyOf)
    abstract = impl.abstractClass(t, {}); // a local: what the function's walk reads
  else
  {
    // A declaration outside a function body (a global, a static or data member, a class
    // instance's member) records nothing of what the check reads: a compile server emits the
    // initializer of a global whose walk read an edited name again, and a data member or a
    // constant has none to emit (incremental 44: Frame's `Shape inner;` read Shape's members, and
    // a layout edit of Shape built the program in full). A member list read outside any unit
    // (RecordInto(nullptr)) would build an edit of its class from scratch, so the reads go to a
    // list of their own, dropped; an error stays the declaration's. An edit that makes the class
    // abstract is reported when the declaration is walked again, by a new process at the latest.
    Dependencies dropped;
    RecordInto into(&dropped);
    if (recordingDependencies)
      reportingUnit = into.savedReporting;
    abstract = impl.abstractClass(t, {});
  }
  if (abstract && *abstract)
    impl.reportCheckerError(file, v->token, "an object of the abstract class " + types.toString(t, &impl.symbols) +
                                                " (a pure virtual function of it or of a base that nothing overrides)");
}

auto GlobalScope::Impl::Walker::checkTextSignature(FunctionDecl* f) -> void
{
  if (probing || !impl.textDecl() || activeSubst)
    return; // (an instance's parameters are its template's, `const T& a` a Text where the template has none)
  const Ty* ft = impl.declaredTypeOf(f, nullptr, 0);
  if (!ft || ft->kind != TyKind::Function)
    return;
  bool generic = inGenericTemplate();
  // (an operator's or a conversion's last part names no symbol)
  std::string fname = !f->name.empty() && f->name.last().symbol != UINT32_MAX ? std::string(symbols.name(f->name.last().symbol)) : std::string();
  for (size_t i = 0; i < f->params.size() && i < ft->args.size(); ++i)
  {
    auto* p = static_cast<ParamDecl*>(f->params[i]);
    std::string name = p->name != UINT32_MAX ? std::string(symbols.name(p->name)) : std::to_string(i + 1);
    if (impl.isText(ft->args[i]))
    {
      if (i + 1 != f->params.size())
        reportTextError(p->token, "the Text parameter " + name + " takes the remaining arguments, so it must be the last parameter (design.md 14)");
      if (p->defaultValue)
        reportTextError(p->token, "the Text parameter " + name +
                                    " takes the remaining arguments, so it has no default argument; no arguments make an empty Text (design.md 14)");
      if (f->flags & DF_ExternC)
        reportTextError(p->token, "a Text parameter cannot be passed from C: " + fname + " is extern \"C\" (design.md 14)");
    }
    else if (!generic && impl.holdsText(ft->args[i]) && types.unqualified(types.stripRef(ft->args[i]))->kind == TyKind::Pointer)
      reportTextError(p->token, textStored("pointed to"));
  }
  // (A template's instance passes one on, `move(closure)`, as the function it is written in does.)
  if (!generic && ft->inner && impl.holdsText(ft->inner))
    reportTextError(f->token, textStored("the result of " + (fname.empty() ? std::string("an operator") : fname)));
  // `void appendTo(const Meters& m, String& out)` outside a class: a free appendTo the
  // program writes would never be called (design.md 14: the member is the one form).
  if (file && !file->isLibrary && f->name.parts.size() == 1 && fname == "appendTo" &&
      !impl.owningClassOf(f) && !(f->flags & DF_Friend))
    reportTextError(f->token, "appendTo is a member: write `void appendTo(String& out) const` in the type, not a free function (design.md 14)");
}

auto GlobalScope::Impl::Walker::checkTextVariable(VariableDecl* v, const Ty* declared) -> void
{
  if (probing || !declared || !impl.holdsText(declared) || inGenericTemplate() || v->name.empty())
    return;
  std::string name(symbols.name(v->name.last().symbol));
  bool member = v->parent && v->parent->kind == NodeKind::ClassDecl && !(v->flags & DF_Static);
  bool local = bodyOf && !member && !(v->flags & (DF_Static | DF_Extern | DF_ThreadLocal));
  if (member)
    reportTextError(v->token, textStored("the type of the data member " + name));
  else if (!local)
    reportTextError(v->token, textStored("the type of the variable " + name));
  else if (types.unqualified(types.stripRef(declared))->kind == TyKind::Pointer)
    reportTextError(v->token, textStored("pointed to"));
  else if (const Ty* c = types.unqualified(types.stripRef(declared)); c->kind == TyKind::Class && !impl.isText(c) && c->decl &&
                                                                     c->decl->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(c->decl)->name.empty())
    reportTextError(v->token, textStored("an argument of " + std::string(symbols.name(static_cast<ClassDecl*>(c->decl)->name.last().symbol))));
  else if (impl.isText(declared) && (!v->init || !v->init->ty || !impl.isText(v->init->ty) ||
                                     (v->init->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(v->init)->text)))
    reportTextError(v->token, "a Text is made only by passing arguments to a Text parameter (design.md 14)");
}

auto GlobalScope::Impl::Walker::packText(Span<Expr*>& args, FunctionDecl* f, uint32_t token) -> void
{
  if (f && impl.namesTextLast(f))
    packTextAt(args, impl.textParameterOf(impl.declaredTypeOf(f, nullptr, 0)), token);
}

auto GlobalScope::Impl::Walker::packTextThrough(Span<Expr*>& args, const Ty* callee, uint32_t token) -> void
{
  const Ty* t = callee ? types.unqualified(types.stripRef(callee)) : nullptr;
  if (t && t->kind == TyKind::Pointer && t->inner)
    t = types.unqualified(t->inner);
  if (t && (t->kind == TyKind::Lambda || t->kind == TyKind::Function) && !t->args.empty() && !t->variadic && impl.isText(t->args[t->args.size() - 1]))
    packTextAt(args, t->args.size() - 1, token);
}

auto GlobalScope::Impl::Walker::packTextAt(Span<Expr*>& args, size_t k, uint32_t token) -> void
{
  if (k == SIZE_MAX || args.size() < k)
    return;
  if (args.size() == k + 1 && args[k] &&
      ((args[k]->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(args[k])->text) || (args[k]->ty && impl.isText(args[k]->ty))))
    return;
  bool known = true;
  auto check = [&](Expr* a, const Ty* t)
  {
    TextPieceKind piece = impl.textPieceOf(a, t);
    if (!piece.error.empty())
    {
      if (!probing)
        impl.reportCheckerError(file, a->token, piece.error);
      known = false;
    }
    else if (!piece.thunk)
      known = false;
  };
  for (size_t i = k; i < args.size(); ++i)
  {
    Expr* a = args[i];
    if (!a || isEmptyExpansion(a))
      continue;
    if (a->kind == NodeKind::BraceInitExpr)
    {
      if (!probing)
        impl.reportCheckerError(file, a->token, "a brace list is no piece of a Text: it has no type (design.md 14)");
      known = false;
      continue;
    }
    bool expansion = a->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(a)->isKeyword && static_cast<UnaryExpr*>(a)->postfix &&
                     P(static_cast<UnaryExpr*>(a)->op) == P::Ellipsis;
    if (a->ty && a->ty->kind == TyKind::Pack && !expansion)
    {
      // `(r.append(text(rest)), ...)`: one element of the pack per expansion of the pattern, typed
      // (and its overload chosen: text(int) before text(Text)) where the pattern expands, never
      // the whole pack packed into one Text.
      known = false;
      continue;
    }
    if (a->ty && a->ty->kind == TyKind::Pack)
      for (const Ty* e : a->ty->args)
        check(a, e);
    else
      check(a, a->ty);
  }
  const Ty* text = impl.textType();
  if (!known || !text || !ownsNodes())
    return;
  Arena& arena = impl.instanceArena();
  auto* packed = arena.make<BraceInitExpr>();
  packed->kind = NodeKind::BraceInitExpr;
  packed->token = k < args.size() ? args[k]->token : token;
  packed->args = Span<Expr*>{arena.array(args.data + k, args.size() - k), uint32_t(args.size() - k)};
  packed->text = true;
  record(packed, text);
  std::vector<Expr*> all(args.begin(), args.begin() + k);
  all.push_back(packed);
  args = Span<Expr*>{arena.array(all.data(), all.size()), uint32_t(all.size())};
}

auto GlobalScope::Impl::Walker::insideConsteval() const -> bool
{
  if (initializersOf && (initializersOf->flags & DF_Consteval))
    return true;
  for (Decl* d = bodyOf; d; d = d->parent)
    if (d->kind == NodeKind::FunctionDecl && (d->flags & DF_Consteval))
      return true;
  return false;
}

auto GlobalScope::Impl::Walker::immediateFails(const std::function<std::optional<ConstValue>()>& evaluate, std::string& why) -> bool
{
  bool outerExact = impl.exactLayoutOnly, outerNeeded = impl.layoutNeeded;
  impl.exactLayoutOnly = true;
  impl.layoutNeeded = false;
  impl.constWhy().clear();
  std::optional<ConstValue> v = evaluate();
  why = v ? std::string() : impl.constWhy();
  bool needed = impl.layoutNeeded;
  impl.exactLayoutOnly = outerExact;
  impl.layoutNeeded = outerNeeded;
  return !v && !needed;
}

auto GlobalScope::Impl::Walker::checkImmediate(FunctionDecl* f, uint32_t token, const std::function<std::optional<ConstValue>()>& evaluate) -> void
{
  if (!f || !(f->flags & DF_Consteval) || probing || inGenericTemplate() || insideConsteval() || f->name.empty() || !file)
    return;
  std::string why;
  if (!immediateFails(evaluate, why))
    return;
  std::string name = f->name.last().symbol != UINT32_MAX ? std::string(symbols.name(f->name.last().symbol)) : std::string("operator");
  if (f->flags & DF_Constructor)
    name += "::" + name;
  reportTextError(token, "the call of consteval " + name + " is no constant" + (why.empty() ? std::string() : ": " + why));
}

auto GlobalScope::Impl::Walker::checkImmediateConversion(Expr* arg, const Ty* param) -> void
{
  const Ty* cls = param ? types.unqualified(types.stripRef(param)) : nullptr;
  if (!arg || !arg->ty || !cls || cls->kind != TyKind::Class || types.isDependent(cls) || !cls->isKnown() || impl.isText(cls))
    return;
  if (types.unqualified(types.stripRef(arg->ty)) == cls)
    return;
  Scope* s = impl.classScopeOf(cls);
  if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
    return;
  std::vector<FunctionDecl*> ctors;
  bool immediate = false;
  for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Delete))
    {
      ctors.push_back(static_cast<FunctionDecl*>(m));
      immediate = immediate || (m->flags & DF_Consteval);
    }
  if (!immediate)
    return;
  Subst subst = impl.substFor(cls);
  bool ambiguous = false;
  FunctionDecl* ctor = impl.chooseAmong(ctors, {argumentType(arg, arg->ty, types)}, subst.empty() ? nullptr : &subst, nullptr, ambiguous);
  if (!ctor || ambiguous || !(ctor->flags & DF_Consteval))
    return;
  checkImmediate(ctor, arg->token, [&]() -> std::optional<ConstValue>
  {
    std::optional<ConstValue> value = impl.constValue(arg, ctx(), 0);
    if (!value)
      return std::nullopt;
    return impl.constRunConstructor(ctor, cls, {*value}, 0);
  });
}

auto GlobalScope::Impl::Walker::pieceCount(Span<Expr*> args, size_t k) -> size_t
{
  if (args.size() == k + 1 && args[k] &&
      ((args[k]->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(args[k])->text) || (args[k]->ty && impl.isText(args[k]->ty))))
    return SIZE_MAX;
  size_t n = 0;
  for (size_t i = k; i < args.size(); ++i)
  {
    Expr* a = args[i];
    if (!a || isEmptyExpansion(a))
      continue;
    bool expansion = a->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(a)->isKeyword && static_cast<UnaryExpr*>(a)->postfix &&
                     P(static_cast<UnaryExpr*>(a)->op) == P::Ellipsis;
    if (a->ty && a->ty->kind == TyKind::Pack)
    {
      if (!expansion)
        return SIZE_MAX;
      n += a->ty->args.size();
    }
    else if (expansion || !a->ty || !a->ty->isKnown())
      return SIZE_MAX;
    else
      ++n;
  }
  return n;
}

auto GlobalScope::Impl::Walker::checkTextPieces(FunctionDecl* f, const Ty* ft, Span<Expr*> args) -> void
{
  if (probing || inGenericTemplate() || insideConsteval() || !file || !ft || ft->kind != TyKind::Function)
    return;
  size_t k = impl.textParameterOf(impl.declaredTypeOf(f, nullptr, 0));
  if (k == SIZE_MAX || k == 0 || k > ft->args.size() || args.size() < k)
    return;
  const Ty* cls = types.unqualified(types.stripRef(ft->args[k - 1]));
  if (!cls || cls->kind != TyKind::Class || types.isDependent(cls) || !cls->isKnown())
    return;
  Scope* s = impl.classScopeOf(cls);
  if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
    return;
  if (checkPiecesSymbol == UINT32_MAX)
    checkPiecesSymbol = impl.symbols.intern("checkPieces");
  FunctionDecl* check = nullptr;
  std::vector<FunctionDecl*> ctors;
  for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
    if (m->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(m)->name.empty() && static_cast<FunctionDecl*>(m)->name.last().symbol == checkPiecesSymbol)
      check = static_cast<FunctionDecl*>(m);
    else if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Delete))
      ctors.push_back(static_cast<FunctionDecl*>(m));
  Expr* arg = args[k - 1];
  if (!check || !arg)
    return;
  if (!(check->flags & DF_Consteval) || check->params.size() != 1)
  {
    reportTextError(arg->token, "checkPieces of " + types.toString(cls, &symbols) + " is not `consteval void checkPieces(ContainerSize pieces) const` (design.md 14)");
    return;
  }
  size_t n = pieceCount(args, k);
  if (n == SIZE_MAX)
    return;
  Subst subst = impl.substFor(cls);
  const Ty* countTy = impl.declaredTypeOf(check->params[0], subst.empty() ? nullptr : &subst, 0);
  // One evaluation builds the argument's object and runs checkPieces on it, so that what the
  // object or a thrown exception holds (a message built in a String) lives until the end.
  bool built = false;
  std::string why;
  bool fails = immediateFails([&]() -> std::optional<ConstValue>
  {
    return impl.constOutermost([&]() -> std::optional<ConstValue>
    {
      std::optional<ConstValue> value = impl.constValue(arg, ctx(), 0);
      if (!value)
        return std::nullopt;
      if (arg->ty && types.unqualified(types.stripRef(arg->ty)) != cls)
      {
        bool ambiguous = false;
        FunctionDecl* ctor = impl.chooseAmong(ctors, {argumentType(arg, arg->ty, types)}, subst.empty() ? nullptr : &subst, nullptr, ambiguous);
        if (!ctor || ambiguous)
          return std::nullopt;
        value = impl.constRunConstructor(ctor, cls, {*value}, 0);
        if (!value)
          return std::nullopt;
      }
      built = true;
      std::optional<ConstValue> self = value;
      return impl.constCallOn(check, subst, {ConstValue::integer(int64_t(n), countTy)}, &self, 0);
    }, 0);
  }, why);
  // (an argument that is no constant here is not checked; a consteval constructor's own failure
  // is reported by its check)
  if (fails && built)
    reportTextError(arg->token, types.toString(cls, &symbols) + " does not take " + std::to_string(n) + (n == 1 ? " piece" : " pieces") +
                                    (why.empty() ? std::string() : ": " + why) + " (design.md 14)");
}

auto GlobalScope::Impl::Walker::textConstructor(const Ty* cls, Span<Expr*> args) -> FunctionDecl*
{
  cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
  if (!cls || cls->kind != TyKind::Class || !cls->isKnown() || types.isDependent(cls))
    return nullptr;
  Scope* s = impl.classScopeOf(cls);
  if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
    return nullptr;
  std::vector<FunctionDecl*> ctors;
  bool any = false;
  for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Delete))
    {
      ctors.push_back(static_cast<FunctionDecl*>(m));
      any = any || impl.namesTextLast(static_cast<FunctionDecl*>(m));
    }
  if (!any)
    return nullptr;
  Subst subst = impl.substFor(cls);
  bool ambiguous = false;
  FunctionDecl* f = impl.chooseAmong(ctors, argTypesOf(args), subst.empty() ? nullptr : &subst, nullptr, ambiguous);
  return f && !ambiguous && impl.namesTextLast(f) ? f : nullptr;
}

auto GlobalScope::Impl::Walker::typeOfCall(CallExpr* c) -> const Ty*
{
  const Ty* t = typeOfCallUnpacked(c);
  Expr* callee = c->callee;
  Decl* d = callee && callee->kind == NodeKind::NameExpr     ? static_cast<NameExpr*>(callee)->resolved
            : callee && callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                             : nullptr;
  if (d && d->kind == NodeKind::FunctionDecl)
  {
    auto* f = static_cast<FunctionDecl*>(d);
    if (const Ty* ft = impl.declaredTypeOf(f, nullptr, 0); ft && ft->kind == TyKind::Function)
    {
      for (size_t i = 0; i < c->args.size() && i < ft->args.size(); ++i)
        checkClosureTo(c->args[i], ft->args[i], c->args[i]->token);
      // A function template's parameters as this call's instance has them (`CountedFormat<sizeof...(A)>`
      // for A = int, String: CountedFormat<2>), its deduced arguments substituted, never as declared;
      // those before a parameter pack only (they are the arguments at their positions), and none
      // while the call's arguments are not deduced.
      size_t checked = c->args.size();
      if (!f->templateParams.empty())
      {
        Subst deduced;
        for (size_t i = 0; i < c->templateArgCount && i < f->templateParams.size(); ++i)
          deduced.pairs.push_back({f->templateParams[i], c->templateArgs[i]});
        ft = deduced.pairs.size() == f->templateParams.size() ? impl.declaredTypeOf(f, &deduced, 0) : nullptr;
        if (ft && (ft->kind != TyKind::Function || types.isDependent(ft)))
          ft = nullptr;
        for (size_t i = 0; i < f->params.size(); ++i)
          if (static_cast<ParamDecl*>(f->params[i])->isPack)
            checked = std::min(checked, i);
      }
      for (size_t i = 0; ft && i < checked && i < ft->args.size(); ++i)
        checkImmediateConversion(c->args[i], ft->args[i]);
      checkTextPieces(f, ft, c->args);
    }
    checkImmediate(f, c->token, [&] { return impl.constValue(c, ctx(), 0); });
    packText(c->args, f, c->token);
  }
  else if (d && t && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::UsingAliasDecl || (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))))
  {
    // In a requirement (`requires { StringView(value); }`), a class built from arguments none of
    // its constructors takes is no well-formed expression: the requirement fails, never holds
    // because the cast has the class's type whatever the constructors.
    if (probing && !c->args.empty() && types.unqualified(t)->kind == TyKind::Class && !types.isDependent(t))
    {
      std::vector<const Ty*> traitArgs{t};
      bool known = true;
      for (const Ty* a : argTypesOf(c->args))
      {
        known = known && a && a->isKnown() && !types.isDependent(a);
        traitArgs.push_back(a);
      }
      if (known && impl.traitValue("isConstructible", traitArgs) == false)
        return unknown(Why::Overload);
    }
    checkConstruction(t, c->args, c->token);
    packText(c->args, textConstructor(t, c->args), c->token); // `Problem("bad ", point)`
  }
  else if (callee)
    packTextThrough(c->args, callee->ty, c->token); // `log("a", b)` with `auto log = [](Text text) {...}` or a function pointer
  return t;
}

auto GlobalScope::Impl::Walker::isBuiltinAppendTo(const Ty* t, uint32_t symbol) -> bool
{
  if (!t || symbol == UINT32_MAX || symbols.name(symbol) != "appendTo")
    return false;
  if (t->kind == TyKind::Enum)
  {
    // (a kpp enum's own appendTo method, in its scope, is called as written)
    Decl* e = t->decl;
    Lookup own;
    return e && !(e->ownScope && impl.lookupInScope(e->ownScope, symbol, own, 0) && own.decl && own.decl->kind == NodeKind::FunctionDecl);
  }
  return t->kind == TyKind::Bool || t->kind == TyKind::Char || t->kind == TyKind::Int || t->kind == TyKind::Float;
}

auto GlobalScope::Impl::Walker::builtinMemberCall(CallExpr* c, MemberExpr* m, const Ty* base, const Ty* builtin) -> const Ty*
{
  std::vector<const Ty*> argTypes{argumentType(m->base, base, types)};
  for (Expr* a : c->args)
    argTypes.push_back(argumentType(a, walkExpr(a), types));
  std::vector<FunctionDecl*> candidates;
  Lookup l;
  if (impl.lookupInScope(&impl.root, m->member.last().symbol, l, 0))
  {
    if (l.overloads)
    {
      for (const Entry& e : *l.overloads)
        if (e.decl && e.decl->kind == NodeKind::FunctionDecl && e.decl->file && e.decl->file->isLibrary)
          candidates.push_back(static_cast<FunctionDecl*>(e.decl));
    }
    else if (l.decl && l.decl->kind == NodeKind::FunctionDecl && l.decl->file && l.decl->file->isLibrary)
      candidates.push_back(static_cast<FunctionDecl*>(l.decl));
  }
  bool ambiguous = false;
  FunctionDecl* f = candidates.empty() ? nullptr : impl.chooseAmong(candidates, argTypes, nullptr, nullptr, ambiguous);
  const Ty* ft = f ? impl.declaredTypeOf(f, nullptr, 0) : nullptr;
  const Ty* takes = ft && ft->kind == TyKind::Function && !ft->args.empty() ? types.unqualified(types.stripRef(ft->args[0])) : nullptr;
  if (!f || ambiguous || !takes || (f->templateParams.empty() && takes != builtin))
  {
    // (an exact one only: no promotion makes a u8 print through the i32's)
    if (!probing && !inGenericTemplate())
      impl.reportCheckerError(file, m->token, types.toString(builtin, &symbols) + " has no appendTo taking it (design.md 14)");
    record(m, types.unknown());
    return unknown(Why::Member);
  }
  if (!ownsNodes())
  {
    record(m, ft);
    return ft->inner;
  }
  Arena& arena = impl.instanceArena();
  auto* name = arena.make<NameExpr>();
  name->kind = NodeKind::NameExpr;
  name->token = m->token;
  name->name = m->member;
  std::vector<Expr*> args{m->base};
  args.insert(args.end(), c->args.begin(), c->args.end());
  c->callee = name;
  c->args = Span<Expr*>{arena.array(args.data(), args.size()), uint32_t(args.size())};
  return typeOfCallUnpacked(c);
}

auto GlobalScope::Impl::Walker::typeOfCallUnpacked(CallExpr* c) -> const Ty*
{
  if (c->callee && c->callee->kind == NodeKind::CallExpr)
    checkPostfixAfterParenthesisedType(c->callee, c->token);
  if (NameExpr* typeName = parenthesisedTypeCallee(c); typeName && !probesSharedNodes())
    c->callee = typeName; // `(ImU32)(255)` as the functional cast `ImU32(255)`, which means the same
  std::vector<const Ty*> walked;
  walked.reserve(c->args.size());
  for (Expr* a : c->args)
    walked.push_back(walkExpr(a));
  std::vector<OverloadSetArgument> sets;
  for (size_t i = 0; i < c->args.size(); ++i)
    if (OverloadSetArgument s; overloadSetArgument(c->args[i], i, s))
    {
      sets.push_back(s);
      walked[i] = types.unknown();
    }
  WalkedArgs walkedArgsScope(*this, c->args, std::move(walked));
  struct SetsScope
  {
    const std::vector<OverloadSetArgument>*& current;
    const std::vector<OverloadSetArgument>* saved;
    ~SetsScope() { current = saved; }
  } setsScope{callSets, callSets};
  callSets = sets.empty() ? nullptr : &sets;
  Expr* callee = c->callee;
  bool ambiguous = false;
  if (callee->kind == NodeKind::NameExpr)
  {
    auto* n = static_cast<NameExpr*>(callee);
    if (n->name.parts.size() == 1 && !n->name.parts[0].hasTemplateArgs && c->args.size() == 2 &&
        c->args[1]->kind == NodeKind::SizeofExpr && static_cast<SizeofExpr*>(c->args[1])->type &&
        impl.symbols.name(n->name.parts[0].symbol) == "va_arg")
    {
      // `va_arg(list, T)` (the parser's form, Parser::parseVaArg): the next argument, a T.
      n->resolved = nullptr;
      ++stats.callsResolved;
      const Ty* t = typeOf(static_cast<SizeofExpr*>(c->args[1])->type);
      record(n, types.function(t, {}, false));
      return t;
    }
    if (n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && impl.isTypeTraitName(n->name.parts[0].symbol))
      if (const Ty* trait = impl.typeTrait(n->name.parts[0], ctx(), 0); trait && trait->isKnown())
      {
        // `Conditional<isSigned<U>, int64_t, uint64_t>(value)`: a functional cast to the type the
        // built-in trait gives, as the same name gives in a declaration (not the library's
        // Conditional struct).
        n->resolved = nullptr;
        for (Node* arg : n->name.parts[0].templateArgs)
          walkNode(arg);
        ++stats.callsResolved;
        record(n, trait);
        return trait;
      }
    if (n->name.parts.size() > 1 && n->name.last().special == 3 && n->name.last().convType)
    {
      // `super::operator bool()`: the conversion function of the class the qualifier names.
      NamedType written{};
      written.kind = NodeKind::NamedType;
      written.token = n->token;
      written.name = n->name;
      written.name.parts.count -= 1;
      const Ty* q = typeOf(&written);
      walkType(n->name.last().convType);
      Subst cs;
      if (FunctionDecl* conv = q && q->isKnown() && !types.isDependent(q)
                                   ? impl.conversionFunctionTo(types.unqualified(types.stripRef(q)), typeOf(n->name.last().convType), cs)
                                   : nullptr)
      {
        const Ty* ft = impl.declaredTypeOf(conv, cs.pairs.empty() ? nullptr : &cs, 0);
        n->resolved = conv;
        ++stats.callsResolved;
        record(n, ft);
        return ft->inner;
      }
    }
    // (before the lookup, which converts them: resolveName)
    for (const NamePart& part : n->name.parts)
      for (Node* arg : part.templateArgs)
        walkNode(arg);
    Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
    Impl::preferValue(l); // (`stat("/", &s)`: the function, not the struct's aggregate initialisation)
    if (!whenLookup(l, n))
    {
      record(n, types.unknown());
      return types.unknown(); // (design.md 19: reported, or the instances' to tell)
    }
    noteImplicitMember(n, l);
    Subst friendArgs; // the class arguments of the friend functions argument-dependent lookup finds
    if (l.found == Found::Global)
      l = impl.withNamespaceUsings(l, 0); // `using lib::size;` beside the namespace's own size(int)
    if ((!l.decl || l.decl->kind != NodeKind::FunctionDecl) && l.found != Found::Dependent && n->name.parts.size() > 1 &&
        n->name.last().special == 2 && P(n->name.last().opToken) == P::Assign && c->args.size() == 1 && thisType)
    {
      // `super::operator=(other)` of a class declaring none: its implicit assignment, of the
      // class the name is qualified with, on this object, as `this->super::operator=(other)`.
      NamedType written{};
      written.kind = NodeKind::NamedType;
      written.token = n->token;
      written.name = n->name;
      written.name.parts.count -= 1;
      if (const Ty* q = typeOf(&written); q && q->isKnown() && types.unqualified(types.stripRef(q))->kind == TyKind::Class)
      {
        const Ty* target = types.unqualified(types.stripRef(q));
        n->resolved = nullptr;
        ++stats.callsResolved;
        record(n, types.function(types.lref(target), {}, false));
        return types.lref(target);
      }
    }
    MemberResult viaInstance; // keeps a merged overload set alive
    if (l.found == Found::No && n->name.parts.size() > 1)
    {
      // `super::paintBackgroundGlow(...)` with super a DragPaneWidget<agui::VerticalFlow, 1>
      // deriving from its parameter: the member of the instance, found in the base its
      // arguments make, which the class template's own scope does not have; a member template
      // with its arguments too (`super::staticInitGenericOnOffControlBehaviorMembers<T>()`).
      NamedType written{};
      written.kind = NodeKind::NamedType;
      written.token = n->token;
      written.name = n->name;
      written.name.parts.count -= 1;
      const Ty* q = typeOf(&written);
      q = q && q->isKnown() && !types.isDependent(q) ? types.unqualified(types.stripRef(q)) : nullptr;
      if (q && q->kind == TyKind::Class)
      {
        viaInstance = memberOf(q, n->name.last().symbol, nullptr, n->token, false);
        if (viaInstance.decl && viaInstance.decl->kind == NodeKind::FunctionDecl && viaInstance.decl->parent &&
            viaInstance.decl->parent->ownScope)
        {
          l.decl = viaInstance.decl;
          l.overloads = viaInstance.overloads;
          l.found = Found::Member;
          l.foundIn = viaInstance.decl->parent->ownScope;
        }
      }
    }
    // Argument-dependent lookup, as C++ has it: an unqualified call also finds the functions of
    // its name in the namespaces of its arguments' classes and enumerations
    // (`shortNumberFormat(time, StringUtil::Compactness::Loose)` from namespace TimeUtil),
    // unless ordinary lookup found something else than namespace functions (a member, a local).
    if (n->name.parts.size() == 1 && !c->args.empty() && (l.found == Found::No || (l.found == Found::Global && l.decl && l.decl->kind == NodeKind::FunctionDecl)) &&
        !(l.found == Found::No && impl.builtinByName(n->name.parts[0].symbol)))
      addArgumentDependentCandidates(l, n->name.parts[0].symbol, argTypesOf(c->args), friendArgs, n->token);
    n->resolved = l.decl;
    switch (l.found)
    {
      case Found::No:
        if (n->name.parts.size() == 1)
          if (const Ty* b = impl.builtinByName(n->name.parts[0].symbol))
          {
            record(n, b); // `uint32_t(x)`: a functional cast, not an unresolved name
            return b;
          }
        ++stats.namesUnresolved;
        countUnresolved(n->name, n->token);
        {
          bool dependentCall = false;
          for (const Ty* a : argTypesOf(c->args))
            dependentCall = dependentCall || !a || !a->isKnown() || types.isDependent(a);
          reportUndeclared(n->name, n->token, nullptr, dependentCall);
        }
        record(n, unknown(Why::Name));
        // An SSE operation clang's intrinsics headers have as a macro (_mm_shuffle_ps), which the code
        // generator builds by name: its macro's result (`(__m128)__builtin_ia32_shufps(...)`), so an
        // overload, a template argument and a converting constructor are chosen by it as C++ does.
        if (n->name.parts.size() == 1 && n->name.parts[0].symbol != UINT32_MAX)
          if (const Ty* t = sseMacroResult(symbols.name(n->name.parts[0].symbol), types))
            return t;
        return types.unknown();
      case Found::Local: ++stats.namesLocal; break;
      case Found::Member: ++stats.namesMember; break;
      case Found::Global: ++stats.namesGlobal; break;
      case Found::Dependent: ++stats.namesDependent; break;
    }
    if (l.found == Found::Dependent)
    {
      // (`Args()` in the pattern of one element's expansion: that element's type.)
      const Ty* t = l.decl ? dependentNameType(l.decl) : types.unknown();
      record(n, t);
      if (l.decl && l.decl->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(l.decl)->isType)
        return callResultOfValue(t && t->kind == TyKind::Value && t->inner ? t->inner : t, c); // `func(a, b)`: a call through a constant parameter
      return t;
    }
    // `save(t)` in a member of a class with `using Vector2::save;` beside its own save overloads.
    Subst usingArgs;
    std::shared_ptr<std::vector<Entry>> usingMerged;
    if (n->name.parts.size() == 1 && l.found == Found::Member && l.foundIn && l.foundIn->kind == ScopeKind::Class)
    {
      const Ty* self = thisType && thisType->kind == TyKind::Pointer ? types.unqualified(thisType->inner) : nullptr;
      if ((usingMerged = withUsingOverloads(l, self, self ? impl.classScopeOf(self) : nullptr, usingArgs)))
      {
        l.decl = usingMerged->front().decl;
        l.overloads = usingMerged.get();
      }
    }
    else if (n->name.parts.size() > 1 && l.foundIn && l.foundIn->kind == ScopeKind::Class)
    {
      // `super::transfer(move(data))` with RollingStock's `using Vehicle::transfer;` beside its
      // own transfer(Fluid&): both sets, of the class the qualifier names.
      NamedType written{};
      written.kind = NodeKind::NamedType;
      written.token = n->token;
      written.name = n->name;
      written.name.parts.count -= 1;
      const Ty* q = typeOf(&written);
      q = q && q->isKnown() && !types.isDependent(q) ? types.unqualified(types.stripRef(q)) : nullptr;
      if ((usingMerged = withUsingOverloads(l, q, q ? impl.classScopeOf(q) : nullptr, usingArgs)))
      {
        l.decl = usingMerged->front().decl;
        l.overloads = usingMerged.get();
      }
    }
    Decl* d = l.decl;
    if (d && d->kind == NodeKind::FunctionDecl)
    {
      // `DamageTypeID::fromIndex(i)` with `using DamageTypeID = ID<DamageType, uint8_t>`: a static
      // member named through an instantiation has that instantiation's arguments.
      Subst qualifier = usingArgs;
      for (const Subst::Pair& p : friendArgs.pairs)
        qualifier.pairs.push_back(p); // a friend defined in an instance's class: that instance's arguments
      if (n->name.parts.size() > 1 && l.foundIn && l.foundIn->kind == ScopeKind::Class)
      {
        qualifier = impl.qualifierSubst(n->name, ctx(), 0, l.foundIn);
        for (const Subst::Pair& p : usingArgs.pairs)
          if (!qualifier.find(p.param))
            qualifier.pairs.push_back(p); // the base instances the using-declarations reach
      }
      else if (n->name.parts.size() == 1 && l.found == Found::Member && activeSubst && l.foundIn && l.foundIn->kind == ScopeKind::Class &&
               l.foundIn->owner)
      {
        // `construct(...)` in a member of UniquePointer<T>: the member of this instantiation (or of
        // the base it was found in), whose parameter types and constraints need T.
        for (const Subst::Pair& p : impl.baseSubstForUnqualified(l, ctx(), 0).pairs)
          if (!qualifier.find(p.param))
            qualifier.pairs.push_back(p);
        // With the enclosing instantiation's arguments, except a candidate's own parameters,
        // which the call deduces afresh (a member template calling itself).
        auto ownOfCandidate = [&](Decl* param)
        {
          auto owns = [&](Decl* f) { return std::find(f->templateParams.begin(), f->templateParams.end(), param) != f->templateParams.end(); };
          if (!l.overloads)
            return owns(d);
          for (const Entry& e : *l.overloads)
            if (e.decl && e.decl->kind == NodeKind::FunctionDecl && owns(e.decl))
              return true;
          return false;
        };
        for (const Subst::Pair& p : activeSubst->pairs)
          if (!qualifier.find(p.param) && !ownOfCandidate(p.param))
            qualifier.pairs.push_back(p);
      }
      const Subst* classSubst = qualifier.empty() ? nullptr : &qualifier;
      std::vector<FunctionDecl*> tied;
      std::vector<const Ty*> argTypes = argTypesOf(c->args);
      // In a template's own walk, a call of several functions (or of a template) with an argument
      // whose type depends on the template's parameters, or that this walk cannot type
      // (`ValueToWidget::make(*value)` with value a `const UniquePtr<T>&`), is resolved in each
      // instance with the instance's types ([temp.dep.candidate]), not here: ranking the
      // candidates against a type not known yet decides nothing, and costs the most of all in a
      // set of hundreds.
      if (!activeSubst && inGenericTemplate() && !onlyPlainCandidate(d, l.overloads))
      {
        bool dependentArgs = false;
        for (const Ty* a : argTypes)
          dependentArgs = dependentArgs || !a || !a->isKnown() || types.isDependent(a);
        if (dependentArgs)
        {
          c->templateArgCount = 0;
          n->resolved = nullptr;
          record(n, types.unknown());
          return unknown(Why::None);
        }
      }
      // A member called on the implicit object (`getSurface()`, `super::getStack(location)` in a
      // member of the class or of one derived from it): *this, whose const-ness chooses between
      // a const and a non-const overload as an explicit object's does.
      const Ty* implicitObject = nullptr;
      if (thisType && thisType->kind == TyKind::Pointer && thisType->inner && l.foundIn && l.foundIn->kind == ScopeKind::Class)
        if (Scope* self = impl.classScopeOf(types.unqualified(thisType->inner)); self && reachesClassScope(self, l.foundIn, 0))
          implicitObject = thisType->inner;
      // (*this is an lvalue: a member qualified && is no candidate, `f() &` is.)
      FunctionDecl* f = pickOverload(d, l.overloads, argTypes, classSubst, implicitObject ? types.lref(implicitObject) : nullptr, ambiguous, &n->name.last(), c->args, &tied);
      if (f && !ambiguous)
      {
        checkConstObjectCall(f, implicitObject, c->token);
        checkExplicitArguments(f, impl.declaredTypeOf(f, classSubst, 0), c->args); // `lane(7)` with `explicit Strict(int)`
        checkScalarArguments(f, impl.declaredTypeOf(f, classSubst, 0), c->args, true); // `g(a)` for a long, a converting two ways
      }
      if (!f)
      {
        // No callee the code generator could trust: not the first of several overloads, nor a
        // template with the arguments an earlier walk of this call recorded (the generic
        // one's). A single plain candidate stays: the code generator converts the arguments
        // itself and says so when it cannot.
        checkConstObjectOverloads(d, l.overloads, argTypes, classSubst, implicitObject, &n->name.last(), c->args, c->token); // `touch(1)` in a const member function
        c->templateArgCount = 0;
        // `holdsAlternative<int>(v)`, `get<int>(v)` on a typed union (E's report): std::variant's
        // functions take none; it is asked with `v == U::Name` and read with `v[Name]` (design.md 8.5).
        // (It was codegen's "a call that did not resolve".)
        for (const Ty* a : argTypes)
          if (const Ty* u = a ? types.unqualified(types.stripRef(a)) : nullptr; u && typedUnionOf(u) && !probing)
          {
            std::string_view called = symbols.name(n->name.last().symbol);
            if (called == "holdsAlternative" || called == "get" || called == "getIf" || called == "visit" || called == "variantSize" || called == "index")
            {
              std::string t = types.toString(u, &impl.symbols);
              reportCheck(c->token, std::string(called) + " takes no typed union (" + t + "): a `union enum` is asked with `v == " + t +
                                        "::Name` (or v.tag()) and read with `v[Name]` (design.md 8.5)");
              break;
            }
          }
        if (!onlyPlainCandidate(d, l.overloads))
          n->resolved = nullptr;
        else // `takeInt(item)` with a UniquePtr<Item>: a class where a parameter is a scalar, the checker's error
        {
          checkScalarArguments(static_cast<FunctionDecl*>(d), impl.declaredTypeOf(d, classSubst, 0), c->args);
          checkExplicitArguments(static_cast<FunctionDecl*>(d), impl.declaredTypeOf(d, classSubst, 0), c->args);
          checkReferenceArguments(static_cast<FunctionDecl*>(d), impl.declaredTypeOf(d, classSubst, 0), c->args); // `setHalf(i)` for a double&
        }
        record(n, types.unknown());
        return unknown(Why::Overload);
      }
      if (ambiguous)
      {
        ++stats.callsAmbiguousArity;
        reportAmbiguousCall(c, n->name.last(), argTypes, tied, classSubst);
      }
      ++stats.callsResolved;
      n->resolved = f;
      const Ty* fnTy = impl.declaredTypeOf(f, classSubst, 0);
      record(n, fnTy);
      if (!sets.empty() && !f->templateParams.empty() && n->name.last().hasTemplateArgs)
      {
        // `call<int, C>(&C::get, k)`: the parameter's type under the arguments written picks the function.
        Subst written = classSubst ? *classSubst : Subst{};
        bindExplicitArgs(f, n->name.last(), Span<Expr*>{}, written, false);
        bindOverloadSets(sets, impl.declaredTypeOf(f, &written, 0));
      }
      else
        bindOverloadSets(sets, fnTy);
      if (fnTy && fnTy->kind == TyKind::Function)
        for (size_t i = 0; i < c->args.size() && i < fnTy->args.size(); ++i)
          noteZeroNull(c->args[i], fnTy->args[i]); // `count(0)` for a Node*
      return returnTypeOfCall(f, classSubst, c->args, &n->name.last(), c);
    }
    // Constructor call, functional cast, callable variable...
    const Ty* t = typeOfResolvedName(l, n->name.last(), n);
    record(n, t);
    if (!sets.empty())
      bindOverloadSetsToConstructor(sets, t, c->args);
    if (d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
              (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))))
    {
      ++stats.callsResolved; // a functional cast, `int64_t(x)` included when int64_t is an imported typedef
      if (n->name.parts.size() > 1 &&
          (d->kind == NodeKind::UsingAliasDecl || d->kind == NodeKind::VariableDecl ||
           ((d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl) && d->templateParams.empty() && !impl.effectiveParams(d).empty())))
      {
        // `Table::value_type(65)`: the alias as the type path reads it, with the qualifier's
        // instantiation substituted (Array<unsigned char, 4>::value_type is unsigned char);
        // `Surface::const_iterator(surface)`: the nested class of the base
        // InfiniteRectangle<Chunk*> with that base's arguments; `Flag<A>::Enum(1)`: that instance's.
        NamedType written{};
        written.kind = NodeKind::NamedType;
        written.token = n->token;
        written.name = n->name;
        if (const Ty* asType = typeOf(&written); asType && asType->isKnown())
          t = asType;
      }
      if (impl.isUndeducedClassTemplate(t))
        if (const Ty* deduced = impl.deduceClassArgs(t, argTypesOf(c->args)))
        {
          record(n, deduced);
          return deduced; // `Pair(a, b)`, `Span(p, n)`: the class's arguments from a constructor (CTAD)
        }
      return t;
    }
    return callResultOfValue(t, c);
  }
  if (callee->kind == NodeKind::MemberExpr)
  {
    auto* m = static_cast<MemberExpr*>(callee);
    checkPostfixAfterParenthesisedType(m->base, m->token);
    const Ty* base = walkExpr(m->base);
    for (const NamePart& part : m->member.parts)
      for (Node* arg : part.templateArgs)
        walkNode(arg);
    const Ty* object = (m->op == P::Arrow || m->op == P::SafeArrow) ? objectForArrow(base) : base;
    if (m->member.empty())
    {
      record(m, types.unknown());
      return unknown(Why::Unsupported);
    }
    const NamePart& last = m->member.last();
    if (!object || !object->isKnown())
    {
      ++stats.membersUnknownBase;
      if (last.symbol != UINT32_MAX)
        ++stats.unknownBaseMembers[last.symbol];
      record(m, types.unknown());
      return unknown(Why::Base);
    }
    if (last.special == 1)
    {
      ++stats.membersBound;
      record(m, types.function(types.voidType(), {}, false));
      return types.voidType();
    }
    if (last.special == 2 && P(last.opToken) == P::LParen && m->member.parts.size() == 1 && types.stripRef(object)->kind == TyKind::Lambda)
    {
      // `runTest.operator()<CraftingMachine>(name)`: the lambda called, its template arguments
      // those written, then those its parameters deduce.
      ++stats.membersBound;
      record(m, types.stripRef(object));
      return callResultOfValue(object, c, &last);
    }
    if (last.special == 3 && last.convType)
    {
      // `commands.operator AllowedCommands::Enum()`: the class's conversion function to that type.
      walkType(last.convType);
      Subst cs;
      if (FunctionDecl* conv = impl.conversionFunctionTo(types.unqualified(types.stripRef(object)), typeOf(last.convType), cs))
      {
        const Ty* ft = impl.declaredTypeOf(conv, cs.pairs.empty() ? nullptr : &cs, 0);
        m->resolved = conv;
        ++stats.membersBound;
        record(m, ft);
        return ft->inner;
      }
    }
    uint32_t symbol = last.special == 2 && last.opToken < uint32_t(P::Count) ? impl.opSymbols[last.opToken] : last.symbol;
    if (symbol == UINT32_MAX)
    {
      record(m, types.unknown());
      return unknown(Why::Unsupported);
    }
    if (const Ty* builtin = types.unqualified(types.stripRef(object)); m->op == P::Dot && m->member.parts.size() == 1 && !last.hasTemplateArgs &&
                                                                       isBuiltinAppendTo(builtin, symbol))
      return builtinMemberCall(c, m, base, builtin);
    MemberResult r = memberOf(object, symbol, &m->member, m->token);
    m->resolved = r.decl;
    if ((!r.decl || r.decl->kind != NodeKind::FunctionDecl) && last.special == 2 && P(last.opToken) == P::Assign && c->args.size() == 1)
    {
      // `this->CollisionMask::operator=(other)` of a class declaring none: its implicit
      // assignment, of the class the name is qualified with (or the object's), giving the object.
      const Ty* target = types.unqualified(types.stripRef(object));
      if (m->member.parts.size() > 1)
      {
        NamedType written{};
        written.kind = NodeKind::NamedType;
        written.token = m->token;
        written.name = m->member;
        written.name.parts.count -= 1;
        if (const Ty* q = typeOf(&written); q && q->isKnown() && types.unqualified(types.stripRef(q))->kind == TyKind::Class)
          target = types.unqualified(types.stripRef(q));
      }
      if (target && target->kind == TyKind::Class)
      {
        ++stats.callsResolved;
        m->resolved = nullptr; // (the qualifier's class, which the name found instead)
        record(m, types.function(types.lref(target), {}, false));
        return types.lref(target);
      }
    }
    if (!r.decl)
    {
      if (r.why == Why::Base)
        ++stats.membersUnknownBase;
      record(m, types.unknown());
      return r.why == Why::None ? types.unknown() : unknown(r.why);
    }
    if (r.decl->kind == NodeKind::FunctionDecl)
    {
      std::vector<FunctionDecl*> tied;
      std::vector<const Ty*> argTypes = argTypesOf(c->args);
      // The object with its value category (argumentType's: an lvalue T&, an xvalue T&&, a prvalue
      // as T&& too), which a ref-qualified member function's candidacy depends on.
      const Ty* category = m->op == P::Arrow || m->op == P::SafeArrow ? types.lref(types.stripRef(object)) : argumentType(m->base, base, types);
      if (category && !category->isReference())
        category = types.rref(category);
      FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypes, &r.subst, category, ambiguous, &last, c->args, &tied);
      if (f && !ambiguous)
      {
        checkConstObjectCall(f, object, m->token);
        checkRefQualifiedCall(f, category, m->token);
        checkExplicitArguments(f, impl.declaredTypeOf(f, &r.subst, 0), c->args);
        checkScalarArguments(f, impl.declaredTypeOf(f, &r.subst, 0), c->args, true);
      }
      if (!f)
      {
        checkConstObjectOverloads(r.decl, r.overloads, argTypes, &r.subst, object, &last, c->args, m->token); // `items.push_back(1)` on a const Vector
        ++stats.membersBound;
        c->templateArgCount = 0; // as for a name
        if (!onlyPlainCandidate(r.decl, r.overloads))
        {
          m->resolved = nullptr;
          reportNoMatchingMember(r.decl, r.overloads, object, symbol, argTypes, m->token); // `s.find("x", 0, 5)`: String has no find of three
        }
        else // `counter.add(item)` with a UniquePtr<Item>, as for a name
        {
          checkScalarArguments(static_cast<FunctionDecl*>(r.decl), impl.declaredTypeOf(r.decl, &r.subst, 0), c->args);
          checkExplicitArguments(static_cast<FunctionDecl*>(r.decl), impl.declaredTypeOf(r.decl, &r.subst, 0), c->args);
          checkReferenceArguments(static_cast<FunctionDecl*>(r.decl), impl.declaredTypeOf(r.decl, &r.subst, 0), c->args);
        }
        record(m, types.unknown());
        return unknown(Why::Overload);
      }
      if (ambiguous)
      {
        ++stats.callsAmbiguousArity;
        reportAmbiguousCall(c, last, argTypes, tied, &r.subst);
      }
      ++stats.callsResolved;
      m->resolved = f;
      const Ty* fnTy = impl.declaredTypeOf(f, &r.subst, 0);
      // A plain `auto` member function's signature: the one its instance deduces (returnTypeOfCall's),
      // never what declaredTypeOf has cached by now, which another thread's walk of the body writes
      // or not yet: the member access was typed `auto(...)` or deduced, and counted dependent or
      // bound, by the threads' timing (`getScreenPositionFunction()`, the headline at -j 8 and 32).
      const Ty* deducedResult = nullptr;
      if (f->templateParams.empty() && f->returnType && !f->trailingReturn && Impl::astMentionsAuto(f->returnType) && fnTy &&
          fnTy->kind == TyKind::Function)
      {
        deducedResult = returnTypeOfCall(f, &r.subst, c->args, &last, c);
        const Ty* declared = impl.computeDeclaredType(f, &r.subst, 0); // (the auto one)
        if (deducedResult && deducedResult->isKnown() && declared && declared->kind == TyKind::Function)
        {
          std::vector<const Ty*> params(declared->args.begin(), declared->args.end());
          fnTy = types.functionLike(declared, impl.deducedShape(declared->inner, deducedResult), Span<const Ty*>{params.data(), uint32_t(params.size())});
        }
      }
      record(m, fnTy);
      if (fnTy && fnTy->kind == TyKind::Function)
        for (size_t i = 0; i < c->args.size() && i < fnTy->args.size(); ++i)
          noteZeroNull(c->args[i], fnTy->args[i]);
      if (types.isDependent(fnTy))
        ++stats.membersDependent;
      else
        ++stats.membersBound;
      bindOverloadSets(sets, fnTy);
      return deducedResult ? deducedResult : returnTypeOfCall(f, &r.subst, c->args, &last, c);
    }
    if (r.type && types.isDependent(r.type))
      ++stats.membersDependent;
    else
      ++stats.membersBound;
    record(m, r.type ? r.type : types.unknown());
    if (r.why != Why::None)
      return unknown(r.why);
    return callResultOfValue(r.type, c);
  }
  const Ty* t = walkExpr(callee);
  return callResultOfValue(t, c);
}

auto GlobalScope::Impl::Walker::genericLambdaCall(const Ty* t, CallExpr* c, const NamePart* written) -> const Ty*
{
  t = types.unqualified(t); // `(*this->f)(x)` with a `const F*` f: the closure type the lambda's environment is kept for
  LambdaExpr* l = Impl::lambdaOf(t);
  std::vector<const Ty*> argTypes = argTypesOf(c->args);
  Subst deduced;
  std::vector<const Ty*> patterns(t->args.begin(), t->args.end());
  if (written && written->hasTemplateArgs)
  {
    bindExplicitArgs(l->templateParams, *written, c->args, deduced);
    for (const Ty*& pattern : patterns)
      pattern = impl.substitute(pattern, deduced);
  }
  impl.deduceParams(l->params, Span<const Ty*>{patterns.data(), uint32_t(patterns.size())}, argTypes, deduced);
  Subst own; // in parameter order: the instantiation's key
  for (Decl* p : l->templateParams)
  {
    const Ty* a = deduced.find(p);
    if (!a || !a->isKnown() || types.isDependent(a))
      return unknown(Why::Deduction);
    own.pairs.push_back({p, a});
  }
  if (own.pairs.size() > 8)
    return unknown(Why::Deduction);
  for (size_t i = 0; i < own.pairs.size(); ++i)
    c->templateArgs[i] = own.pairs[i].type;
  c->templateArgCount = uint8_t(own.pairs.size()); // the count last: other threads type the same call
  // The instantiation's own copy of the body, typed once (null while the lambda calls itself
  // through a capture).
  const Ty* callTy = instantiationDepth > 16 ? nullptr : impl.checkLambdaInstance(t, own, nullptr, &stats);
  return callTy && callTy->kind == TyKind::Function ? callTy->inner : unknown(Why::AutoReturn);
}

auto GlobalScope::Impl::Walker::callResultOfValue(const Ty* t, CallExpr* c, const NamePart* written) -> const Ty*
{
  t = types.stripRef(t);
  if (!t || !t->isKnown())
    return types.unknown();
  // The value called as a message names it: a variable's name (`pointer(d)`, `sink(d)`).
  auto calleeName = [&]() -> std::string
  {
    if (c && c->callee && c->callee->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(c->callee)->name.empty())
      if (uint32_t s = static_cast<NameExpr*>(c->callee)->name.last().symbol; s != UINT32_MAX)
        return std::string(symbols.name(s));
    if (c && c->callee && c->callee->kind == NodeKind::MemberExpr && !static_cast<MemberExpr*>(c->callee)->member.empty())
      if (uint32_t s = static_cast<MemberExpr*>(c->callee)->member.last().symbol; s != UINT32_MAX)
        return std::string(symbols.name(s));
    return "the function called";
  };
  if (t->kind == TyKind::Function)
  {
    if (c)
      checkReferenceBindings(calleeName(), {}, t->args, c->args);
    return t->inner;
  }
  if (t->kind == TyKind::Pointer && t->inner->kind == TyKind::Function)
  {
    if (c)
      checkReferenceBindings(calleeName(), {}, t->inner->args, c->args); // `pointer(d)` with a `void (*)(Base&&)`
    return t->inner->inner;
  }
  if (t->kind == TyKind::Lambda)
  {
    if (c && Impl::isGenericLambda(t))
      return genericLambdaCall(t, c, written);
    if (LambdaExpr* l = Impl::lambdaOf(t); c && l)
      checkReferenceBindings(calleeName(), l->params, t->args, c->args); // `lambda(d)` with `[](Base&& x)`
    return t->inner ? t->inner : unknown(Why::AutoReturn);
  }
  if (t->kind == TyKind::Class)
  {
    // `fn(4)` with a conversion of fn to a pointer to a function: that function's result.
    auto surrogateCall = [&]() -> const Ty*
    {
      bool ambiguous = false;
      const Ty* through = impl.surrogateFunction(t, argTypesOf(c->args), ambiguous);
      if (!through)
        return nullptr;
      if (ambiguous)
        reportCheck(c->token, "the call of an object of " + types.toString(types.unqualified(t), &impl.symbols) + " is ambiguous between the functions its conversions give");
      const Ty* callee = types.unqualified(types.stripRef(through));
      callee = callee->kind == TyKind::Pointer ? types.unqualified(callee->inner) : callee;
      checkReferenceBindings(calleeName(), {}, callee->args, c->args);
      return callee->inner;
    };
    MemberResult r = memberOf(t, impl.opSymbols[uint32_t(P::LParen)], nullptr, c->token, false);
    if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
    {
      bool ambiguous;
      std::vector<const Ty*> argTypes = argTypesOf(c->args);
      std::vector<FunctionDecl*> tied;
      if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypes, &r.subst, t, ambiguous, nullptr, {}, &tied))
      {
        if (!ambiguous)
          checkConstObjectCall(f, t, c->token);
        // A closure base's call operator (`using Ts::operator()...;`, the overload-set idiom): the
        // lambda's call, as on the closure itself. Two that take the arguments alike (`[](int)
        // {...}` twice) are an ambiguous call, as in C++, never the first one's.
        bool closureTied = false;
        for (FunctionDecl* g : tied)
          closureTied = closureTied || impl.closureCallOf(g);
        if (ambiguous && closureTied)
        {
          NamePart callOperator;
          callOperator.special = 2;
          callOperator.opToken = uint32_t(P::LParen);
          reportAmbiguousCall(c, callOperator, argTypes, tied, &r.subst);
        }
        if (ClosureCall* call = impl.closureCallOf(f))
          return callResultOfValue(call->closure, c, written);
        return returnTypeOfCall(f, &r.subst, c->args, nullptr);
      }
      if (const Ty* through = surrogateCall())
        return through;
      if (onlyPlainCandidate(r.decl, r.overloads)) // `sink(d)` with a `void operator()(Base&& x)`, as for a name
        checkReferenceArguments(static_cast<FunctionDecl*>(r.decl), impl.declaredTypeOf(r.decl, &r.subst, 0), c->args, calleeName());
      return unknown(Why::Overload);
    }
    if (const Ty* through = surrogateCall())
      return through;
    return r.why == Why::Member ? unknown(Why::Member) : types.unknown();
  }
  if (t->kind == TyKind::Param)
    return t;
  return unknown(Why::Unsupported);
}

auto GlobalScope::Impl::Walker::reversedSubscript(const Ty* b, const Ty* base) -> bool
{
  if (b->kind == TyKind::Int || b->kind == TyKind::Char || b->kind == TyKind::Bool)
    return true;
  if (b->kind == TyKind::Enum)
    return !static_cast<EnumDecl*>(b->decl)->isClass;
  if (b->kind != TyKind::Class || typedUnionOf(b) || memberOf(b, impl.opSymbols[uint32_t(P::LBracket)], nullptr, 0, false).decl)
    return false;
  return impl.conversionRank(base, types.intType(64)) < Impl::NotViable; // (`oi[arr]`, through its conversion to an integer)
}

auto GlobalScope::Impl::Walker::promote(const Ty* t) -> const Ty*
{
  t = types.unqualified(types.stripRef(t));
  switch (t->kind)
  {
    case TyKind::Bool: case TyKind::Char: case TyKind::Int: return types.promoted(t); // (char32_t to unsigned int)
    case TyKind::Enum:
    {
      // One with a fixed underlying type promotes as that type does ([conv.prom]/4: an `enum :
      // long long` stays 64 bits, an `enum : uint8_t` is an int); another is an int (MSVC's).
      const Ty* u = impl.declaredUnderlying(t);
      return u && u->isKnown() && u->kind != TyKind::Enum ? promote(u) : types.intType(32);
    }
    default: return t;
  }
}

auto GlobalScope::Impl::Walker::operandType(Expr* e, const Ty* t) -> const Ty*
{
  if (!designatedBitField(e))
  {
    // (or a structured binding, which may name a bit field: bitFieldPromotion finds which)
    Expr* x = designatedName(e);
    Decl* d = x && x->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(x)->resolved : nullptr;
    if (!d || d->kind != NodeKind::VariableDecl || static_cast<VariableDecl*>(d)->structuredBindings.empty())
      return t; // (any other operand at once: no context made)
  }
  const Ty* promoted = impl.bitFieldPromotion(e, t, ctx().subst);
  return promoted ? promoted : t;
}

auto GlobalScope::Impl::Walker::arithmetic(const Ty* a, const Ty* b) -> const Ty*
{
  a = promote(a);
  b = promote(b);
  return types.usualArithmetic(a, b);
}

// Whether f, an operator= member lookup found for class def, is one `a = b` of a def calls: def's
// own, or a base's that a using-declaration in def brings in (BoolConfigItem's `using
// StorageConfigItem<bool>::operator=;`). Any other base's is hidden by def's implicit assignment.
static bool callableAssignment(ClassDecl* def, FunctionDecl* f)
{
  if (!def || f->parent == def)
    return true;
  for (Decl* m : def->members)
    if (m->kind == NodeKind::UsingNameDecl)
    {
      const QualifiedName& name = static_cast<UsingNameDecl*>(m)->name;
      if (!name.empty() && name.last().special == 2 && P(name.last().opToken) == P::Assign)
        return true;
    }
  return false;
}

auto GlobalScope::Impl::Walker::operatorFunction(P op, const Ty* lhs, const Ty* rhs, uint32_t token, size_t arity, Subst& subst, bool& isMember) -> FunctionDecl*
{
  FunctionDecl* constViolation = nullptr; // the class's own operator, non-const, on a const lhs
  Subst violationSubst;
  FunctionDecl* member = nullptr; // the class's own operator that takes the operands
  Subst memberSubst;
  uint32_t symbol = impl.opSymbols[uint32_t(op)];
  const Ty* l = types.stripRef(lhs);
  isMember = false;
  if (l && l->kind == TyKind::Class)
  {
    MemberResult r = memberOf(l, symbol, nullptr, token, false);
    if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
    {
      bool ambiguous;
      std::vector<const Ty*> argTypes;
      if (arity == 2)
        argTypes.push_back(rhs);
      FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypes, &r.subst, lhs, ambiguous);
      // `a = b` of a class that declares no operator= of its own: its implicit copy or move
      // assignment, which hides every base's operator= (C++ [class.copy.assign]; a using-declaration
      // brings one in: BoolConfigItem's `using StorageConfigItem<bool>::operator=;`). Not the base's, which would leave the class's own members as they
      // were (classes/implicit_assignment_hides_base_operator).
      if (f && op == P::Assign && f->parent && !callableAssignment(impl.classDefinition(l), f))
        f = nullptr;
      if (f)
      {
        if (!nonConstOnConstObject(f, lhs))
        {
          member = f;
          memberSubst = r.subst;
        }
        else
        {
          constViolation = f;
          violationSubst = r.subst;
        }
      }
    }
  }
  // Free function: look in the namespaces of both operand classes and enums and the current
  // scope; members are not candidates here (a class's own operator== hides no free one). The
  // operands' classes' friend operators are candidates beside them, with their classes'
  // arguments in out. quiet: a member operator takes the operands too, and the free one is only
  // ranked against it: no built-in operator is a candidate then, and nothing is reported.
  std::vector<const Ty*> argTypes{lhs};
  if (arity == 2)
    argTypes.push_back(rhs);
  auto findFree = [&](bool quiet, Subst& out) -> FunctionDecl*
  {
    Subst friendArgs;
    std::string conflict;
    Lookup friends = impl.hiddenFriendLookup(symbol, argTypes, friendArgs, &conflict);
    if (!conflict.empty() && !quiet)
      reportHere(token, conflict);
    auto pick = [&](const Lookup& look) -> FunctionDecl*
    {
      bool ambiguous;
      FunctionDecl* f = pickOverload(look.decl, look.overloads, argTypes, friendArgs.empty() ? nullptr : &friendArgs, nullptr, ambiguous);
      if (!f)
        return nullptr;
      // Against the built-in operator on the operands' conversions (no member operator here).
      if (!quiet)
        switch (impl.freeOperatorAgainstBuiltin(f, (f->flags & DF_Friend) && !friendArgs.empty() ? &friendArgs : nullptr, op, argTypes))
        {
          case 0:
            return nullptr;
          case -1:
            reportHere(token, "the operator " + std::string(punctuatorSpelling(op)) + " on " + types.toString(types.unqualified(l), &symbols) +
                                  (arity == 2 ? " and " + types.toString(types.unqualified(types.stripRef(rhs)), &symbols) : std::string()) +
                                  " is ambiguous: the free operator" +
                                  std::string(punctuatorSpelling(op)) + " at " + impl.locationOf(f->file, f->token) +
                                  " takes an operand by a user-defined conversion, as the built-in operator does by a conversion operator");
            break;
        }
      if ((f->flags & DF_Friend) && !friendArgs.empty())
        out = friendArgs;
      return f;
    };
    std::vector<Scope*> places;
    for (const Ty* t : {l, types.stripRef(rhs)})
      if (t && (t->kind == TyKind::Class || t->kind == TyKind::Enum) && t->decl)
        places.push_back(impl.lexicalScopeOf(t->decl));
    places.push_back(scope);
    // Every place's operators ranked together, as C++ ranks argument-dependent lookup's and
    // ordinary lookup's ([over.match.oper] 3): from each place the innermost scope declaring the
    // name, and the operands' classes' friends. The first place with a viable one won:
    // `left / Path(right)` with left a filesystem::path took filesystem's operator/(const path&,
    // const path&), a path, over Filesystem's operator/(const path&, const Path&), which takes the
    // operands exactly and gives the Path C++ gives (language/operator_from_every_associated_namespace).
    std::vector<Entry> all;
    std::unordered_set<Decl*> seen;
    auto add = [&](const Lookup& look) {
      if (look.overloads)
      {
        for (const Entry& e : *look.overloads)
          if (e.decl && e.decl->kind == NodeKind::FunctionDecl && seen.insert(e.decl).second)
            all.push_back(e);
      }
      else if (look.decl && look.decl->kind == NodeKind::FunctionDecl && seen.insert(look.decl).second)
        all.push_back(Entry{look.decl, nullptr});
    };
    for (Scope* place : places)
      for (Scope* s = place; s; s = s->parent)
      {
        if (s->kind == ScopeKind::Class)
          continue;
        Lookup look;
        if (!impl.lookupInScope(s, symbol, look, 0))
          continue;
        if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
          add(look);
        break;
      }
    if (friends.decl)
      add(friends);
    if (!all.empty())
    {
      Lookup merged;
      merged.found = Found::Global;
      merged.decl = all[0].decl;
      merged.overloads = &all;
      if (FunctionDecl* f = pick(merged))
        return f;
    }
    return nullptr;
  };
  if (member)
  {
    // C++ ranks the class's own operator and the free ones together ([over.match.oper]): a free one
    // that takes the operands strictly better is called (`A{1} + 1.5` with A::operator+(int) beside
    // operator+(const A&, double): the free one, where the member was, silently another function
    // than C++'s; classes/operator_member_and_free_ranked_together).
    Subst freeSubst;
    if (FunctionDecl* free = impl.memberOperatorExact(member, memberSubst, rhs, arity) ? nullptr : findFree(true, freeSubst))
    {
      int better = impl.compareFreeWithMember(free, freeSubst, member, memberSubst, lhs, rhs, arity);
      if (better == 1)
      {
        subst = freeSubst;
        return free;
      }
      // Neither takes the operands better: ambiguous, as in C++ (`m + 1` with M::operator+(long)
      // beside operator+(const M&, unsigned); the member was taken).
      if (better == 0 && !probing)
        reportHere(token, "the operator " + std::string(op < P::Count ? punctuatorSpelling(op) : "?") + " on " +
                            types.toString(types.unqualified(types.stripRef(lhs)), &symbols) + " and " +
                            types.toString(types.unqualified(types.stripRef(rhs)), &symbols) + " is ambiguous: the class's member operator" +
                            std::string(op < P::Count ? punctuatorSpelling(op) : "?") + " and a free one take the operands equally well");
    }
    subst = memberSubst;
    isMember = true;
    return member;
  }
  if (FunctionDecl* free = findFree(false, subst))
    return free;
  if (constViolation)
  {
    // (no other operator takes it: the error, and the operator for the walk to go on with)
    checkConstObjectCall(constViolation, lhs, token);
    subst = violationSubst;
    isMember = true;
    return constViolation;
  }
  return nullptr;
}

auto GlobalScope::Impl::Walker::threeWayDefaulted(const Ty* lhs, const Ty* rhs, uint32_t token) -> bool
{
  Subst subst;
  bool isMember;
  FunctionDecl* f = operatorFunction(P::Spaceship, lhs, rhs, token, 2, subst, isMember);
  return f && (f->flags & DF_Default);
}

auto GlobalScope::Impl::Walker::implicitEquality(const Ty* lhs, const Ty* rhs, uint32_t token) -> bool
{
  const Ty* cls = types.unqualified(types.stripRef(lhs));
  if (!cls || cls->kind != TyKind::Class || !threeWayDefaulted(cls, cls, token))
    return false;
  return impl.conversionRank(rhs, types.lref(types.qualified(cls, true, false))) < Impl::NotViable;
}

auto GlobalScope::Impl::Walker::rewrittenBeats(P op, FunctionDecl* f, const Subst& subst, bool member, const Ty* lhs, const Ty* rhs, uint32_t token) -> bool
{
  std::vector<int> mine = impl.binaryOperatorRanks(f, subst, member, lhs, rhs);
  if (mine.size() != 2)
    return false;
  bool mineTemplate = !f->templateParams.empty();
  // (theirs ranks the operands in the candidate's own order: the reversed one takes rhs first)
  auto beats = [&](FunctionDecl* g, const std::vector<int>& theirs, bool reversed) {
    if (!g || g == f || theirs.size() != 2)
      return false;
    int l = reversed ? theirs[1] : theirs[0], r = reversed ? theirs[0] : theirs[1];
    if (l <= mine[0] && r <= mine[1] && (l < mine[0] || r < mine[1]))
      return true;
    return l == mine[0] && r == mine[1] && mineTemplate && g->templateParams.empty();
  };
  P with = op == P::EqEq || op == P::NotEq ? P::EqEq : P::Spaceship;
  if (op != P::EqEq) // (`a == b`'s forward candidate is f itself)
  {
    Subst forward;
    bool forwardMember = false;
    if (FunctionDecl* g = operatorFunction(with, lhs, rhs, token, 2, forward, forwardMember))
      if (beats(g, impl.binaryOperatorRanks(g, forward, forwardMember, lhs, rhs), false))
        return true;
  }
  Subst reversed;
  bool reversedMember = false;
  if (FunctionDecl* g = operatorFunction(with, rhs, lhs, token, 2, reversed, reversedMember))
    if (beats(g, impl.binaryOperatorRanks(g, reversed, reversedMember, rhs, lhs), true))
      return true;
  return false;
}

auto GlobalScope::Impl::Walker::overloadedOperator(P op, const Ty* lhs, const Ty* rhs, uint32_t token, size_t arity, bool countFailure, const Ty* lhsOperand) -> const Ty*
{
  const Ty* l = types.stripRef(lhs);
  Subst subst;
  bool isMember;
  Span<Expr*> none;
  // The left operand as an argument (an lvalue a reference): a free `Stream& operator<<(Stream&,
  // const char*)` binds the stream lvalue.
  operatorChosen = nullptr;
  operatorCall = OperatorCall::Undecided;
  if (FunctionDecl* f = operatorFunction(op, lhsOperand ? lhsOperand : lhs, rhs, token, arity, subst, isMember))
  {
    // `one < 1.5` with a member operator<(const Box&), which converts 1.5 by a constructor, and an
    // operator<=> template taking it as it is: the rewritten `(one <=> 1.5) < 0`, a better match
    // (classes/rewritten_three_way_beats_operator_needing_a_conversion; it compared 1 with 1);
    // `w != x` with an operator!= template beside an operator== taking x as well: `!(w == x)`.
    if (arity == 2 && (op == P::Less || op == P::Greater || op == P::LessEq || op == P::GreaterEq || op == P::EqEq || op == P::NotEq) &&
        rewrittenBeats(op, f, subst, isMember, lhsOperand ? lhsOperand : lhs, rhs, token))
    {
      operatorCall = OperatorCall::Rewritten;
      return types.boolType();
    }
    // A member that takes the right operand by a user-defined conversion against the built-in
    // operator through the left one's conversion operator: neither is better, as C++ ranks them
    // (B's `float32x4 < __m128`, which clang rejects; kpp took the member).
    if (isMember && arity == 2 && !probing && impl.memberOperatorAgainstBuiltin(f, subst.empty() ? nullptr : &subst, op, lhs, rhs) < 0)
      reportHere(token, "the operator " + std::string(punctuatorSpelling(op)) + " on " + types.toString(types.unqualified(l), &symbols) + " and " +
                          types.toString(types.unqualified(types.stripRef(rhs)), &symbols) + " is ambiguous: the member operator" +
                          std::string(punctuatorSpelling(op)) + " at " + impl.locationOf(f->file, f->token) +
                          " takes the right operand by a user-defined conversion, as the built-in operator takes the left one by its conversion operator");
    operatorChosen = f;
    operatorIsMember = isMember;
    operatorCall = OperatorCall::Function;
    // The operands a template deduces from: a member's object is not one of them.
    std::vector<const Ty*> operands;
    if (!isMember)
      operands.push_back(lhsOperand ? lhsOperand : lhs);
    if (arity == 2)
      operands.push_back(rhs);
    return returnTypeOfCall(f, isMember || !subst.empty() ? &subst : nullptr, none, nullptr, nullptr, &operands);
  }
  if (arity == 2)
  {
    // The comparisons C++ rewrites when the class defines no operator for them: `a != b` is
    // `!(a == b)`, `a < b` and the other orderings are `(a <=> b) < 0`, and a defaulted
    // operator<=> gives `==` too.
    switch (op)
    {
      case P::NotEq:
        if (operatorFunction(P::EqEq, lhs, rhs, token, 2, subst, isMember) || threeWayDefaulted(lhs, rhs, token) ||
            operatorFunction(P::EqEq, rhs, lhs, token, 2, subst, isMember) || implicitEquality(lhs, rhs, token) ||
            implicitEquality(rhs, lhs, token))
        {
          operatorCall = OperatorCall::Rewritten; // `3 != id` through Id::operator==(int): the operands reversed
          return types.boolType();
        }
        break;
      case P::EqEq:
        if (threeWayDefaulted(lhs, rhs, token) || operatorFunction(P::EqEq, rhs, lhs, token, 2, subst, isMember) ||
            implicitEquality(lhs, rhs, token) || implicitEquality(rhs, lhs, token))
        {
          operatorCall = OperatorCall::Rewritten;
          return types.boolType();
        }
        break;
      case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
        // `minimum < scale` through scale's operator<=>(const double&) too: C++20's reversed
        // candidate, `0 < (scale <=> minimum)`.
        if (operatorFunction(P::Spaceship, lhs, rhs, token, 2, subst, isMember) || operatorFunction(P::Spaceship, rhs, lhs, token, 2, subst, isMember))
        {
          operatorCall = OperatorCall::Rewritten;
          return types.boolType();
        }
        break;
      case P::Spaceship:
      {
        // `minimum <=> scale`: the reversed `0 <=> (scale <=> minimum)`, of that call's type.
        Subst reversed;
        if (FunctionDecl* f = operatorFunction(P::Spaceship, rhs, lhs, token, 2, reversed, isMember))
        {
          std::vector<const Ty*> operands;
          if (!isMember)
            operands.push_back(rhs);
          operands.push_back(lhs);
          operatorCall = OperatorCall::Rewritten;
          return returnTypeOfCall(f, isMember || !reversed.empty() ? &reversed : nullptr, none, nullptr, nullptr, &operands);
        }
        break;
      }
      default:
        break;
    }
  }
  // Operators every class has or that always yield bool.
  switch (op)
  {
    case P::Assign:                                   // implicit copy/move assignment
      if (l && l->kind == TyKind::Class)
      {
        checkConstModification(token, l, "assignment to");
        checkExplicitCopy(rhs, types.lref(types.qualified(l, true, false)), token); // `s = 5;`: its operator=(const S&) given 5
      }
      operatorCall = OperatorCall::Builtin;
      return lhs;
    case P::AndAnd: case P::OrOr: case P::Bang:
      operatorCall = OperatorCall::Builtin;
      return types.boolType();
    case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
      if (l && l->kind == TyKind::Enum)
      {
        operatorCall = OperatorCall::Builtin;
        return types.boolType();
      }
      break;
    default:
      break;
  }
  return countFailure ? unknown(Why::Operator) : nullptr;
}

auto GlobalScope::Impl::Walker::deletedAssignment(const Ty* cls, bool move, int level, const std::string& prefix) -> std::string
{
  cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
  ClassDecl* def = cls && cls->kind == TyKind::Class && cls->isKnown() && !types.isDependent(cls) && !Impl::mentionsUnknown(cls) && !Impl::lambdaOf(cls)
                     ? impl.classDefinition(cls)
                     : nullptr;
  if (!def || level > 8 || def->isTypedUnion || def->keyword == Kw::union_ || (def->attributes & ATTR_Draft))
    return {};
  Subst s = impl.substFor(cls);
  const Subst* sp = s.pairs.empty() ? nullptr : &s;
  // Its own special members: an operator= or a constructor taking the class alone (the rest defaulted).
  FunctionDecl* copyAssign = nullptr;
  FunctionDecl* moveAssign = nullptr;
  bool copyConstructor = false, moveConstructor = false, destructor = false;
  for (Decl* m : membersRead(def))
  {
    if (m->kind != NodeKind::FunctionDecl || m->builtin)
      continue;
    auto* f = static_cast<FunctionDecl*>(m);
    if (f->flags & DF_Destructor)
    {
      destructor = true;
      continue;
    }
    if (!f->templateParams.empty() || f->params.empty() || f->name.empty())
      continue;
    bool rest = true;
    for (size_t i = 1; i < f->params.size(); ++i)
      rest = rest && static_cast<ParamDecl*>(f->params[i])->defaultValue;
    const Ty* p = impl.declaredTypeOf(f->params[0], sp, 0);
    if (!rest || !p || !p->isKnown() || types.unqualified(types.stripRef(p)) != cls)
      continue;
    bool assign = f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign && f->params.size() == 1;
    if (f->flags & DF_Constructor)
      (p->kind == TyKind::RRef ? moveConstructor : copyConstructor) = p->isReference();
    else if (assign)
      (p->kind == TyKind::RRef ? moveAssign : copyAssign) = f;
  }
  if (level == 0 && (copyAssign || (moveAssign && move)))
    return {}; // (an operator= of its own, the walk's choice or none: not the implicit one)
  if (level == 0 && (moveConstructor || moveAssign))
    return std::string("it declares a move ") + (moveConstructor ? "constructor" : "assignment") + ", so its copy assignment is deleted";
  if (level > 0)
  {
    std::string member = "its " + prefix;
    // A move takes its move assignment, where it has one (declared, or implicit: none of its
    // copy constructor, copy assignment, move constructor or destructor declared); else its copy.
    if (move && !moveAssign && (copyConstructor || copyAssign || moveConstructor || destructor))
      move = false;
    FunctionDecl* own = move ? moveAssign : copyAssign;
    if (own && (own->flags & DF_Delete))
      return member + " has a deleted " + (move ? "move" : "copy") + " assignment";
    if (own && !(own->flags & DF_Default))
      return {}; // (its own, which assigns as it is written)
    if (!own && !move && (moveConstructor || moveAssign))
      return member + " declares a move " + (moveConstructor ? "constructor" : "assignment") + ", so its copy assignment is deleted";
  }
  // The implicit (or defaulted) one assigns the bases and members: each must be assignable.
  std::vector<const Ty*> bases;
  if (impl.directBases(cls, def, bases))
    for (const Ty* b : bases)
    {
      std::string name = types.toString(types.unqualified(types.stripRef(b)), &impl.symbols);
      if (std::string why = deletedAssignment(b, move, level + 1, "base " + name); !why.empty())
        return why;
    }
  for (Decl* m : membersRead(def))
  {
    if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)) || static_cast<VariableDecl*>(m)->name.empty())
      continue;
    const Ty* t = impl.declaredTypeOf(m, sp, 0);
    if (!t || !t->isKnown() || types.isDependent(t))
      continue;
    std::string name = (level > 0 ? prefix + "'s member " : "member ") + std::string(symbols.name(static_cast<VariableDecl*>(m)->name.last().symbol));
    if (t->isReference())
      return "its " + name + " is a reference";
    if (Impl::isConstObject(t))
      return "its " + name + " is const";
    while (t->kind == TyKind::Array && t->inner)
      t = t->inner;
    if (t->kind == TyKind::Class)
      if (std::string why = deletedAssignment(t, move, level + 1, name); !why.empty())
        return why;
  }
  return {};
}

auto GlobalScope::Impl::Walker::isAssignmentOp(P op) -> bool
{
  switch (op)
  {
    case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign: case P::PercentAssign:
    case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign: case P::ShrAssign:
      return true;
    default:
      return false;
  }
}

auto GlobalScope::Impl::Walker::builtinOperand(const Ty* t) -> const Ty*
{
  const Ty* s = types.stripRef(t);
  if (!s || s->kind != TyKind::Class)
    return t;
  return impl.scalarConversionOf(s);
}

auto GlobalScope::Impl::Walker::enumUnaryOperator(P op, bool postfix, const Ty* operand, uint32_t token, FunctionDecl** chosen) -> const Ty*
{
  // Postfix `d++` is `operator++(d, 0)` ([over.oper.incr]): the candidates taking (E&, int).
  bool step = op == P::PlusPlus || op == P::MinusMinus;
  bool twoOperands = step && postfix;
  Subst subst;
  bool isMember;
  FunctionDecl* f = operatorFunction(op, operand, twoOperands ? types.intType() : nullptr, token, twoOperands ? 2 : 1, subst, isMember);
  if (!f || isMember || !f->templateParams.empty())
    return nullptr;
  // Only one that takes the enumeration as it is (not through a conversion to some class).
  const Ty* ft = impl.declaredTypeOf(f, nullptr, 0);
  if (!ft || ft->kind != TyKind::Function || ft->args.size() != (twoOperands ? 2u : 1u) ||
      types.unqualified(types.stripRef(ft->args[0])) != types.unqualified(types.stripRef(operand)))
    return nullptr;
  if (chosen)
    *chosen = f;
  Span<Expr*> none;
  return returnTypeOfCall(f, nullptr, none, nullptr);
}

auto GlobalScope::Impl::Walker::enumOperator(P op, const Ty* lhs, const Ty* rhs, uint32_t token, FunctionDecl** chosen) -> const Ty*
{
  Subst subst;
  bool isMember;
  FunctionDecl* f = operatorFunction(op, lhs, rhs, token, 2, subst, isMember);
  if (!f || isMember || !impl.takesEnumOperands(f, lhs, rhs))
    return nullptr;
  if (chosen)
    *chosen = f;
  Span<Expr*> none;
  return returnTypeOfCall(f, subst.empty() ? nullptr : &subst, none, nullptr);
}

auto GlobalScope::Impl::Walker::unparenthesized(Expr* e) -> Expr*
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  return e;
}

auto GlobalScope::Impl::Walker::checkTextOperators(BinaryExpr* b, P op, const Ty* l, const Ty* r) -> void
{
  if (probing || inGenericTemplate() || !file)
    return;
  auto isLiteral = [](Expr* e)
  {
    e = unparenthesized(e);
    return e && e->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(e)->literalKind == TokenKind::String;
  };
  auto isInteger = [&](const Ty* t)
  {
    t = t ? types.unqualified(types.stripRef(t)) : nullptr;
    return t && (t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Bool || t->kind == TyKind::Enum);
  };
  auto isPointer = [&](const Ty* t)
  {
    t = t ? types.unqualified(types.stripRef(t)) : nullptr;
    return t && (t->kind == TyKind::Pointer || t->kind == TyKind::Array);
  };
  if ((op == P::Plus || op == P::Minus) && ((isLiteral(b->lhs) && isPointer(r)) || (isLiteral(b->rhs) && isPointer(l))))
  {
    impl.reportCheckerError(file, b->token, std::string("a string literal ") + (op == P::Plus ? "+" : "-") +
                                              " a pointer is no text: write StringView(\"...\") + x for text (design.md 14)");
    return;
  }
  if ((op == P::Plus || op == P::Minus) && ((isLiteral(b->lhs) && isInteger(r)) || (isLiteral(b->rhs) && isInteger(l))))
    impl.reportCheckerError(file, b->token, std::string("a string literal ") + (op == P::Plus ? "+" : "-") +
                                              " an integer is pointer arithmetic, not text: write &\"...\"[n] for a pointer into it (design.md 14)");
  if (op != P::Amp && op != P::Pipe && op != P::Caret)
    return;
  Decl* string = impl.libraryDecl("String", NodeKind::ClassDecl);
  auto intoString = [&](Expr* e)
  {
    // (written without parentheses: a ParenExpr around it is the program's own grouping)
    if (!e || e->kind != NodeKind::BinaryExpr || static_cast<BinaryExpr*>(e)->isKeyword || P(static_cast<BinaryExpr*>(e)->op) != P::Shl)
      return false;
    while (e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->isKeyword && P(static_cast<BinaryExpr*>(e)->op) == P::Shl)
      e = static_cast<BinaryExpr*>(e)->lhs;
    const Ty* t = e && e->ty ? types.unqualified(types.stripRef(e->ty)) : nullptr;
    return string && t && t->kind == TyKind::Class && t->decl == string;
  };
  if (intoString(b->lhs) || intoString(b->rhs))
  {
    const char* o = op == P::Amp ? "&" : op == P::Pipe ? "|" : "^";
    impl.reportCheckerError(file, b->token, std::string("a << chain into a String mixed with ") + o + " needs parentheses: `out << (a " + o +
                                              " b)` (design.md 14)");
  }
}

auto GlobalScope::Impl::Walker::typeOfBinary(BinaryExpr* b) -> const Ty*
{
  OperatorCall call = OperatorCall::Undecided;
  FunctionDecl* callee = nullptr;
  bool member = false;
  const Ty* t = typeOfBinaryOperator(b, call, callee, member);
  recordOperatorCall(b, t, b->lhs, b->rhs, call, callee, member);
  return t;
}

auto GlobalScope::Impl::Walker::typeOfBinaryOperator(BinaryExpr* b, OperatorCall& call, FunctionDecl*& callee, bool& member) -> const Ty*
{
  const Ty* l = walkExpr(b->lhs);
  const Ty* r = walkExpr(b->rhs);
  if (!b->isKeyword && (P(b->op) == P::Assign || P(b->op) == P::EqEq || P(b->op) == P::NotEq))
  {
    noteZeroNull(b->rhs, l); // `p = 0`, `p == 0`
    if (P(b->op) != P::Assign)
      noteZeroNull(b->lhs, r); // `0 != p`
  }
  if (!b->isKeyword && P(b->op) == P::Assign && l && !probesSharedNodes())
    if (const Ty* target = types.unqualified(types.stripRef(l));
        (target->kind == TyKind::Pointer || target->kind == TyKind::MemberPointer) && target->inner && target->inner->kind == TyKind::Function)
    {
      // `h = &twice;` beside a twice(int), `make = construct<Add>;`: the function of the pointer's type, as for an
      // initializer (bindTemplateAddresses).
      bindTemplateAddresses(b->rhs, target);
      r = b->rhs->ty ? b->rhs->ty : r;
    }
  if (b->isKeyword)
  {
    switch (Kw(b->op))
    {
      case Kw::and_: case Kw::or_: case Kw::not_eq_: return types.boolType();
      default: return arithmetic(operandType(b->lhs, l), operandType(b->rhs, r));
    }
  }
  P op = P(b->op);
  if (op == P::Comma)
  {
    // `(a, b)` with an operator, that takes a's and b's classes (C++ lets a class overload it):
    // its call; else the built-in comma, b. Until 2026-11-06 always b, the operator ignored.
    const Ty* ls = types.stripRef(l), *rs = types.stripRef(r);
    if (ls && rs && (ls->kind == TyKind::Class || rs->kind == TyKind::Class) && ls->isKnown() && rs->isKnown() && !types.isDependent(ls) &&
        !types.isDependent(rs))
      if (const Ty* t = overloadedOperator(op, l, argumentType(b->rhs, r, types), b->token, 2, false, argumentType(b->lhs, l, types));
          t && operatorCall == OperatorCall::Function && operatorChosen)
      {
        call = operatorCall;
        callee = operatorChosen;
        member = operatorIsMember;
        return t;
      }
    return r;
  }
  checkTextOperators(b, op, l, r);
  const Ty* ls = types.stripRef(l);
  const Ty* rs = types.stripRef(r);
  if (!ls || !rs)
    return types.unknown();
  // (`obj->*k` with obj of a class that overloads operator->*: its call, below)
  if ((op == P::DotStar || op == P::ArrowStar) && (rs->kind == TyKind::MemberPointer || ls->kind != TyKind::Class))
    return rs->kind == TyKind::MemberPointer ? types.lref(rs->inner) : types.unknown();
  if ((op == P::EqEq || op == P::NotEq) &&
      ((typedUnionOf(ls) && rs->kind == TyKind::Enum) || (typedUnionOf(rs) && ls->kind == TyKind::Enum)))
    return types.boolType(); // `p == Property::Integer`: the typed union's tag (design.md 8.5)
  if (ls->kind == TyKind::Class || rs->kind == TyKind::Class)
  {
    if (!ls->isKnown() || !rs->isKnown())
      return types.unknown();
    if (types.isDependent(ls) || types.isDependent(rs))
      return types.param(ls->kind == TyKind::Param ? ls->decl : rs->decl);
    if (op == P::Assign)
      checkClosureTo(b->rhs, l, b->token); // `later = [=] { ... text ... }`
    if (const Ty* t = overloadedOperator(op, l, argumentType(b->rhs, r, types), b->token, 2, false, argumentType(b->lhs, l, types)))
    {
      // `a = b` by the class's implicit assignment (no operator= of its own took b): one C++ deletes
      // is an error. A copy from an lvalue of the class (or of one derived from it), a move from
      // anything else. Not a typed union's alternative (`target[a] = a.type(input)`), which the
      // assignment builds in place (design.md 8.5).
      if (op == P::Assign && operatorCall == OperatorCall::Builtin && ls->kind == TyKind::Class && !probing)
      {
        Expr* target = b->lhs;
        while (target && target->kind == NodeKind::ParenExpr)
          target = static_cast<ParenExpr*>(target)->inner;
        bool alternative = target && target->kind == NodeKind::IndexExpr && static_cast<IndexExpr*>(target)->base &&
                           typedUnionOf(types.stripRef(static_cast<IndexExpr*>(target)->base->ty));
        const Ty* given = argumentType(b->rhs, r, types);
        const Ty* object = given ? types.unqualified(types.stripRef(given)) : nullptr;
        bool copy = given && given->kind == TyKind::LRef && object &&
                    (object == types.unqualified(ls) || (object->kind == TyKind::Class && impl.isBaseClass(types.unqualified(ls), object)));
        if (!alternative && given && given->isKnown())
          if (std::string why = deletedAssignment(ls, !copy, 0, {}); !why.empty())
            reportCheck(b->token, types.toString(types.unqualified(ls), &impl.symbols) +
                                      " is assigned by its implicit assignment, which C++ defines as deleted: " + why + " ([class.copy.assign] 2, 7)");
      }
      call = operatorCall;
      callee = operatorChosen;
      member = operatorIsMember;
      if (FunctionDecl* f = operatorChosen)
      {
        // `out << point` through `operator<<(String&, Text)`: the right operand packed (packText).
        Expr* operands[2] = {b->lhs, b->rhs};
        Span<Expr*> args{operatorIsMember ? operands + 1 : operands, operatorIsMember ? 1u : 2u};
        packText(args, f, b->token);
        if (args.size() > 0 && args[args.size() - 1] != b->rhs)
          b->rhs = args[args.size() - 1];
      }
      return t;
    }
    // Two typed unions of one type with no operator of their own compare as std::variant's
    // (design.md 8.5): by the alternative held, then by its values.
    if (typedUnionOf(ls) && types.unqualified(ls) == types.unqualified(rs))
      switch (op)
      {
        case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
          return types.boolType();
        case P::Spaceship:
          return impl.orderingType();
        default:
          break;
      }
    // No operator function: the built-in operator on the operands' scalar conversions (an
    // assignment's left operand is the object itself and converts to nothing).
    const Ty* lc = isAssignmentOp(op) ? (ls->kind == TyKind::Class ? nullptr : l) : builtinOperand(l);
    const Ty* rc = builtinOperand(r);
    if (!lc || !rc)
      return unknown(Why::Operator);
    l = lc;
    r = rc;
    ls = types.stripRef(l);
    rs = types.stripRef(r);
  }
  if (!ls->isKnown() || !rs->isKnown())
  {
    // Comparisons and logic have a known result regardless of the operands.
    switch (op)
    {
      case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
      case P::AndAnd: case P::OrOr:
        return types.boolType();
      default:
        if (op == P::Assign || op == P::PlusAssign || op == P::MinusAssign || op == P::StarAssign ||
            op == P::SlashAssign || op == P::PercentAssign || op == P::AmpAssign || op == P::PipeAssign ||
            op == P::CaretAssign || op == P::ShlAssign || op == P::ShrAssign)
          return ls->isKnown() ? l : types.unknown();
        return types.unknown();
    }
  }
  if (ls->kind == TyKind::Param || rs->kind == TyKind::Param)
    return ls->kind == TyKind::Param ? ls : rs;
  if ((ls->kind == TyKind::Enum || rs->kind == TyKind::Enum) && op != P::Assign)
    if (const Ty* user = enumOperator(op, argumentType(b->lhs, l, types), argumentType(b->rhs, r, types), b->token, &callee))
    {
      call = OperatorCall::Function;
      member = false;
      return user;
    }
  switch (op)
  {
    case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign:
    case P::PercentAssign: case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign:
    case P::ShrAssign:
      checkConstModification(b->token, ls, "assignment to", b->lhs);
      checkAssignable(b->token, "the left operand of an assignment", b->lhs, l); // `f() = 5`, `x++ = 5`
      return l;
    case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
    case P::AndAnd: case P::OrOr:
      return types.boolType();
    case P::Spaceship:
      return impl.orderingType();
    case P::Shl: case P::Shr:
      return promote(operandType(b->lhs, l));
    case P::Plus: case P::Minus:
    {
      bool lp = ls->kind == TyKind::Pointer || ls->kind == TyKind::Array;
      bool rp = rs->kind == TyKind::Pointer || rs->kind == TyKind::Array;
      if (lp && rp)
        return types.ptrdiffType();
      if (lp)
        return types.decay(ls);
      if (rp)
        return types.decay(rs);
      return arithmetic(operandType(b->lhs, l), operandType(b->rhs, r));
    }
    case P::Star: case P::Slash: case P::Percent: case P::Amp: case P::Pipe: case P::Caret:
      return arithmetic(operandType(b->lhs, l), operandType(b->rhs, r));
    case P::DotStar: case P::ArrowStar:
      return rs->kind == TyKind::MemberPointer ? types.lref(rs->inner) : types.unknown();
    default:
      return unknown(Why::Unsupported);
  }
}

auto GlobalScope::Impl::Walker::typeOfExpansion(UnaryExpr* u) -> const Ty*
{
  Decl* pd = activeSubst ? packParamIn(u->operand) : nullptr;
  const Ty* pt = pd ? impl.declaredTypeOf(pd, activeSubst, 0) : nullptr;
  if (!pt || pt->kind != TyKind::Pack)
    return nullptr;
  // An expansion of the packs whose one element the code generator emits (elementPacks): its
  // pattern typed once, for that element, as typeOfUnary types an expansion of no pack.
  if (elementPacks && !elementPacks->empty() && pd->kind == NodeKind::ParamDecl)
  {
    std::vector<Decl*> packs;
    impl.packsNamedIn(static_cast<ParamDecl*>(pd)->type, *activeSubst, packs);
    for (Decl* p : packs)
      if (std::find(elementPacks->begin(), elementPacks->end(), p) != elementPacks->end())
        return nullptr;
  }
  size_t n = pt->args.size();
  if (n == 0)
  {
    // No element: C++ instantiates no pattern, so nothing is typed or counted; its names are
    // still resolved, on a scratch walk, for the code generator, which finds the pack
    // through them.
    ResolveStats scratch;
    Walker probe(impl, scratch, tokens, text);
    probe.file = file;
    FileOfLookups walkedFile(file);
    probe.scope = scope;
    probe.locals = locals;
    probe.activeSubst = activeSubst;
    probe.thisType = thisType;
    probe.walkExpr(u->operand);
    return types.pack({});
  }
  const Subst* outer = activeSubst;
  std::vector<const Ty*> elements;
  for (size_t i = 0; i < n; ++i)
  {
    Subst one;
    for (const Subst::Pair& p : outer->pairs)
    {
      bool pack = p.param && p.param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(p.param)->isPack;
      one.pairs.push_back({p.param, pack && p.type && p.type->kind == TyKind::Pack && p.type->args.size() == n ? p.type->args[i] : p.type});
    }
    activeSubst = &one;
    const Ty* e = walkExpr(u->operand);
    activeSubst = outer;
    elements.push_back(e ? e : types.unknown());
  }
  return types.pack(Span<const Ty*>{elements.data(), uint32_t(elements.size())});
}

auto GlobalScope::Impl::Walker::typeOfUnary(UnaryExpr* u) -> const Ty*
{
  OperatorCall call = OperatorCall::Undecided;
  FunctionDecl* callee = nullptr;
  bool member = false;
  const Ty* t = typeOfUnaryOperator(u, call, callee, member);
  recordOperatorCall(u, t, u->operand, nullptr, call, callee, member);
  return t;
}

auto GlobalScope::Impl::Walker::typeOfUnaryOperator(UnaryExpr* u, OperatorCall& call, FunctionDecl*& callee, bool& member) -> const Ty*
{
  if (!u->isKeyword && P(u->op) == P::Ellipsis && u->postfix)
    if (const Ty* pack = typeOfExpansion(u))
      return pack;
  const Ty* t = walkExpr(u->operand);
  const Ty* s = types.stripRef(t);
  if (u->isKeyword)
    return Kw(u->op) == Kw::not_ ? types.boolType() : promote(operandType(u->operand, t));
  P op = P(u->op);
  if (op == P::Amp && !u->postfix && t && impl.isText(t) && !inGenericTemplate())
    reportTextError(u->token, textStored("pointed to"));
  if (op == P::Ellipsis)
    return t;
  if (!s || !s->isKnown())
    return op == P::Bang ? types.boolType() : types.unknown();
  if (op == P::Amp && u->operand->kind == NodeKind::NameExpr)
  {
    // `&Style::field`: a pointer to a member of the class, not to the field of some object.
    auto* n = static_cast<NameExpr*>(u->operand);
    Decl* d = n->resolved;
    if (n->name.parts.size() > 1 && d && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) &&
        !(d->flags & DF_Static) && d->parent && d->parent->kind == NodeKind::ClassDecl)
      return types.memberPointer(s, impl.selfType(d->parent));
  }
  if (s->kind == TyKind::Param)
    return op == P::Bang ? types.boolType() : op == P::Amp ? types.pointer(s) : s;
  if (s->kind == TyKind::Enum && op != P::Amp && op != P::Star)
    if (const Ty* r = enumUnaryOperator(op, u->postfix, argumentType(u->operand, t, types), u->token, &callee))
    {
      call = OperatorCall::Function;
      member = false;
      return r;
    }
  if (s->kind == TyKind::Class)
  {
    // `&x` of a class with its own unary operator& is that operator's result (CComPtr<T>'s
    // gives T**), the built-in address only where the class has none.
    if (op == P::Amp)
    {
      if (!types.isDependent(s))
        if (const Ty* r = overloadedOperator(op, t, nullptr, u->token, 1, false, argumentType(u->operand, t, types)))
        {
          call = operatorCall;
          callee = operatorChosen;
          member = operatorIsMember;
          return r;
        }
      return types.pointer(s);
    }
    if (types.isDependent(s))
      return types.unknown();
    if (u->postfix && (op == P::PlusPlus || op == P::MinusMinus))
    {
      // `c++`: the class's postfix operator, `operator++(int)` (a member's, or a free one taking the
      // object and an int), never the prefix `operator++()`, which `c++` called (MSVC's extension:
      // C++ rejects it, [over.inc]).
      if (const Ty* r = overloadedOperator(op, t, types.intType(), u->token, 2, false, argumentType(u->operand, t, types)))
      {
        call = operatorCall;
        callee = operatorChosen;
        member = operatorIsMember;
        return r;
      }
      if (const Ty* prefix = overloadedOperator(op, t, nullptr, u->token, 1, false, argumentType(u->operand, t, types)))
      {
        Expr* operand = u->operand;
        while (operand && operand->kind == NodeKind::ParenExpr)
          operand = static_cast<ParenExpr*>(operand)->inner;
        std::string written = operand && operand->kind == NodeKind::NameExpr ? std::string(symbols.name(static_cast<NameExpr*>(operand)->name.last().symbol)) : "an object";
        reportHere(u->token, written + std::string(punctuatorSpelling(op)) + " of " + types.toString(types.unqualified(s), &symbols) +
                                 ", which declares no postfix operator" + std::string(punctuatorSpelling(op)) +
                                 "(int): C++ calls that one for a postfix " + std::string(punctuatorSpelling(op)) + ", never the prefix one ([over.inc])");
        operatorCall = OperatorCall::Undecided;
        return prefix;
      }
    }
    else if (const Ty* r = overloadedOperator(op, t, nullptr, u->token, 1, false, argumentType(u->operand, t, types)))
    {
      call = operatorCall;
      callee = operatorChosen;
      member = operatorIsMember;
      return r;
    }
    // No operator function: the built-in one on the operand's scalar conversion (`-speed`
    // with a Const<double>); ++ and -- change the object itself.
    const Ty* c = op == P::PlusPlus || op == P::MinusMinus ? nullptr : builtinOperand(t);
    if (!c)
      return unknown(Why::Operator);
    t = c;
    s = types.stripRef(c);
  }
  switch (op)
  {
    case P::Star:
      if (s->kind == TyKind::Pointer || s->kind == TyKind::Array)
        return types.lref(s->inner);
      return unknown(Why::Operator);
    case P::Amp: return types.pointer(s);
    case P::Bang: return types.boolType();
    case P::Minus: case P::Plus: case P::Tilde:
      // `+closure`: the pointer to its function, noexcept as its call operator is.
      if (op == P::Plus && s->kind == TyKind::Lambda && !Impl::isGenericLambda(s) && s->inner)
      {
        ClosureCall* call = impl.closureCall(types.unqualified(s));
        bool isNoexcept = call && impl.isNoexcept(&call->decl, activeSubst) == std::optional<bool>(true);
        return types.pointer(types.function(s->inner, s->args, false, false, isNoexcept));
      }
      return promote(operandType(u->operand, t));
    case P::PlusPlus: case P::MinusMinus:
      checkConstModification(u->token, s, op == P::PlusPlus ? "increment of" : "decrement of", u->operand);
      // `f()++`, `++(x + 1)`. Not of a pointer: `++v.begin()`, `--this->groups.end()` increment a
      // temporary iterator, which C++'s are classes taking it (vector.md: kpp's are pointers).
      if (s->kind != TyKind::Pointer)
        checkAssignable(u->token, op == P::PlusPlus ? "the operand of ++" : "the operand of --", u->operand, t);
      return u->postfix ? types.unqualified(s) : t;
    default: return unknown(Why::Unsupported);
  }
}

auto GlobalScope::Impl::Walker::typeOfLiteral(LiteralExpr* l) -> const Ty*
{
  switch (l->literalKind)
  {
    case TokenKind::Keyword:
      return l->keyword == Kw::nullptr_ ? types.nullptrType() : types.boolType();
    case TokenKind::Char:
    {
      std::string_view s = spelling(l);
      if (s.starts_with("u8")) return types.charType(8, true);
      if (s.starts_with("u")) return types.charType(16, true);
      if (s.starts_with("U")) return types.charType(32, true);
      if (s.starts_with("L")) return types.wcharType();
      return multiCharacterLiteral(s) ? types.intType(32) : types.charType(8, false);
    }
    case TokenKind::String:
    {
      std::string_view s = spelling(l);
      const Ty* ch = types.charType(8, false);
      if (s.starts_with("u8")) ch = types.charType(8, true);
      else if (s.starts_with("u")) ch = types.charType(16, true);
      else if (s.starts_with("U")) ch = types.charType(32, true);
      else if (s.starts_with("L")) ch = types.wcharType();
      return types.pointer(types.qualified(ch, true, false));
    }
    case TokenKind::Number:
    {
      // The type the compile-time evaluator gives the literal (consteval.cpp: C++'s rules for
      // suffixes and the first type that holds the value), so that the two agree.
      ConstArith arith{types};
      if (std::optional<ConstValue> v = arith.literal(TokenKind::Number, spelling(l)); v && v->type)
        return v->type;
      return types.unknown();
    }
    default:
      return types.unknown();
  }
}

auto GlobalScope::Impl::Walker::freeRangeFunction(const Ty* range, uint32_t symbol) -> FunctionDecl*
{
  // The range as the argument, its const kept: a const range's `begin(const Bag&)`.
  const Ty* t = types.unqualified(types.stripRef(range));
  std::vector<const Ty*> argTypes{types.lref(types.stripRef(range))};
  for (Scope* place : {impl.lexicalScopeOf(t->decl), scope})
    for (Scope* s = place; s; s = s->parent)
    {
      if (s->kind == ScopeKind::Class)
        continue;
      Lookup look;
      if (!impl.lookupInScope(s, symbol, look, 0))
        continue;
      if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
      {
        bool ambiguous = false;
        FunctionDecl* f = pickOverload(look.decl, look.overloads, argTypes, nullptr, nullptr, ambiguous);
        return ambiguous ? nullptr : f; // (an ambiguous call recorded as none: codegen decides, or fails, as before)
      }
      break;
    }
  return nullptr;
}

auto GlobalScope::Impl::Walker::recordRangeFunctions(RangeForStmt* f, const Ty* range) -> void
{
  if (probesSharedNodes())
    return;
  f->begin = f->end = nullptr;
  f->rangeMembers = false;
  const Ty* t = types.unqualified(types.stripRef(range));
  if (!t || !t->isKnown() || t->kind != TyKind::Class || types.isDependent(t))
    return;
  // C++ [stmt.ranged]: the class's members when it has a member named begin or end, else the free
  // functions found for the range.
  MemberResult begin = memberOf(range, impl.symBegin, nullptr, 0, false);
  MemberResult end = memberOf(range, impl.symEnd, nullptr, 0, false);
  if (begin.decl || end.decl)
  {
    if (!begin.decl || !end.decl || begin.decl->kind != NodeKind::FunctionDecl || end.decl->kind != NodeKind::FunctionDecl)
      return;
    bool ambiguousBegin = false, ambiguousEnd = false;
    f->begin = pickOverload(begin.decl, begin.overloads, {}, &begin.subst, range, ambiguousBegin);
    f->end = pickOverload(end.decl, end.overloads, {}, &end.subst, range, ambiguousEnd);
    if (ambiguousBegin || ambiguousEnd)
      f->begin = f->end = nullptr; // (recorded as none: codegen decides, or fails, as before)
    f->rangeMembers = true;
  }
  else
  {
    f->begin = freeRangeFunction(range, impl.symBegin);
    f->end = freeRangeFunction(range, impl.symEnd);
  }
  if (!f->begin || !f->end)
    f->begin = f->end = nullptr;
}

auto GlobalScope::Impl::Walker::freeBeginType(const Ty* t, Expr* rangeExpr) -> const Ty*
{
  std::vector<const Ty*> argTypes{types.lref(t)};
  for (Scope* place : {impl.lexicalScopeOf(t->decl), scope})
    for (Scope* s = place; s; s = s->parent)
    {
      if (s->kind == ScopeKind::Class)
        continue;
      Lookup look;
      if (!impl.lookupInScope(s, impl.symBegin, look, 0))
        continue;
      if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
      {
        bool ambiguous;
        if (FunctionDecl* f = pickOverload(look.decl, look.overloads, argTypes, nullptr, nullptr, ambiguous))
          return returnTypeOfCall(f, nullptr, Span<Expr*>{&rangeExpr, 1}, nullptr);
      }
      break;
    }
  return nullptr;
}

auto GlobalScope::Impl::Walker::unboundedArray(Node* e) -> VariableDecl*
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  Decl* d = !e ? nullptr
          : e->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(e)->resolved
          : e->kind == NodeKind::NameExpr   ? static_cast<NameExpr*>(e)->resolved
          : e->kind == NodeKind::NamedType  ? static_cast<NamedType*>(e)->resolved
                                            : nullptr;
  auto* v = d && d->kind == NodeKind::VariableDecl && !(d->flags & DF_Typedef) ? static_cast<VariableDecl*>(d) : nullptr;
  bool member = v && !(v->flags & DF_Static) && impl.owningClassOf(v);
  return member && v->type && v->type->kind == NodeKind::ArrayType && !static_cast<ArrayType*>(v->type)->size ? v : nullptr;
}

auto GlobalScope::Impl::Walker::constObjectPlaced(Expr* place) -> Decl*
{
  auto bare = [](Expr* e)
  {
    while (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    return e;
  };
  Expr* e = bare(place);
  bool addressTaken = false;
  if (e && e->kind == NodeKind::UnaryExpr)
  {
    auto* u = static_cast<UnaryExpr*>(e);
    if (u->isKeyword || u->postfix || u->op != uint32_t(P::Amp))
      return nullptr;
    e = bare(u->operand);
    addressTaken = true;
  }
  auto declaredArray = [&](Decl* d)
  {
    const Ty* t = d ? impl.declaredTypeOf(d, activeSubst, 0) : nullptr;
    return t && t->kind == TyKind::Array;
  };
  for (;;)
  {
    if (e && e->kind == NodeKind::MemberExpr && static_cast<MemberExpr*>(e)->op == P::Dot && addressTaken)
      e = bare(static_cast<MemberExpr*>(e)->base);
    else if (e && e->kind == NodeKind::IndexExpr && addressTaken)
    {
      Expr* base = bare(static_cast<IndexExpr*>(e)->base);
      if (!base || base->kind != NodeKind::NameExpr || !declaredArray(static_cast<NameExpr*>(base)->resolved))
        return nullptr; // (a pointer's element: not visibly the object)
      e = base;
    }
    else
      break;
  }
  if (!e || e->kind != NodeKind::NameExpr)
    return nullptr;
  Decl* d = static_cast<NameExpr*>(e)->resolved;
  if (!d || (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::ParamDecl) || (d->flags & DF_Typedef))
    return nullptr;
  if (!addressTaken && !declaredArray(d))
    return nullptr; // (a pointer variable's value is the place, not its own object)
  const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
  if (!t || t->kind == TyKind::LRef || t->kind == TyKind::RRef)
    return nullptr; // (a reference names an object declared elsewhere, maybe not const)
  while (t && t->kind == TyKind::Array)
    t = t->inner;
  return t && t->isConst ? d : nullptr;
}

auto GlobalScope::Impl::Walker::functionThrows(FunctionDecl* f, const Subst* subst, std::string& unsupported) -> bool
{
  std::optional<bool> isNoexcept = impl.isNoexcept(f, subst);
  if (!isNoexcept)
    unsupported = "a call of a function whose noexcept condition is no constant here";
  return isNoexcept && !*isNoexcept;
}

auto GlobalScope::Impl::Walker::destroyingThrows(const Ty* t, std::string& unsupported) -> bool
{
  std::optional<bool> mayThrow = impl.destructorMayThrow(t);
  if (!mayThrow)
    unsupported = "the destruction of a class holding a closure";
  return mayThrow.value_or(false);
}

auto GlobalScope::Impl::Walker::defaultMembersThrow(const Ty* t, size_t given, std::string& unsupported, int depth) -> bool
{
  // [except.spec] 7: the constructors of the parts it builds, and the default member initializers, from
  // the given-th part on (an aggregate's first ones are the list's).
  ClassDecl* def = impl.classDefinition(t);
  std::vector<const Ty*> bases;
  if (!def || !impl.directBases(t, def, bases))
  {
    unsupported = "a construction of a class not known here";
    return false;
  }
  bool any = false;
  size_t part = 0;
  for (const Ty* b : bases)
    if (part++ >= given)
      any = constructingThrows(b, {}, unsupported, depth + 1) || any;
  Subst subst = impl.substFor(t);
  for (Decl* m : membersRead(def))
  {
    if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef | DF_Friend)) || part++ < given)
      continue;
    auto* v = static_cast<VariableDecl*>(m);
    if (v->init)
      any = canThrow(v->init, unsupported) || any;
    else
      any = constructingThrows(impl.declaredTypeOf(v, subst.pairs.empty() ? nullptr : &subst, 0), {}, unsupported, depth + 1) || any;
  }
  return any;
}

auto GlobalScope::Impl::Walker::constructingThrows(const Ty* t, const std::vector<const Ty*>& argTypes, std::string& unsupported, int depth) -> bool
{
  const Ty* u = t ? types.unqualified(types.stripRef(t)) : nullptr;
  while (u && u->kind == TyKind::Array)
    u = types.unqualified(u->inner);
  if (!u || !u->isKnown() || types.isDependent(u) || depth > 32)
  {
    unsupported = "a construction of a type not known here";
    return false;
  }
  if (u->kind != TyKind::Class)
    return false;
  // A copy or a move, of the class or of one derived from it.
  if (argTypes.size() == 1 && argTypes[0])
    if (const Ty* a = types.unqualified(types.stripRef(argTypes[0])); a == u || (a->kind == TyKind::Class && impl.isBaseClass(u, a)))
    {
      std::optional<bool> nothrow = impl.constructibleFromSelf(u, argTypes[0]->kind != TyKind::LRef && !argTypes[0]->isConst, TraitForm::Nothrow);
      if (!nothrow)
        unsupported = "a copy of a class that has none here";
      return !nothrow.value_or(true);
    }
  bool ambiguous = false;
  FunctionDecl* f = impl.chooseConstructor(u, argTypes, nullptr, nullptr, false, &ambiguous);
  if (ambiguous)
  {
    unsupported = "a construction of a class by constructors equally good";
    return false;
  }
  Subst subst = impl.substFor(u);
  if (f && !f->templateParams.empty())
    impl.deduceCall(f, impl.declaredTypeOf(f, &subst, 0), argTypes, subst);
  if (f && (!(f->flags & DF_Default) || (f->flags & DF_Noexcept)))
    return functionThrows(f, subst.pairs.empty() ? nullptr : &subst, unsupported);
  // No constructor written: an implicit one, or an aggregate's elements each to its part in order.
  bool any = false;
  if (!f && !argTypes.empty())
  {
    ClassDecl* def = impl.classDefinition(u);
    std::vector<const Ty*> parts;
    if (!def || !impl.directBases(u, def, parts) || !impl.fieldTypes(u, def, parts) || argTypes.size() > parts.size())
    {
      unsupported = "a construction of a class not known here";
      return false;
    }
    for (size_t i = 0; i < argTypes.size(); ++i)
      any = constructingThrows(parts[i], {argTypes[i]}, unsupported, depth + 1) || any;
  }
  return defaultMembersThrow(u, argTypes.size(), unsupported, depth) || any;
}

auto GlobalScope::Impl::Walker::convertingThrows(Expr* from, const Ty* to, bool explicitCast, std::string& unsupported) -> bool
{
  const Ty* a = from && from->ty ? types.unqualified(types.stripRef(from->ty)) : nullptr;
  const Ty* t = to ? types.unqualified(types.stripRef(to)) : nullptr;
  if (!a || !t || !a->isKnown() || !t->isKnown() || types.isDependent(a) || types.isDependent(t))
  {
    unsupported = "an argument of a type not known here";
    return false;
  }
  if (a->kind != TyKind::Class && t->kind != TyKind::Class)
    return false;
  bool byReference = to->isReference();
  // A prvalue of the class is built where the parameter is; a reference binds what it can.
  bool same = a == t || (a->kind == TyKind::Class && t->kind == TyKind::Class && impl.isBaseClass(t, a));
  if (same && byReference)
    return false;
  if (t->kind == TyKind::Class)
  {
    bool built = same && a == t && !from->ty->isReference() && !isLvalueExpression(from);
    bool constructs = !built && constructingThrows(t, {argumentType(from, from->ty, types)}, unsupported);
    return constructs || destroyingThrows(t, unsupported);
  }
  const Ty* foundIn = nullptr;
  Subst opArgs;
  bool ambiguous = false;
  FunctionDecl* op = impl.chooseConversion(a, t, GlobalScope::ConversionKind::ToType, explicitCast, &foundIn, &opArgs, explicitCast, &ambiguous);
  if (!op || ambiguous)
  {
    unsupported = "a conversion of a class to a scalar";
    return false;
  }
  return functionThrows(op, opArgs.pairs.empty() ? nullptr : &opArgs, unsupported);
}

auto GlobalScope::Impl::Walker::callArgumentsThrow(FunctionDecl* f, const Ty* signature, const Subst* subst, Span<Expr*> args, std::string& unsupported) -> bool
{
  bool any = false;
  for (size_t i = 0; i < args.size(); ++i)
  {
    Expr* a = args[i];
    if (a && a->kind == NodeKind::BraceInitExpr)
    {
      unsupported = "a braced list as an argument";
      return false;
    }
    any = canThrow(a, unsupported) || any;
    const Ty* parameter = signature && i < signature->args.size() ? signature->args[i] : nullptr;
    if (parameter && a && (!parameter->isKnown() || types.isDependent(parameter) || parameter->kind == TyKind::Pack))
      parameter = nullptr; // (a parameter of a template, as the call's instance has it: by the argument's own type)
    if (parameter)
      any = convertingThrows(a, parameter, false, unsupported) || any;
  }
  // The default arguments of those left out ([expr.call] 4 evaluates them where the call is).
  if (f)
    for (size_t i = args.size(); i < f->params.size(); ++i)
      if (auto* p = static_cast<ParamDecl*>(f->params[i]); p->defaultValue)
        any = canThrow(p->defaultValue, unsupported) || any;
  (void)subst;
  return any;
}

auto GlobalScope::Impl::Walker::canThrow(Expr* e, std::string& unsupported) -> bool
{
  if (!e || !unsupported.empty())
    return false;
  auto scalar = [&](const Expr* x)
  {
    const Ty* t = x && x->ty ? types.unqualified(types.stripRef(x->ty)) : nullptr;
    return t && t->isKnown() && t->kind != TyKind::Class;
  };
  // A class by value, what a full-expression destroys at its end.
  auto temporaryThrows = [&](const Expr* x)
  {
    return x && x->ty && x->ty->kind == TyKind::Class && !x->ty->isReference() && !isLvalueExpression(x) && destroyingThrows(x->ty, unsupported);
  };
  // An operator function: its operands to its parameters, and a result by value.
  auto operatorThrows = [&](FunctionDecl* f, bool member, Expr* first, Expr* second)
  {
    Subst subst;
    if (member && first && first->ty)
      subst = impl.substFor(types.unqualified(types.stripRef(first->ty)));
    const Subst* sp = subst.pairs.empty() ? nullptr : &subst;
    const Ty* signature = impl.declaredTypeOf(f, sp, 0);
    size_t next = 0;
    bool any = canThrow(first, unsupported);
    if (second)
      any = canThrow(second, unsupported) || any;
    if (!f->templateParams.empty())
      return any || functionThrows(f, sp, unsupported); // (its parameters are the instance's)
    if (!member && first && signature && next < signature->args.size())
      any = convertingThrows(first, signature->args[next++], false, unsupported) || any;
    if (second && signature && next < signature->args.size())
      any = convertingThrows(second, signature->args[next], false, unsupported) || any;
    return any || functionThrows(f, sp, unsupported);
  };
  switch (e->kind)
  {
    case NodeKind::LiteralExpr: case NodeKind::NameExpr: case NodeKind::ThisExpr: case NodeKind::SizeofExpr:
    case NodeKind::HeldExpr: case NodeKind::DefaultValueExpr:
    case NodeKind::NameofExpr: case NodeKind::NoexceptExpr: case NodeKind::RequiresExpr:
      return false;
    case NodeKind::ParenExpr: return canThrow(static_cast<ParenExpr*>(e)->inner, unsupported);
    case NodeKind::ThrowExpr: return true;
    case NodeKind::DeleteExpr:
    {
      // As the destructor says, and a deallocation function that says otherwise itself ([except.spec] 10).
      auto* d = static_cast<DeleteExpr*>(e);
      bool any = canThrow(d->operand, unsupported);
      const Ty* p = d->operand->ty ? types.unqualified(types.stripRef(d->operand->ty)) : nullptr;
      std::optional<bool> mayThrow = p && p->kind == TyKind::Pointer ? impl.destructorMayThrow(p->inner) : std::nullopt;
      if (!mayThrow)
      {
        unsupported = "a delete of what is no pointer, or of a class holding a closure";
        return false;
      }
      return any || *mayThrow || (d->deallocator && (d->deallocator->flags & DF_Noexcept) && impl.isNoexcept(d->deallocator, activeSubst) != std::optional<bool>(true));
    }
    case NodeKind::NewExpr:
    {
      // The allocation function, unless it is the placement one or a class's that says it does not; the
      // constructor; the arguments of both. (A failing allocation is the allocator's: `new (nothrow)`.)
      auto* n = static_cast<NewExpr*>(e);
      bool any = false;
      for (Expr* a : n->placement)
        any = canThrow(a, unsupported) || any;
      std::vector<const Ty*> argTypes;
      Span<Expr*> args = n->args;
      if (n->braced && args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr)
        args = static_cast<BraceInitExpr*>(args[0])->args;
      for (Expr* a : args)
      {
        any = canThrow(a, unsupported) || any;
        argTypes.push_back(a && a->ty ? argumentType(a, a->ty, types) : nullptr);
      }
      const Ty* made = e->ty && e->ty->kind == TyKind::Pointer ? e->ty->inner : nullptr;
      any = constructingThrows(made, argTypes, unsupported) || any;
      if (n->allocator)
        return functionThrows(n->allocator, activeSubst, unsupported) || any;
      return n->placement.empty() || any;
    }
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<UnaryExpr*>(e);
      if (u->call == OperatorCall::Function && u->callee)
        return operatorThrows(u->callee, u->calleeMember, u->operand, nullptr) || temporaryThrows(e);
      if (scalar(u->operand))
        return canThrow(u->operand, unsupported);
      if (!u->isKeyword && P(u->op) == P::Bang)
        return canThrow(u->operand, unsupported) || convertingThrows(u->operand, types.boolType(), true, unsupported); // `!x`, a condition
      if (!u->isKeyword && P(u->op) == P::Amp)
        return canThrow(u->operand, unsupported); // the built-in `&` of an object
      unsupported = "a built-in operator on a class's conversion";
      return false;
    }
    case NodeKind::BinaryExpr:
    {
      auto* b = static_cast<BinaryExpr*>(e);
      if (b->call == OperatorCall::Function && b->callee)
        return operatorThrows(b->callee, b->calleeMember, b->lhs, b->rhs) || temporaryThrows(e);
      // (`o.*p`, `q->*p`: the built-in one on any object)
      bool memberPointer = !b->isKeyword && (P(b->op) == P::DotStar || P(b->op) == P::ArrowStar);
      if ((scalar(b->lhs) && scalar(b->rhs)) || memberPointer)
      {
        bool l = canThrow(b->lhs, unsupported), r = canThrow(b->rhs, unsupported);
        return l || r;
      }
      if (b->call == OperatorCall::Rewritten)
      {
        unsupported = "a comparison C++ rewrites";
        return false;
      }
      if (!b->isKeyword && P(b->op) == P::Comma)
      {
        bool l = canThrow(b->lhs, unsupported), r = canThrow(b->rhs, unsupported);
        return l || r || temporaryThrows(b->lhs);
      }
      // `a = b` of a class with no operator= written.
      if (!b->isKeyword && P(b->op) == P::Assign && b->lhs->ty && b->rhs->ty && !scalar(b->lhs))
      {
        const Ty* t = types.unqualified(types.stripRef(b->lhs->ty));
        std::optional<bool> nothrow = impl.assignableFromSelf(t, b->rhs->ty->kind != TyKind::LRef && !isLvalueExpression(b->rhs), TraitForm::Nothrow);
        if (!nothrow)
          unsupported = "an assignment of a class that has none here";
        bool l = canThrow(b->lhs, unsupported), r = canThrow(b->rhs, unsupported);
        return l || r || !nothrow.value_or(true);
      }
      unsupported = "a built-in operator on a class's conversion";
      return false;
    }
    case NodeKind::ConditionalExpr:
    {
      auto* c = static_cast<ConditionalExpr*>(e);
      bool any = canThrow(c->cond, unsupported);
      if (c->cond && !scalar(c->cond))
        any = convertingThrows(c->cond, types.boolType(), true, unsupported) || any;
      any = canThrow(c->then, unsupported) || any;
      any = canThrow(c->else_, unsupported) || any;
      return any || temporaryThrows(e);
    }
    case NodeKind::IndexExpr:
    {
      auto* x = static_cast<IndexExpr*>(e);
      bool any = canThrow(x->base, unsupported);
      if (scalar(x->base))
      {
        for (Expr* a : x->args)
          any = canThrow(a, unsupported) || any;
        return any;
      }
      const Ty* t = x->base->ty ? types.unqualified(types.stripRef(x->base->ty)) : nullptr;
      MemberResult r = t ? memberOf(t, impl.opSymbols[uint32_t(P::LBracket)], nullptr, x->token, false) : MemberResult{};
      bool ambiguous = false;
      FunctionDecl* f = r.decl ? pickOverload(r.decl, r.overloads, argTypesOf(x->args), &r.subst, t, ambiguous, nullptr, {}, nullptr) : nullptr;
      if (!f || ambiguous)
      {
        unsupported = "an index of a class";
        return false;
      }
      const Subst* sp = r.subst.pairs.empty() ? nullptr : &r.subst;
      return callArgumentsThrow(f, impl.declaredTypeOf(f, sp, 0), sp, x->args, unsupported) || any || functionThrows(f, sp, unsupported) || temporaryThrows(e);
    }
    case NodeKind::MemberExpr:
    {
      auto* m = static_cast<MemberExpr*>(e);
      if (m->dynamicMember)
        break;
      const Ty* t = m->base && m->base->ty ? types.unqualified(types.stripRef(m->base->ty)) : nullptr;
      if (m->op != P::Dot && t && t->kind == TyKind::Class)
      {
        // `p->x` through the class's operator->.
        MemberResult r = memberOf(t, impl.opSymbols[uint32_t(P::Arrow)], nullptr, m->token, false);
        bool ambiguous = false;
        FunctionDecl* f = r.decl ? pickOverload(r.decl, r.overloads, {}, &r.subst, t, ambiguous, nullptr, {}, nullptr) : nullptr;
        if (!f || ambiguous)
        {
          unsupported = "a member through a class's operator->";
          return false;
        }
        return canThrow(m->base, unsupported) || functionThrows(f, r.subst.pairs.empty() ? nullptr : &r.subst, unsupported);
      }
      return canThrow(m->base, unsupported);
    }
    case NodeKind::CastExpr:
    case NodeKind::NamedCastExpr:
    {
      Type* written = e->kind == NodeKind::CastExpr ? static_cast<CastExpr*>(e)->type : static_cast<NamedCastExpr*>(e)->type;
      Expr* operand = e->kind == NodeKind::CastExpr ? static_cast<CastExpr*>(e)->operand : static_cast<NamedCastExpr*>(e)->operand;
      bool any = canThrow(operand, unsupported);
      const Ty* to = written ? typeOf(written) : e->ty;
      // [except.spec] 6: a dynamic_cast to a reference that needs a runtime check.
      if (e->kind == NodeKind::NamedCastExpr && static_cast<NamedCastExpr*>(e)->castKind == Kw::dynamic_cast_)
      {
        const Ty* from = operand->ty ? types.unqualified(types.stripRef(operand->ty)) : nullptr;
        const Ty* target = to ? types.unqualified(types.stripRef(to)) : nullptr;
        return any || (to && to->isReference() && !(from && target && (from == target || impl.isBaseClass(target, from))));
      }
      if (to && to->isReference())
        return any;
      if (to && operand->ty)
        any = convertingThrows(operand, to, true, unsupported) || any;
      return any;
    }
    case NodeKind::FunctionalCastExpr:
    {
      auto* f = static_cast<FunctionalCastExpr*>(e);
      Span<Expr*> args = f->args;
      if (f->braced && args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr)
        args = static_cast<BraceInitExpr*>(args[0])->args;
      bool any = false;
      std::vector<const Ty*> argTypes;
      for (Expr* a : args)
      {
        any = canThrow(a, unsupported) || any;
        argTypes.push_back(a && a->ty ? argumentType(a, a->ty, types) : nullptr);
      }
      const Ty* t = f->ty ? types.unqualified(types.stripRef(f->ty)) : nullptr;
      if (!t || !t->isKnown())
      {
        unsupported = "a construction of a type not known here";
        return false;
      }
      if (t->kind != TyKind::Class)
        return any || (args.size() == 1 && args[0] && !scalar(args[0]) && convertingThrows(args[0], t, true, unsupported));
      return constructingThrows(t, argTypes, unsupported) || any || destroyingThrows(t, unsupported);
    }
    case NodeKind::TypeidExpr:
    {
      // Of a glvalue of a polymorphic class, which a null pointer makes throw.
      auto* t = static_cast<TypeidExpr*>(e);
      if (t->operandType || !t->operand || uint8_t(t->operand->kind) < uint8_t(NodeKind::NameExpr))
        return false;
      auto* x = static_cast<Expr*>(t->operand);
      const Ty* c = x->ty ? types.unqualified(types.stripRef(x->ty)) : nullptr;
      std::optional<bool> polymorphic = c && c->kind == TyKind::Class ? impl.polymorphicClass(c) : std::optional<bool>(false);
      Expr* bare = x;
      while (bare && bare->kind == NodeKind::ParenExpr)
        bare = static_cast<ParenExpr*>(bare)->inner;
      bool throughPointer = bare && bare->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(bare)->isKeyword && P(static_cast<UnaryExpr*>(bare)->op) == P::Star;
      return polymorphic.value_or(true) && throughPointer;
    }
    case NodeKind::LambdaExpr:
    {
      // What it holds by value is copy-initialized where it is made, a variable's copy from it as an
      // lvalue and an init-capture from its initializer ([expr.prim.lambda.capture] 15); the closure, a
      // temporary, is destroyed at the end of the expression. (Until 2026-11-07 a capture's copy was
      // never asked: `noexcept([big] {})` was true with Big's copy constructor potentially-throwing.)
      auto* l = static_cast<LambdaExpr*>(e);
      bool any = false;
      for (const LambdaCapture& cap : l->captures)
        if (cap.init)
          any = canThrow(cap.init, unsupported) || any;
      std::optional<std::vector<GlobalScope::ClosurePart>> parts = impl.closureValueParts(l->ty);
      std::optional<bool> mayThrow = impl.destructorMayThrow(l->ty);
      if (!parts || !mayThrow)
      {
        unsupported = "a lambda whose captures are not known here";
        return any;
      }
      for (const GlobalScope::ClosurePart& p : *parts)
      {
        const Ty* from = p.init ? (p.init->ty ? argumentType(p.init, p.init->ty, types) : nullptr) : types.lref(p.type);
        if (!from)
        {
          unsupported = "a lambda whose captures are not known here";
          continue;
        }
        if (p.init && !from->isReference() && types.unqualified(from) == types.unqualified(p.type))
          continue; // `[s = make()]`: the prvalue is the capture
        any = constructingThrows(p.type, {from}, unsupported) || any;
      }
      return any || *mayThrow;
    }
    case NodeKind::CallExpr:
    {
      auto* c = static_cast<CallExpr*>(e);
      // `x.~X()`, `p->~X()`: as the destructor says, a scalar's none ([except.spec] 8).
      if (auto* m = c->callee && c->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(c->callee) : nullptr; m && !m->member.empty() && m->member.last().special == 1)
      {
        bool any = canThrow(m->base, unsupported);
        const Ty* object = m->base->ty ? types.unqualified(types.stripRef(m->base->ty)) : nullptr;
        if (object && m->op != P::Dot)
          object = object->kind == TyKind::Pointer ? types.unqualified(object->inner) : nullptr; // (a class's operator->: said below)
        if (object && m->member.parts.size() > 1)
        {
          // `p->Base::~Base()`: that class's.
          NamedType qualifier{};
          qualifier.kind = NodeKind::NamedType;
          qualifier.token = m->token;
          qualifier.name = m->member;
          qualifier.name.parts.count -= 1;
          object = typeOf(&qualifier);
        }
        std::optional<bool> mayThrow = object ? impl.destructorMayThrow(object) : std::nullopt;
        if (!mayThrow)
          unsupported = "a call of a destructor of a class holding a closure";
        return any || mayThrow.value_or(false);
      }
      Decl* callee = c->callee && c->callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(c->callee)->resolved
                     : c->callee && c->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(c->callee)->resolved
                                                                            : nullptr;
      // `T(a, b)`, `T{a}` parsed as a call: a construction, and a destruction at the expression's end.
      if (callee && (callee->kind == NodeKind::ClassDecl || (callee->flags & DF_Constructor) || callee->kind == NodeKind::UsingAliasDecl ||
                     (callee->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(callee)->isType) ||
                     (callee->kind == NodeKind::VariableDecl && (callee->flags & DF_Typedef))))
      {
        const Ty* t = e->ty ? types.unqualified(types.stripRef(e->ty)) : nullptr;
        bool any = false;
        std::vector<const Ty*> argTypes;
        for (Expr* a : c->args)
        {
          any = canThrow(a, unsupported) || any;
          argTypes.push_back(a && a->ty ? argumentType(a, a->ty, types) : nullptr);
        }
        return constructingThrows(t, argTypes, unsupported) || any || (t && t->kind == TyKind::Class && destroyingThrows(t, unsupported));
      }
      // `p()`, `(o.*m)()`, `l()`: noexcept as the pointer's function type, or the closure's call operator, says.
      const Ty* through = c->callee && c->callee->ty ? types.unqualified(types.stripRef(c->callee->ty)) : nullptr;
      if (through && through->kind == TyKind::Pointer)
        through = types.unqualified(through->inner);
      if (callee && callee->kind == NodeKind::FunctionDecl)
        through = nullptr;
      if (through && through->kind == TyKind::Lambda)
      {
        ClosureCall* closure = impl.closureCall(through);
        std::optional<bool> isNoexcept = closure ? impl.isNoexcept(&closure->decl, activeSubst) : std::nullopt;
        through = isNoexcept ? types.function(types.voidType(), {}, false, false, *isNoexcept) : nullptr;
      }
      // `o(1)`, an object's operator().
      if (through && through->kind == TyKind::Class)
      {
        MemberResult r = memberOf(through, impl.opSymbols[uint32_t(P::LParen)], nullptr, c->token, false);
        bool ambiguous = false;
        FunctionDecl* f = r.decl ? pickOverload(r.decl, r.overloads, argTypesOf(c->args), &r.subst, through, ambiguous, nullptr, {}, nullptr) : nullptr;
        if (!f || ambiguous)
        {
          unsupported = "a call of an object";
          return false;
        }
        const Subst* sp = r.subst.pairs.empty() ? nullptr : &r.subst;
        bool any = canThrow(c->callee, unsupported);
        any = callArgumentsThrow(f, impl.declaredTypeOf(f, sp, 0), sp, c->args, unsupported) || any;
        return any || functionThrows(f, sp, unsupported) || temporaryThrows(e);
      }
      if (through && through->kind != TyKind::Function)
        through = nullptr;
      if (!through && (!callee || callee->kind != NodeKind::FunctionDecl))
      {
        unsupported = "a call through a pointer or a closure";
        return false;
      }
      bool any = temporaryThrows(e); // a class by value: built where it is wanted, destroyed at the expression's end
      if (through)
      {
        any = canThrow(c->callee, unsupported) || any;
        any = callArgumentsThrow(nullptr, through, nullptr, c->args, unsupported) || any;
        return any || !through->isNoexcept();
      }
      if (c->callee->kind == NodeKind::MemberExpr)
        any = canThrow(static_cast<MemberExpr*>(c->callee)->base, unsupported) || any;
      // A function template's condition under the call's own template arguments (c<int>() of
      // `template<class T> void c() noexcept(sizeof(T) == 4)`: T int), beside the walk's.
      auto* fd = static_cast<FunctionDecl*>(callee);
      Subst withCall;
      const Subst* subst = activeSubst;
      // A member of a class template: under the instance it is called on, or named through.
      if (fd->parent && fd->parent->kind == NodeKind::ClassDecl && !Impl::effectiveParams(fd->parent).empty())
      {
        if (c->callee->kind == NodeKind::MemberExpr)
        {
          auto* m = static_cast<MemberExpr*>(c->callee);
          const Ty* object = m->base->ty ? types.unqualified(types.stripRef(m->base->ty)) : nullptr;
          if (object && object->kind == TyKind::Pointer)
            object = types.unqualified(object->inner);
          if (object && object->kind == TyKind::Class)
            withCall = impl.memberSubst(object, fd->parent->ownScope, 0);
        }
        else if (auto* named = static_cast<NameExpr*>(c->callee); named->name.parts.size() > 1)
          withCall = impl.qualifierSubst(named->name, TypeContext{scope, &locals, activeSubst}, 0, impl.lexicalScopeOf(fd));
      }
      if (activeSubst)
        for (const Subst::Pair& p : activeSubst->pairs)
          if (!withCall.find(p.param))
            withCall.pairs.push_back(p);
      if (!fd->templateParams.empty() && c->templateArgCount == fd->templateParams.size())
        for (size_t i = 0; i < c->templateArgCount; ++i)
          withCall.pairs.push_back({fd->templateParams[i], c->templateArgs[i]});
      if (!withCall.pairs.empty())
        subst = &withCall;
      any = callArgumentsThrow(fd, impl.declaredTypeOf(fd, subst, 0), subst, c->args, unsupported) || any;
      return any || functionThrows(fd, subst, unsupported);
    }
    default:
      break;
  }
  unsupported = std::string("its operand (") + nodeKindName(e->kind) + ")";
  return false;
}

auto GlobalScope::Impl::Walker::noteZeroNull(Expr* e, const Ty* target) -> void
{
  if (!file || file->isImported || probing) // (a template's own walk too: `T*` is a pointer whatever T is)
    return;
  target = target ? types.unqualified(types.stripRef(target)) : nullptr;
  if (!target || (target->kind != TyKind::Pointer && target->kind != TyKind::MemberPointer))
    return;
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (!e || e->kind != NodeKind::LiteralExpr || static_cast<LiteralExpr*>(e)->literalKind != TokenKind::Number)
    return;
  auto* l = static_cast<LiteralExpr*>(e);
  const ParsedFile* in = l->file ? l->file : file;
  if (!in || in->isImported || !in->tokens || l->token >= in->tokens->size())
    return;
  const Token& t = (*in->tokens)[l->token];
  std::string_view text = in->text.substr(t.offset, t.length);
  if (!isZeroLiteral(text))
    return;
  if (!firstZeroNull(in, l->token))
    return;
  if (zeroNullWarningsOn()) // (convert.cmd's nullptr pass: warnings, for fix-null.py)
    noteZeroNullWarning(Impl::locationOf(in, l->token) + ": warning: the literal 0 is no pointer: write nullptr (design.md 4)");
  else
    impl.reportCheckerError(in, l->token, "the literal 0 is no pointer: write nullptr (design.md 4)");
}

auto GlobalScope::Impl::Walker::elementTypeOf(const Ty* range, Expr* rangeExpr) -> const Ty*
{
  const Ty* t = types.stripRef(range);
  if (!t || !t->isKnown())
    return types.unknown();
  if (t->kind == TyKind::Array || t->kind == TyKind::Pointer)
    return types.lref(t->inner);
  if (t->kind == TyKind::Class)
  {
    if (types.isDependent(t))
      return types.unknown();
    MemberResult begin = memberOf(t, impl.symBegin, nullptr, 0, false);
    const Ty* iterator = nullptr;
    if (begin.decl && begin.decl->kind == NodeKind::FunctionDecl)
    {
      bool ambiguous;
      Span<Expr*> none;
      if (FunctionDecl* f = pickOverload(begin.decl, begin.overloads, {}, &begin.subst, t, ambiguous))
        iterator = returnTypeOfCall(f, &begin.subst, none, nullptr);
    }
    else if (!begin.decl && rangeExpr)
      iterator = freeBeginType(t, rangeExpr);
    if (!iterator || !iterator->isKnown())
      return types.unknown();
    const Ty* it = types.stripRef(iterator);
    if (it->kind == TyKind::Pointer)
      return types.lref(it->inner);
    if (it->kind == TyKind::Class && !types.isDependent(it))
    {
      MemberResult deref = memberOf(it, impl.opSymbols[uint32_t(P::Star)], nullptr, 0, false);
      if (deref.decl && deref.decl->kind == NodeKind::FunctionDecl)
      {
        bool ambiguous;
        Span<Expr*> none;
        if (FunctionDecl* f = pickOverload(deref.decl, deref.overloads, {}, &deref.subst, it, ambiguous))
          return returnTypeOfCall(f, &deref.subst, none, nullptr);
      }
    }
    return types.unknown();
  }
  return types.unknown();
}

auto GlobalScope::Impl::Walker::walkNode(Node* n) -> void
{
  if (!n)
    return;
  if (uint8_t(n->kind) <= uint8_t(NodeKind::PackType))
    walkType(static_cast<Type*>(n));
  else if (uint8_t(n->kind) >= uint8_t(NodeKind::NameExpr))
    walkExpr(static_cast<Expr*>(n));
  else if (uint8_t(n->kind) >= uint8_t(NodeKind::BlockStmt))
    walkStmt(static_cast<Stmt*>(n));
  else
    walkDecl(static_cast<Decl*>(n));
}

auto GlobalScope::Impl::Walker::walkType(Type* t) -> void
{
  if (!t)
    return;
  switch (t->kind)
  {
    case NodeKind::AutoType:
      // A constrained placeholder that no parameter's invented template parameter took (a variable's,
      // a result's): its constraint is not checked yet, so an error, never an unchecked `auto`.
      if (static_cast<AutoType*>(t)->constraint && !probing && file && !file->isImported)
        reportHere(t->token, "a constrained placeholder (`Concept auto`) is supported for a function's parameters only yet");
      break;
    case NodeKind::NamedType:
      walkType(static_cast<NamedType*>(t)->name.typeQualifier); // `decltype(x)::member`: x typed first
      resolveTypeName(static_cast<NamedType*>(t));
      break;
    case NodeKind::PointerType: walkType(static_cast<PointerType*>(t)->pointee); break;
    case NodeKind::LRefType: walkType(static_cast<LRefType*>(t)->referenced); break;
    case NodeKind::RRefType: walkType(static_cast<RRefType*>(t)->referenced); break;
    case NodeKind::ConstType: walkType(static_cast<ConstType*>(t)->inner); break;
    case NodeKind::PackType: walkType(static_cast<PackType*>(t)->pattern); break;
    case NodeKind::ArrayType:
      walkType(static_cast<ArrayType*>(t)->element);
      walkExpr(static_cast<ArrayType*>(t)->size);
      break;
    case NodeKind::FunctionType:
    {
      auto* f = static_cast<FunctionType*>(t);
      walkType(f->result);
      size_t mark = locals.mark();
      for (Decl* p : f->params)
        walkParam(static_cast<ParamDecl*>(p));
      locals.pop(mark);
      break;
    }
    case NodeKind::DecltypeType: walkExpr(static_cast<DecltypeType*>(t)->expr); break;
    case NodeKind::MemberPointerType:
      walkType(static_cast<MemberPointerType*>(t)->classType);
      walkType(static_cast<MemberPointerType*>(t)->pointee);
      break;
    default: break;
  }
}

auto GlobalScope::Impl::Walker::checkTypeNamesNoVariable(Node* base, uint32_t token) -> void
{
  for (;;)
    if (base && base->kind == NodeKind::PointerType)
      base = static_cast<PointerType*>(base)->pointee;
    else if (base && base->kind == NodeKind::LRefType)
      base = static_cast<LRefType*>(base)->referenced;
    else if (base && base->kind == NodeKind::RRefType)
      base = static_cast<RRefType*>(base)->referenced;
    else if (base && base->kind == NodeKind::ConstType)
      base = static_cast<ConstType*>(base)->inner;
    else
      break;
  Decl* named = base && base->kind == NodeKind::NamedType ? static_cast<NamedType*>(base)->resolved : nullptr;
  if (named && (named->kind == NodeKind::ParamDecl || (named->kind == NodeKind::VariableDecl && !(named->flags & DF_Typedef))) && file &&
      !file->isImported)
    impl.reportCheckerError(file, token, "'" + std::string(symbols.name(static_cast<NamedType*>(base)->name.last().symbol)) +
                                             "' is a variable, not a type: `a * b;` and `a b;` declare b, `T f(a)` at namespace scope a function (design.md 22)");
}

auto GlobalScope::Impl::Walker::walkParam(ParamDecl* p) -> void
{
  ++declaredTypes;
  walkType(p->type);
  --declaredTypes;
  if (!p->inheritedDefault)
    walkExpr(p->defaultValue);
  locals.push(p->name, p);
}

auto GlobalScope::Impl::Walker::pushTemplateParams(Span<Decl*> params) -> void
{
  for (Decl* d : params)
  {
    auto* p = static_cast<TemplateParamDecl*>(d);
    walkType(p->valueType);
    walkNode(p->defaultArg);
    locals.push(p->name, p);
  }
}

auto GlobalScope::Impl::Walker::semanticScopeFor(const QualifiedName& name) -> Scope*
{
  if (name.parts.size() <= 1)
    return scope;
  QualifiedName qualifier = name;
  qualifier.parts.count -= 1;
  Lookup l = impl.lookupQualified(qualifier, scope, &locals, 0);
  if (l.scope && l.scope->kind == ScopeKind::Class)
    if (Scope* special = impl.fullSpecializationScope(qualifier, scope, &locals, file, 0))
      return special; // `int Flag<true>::value() {...}`: the body of the specialisation's member
  return l.scope ? l.scope : scope;
}

auto GlobalScope::Impl::Walker::signatureFromWalk(FunctionDecl* f) -> bool
{
  bool found = false;
  auto look = [&](Node* t)
  {
    if (t)
      forEachNode(t, [&](Node* n) { found = found || n->kind == NodeKind::DecltypeType || n->kind == NodeKind::AutoType; });
  };
  look(f->returnType);
  look(f->trailingReturn);
  for (Decl* p : f->params)
    look(static_cast<ParamDecl*>(p)->type);
  return found;
}

auto GlobalScope::Impl::Walker::inGenericTemplate() const -> bool
{
  for (const LocalStack::Item& item : locals.items)
    if (item.decl && item.decl->kind == NodeKind::TemplateParamDecl && (!activeSubst || !activeSubst->find(item.decl)))
      return true;
  return false;
}

auto GlobalScope::Impl::Walker::replacedBySpecialization(Decl* d) -> bool
{
  if (!activeSubst)
    return false;
  for (Decl* p = d->parent; p; p = p->parent)
    if (p->kind == NodeKind::FunctionDecl)
    {
      auto* f = static_cast<FunctionDecl*>(p);
      return !f->templateParams.empty() && impl.functionSpecializationFor(f, *activeSubst);
    }
  return false;
}

auto GlobalScope::Impl::Walker::instanceDescription() const -> std::string
{
  std::string out;
  std::vector<std::string> seen; // an out-of-line member's parameters repeat its class's
  if (activeSubst)
    for (const Subst::Pair& p : activeSubst->pairs)
      if (p.param && p.param->kind == NodeKind::TemplateParamDecl)
      {
        uint32_t name = static_cast<TemplateParamDecl*>(p.param)->name;
        std::string one = std::string(name == UINT32_MAX ? "?" : impl.symbols.name(name)) + " = " + types.toString(p.type, &impl.symbols);
        if (std::find(seen.begin(), seen.end(), one) != seen.end())
          continue;
        seen.push_back(one);
        out += (out.empty() ? "" : ", ") + one;
      }
  return out;
}

auto GlobalScope::Impl::Walker::computeThisType() -> const Ty*
{
  Decl* cls = enclosingClassDecl(scope);
  if (!cls)
    return nullptr;
  if (cls->kind == NodeKind::EnumDecl)
    return types.lref(impl.enumTypeIn(cls, activeSubst)); // in an enum's methods, `this` is the value
  return types.pointer(impl.selfType(cls));
}

auto GlobalScope::Impl::Walker::walkDecls(Span<Decl*> decls) -> void
{
  for (Decl* d : decls)
    walkDecl(d);
}

auto GlobalScope::Impl::Walker::walkWhenCondition(Decl* d) -> void
{
  // A decided `when`'s condition (design.md 19: the parser's at namespace and class scope, the walk's
  // for a local) is walked for its names alone, so that they resolve as any other expression's do
  // (the language server's hover, definition, references on `config::debug` there; item 13 of the
  // refactoring list): what it decided stays.
  if (d && d->when && d->whenKnown >= 0 && !probing && !d->when->ty)
    walkExpr(d->when);
}

auto GlobalScope::Impl::Walker::walkDecl(Decl* d) -> void
{
  if (!d)
    return;
  walkWhenCondition(d);
  // While the files are resolved, a template function's own walk (the generic one, outside any
  // other unit) is a task for whichever worker is free: one file of heavy templates
  // (ValueToWidget.kpp: 18 template functions of 50 to 100 ms each, 1.26 s of the game's 1.31 s
  // resolve) no longer walks them all on one thread. Nothing outside the body reads what its walk
  // leaves on its nodes (the evaluator runs instances' copies), except a signature the walk types
  // (a decltype, a deduced auto: its callers read it), so such a function is walked in place.
  if (d->kind == NodeKind::FunctionDecl && !walkingQueued && file && file->tokens && !recordingInto && !activeSubst && instantiationDepth == 0 &&
      static_cast<FunctionDecl*>(d)->body && (!d->templateParams.empty() || inGenericTemplate()) &&
      impl.queueMemberBodies.load(std::memory_order_relaxed) && !signatureFromWalk(static_cast<FunctionDecl*>(d)))
  {
    auto dw = std::make_shared<Impl::DeclWalk>(Impl::DeclWalk{static_cast<FunctionDecl*>(d), scope, thisType, locals.items, file, &stats});
    Impl::MemberTask task{nullptr, nullptr, nullptr, 0, 0, nullptr, std::move(dw)};
    impl.queueMemberBody(task);
    return;
  }
  // A declaration's walk outside any other unit (a function's body, a variable's initializer, a
  // static_assert, an alias, an enumeration's values) is a unit of its own; what is declared
  // inside it belongs to it. Not a namespace's or a class's: their members are units each.
  std::optional<RecordInto> unit;
  if (!recordingInto && !activeSubst && d->kind != NodeKind::NamespaceDecl && d->kind != NodeKind::ClassDecl)
    unit.emplace(recordingDependencies ? impl.dependenciesOf(d, Impl::UNIT_Walk) : nullptr);
  size_t mark = locals.mark();
  if (!d->templateParams.empty())
    pushTemplateParams(d->templateParams);
  switch (d->kind)
  {
    case NodeKind::NamespaceDecl:
    {
      auto* ns = static_cast<NamespaceDecl*>(d);
      Scope* saved = scope;
      if (ns->ownScope)
        scope = ns->ownScope;
      walkDecls(ns->decls);
      scope = saved;
      break;
    }
    case NodeKind::ClassDecl:
    {
      auto* c = static_cast<ClassDecl*>(d);
      if (overrideWarningsOn() && Impl::effectiveParams(c).empty() && !activeSubst && !probing && file && !file->isImported)
        impl.noteNonVirtualDestructor(impl.selfType(c), c); // (a class template's, per instance: checkInstanceStaticAsserts)
      Scope* saved = scope;
      const Ty* savedThis = thisType;
      // The bases are named from where the class is (its scope's parent: `class A::B : Base`
      // sees A's Base, as in C++), not from where its definition is written.
      if (c->ownScope && c->ownScope->parent)
        scope = c->ownScope->parent;
      for (const ClassDecl::Base& b : c->bases)
      {
        walkType(b.type);
        // A virtual base would be laid out as an ordinary one (a diamond's shared base twice,
        // silently): an error until it is supported.
        if (b.isVirtual && b.type && !activeSubst && !probing && file && !file->isImported)
        {
          Type* named = b.type->kind == NodeKind::PackType ? static_cast<PackType*>(b.type)->pattern : b.type;
          std::string base = named->kind == NodeKind::NamedType && !static_cast<NamedType*>(named)->name.empty()
                               ? impl.namePartText(static_cast<NamedType*>(named)->name.last())
                               : std::string("?");
          std::string of = c->name.empty() ? std::string("a class") : impl.namePartText(c->name.last());
          impl.reportCheckerError(file, b.type->token, "virtual base classes are not supported (" + base + ", a base of " + of + ")");
        }
      }
      // A struct is a public value type (design.md "struct and class"): one deriving from a
      // polymorphic class is none (its own virtual functions the parser reports).
      if (impl.structRuleErrors && c->keyword == Kw::struct_ && c->isDefinition && !c->isClosure && !c->isTypedUnion && !activeSubst && !probing && file &&
          !file->isImported &&
          !Impl::declaresVirtual(c, false))
        if (ClassDecl* base = impl.polymorphicBaseOf(c, 0, false))
          impl.reportCheckerError(file, c->token, "struct " + (c->name.empty() ? std::string("(anonymous)") : impl.namePartText(c->name.last())) +
                                                      " derives from " + (base->name.empty() ? std::string("a") : impl.namePartText(base->name.last()) + ",") +
                                                      " a polymorphic class (a struct is a public value type, design.md \"struct and class\"): make it a class");
      checkDestructorNoexcept(c);
      if (c->ownScope)
        scope = c->ownScope;
      else if (c->name.parts.size() > 1)
        scope = semanticScopeFor(c->name);
      if (!activeSubst || c->ownScope != scope)
        thisType = computeThisType();
      // A class declared in a function body: its member functions find the class's members and
      // bases before the function's locals, which are below this frame (lookupUnqualified).
      bool local = bodyOf && c->ownScope;
      if (local)
        locals.classFrames.push_back({locals.mark(), c->ownScope});
      walkDecls(c->members);
      if (local)
        locals.classFrames.pop_back();
      scope = saved;
      thisType = savedThis;
      break;
    }
    case NodeKind::EnumDecl:
    {
      auto* e = static_cast<EnumDecl*>(d);
      walkType(e->underlying);
      // Local to an instance of a function, this copy is that instance's alone, and only this walk
      // has its arguments (a class template's enumeration has them in its type: substFor): what
      // they make its underlying type is kept for all who ask later.
      if (activeSubst && ownsNodes() && Impl::effectiveParams(e).empty())
        impl.declaredUnderlying(types.enumType(e), activeSubst);
      Scope* saved = scope;
      const Ty* savedThis = thisType;
      if (e->ownScope)
      {
        scope = e->ownScope;
        thisType = computeThisType(); // the enum's methods see `this` as the value
      }
      for (Decl* en : e->enumerators)
        if (en->kind == NodeKind::EnumeratorDecl)
          walkExpr(static_cast<EnumeratorDecl*>(en)->value);
      walkDecls(e->members);
      scope = saved;
      thisType = savedThis;
      break;
    }
    case NodeKind::FunctionDecl:
    {
      auto* f = static_cast<FunctionDecl*>(d);
      if (activeSubst && (f->attributes & ATTR_InstanceCopy))
        impl.recordInstanceSubst(f, *activeSubst); // the classes of its body see these arguments
      Scope* saved = scope;
      const Ty* savedThis = thisType;
      ++declaredTypes;
      walkType(f->returnType);
      --declaredTypes;
      if (f->name.parts.size() > 1)
      {
        scope = semanticScopeFor(f->name);
        if (!activeSubst)
          thisType = computeThisType();
      }
      else if (f->parent && f->parent->kind == NodeKind::NamespaceDecl && f->parent->ownScope)
      {
        // finishIndex simplified the qualified name but set the parent to the namespace decl.
        scope = f->parent->ownScope;
      }
      for (const NamePart& part : f->name.parts)
      {
        for (Node* arg : part.templateArgs)
          walkNode(arg);
        if (part.special == 3)
          walkType(part.convType);
      }
      for (Decl* p : f->params)
      {
        walkParam(static_cast<ParamDecl*>(p));
        checkTypeNamesNoVariable(static_cast<ParamDecl*>(p)->type, p->token); // (a function's own: not a function type's, `gcd(P1::num, P2::num)`, which may be a call)
      }
      walkType(f->trailingReturn);
      walkExpr(f->requiresClause);
      walkExpr(f->condition);
      checkDefinedOnce(f);
      checkTestDeclaration(f);
      checkRefQualifierMix(f);
      checkTextSignature(f);
      checkOverride(f);
      Decl* initialized = f->ctorInits.empty() ? nullptr : impl.owningClassOf(f);
      for (FunctionDecl::CtorInit& init : f->ctorInits)
      {
        if (initialized && initialized->kind == NodeKind::ClassDecl && absentMemberInit(static_cast<ClassDecl*>(initialized), init))
          continue; // a member a false `when` dropped: its initializer is not there either (design.md 19)
        Lookup l = impl.lookupQualified(init.member, scope, &locals, 0);
        if (l.found == Found::No)
        {
          ++stats.namesUnresolved;
          countUnresolved(init.member, f->token);
        }
        else
          ++stats.namesMember;
        FunctionDecl* savedInitializers = initializersOf;
        initializersOf = f;
        for (Expr* a : init.args)
          walkExpr(a);
        initializersOf = savedInitializers;
        // `x{d}` with a double d for an int member: a brace initialization that narrows.
        if (init.braced && !init.isPack && l.decl && (l.decl->kind == NodeKind::VariableDecl || l.decl->kind == NodeKind::ClassDecl))
        {
          const Ty* target = l.decl->kind == NodeKind::VariableDecl ? impl.declaredTypeOf(l.decl, activeSubst, 0) : impl.selfType(l.decl);
          Span<Expr*> elements = init.args;
          if (elements.size() == 1 && elements[0] && elements[0]->kind == NodeKind::BraceInitExpr)
            elements = static_cast<BraceInitExpr*>(elements[0])->args;
          if (target)
            checkListNarrowing(elements, target);
        }
        // `problem("bad ", point)` for a member (or base) constructed with a Text (design.md 14).
        if (!init.braced && !init.isPack && l.decl)
        {
          const Ty* initialized = l.decl->kind == NodeKind::VariableDecl ? impl.declaredTypeOf(l.decl, activeSubst, 0)
                                : l.decl->kind == NodeKind::ClassDecl  ? impl.selfType(l.decl)
                                                                       : nullptr;
          if (initialized && initialized->kind == TyKind::Class && !types.isDependent(initialized))
            packText(init.args, textConstructor(initialized, init.args), f->token);
          if (initialized && initialized->kind == TyKind::Array && l.decl->kind == NodeKind::VariableDecl && !init.args.empty())
            checkParenthesizedArray(init.args, initialized, f->token); // `values(4, 5)` (C++20)
        }
      }
      bool autoReturn = f->returnType && Impl::astMentionsAuto(f->returnType) && !f->trailingReturn;
      if (autoReturn)
      {
        returnFrames.push_back(nullptr);
        decltypeReturns.push_back(f->returnType->kind == NodeKind::AutoType && static_cast<AutoType*>(f->returnType)->isDecltypeAuto);
      }
      if (f->flags & DF_Static)
        thisType = nullptr;
      else if ((f->flags & (DF_Const | DF_Volatile)) && thisType && thisType->kind == TyKind::Pointer)
        thisType = impl.types.pointer(impl.types.qualified(thisType->inner, thisType->inner->isConst || (f->flags & DF_Const), // `const C* this` in a const method,
                                                           thisType->inner->isVolatile || (f->flags & DF_Volatile)));       // `volatile C* this` in a volatile one
      FunctionDecl* savedBodyOf = bodyOf;
      bodyOf = f;
      walkStmt(f->body);
      bodyOf = savedBodyOf;
      if ((impl.accessWarnings || impl.accessErrors) && !probing && !retypingElements)
      {
        // In an instance of a class template, the instance: its bases are as its arguments make them.
        const Ty* self = nullptr;
        if (activeSubst)
          if (Decl* cls = impl.owningClassOf(f); cls && cls->kind == NodeKind::ClassDecl)
            self = impl.substitute(impl.selfType(cls), *activeSubst);
        impl.checkAccessIn(f, self);
      }
      if (autoReturn)
      {
        const Ty* deduced = returnFrames.back();
        returnFrames.pop_back();
        decltypeReturns.pop_back();
        if (deduced && !returnFrames.empty() && !returnFrames.back())
          returnFrames.back() = deduced; // an instantiation's frame around the body takes it
        // Not from a template's own (generic) walk: what its body deduces there is no
        // instance's type (a closure of the generic body), and callers read it or not by which
        // file was walked first.
        if (deduced && deduced->isKnown() && !types.isDependent(deduced) && !inGenericTemplate())
        {
          // Cache the deduced signature for callers in other files.
          const Ty* fnTy = impl.declaredTypeOf(f, nullptr, 0);
          std::vector<const Ty*> params(fnTy->args.begin(), fnTy->args.end());
          f->cachedType.store(types.functionLike(fnTy, impl.deducedShape(fnTy->inner, deduced), Span<const Ty*>{params.data(), uint32_t(params.size())}), std::memory_order_release);
        }
      }
      scope = saved;
      thisType = savedThis;
      break;
    }
    case NodeKind::VariableDecl:
    {
      auto* v = static_cast<VariableDecl*>(d);
      deducedLocal = nullptr; // this variable's own, if it deduces one (not the one walked before)
      // A class template's data member a `when` on its parameters declares (design.md 19): its
      // condition, which codegen folds per instance for the layout, walked with the member.
      if (v->when && v->whenKnown < 0 && v->parent && v->parent->kind == NodeKind::ClassDecl)
        walkExpr(v->when);
      bool member = v->parent && v->parent->kind == NodeKind::ClassDecl;
      declaredTypes += member;
      walkType(v->type);
      declaredTypes -= member;
      checkTypeNamesNoVariable(v->type, v->token);
      checkArrayBounds(v);
      walkExpr(v->bitfield);
      walkExpr(v->alignment); // (`alignas(T)`: its name resolved, a type's or a constant's, for codegen's alignmentValue)
      Scope* saved = scope;
      if (v->name.parts.size() > 1)
        scope = semanticScopeFor(v->name);
      const Ty* initTy = walkExpr(v->init);
      scope = saved;
      if ((impl.accessWarnings || impl.accessErrors) && !probing && !retypingElements && !bodyOf && returnFrames.empty())
        impl.checkAccessIn(v); // (a local, also a lambda's in a member initializer: with the function it is in)
      // Deduce `auto` now that the initializer is typed, and cache it on the declaration.
      const Ty* declared = impl.declaredTypeOf(v, activeSubst, 0);
      // A retype with no arguments (a plain function's walk again after one per element of a `for
      // constexpr` in its body or a lambda's there) deduces an `auto` local again: the type cached on
      // it is the last element's walk's (a lambda's closure of that element, whose call operator
      // codegen then emitted a second time).
      if (retypingElements && !activeSubst && v->type && Impl::astMentionsAuto(v->type))
        declared = typeOf(v->type);
      if (v->init && declared && declared->isKnown() && !impl.containsAuto(declared) && !probesSharedNodes())
        bindTemplateAddresses(v->init, declared); // `const Expr& (*binary)(const Expr&, const Expr&) = construct<Add>;`
      checkTextVariable(v, declared);
      checkElidedBound(v, declared);
      if (v->init && !v->bracedInit)
        noteZeroNull(v->init, declared); // `Node* a = 0;`
      if (v->init && declared && !impl.containsAuto(declared))
        checkClosureTo(v->init, declared, v->token); // `Function<void()> f = [&] { ... text ... };`
      // `int count = item;`, `Item* raw = item;`: a class copy-initializing a scalar (not `T x(args)` or a brace list,
      // which take explicit conversion operators too).
      if (v->init && declared && !impl.containsAuto(declared) && !v->bracedInit && v->init->kind != NodeKind::BraceInitExpr &&
          !(v->init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(v->init)->type == v->type))
      {
        checkClassToScalar(initTy, declared, v->init->token);
        checkExplicitCopy(initTy, declared, v->init->token); // `Strict s = 5;`
      }
      if (v->init && declared && !impl.containsAuto(declared) && !v->bracedInit && v->init->kind == NodeKind::BraceInitExpr)
        checkExplicitListCopy(static_cast<BraceInitExpr*>(v->init), declared); // `Strict s = {5};`
      if (v->init && declared && declared->kind == TyKind::Array && v->init->kind == NodeKind::FunctionalCastExpr && !static_cast<FunctionalCastExpr*>(v->init)->braced)
        checkParenthesizedArray(static_cast<FunctionalCastExpr*>(v->init)->args, declared, v->init->token); // `int a[3](1, 2)` (C++20)
      // `int x{1.5};`, `char c = {300};`, `int a[2] = {1, 2.5};`: a brace list narrowing an element.
      if (v->init && declared && !impl.containsAuto(declared) && v->init->kind == NodeKind::BraceInitExpr &&
          static_cast<BraceInitExpr*>(v->init)->designators.empty() && !static_cast<BraceInitExpr*>(v->init)->text)
        checkListNarrowing(static_cast<BraceInitExpr*>(v->init)->args, declared);
      // `int& r = c;` with `const int c`, `int& e{c};`: a non-const reference given a const object.
      if (v->init && declared && declared->isReference() && !impl.containsAuto(declared) && !v->name.empty())
      {
        Expr* bound = v->init;
        const Ty* boundTy = initTy;
        if (v->bracedInit && bound->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(bound)->args.size() == 1)
        {
          bound = static_cast<BraceInitExpr*>(bound)->args[0];
          boundTy = bound ? bound->ty : nullptr;
        }
        if (bound && bound->kind != NodeKind::BraceInitExpr && boundTy)
        {
          checkConstDropped(bound->token, std::string(symbols.name(v->name.last().symbol)), declared, argumentType(bound, boundTy, types));
          checkTemporaryBound(bound->token, std::string(symbols.name(v->name.last().symbol)), declared, argumentType(bound, boundTy, types)); // `int& r = value();`
        }
      }
      bool isDraft = declared && declared->kind == TyKind::Class && declared->decl && (declared->decl->attributes & ATTR_Draft);
      if ((!declared || isDraft || Impl::mentionsUnknown(declared)) && v->type && !probesSharedNodes())
      {
        // A type only the function's locals know (`using Match = Pair<int, int>;` in the body,
        // `Map<Key, int>` with a local class Key): resolved here, where the locals are, and
        // cached for everyone asking later.
        const Ty* local = typeOf(v->type);
        if (local && local->isKnown() && (!declared || !declared->isKnown() || isDraft || !Impl::mentionsUnknown(local)))
        {
          v->cachedType.store(local, std::memory_order_release);
          declared = local;
          deducedLocal = local; // kept with the local by walkLocalDecl, for this walk's uses of it
        }
      }
      // (`NP p;` has no initializer. A template's own walk leaves it to its instances: the name there may
      // be the class's own, which means the instance.)
      bool defaultInitialized = !v->init && !(v->flags & (DF_Extern | DF_Typedef)) && !inGenericTemplate() &&
                                !(v->parent && v->parent->kind == NodeKind::ClassDecl && !(v->flags & DF_Static));
      if ((v->init || defaultInitialized) && !probesSharedNodes() && impl.isUndeducedClassTemplate(declared))
      {
        // `ScopedSetter guard(x, true)`: the class's arguments from a constructor (CTAD);
        // `Array names = {a, b}`: from an aggregate's members; `Duration budget =
        // Milliseconds(5)`: from the initializer's value, one argument.
        Span<Expr*> args;
        bool copyInit = false;
        if (!v->init)
          ;
        else if (v->init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(v->init)->type == v->type)
          args = static_cast<FunctionalCastExpr*>(v->init)->args; // the parser's `T x(args)`
        else if (v->init->kind == NodeKind::BraceInitExpr)
          args = static_cast<BraceInitExpr*>(v->init)->args;
        else
          copyInit = initTy && initTy->isKnown();
        std::vector<const Ty*> argTypes = copyInit ? std::vector<const Ty*>{argumentType(v->init, initTy, types)} : argTypesOf(args);
        // `T x = ...` and `T x = {...}` copy-initialise: an explicit deduction guide is none for them.
        bool direct = !v->init || (v->init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(v->init)->type == v->type) || v->bracedInit;
        const Ty* deduced = impl.deduceClassArgs(declared, argTypes, !direct);
        if (!deduced && v->init && v->init->kind == NodeKind::BraceInitExpr)
          deduced = impl.deduceAggregateArgs(declared, argTypes);
        // `constexpr Array names = {...}` is const, and `const Box b{1};` is a const Box<int> as
        // `const Box<int>` is: the declaration's qualifiers stay (until 2026-11-06 a const one
        // was dropped, so b.get() called the non-const get() and `b.v = 5` compiled).
        if (deduced && ((v->flags & DF_Constexpr) || declared->isConst || declared->isVolatile))
          deduced = types.qualified(deduced, (v->flags & DF_Constexpr) || declared->isConst, declared->isVolatile);
        if (deduced)
        {
          v->cachedType.store(deduced, std::memory_order_release);
          if (v->init && v->init->kind == NodeKind::FunctionalCastExpr)
            record(v->init, deduced);
          declared = deduced;
          deducedLocal = deduced; // the uses in this walk (a template instance's body asks under its substitution)
        }
      }
      if (impl.containsAuto(declared))
      {
        // From the type this walk gave the initializer, not the one on its (shared) node.
        Expr* initExpr = v->init; // (whose value category an `auto&&` takes)
        if (declared == types.decltypeAutoType())
          initTy = decltypeOf(v->init, initTy); // `decltype(auto) r = (x);` is a reference
        if (v->bracedInit && v->init && v->init->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(v->init)->args.size() == 1)
          if (Expr* only = static_cast<BraceInitExpr*>(v->init)->args[0]; only && only->ty)
          {
            initTy = only->ty; // `auto x{e}`, the init-capture `[callback{::move(callback)}]`: e's type
            initExpr = only;
          }
        if (!v->bracedInit && v->init && v->init->kind == NodeKind::BraceInitExpr && declared != types.decltypeAutoType())
          if (const Ty* list = impl.initializerListOfElements(static_cast<BraceInitExpr*>(v->init)))
            initTy = list; // `const auto entityNames = {"pipe", "storage-tank"};` is an InitializerList<const char*>
        const Ty* deduced = v->init && initTy ? impl.deduceVariable(v, declared, initTy, initExpr) : types.unknown();
        if (deduced->isKnown() && (v->flags & DF_Constexpr) && !deduced->isReference())
          deduced = types.qualified(deduced, true, false); // `constexpr auto limit = 5;` is a const int
        if (deduced->isKnown())
        {
          deducedLocal = deduced; // kept with the local by walkLocalDecl, per walk
          if (!probesSharedNodes())
            v->cachedType.store(deduced, std::memory_order_release);
        }
        else if ((activeSubst && !activeSubst->empty()) || !d->file || d->file->index == stats.fileIndex)
          ++stats.unknownAuto; // once, by its own file's walk (or its instance's): a global walked for another file's statistics may come before or not
      }
      // A constexpr variable is a context that needs its constant: checked where its type is
      // known, as a static_assert is (not in a template's own walk: its instances are).
      if ((v->flags & DF_Constexpr) && v->init && !(v->flags & DF_Typedef) && undecidedBranches == 0 && !inGenericTemplate() &&
          !retypingElements && !probing)
      {
        const Ty* type = deducedLocal ? deducedLocal : declared;
        impl.checkConstexprVariable(v, type, ctx(), activeSubst ? bodyOf : nullptr, file);
      }
      checkAbstractObject(v, deducedLocal ? deducedLocal : declared);
      break;
    }
    case NodeKind::ParamDecl: walkParam(static_cast<ParamDecl*>(d)); break;
    case NodeKind::UsingAliasDecl:
      checkNotLibrary(static_cast<UsingAliasDecl*>(d)->type, d->token);
      walkType(static_cast<UsingAliasDecl*>(d)->type);
      break;
    case NodeKind::UsingNameDecl:
    {
      auto* u = static_cast<UsingNameDecl*>(d);
      Lookup l = impl.lookupQualified(u->name, scope, &locals, 0);
      if (l.found == Found::No)
      {
        ++stats.typesUnresolved;
        countUnresolved(u->name, u->token);
      }
      else
        ++stats.typesResolved;
      break;
    }
    case NodeKind::UsingDirectiveDecl:
    {
      auto* u = static_cast<UsingDirectiveDecl*>(d);
      checkNotLibrary(u->name, u->token);
      Lookup l = impl.lookupQualified(u->name, scope, &locals, 0);
      if (l.found == Found::No)
      {
        ++stats.typesUnresolved;
        countUnresolved(u->name, u->token);
      }
      else
        ++stats.typesResolved;
      break;
    }
    case NodeKind::StaticAssertDecl:
    {
      auto* s = static_cast<StaticAssertDecl*>(d);
      walkExpr(s->cond);
      // Checked where its condition is known: not in a template's own walk (its instances
      // are), nor in a branch of an instance's `if constexpr` left undecided (it may be the
      // discarded one).
      if (undecidedBranches == 0 && !inGenericTemplate() && !replacedBySpecialization(s) && !retypingElements)
      {
        std::string instance = instanceDescription();
        if (!instance.empty() && requestFile)
          instance += ", asked for at " + Impl::locationOf(requestFile, requestToken);
        // In an instance's function, it counts if the program uses the function (C++ does not
        // instantiate the members, or the calls in the branches, nobody uses).
        impl.checkStaticAssert(s, ctx(), instance, activeSubst ? bodyOf : nullptr);
      }
      break;
    }
    case NodeKind::FriendDecl: walkType(static_cast<FriendDecl*>(d)->type); break;
    default: break;
  }
  locals.pop(mark);
}

auto GlobalScope::Impl::Walker::declareLocal(Decl* d) -> void
{
  switch (d->kind)
  {
    case NodeKind::VariableDecl:
    {
      auto* v = static_cast<VariableDecl*>(d);
      if ((v->flags & DF_Extern) && v->name.parts.size() == 1)
      {
        // `extern int x;` in a block: the namespace's x, whatever hides it here ([basic.link] 7),
        // no variable of its own (until 2026-11-06 it was one, a local reading 0).
        uint32_t symbol = v->name.parts[0].symbol;
        Lookup l = impl.lookupUnqualified(symbol, scope, nullptr, 0);
        if (l.found == Found::No || !l.decl || l.decl->kind != NodeKind::VariableDecl)
          reportCheck(v->token, "extern " + std::string(impl.symbols.name(symbol)) + " in a block: no namespace declares a variable of the name");
        else
          locals.push(symbol, l.decl);
        break;
      }
      if (v->name.parts.size() == 1)
        locals.push(v->name.parts[0].symbol, v);
      for (Expr* b : v->structuredBindings)
      {
        // A tuple-like binding names its hidden variable (tupleBindings), any other the object.
        Decl* own = static_cast<NameExpr*>(b)->resolved;
        bool tuple = own && own->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(own)->tupleBinding;
        locals.push(static_cast<NameExpr*>(b)->name.parts[0].symbol, tuple ? own : v);
        if (tuple)
          locals.items.back().type = own->cachedType.load(std::memory_order_acquire); // as its walk deduced it
      }
      break;
    }
    case NodeKind::ClassDecl:
    {
      auto* c = static_cast<ClassDecl*>(d);
      if (c->name.parts.size() == 1)
        locals.push(c->name.parts[0].symbol, c);
      break;
    }
    case NodeKind::EnumDecl:
    {
      auto* e = static_cast<EnumDecl*>(d);
      if (e->name.parts.size() == 1)
        locals.push(e->name.parts[0].symbol, e);
      for (Decl* en : e->isClass ? Span<Decl*>{} : e->enumerators) // a scoped enum's stay in it
        if (en->kind == NodeKind::EnumeratorDecl)
          locals.push(static_cast<EnumeratorDecl*>(en)->name, en);
      break;
    }
    case NodeKind::UsingAliasDecl:
      if (d->whenKnown != 0) // (a false `when`'s is not checked: design.md 19)
        checkNotLibrary(static_cast<UsingAliasDecl*>(d)->type, d->token);
      locals.push(static_cast<UsingAliasDecl*>(d)->name, d);
      break;
    case NodeKind::UsingDirectiveDecl: // `using namespace Helpers;`
      checkNotLibrary(static_cast<UsingDirectiveDecl*>(d)->name, d->token);
      locals.push(LocalStack::usingDirective, d);
      break;
    case NodeKind::UsingNameDecl:
    {
      // `using Graphics::Architecture;` in a block: the name it brings in, for the block.
      auto* u = static_cast<UsingNameDecl*>(d);
      if (u->isEnum)
      {
        impl.pushLocalUsingEnum(u, scope, locals); // `using enum UndoActionDescription::Type;`: its enumerators
        break;
      }
      if (u->name.empty())
        break;
      if (Decl* target = impl.localUsingTarget(u, scope, &locals))
        locals.push(u->name.last().symbol, target);
      else
        locals.push(u->name.last().symbol, u); // functions: the overload set it names, through the using-declaration (lookupUnqualified)
      break;
    }
    case NodeKind::FunctionDecl:
    {
      auto* f = static_cast<FunctionDecl*>(d);
      if (f->name.parts.size() == 1 && f->name.parts[0].special == 0)
        locals.push(f->name.parts[0].symbol, f);
      break;
    }
    default: break;
  }
}

auto GlobalScope::Impl::Walker::tupleBindings(VariableDecl* v, const Ty* object) -> void
{
  if (v->structuredBindings.empty() || probesSharedNodes())
    return;
  // The hidden variables an earlier walk of these same nodes made are walked again, not made anew:
  // a retype in place (GlobalScope::retypeFor, per element of a pack expansion or a `for constexpr`)
  // walks the body the code generator is emitting, which declared them and names them. (Until
  // 2026-11-07 it made new ones, and the uses after `T(args...)` named variables of no function.) A
  // copy's nodes have none (InstanceCopier).
  std::vector<VariableDecl*> earlier(v->structuredBindings.size(), nullptr);
  for (size_t i = 0; i < v->structuredBindings.size(); ++i)
  {
    auto* b = static_cast<NameExpr*>(v->structuredBindings[i]);
    if (b->resolved && b->resolved->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(b->resolved)->tupleBinding)
      earlier[i] = static_cast<VariableDecl*>(b->resolved);
    b->resolved = nullptr; // (named again below, unless the object binds member by member here)
  }
  const Ty* e = object ? types.unqualified(types.stripRef(object)) : nullptr;
  if (!e || e->kind != TyKind::Class || !e->isKnown() || types.isDependent(e) || Impl::mentionsUnknown(e))
    return;
  std::optional<int64_t> size;
  if (!impl.tupleSizeOf(e, size, 0))
    return;
  size_t n = v->structuredBindings.size();
  if (!size || *size != int64_t(n))
  {
    if (!inGenericTemplate())
      impl.reportCheckerError(v->file, v->token,
                              !size ? "a structured binding of " + types.toString(e, &impl.symbols) + ", whose TupleSize<>::value is no constant"
                                    : std::to_string(n) + " names bound to " + types.toString(e, &impl.symbols) + ", whose TupleSize<>::value is " +
                                          std::to_string(*size));
    return;
  }
  if (n > impl.indexTokens.size())
    return;
  bool member = impl.hasMemberGet(e, 0);
  Arena& arena = impl.instanceArena();
  size_t mark = locals.mark();
  locals.items.push_back({LocalStack::hiddenObject, v, object}); // the object as the calls name it (resolveName)
  for (size_t i = 0; i < n; ++i)
  {
    auto* b = static_cast<NameExpr*>(v->structuredBindings[i]);
    if (VariableDecl* again = earlier[i]; again && again->init && again->init->kind == NodeKind::CallExpr &&
        (static_cast<CallExpr*>(again->init)->callee->kind == NodeKind::MemberExpr) == member)
    {
      deducedLocal = nullptr;
      walkDecl(again);
      deducedLocal = nullptr;
      b->resolved = again;
      continue;
    }
    auto* hidden = arena.make<NameExpr>();
    hidden->kind = NodeKind::NameExpr;
    hidden->token = b->token;
    hidden->resolved = v; // no name: the hidden object itself (resolveName)
    auto* index = arena.make<LiteralExpr>();
    index->kind = NodeKind::LiteralExpr;
    index->token = uint32_t(i);
    index->literalKind = TokenKind::Number;
    index->keyword = Kw{};
    index->file = &impl.indexFile;
    Node* indexArg = index;
    NamePart part{};
    part.symbol = impl.symGet;
    part.templateArgs = Span<Node*>{arena.array(&indexArg, 1), 1};
    part.hasTemplateArgs = true;
    auto* call = arena.make<CallExpr>();
    call->kind = NodeKind::CallExpr;
    call->token = b->token;
    if (member)
    {
      auto* m = arena.make<MemberExpr>();
      m->kind = NodeKind::MemberExpr;
      m->token = b->token;
      m->base = hidden;
      m->member.parts = Span<NamePart>{arena.array(&part, 1), 1};
      m->op = P::Dot;
      call->callee = m;
    }
    else
    {
      // `::get<i>(object)`: the library's get and those argument-dependent lookup finds, never
      // a member or a local named get.
      auto* callee = arena.make<NameExpr>();
      callee->kind = NodeKind::NameExpr;
      callee->token = b->token;
      callee->name.parts = Span<NamePart>{arena.array(&part, 1), 1};
      callee->name.global = true;
      Expr* argument = hidden;
      call->callee = callee;
      call->args = Span<Expr*>{arena.array(&argument, 1), 1};
    }
    auto* deduced = arena.make<AutoType>();
    deduced->kind = NodeKind::AutoType;
    deduced->token = b->token;
    auto* reference = arena.make<RRefType>();
    reference->kind = NodeKind::RRefType;
    reference->token = b->token;
    reference->referenced = deduced;
    auto* var = arena.make<VariableDecl>();
    var->kind = NodeKind::VariableDecl;
    var->token = b->token;
    var->endToken = v->endToken;
    var->file = v->file;
    var->parent = v->parent;
    var->type = reference;
    var->name = b->name;
    var->init = call;
    var->tupleBinding = true;
    deducedLocal = nullptr;
    walkDecl(var);
    deducedLocal = nullptr;
    b->resolved = var;
  }
  locals.pop(mark);
}

auto GlobalScope::Impl::Walker::conditionText(Decl* d) -> std::string
{
  const ParsedFile* f = d->file ? d->file : file;
  if (!d->when || !f || !f->tokens)
    return "its condition";
  uint32_t first = UINT32_MAX;
  forEachNode(d->when, [&](Node* n) { first = std::min(first, n->token); });
  const std::vector<Token>& tokens = *f->tokens;
  uint32_t end = first;
  for (int depth = 0; end < tokens.size(); ++end)
    if (tokens[end].is(P::LParen))
      ++depth;
    else if (tokens[end].is(P::RParen) && depth-- == 0)
      break;
  if (first >= end || end >= tokens.size())
    return "its condition";
  return std::string(f->text.substr(tokens[first].offset, tokens[end - 1].offset + tokens[end - 1].length - tokens[first].offset));
}

auto GlobalScope::Impl::Walker::whenWhere() -> std::string
{
  std::string where = "here";
  if (activeSubst && bodyOf && !bodyOf->name.empty())
  {
    where = "in this instance (" + std::string(symbols.name(bodyOf->name.last().symbol)) + "<";
    for (size_t k = 0; k < activeSubst->pairs.size(); ++k)
      where += (k ? ", " : "") + types.toString(activeSubst->pairs[k].type, &symbols);
    where += ">)";
  }
  return where;
}

auto GlobalScope::Impl::Walker::whenUse(uint32_t symbol, uint32_t token, Decl* found, bool& undecided) -> Decl*
{
  undecided = false;
  size_t at = locals.items.size();
  while (at > 0 && locals.items[at - 1].decl != found)
    --at;
  if (at == 0)
    return found;
  size_t start = 0;
  for (size_t s : blockStarts)
    if (s < at)
      start = s;
  std::vector<Decl*> candidates, existing;
  std::string conditions;
  for (size_t i = start; i < locals.items.size(); ++i)
    if (Decl* c = locals.items[i].decl;
        locals.items[i].symbol == symbol && c && std::find(candidates.begin(), candidates.end(), c) == candidates.end())
    {
      candidates.push_back(c);
      if (!c->when || c->whenKnown == 1)
        existing.push_back(c);
      else if (c->whenKnown < 0)
        undecided = true;
      if (c->when)
        conditions += (conditions.empty() ? "" : ", ") + conditionText(c);
    }
  if (undecided)
    return candidates.size() == 1 ? found : nullptr; // (a lone one typed as itself; of several, the instances tell)
  if (!existing.empty())
    return existing[0];
  if (file && !file->isImported && !probing)
  {
    std::string name(symbols.name(symbol));
    std::string message = candidates.size() == 1 ? "'" + name + "' exists only when " + conditions + " holds: use it inside `if constexpr (" +
                                                       conditions + ")` or a `when` on it"
                                                 : "'" + name + "' does not exist here: none of its `when` conditions holds (" + conditions + ")";
    if (activeSubst)
      reportInInstance(token, message);
    else
      impl.reportCheckerError(file, token, message);
  }
  return nullptr;
}

auto GlobalScope::Impl::Walker::whenLookup(Lookup& l, NameExpr* n) -> bool
{
  if (l.found == Found::Member && activeSubst && l.decl && l.decl->kind == NodeKind::VariableDecl && l.decl->when && l.decl->whenKnown < 0 &&
      l.overloads && l.overloads->size() > 1 && n->name.parts.size() == 1)
    return whenMemberLookup(l, n);
  if (l.found != Found::Local || !l.decl || !l.decl->when || n->name.parts.size() != 1)
    return true;
  bool undecided = false;
  Decl* named = whenUse(n->name.parts[0].symbol, n->token, l.decl, undecided);
  if (!named)
    return false;
  l.decl = named;
  n->resolved = named;
  return true;
}

auto GlobalScope::Impl::Walker::whenMemberLookup(Lookup& l, NameExpr* n) -> bool
{
  Decl* c = l.decl->parent;
  if (!c || c->kind != NodeKind::ClassDecl || l.foundIn != c->ownScope)
    return true;
  for (Decl* p : Impl::effectiveParams(c))
    if (!activeSubst->find(p))
      return true;
  int holding = 0;
  bool undecided = false;
  Decl* chosen = impl.whenMemberOf(l.decl, *activeSubst, holding, undecided);
  if (undecided)
    return true;
  if (chosen)
  {
    l.decl = chosen;
    l.overloads = nullptr;
    n->resolved = chosen;
    return true;
  }
  reportInInstance(n->token, "'" + std::string(symbols.name(n->name.parts[0].symbol)) + "' " +
                               (holding == 0 ? std::string("does not exist in this instance: none of its `when` conditions holds there")
                                             : "is declared twice in this instance: the `when` conditions of " + std::to_string(holding) +
                                                 " of its declarations hold there") +
                               " (design.md 19)");
  return false;
}

auto GlobalScope::Impl::Walker::configCondition(Node* n, int depth) -> bool
{
  if (!n || depth > 32)
    return false;
  switch (n->kind)
  {
    case NodeKind::LiteralExpr: return true;
    case NodeKind::ParenExpr: return configCondition(static_cast<ParenExpr*>(n)->inner, depth + 1);
    case NodeKind::UnaryExpr: return configCondition(static_cast<UnaryExpr*>(n)->operand, depth + 1);
    case NodeKind::BinaryExpr:
      return configCondition(static_cast<BinaryExpr*>(n)->lhs, depth + 1) && configCondition(static_cast<BinaryExpr*>(n)->rhs, depth + 1);
    case NodeKind::NameExpr:
    {
      Decl* d = static_cast<NameExpr*>(n)->resolved;
      if (d && d->kind == NodeKind::EnumeratorDecl)
      {
        // `config::platform == Platform::Linux`: an enumerator of an enumeration no template
        // declares, the same in every instance (Util::sleep_for's Windows branch named Sleep, which
        // the template's own walk checked on Linux: templates/discarded_config_branch_in_template).
        for (Decl* p = d->parent; p; p = p->parent)
          if (p->isTemplate || !p->templateParams.empty())
            return false;
        return true;
      }
      if (!d || d->kind != NodeKind::VariableDecl)
        return false;
      Decl* top = d->parent;
      while (top && top->parent)
        top = top->parent;
      return top && top->kind == NodeKind::NamespaceDecl && static_cast<NamespaceDecl*>(top)->name != UINT32_MAX &&
             symbols.name(static_cast<NamespaceDecl*>(top)->name) == "config";
    }
    default: return false;
  }
}

auto GlobalScope::Impl::Walker::whenHolds(Decl* d) -> bool
{
  if (!d->when)
    return true;
  if (keepConstexprBranches && d->whenKnown >= 0)
    return d->whenKnown != 0;
  walkExpr(d->when);
  if (!activeSubst && (impl.converting || (inGenericTemplate() && !configCondition(d->when))))
    return true; // (a template's own walk decides only a condition of config constants and literals)
  std::optional<bool> known = impl.constCondition(d->when, ctx(), 0);
  if (!known)
  {
    impl.reportCheckerError(file, d->when->token, "a when condition is a constant expression, as if constexpr's (design.md 19)");
    return true;
  }
  d->whenKnown = int8_t(*known ? 1 : 0);
  // Several `when`s may declare one name, as an #ifdef/#else pair (`when (c) A t; when (!c) B
  // t;`); two that exist at once are a clash.
  uint32_t name = impl.absentName(d); // (a variable's, an alias's, a class's or an enumeration's)
  if (*known && name != UINT32_MAX)
    for (size_t i = blockLocals; i < locals.items.size(); ++i)
      if (Decl* other = locals.items[i].decl; locals.items[i].symbol == name && other && (!other->when || other->whenKnown != 0))
      {
        impl.reportCheckerError(file, d->token, "'" + std::string(symbols.name(name)) + "' is declared twice " + whenWhere() +
                                                    ": both `when` conditions hold");
        break;
      }
  return *known;
}

auto GlobalScope::Impl::Walker::walkLocalDecl(Decl* d) -> void
{
  if (!whenHolds(d))
  {
    // Declared all the same, absent (whenKnown 0): a use of its name finds it, never a name of an
    // outer scope or of the library, and is an error unless it is discarded code (whenUse).
    walkWhenCondition(d);
    declareLocal(d);
    return;
  }
  // Classes are in scope in their own bodies. A variable whose type is written is in scope
  // in its own initializer, as C++ declares it before that (`Function<void(int)> visit =
  // [&](int n) { visit(n - 1); };`); an `auto` one is not, its type comes from there.
  if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl)
  {
    declareLocal(d);
    walkDecl(d);
  }
  else
  {
    auto* v = d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : nullptr;
    bool early = v && v->type && v->init && v->structuredBindings.empty() && !Impl::astMentionsAuto(v->type);
    size_t declaredAt = locals.mark();
    if (early)
      declareLocal(d);
    deducedLocal = nullptr;
    walkDecl(d);
    const Ty* deduced = v ? deducedLocal : nullptr;
    deducedLocal = nullptr;
    if (v && !v->structuredBindings.empty())
      tupleBindings(v, deduced ? deduced : hiddenObjectType(v));
    if (!early)
    {
      declaredAt = locals.mark();
      declareLocal(d);
    }
    if (deduced)
      for (size_t i = declaredAt; i < locals.items.size(); ++i)
        if (locals.items[i].decl == d)
          locals.items[i].type = deduced; // `auto x = e;`: the type this walk deduced
  }
}

auto GlobalScope::Impl::Walker::walkStmt(Stmt* s) -> void
{
  if (!s)
    return;
  switch (s->kind)
  {
    case NodeKind::BlockStmt:
    {
      size_t mark = locals.mark(), savedBlock = blockLocals;
      blockLocals = mark;
      blockStarts.push_back(mark);
      for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
        walkStmt(inner);
      blockStarts.pop_back();
      blockLocals = savedBlock;
      locals.pop(mark);
      break;
    }
    case NodeKind::ExprStmt:
      walkExpr(static_cast<ExprStmt*>(s)->expr);
      checkDiscarded(static_cast<ExprStmt*>(s)->expr);
      break;
    case NodeKind::AssertStmt:
      // Where config::debug is false an assert compiles to nothing (design.md 7): its expression is
      // discarded code, not checked (a `when (config::debug)` member it names, design.md 19).
      if (auto* a = static_cast<AssertStmt*>(s); a->release || a->testKind || impl.debug)
        walkExpr(a->cond);
      checkTestStatementName(static_cast<AssertStmt*>(s));
      break;
    case NodeKind::DeclStmt:
      for (Decl* d : static_cast<DeclStmt*>(s)->decls)
        walkLocalDecl(d);
      break;
    case NodeKind::IfStmt:
    {
      auto* i = static_cast<IfStmt*>(s);
      size_t mark = locals.mark();
      walkStmt(i->init);
      if (i->isConstexpr && keepConstexprBranches && i->constexprKnown >= 0)
      {
        walkStmt(i->constexprKnown ? i->then : i->else_); // the instance's branch (keepConstexprBranches)
        locals.pop(mark);
        break;
      }
      walkNode(i->cond);
      // In an instantiation, and in a function that is no template (`if constexpr (config::debug)`,
      // whose discarded branch names what only the other configuration declares: design.md
      // 7.4, discarded branches); a template's own walk leaves both to its instances, but for a
      // condition of config constants and literals, the same in every instance (a `when` member
      // the discarded branch names: design.md 19).
      if (i->isConstexpr && (activeSubst || (!impl.converting && (!inGenericTemplate() || configCondition(i->cond)))) && i->cond &&
          uint8_t(i->cond->kind) > uint8_t(NodeKind::PackType) && uint8_t(i->cond->kind) >= uint8_t(NodeKind::NameExpr))
        if (std::optional<bool> known = impl.constCondition(static_cast<Expr*>(i->cond), ctx(), 0))
        {
          // if constexpr with a known condition: the discarded branch is not checked (nor are
          // its calls instantiated), as in C++.  Store the result so codegen can read it
          // without re-evaluating (codegen's TypeContext lacks function-body locals).
          i->constexprKnown = int8_t(*known ? 1 : 0);
          walkStmt(*known ? i->then : i->else_);
          locals.pop(mark);
          break;
        }
      bool undecided = i->isConstexpr && activeSubst;
      undecidedBranches += undecided;
      walkStmt(i->then);
      walkStmt(i->else_);
      undecidedBranches -= undecided;
      locals.pop(mark);
      break;
    }
    case NodeKind::ForStmt:
    {
      auto* f = static_cast<ForStmt*>(s);
      size_t mark = locals.mark();
      walkStmt(f->init);
      walkNode(f->cond);
      walkExpr(f->step);
      walkStmt(f->body);
      locals.pop(mark);
      break;
    }
    case NodeKind::RangeForStmt:
    {
      auto* f = static_cast<RangeForStmt*>(s);
      size_t mark = locals.mark();
      const Ty* range = walkExpr(f->range);
      if (VariableDecl* unbounded = unboundedArray(f->range))
        impl.reportCheckerError(file, f->token, "a range-for over " + std::string(symbols.name(unbounded->name.last().symbol)) +
                                                    ", an array without a length (a flexible array member): where it ends is not known");
      if (f->range->kind == NodeKind::BraceInitExpr)
      {
        // `for (X* kids : {&a, &b})`: an array of the first element's type.
        auto* b = static_cast<BraceInitExpr*>(f->range);
        const Ty* first = !b->args.empty() && b->args[0]->ty ? types.decay(b->args[0]->ty) : nullptr;
        if (first && first->isKnown())
          range = record(f->range, types.array(first, uint32_t(b->args.size())));
      }
      recordRangeFunctions(f, range);
      auto* v = static_cast<VariableDecl*>(f->var);
      walkType(v->type);
      const Ty* declared = impl.declaredTypeOf(v, activeSubst, 0);
      const Ty* loopVarType = nullptr;
      bool isDraft = declared && types.unqualified(types.stripRef(declared))->decl &&
                     (types.unqualified(types.stripRef(declared))->decl->attributes & ATTR_Draft);
      if ((!declared || isDraft || Impl::mentionsUnknown(declared)) && v->type && !probesSharedNodes())
      {
        // `for (ToCall& toCall : ...)` with ToCall a class of the function's own: the type
        // where the locals are (as for any local variable).
        const Ty* local = typeOf(v->type);
        if (local && local->isKnown() && !Impl::mentionsUnknown(local))
        {
          v->cachedType.store(local, std::memory_order_release);
          declared = local;
          loopVarType = local;
        }
      }
      // `for (int& x : numbers)` over const elements: a non-const reference given a const object.
      if (declared && declared->isReference() && !impl.containsAuto(declared) && !v->name.empty())
      {
        checkConstDropped(v->token, std::string(symbols.name(v->name.last().symbol)), declared, elementTypeOf(range, f->range));
        checkTemporaryBound(v->token, std::string(symbols.name(v->name.last().symbol)), declared, elementTypeOf(range, f->range));
      }
      if (impl.containsAuto(declared))
      {
        const Ty* element = elementTypeOf(range, f->range);
        const Ty* deduced = element->isKnown() ? impl.deduceVariable(v, declared, element) : types.unknown();
        if (deduced->isKnown())
        {
          v->cachedType.store(deduced, std::memory_order_release);
          loopVarType = deduced;
        }
        else if (range && range->isKnown() && types.isDependent(range))
          loopVarType = impl.deduceAuto(declared, types.lref(types.param(nullptr))); // typed per instantiation
        else
          ++stats.unknownAuto;
      }
      if (!v->structuredBindings.empty())
        tupleBindings(v, loopVarType ? loopVarType : hiddenObjectType(v)); // `for (auto& [key, value] : tuples)`, each iteration's get<i>
      size_t declaredAt = locals.mark();
      declareLocal(v);
      if (loopVarType)
        for (size_t i = declaredAt; i < locals.items.size(); ++i)
          if (locals.items[i].decl == v)
            locals.items[i].type = loopVarType;
      walkStmt(f->body);
      locals.pop(mark);
      break;
    }
    case NodeKind::ForConstexprStmt:
    {
      // The loop variable is a template parameter of the body: in the generic pass the body
      // depends on it; the code generator re-types it per element (as a pack expansion).
      auto* f = static_cast<ForConstexprStmt*>(s);
      size_t mark = locals.mark();
      auto* v = static_cast<TemplateParamDecl*>(f->var);
      walkType(v->valueType);
      if (f->list && f->list->kind == NodeKind::NamedType && isTypesList(static_cast<NamedType*>(f->list)))
        for (Node* arg : static_cast<NamedType*>(f->list)->name.parts[0].templateArgs)
          walkNode(arg); // `types<A, B>`: the elements; the list itself is the compiler's
      else if (Expr* subject = reflectionSubject(f->list))
        walkExpr(subject); // `members(T)`: the type reflected on
      else
        walkNode(f->list);
      locals.push(v->name, v);
      ++forConstexprDepth;
      walkStmt(f->body);
      --forConstexprDepth;
      // In an instantiation of an auto function whose return type is still open (`visit(f, u)`
      // returning f's result from inside the loop): the body once per element, the variable
      // bound to it, as the code generator emits it, until a return gives the type.
      std::vector<const Ty*> elements;
      if (activeSubst && !returnFrames.empty() && !returnFrames.back() && forConstexprElements(f, elements))
      {
        Subst bound = *activeSubst;
        bound.pairs.push_back({v, nullptr});
        const Subst* saved = activeSubst;
        activeSubst = &bound;
        for (const Ty* element : elements)
        {
          bound.pairs.back().type = element;
          size_t inner = locals.mark();
          walkStmt(f->body);
          locals.pop(inner);
          if (returnFrames.back())
            break;
        }
        activeSubst = saved;
      }
      locals.pop(mark);
      break;
    }
    case NodeKind::WhileStmt:
    {
      auto* w = static_cast<WhileStmt*>(s);
      size_t mark = locals.mark();
      walkNode(w->cond);
      walkStmt(w->body);
      locals.pop(mark);
      break;
    }
    case NodeKind::DoStmt:
      walkStmt(static_cast<DoStmt*>(s)->body);
      walkExpr(static_cast<DoStmt*>(s)->cond);
      break;
    case NodeKind::SwitchStmt:
    {
      auto* w = static_cast<SwitchStmt*>(s);
      checkCaseEnds(w->body);
      size_t mark = locals.mark();
      walkStmt(w->init);
      walkNode(w->cond);
      walkStmt(w->body);
      locals.pop(mark);
      break;
    }
    case NodeKind::CaseStmt:
      walkExpr(static_cast<CaseStmt*>(s)->value);
      walkExpr(static_cast<CaseStmt*>(s)->rangeEnd);
      break;
    case NodeKind::ReturnStmt:
    {
      const Ty* t = walkExpr(static_cast<ReturnStmt*>(s)->value);
      if (bodyOf && returnFrames.empty()) // (`return 0;` from a function returning T*: design.md 4)
        if (const Ty* ft = impl.declaredTypeOf(bodyOf, activeSubst, 0); ft && ft->kind == TyKind::Function)
        {
          noteZeroNull(static_cast<ReturnStmt*>(s)->value, ft->inner);
          // `return 5;` from a function returning Strict: a copy-initialisation of the result.
          if (Expr* value = static_cast<ReturnStmt*>(s)->value; value && ft->inner && !impl.containsAuto(ft->inner) && !(bodyOf->flags & DF_Constructor))
          {
            if (value->kind == NodeKind::BraceInitExpr)
            {
              checkExplicitListCopy(static_cast<BraceInitExpr*>(value), ft->inner);
              if (static_cast<BraceInitExpr*>(value)->designators.empty() && !static_cast<BraceInitExpr*>(value)->text)
                checkListNarrowing(static_cast<BraceInitExpr*>(value)->args, ft->inner); // `return {1.5};` from a function returning int
            }
            else
            {
              checkExplicitCopy(t, ft->inner, value->token);
              // `int& get() const { return this->v; }`: the result, a non-const reference, given a const object.
              if (ft->inner->isReference() && t)
                if (uint32_t symbol = impl.functionSymbol(bodyOf->name); symbol != UINT32_MAX)
                {
                  checkConstDropped(value->token, "the result of " + std::string(symbols.name(symbol)), ft->inner, argumentType(value, t, types));
                  checkTemporaryBound(value->token, "the result of " + std::string(symbols.name(symbol)), ft->inner, argumentType(value, t, types)); // `return 5;` for an int&
                }
            }
          }
        }
      if (!decltypeReturns.empty() && decltypeReturns.back())
        t = decltypeOf(static_cast<ReturnStmt*>(s)->value, t);
      else if (t && t->isKnown() && t->kind != TyKind::Pack && !t->isReference() && isLvalueExpression(static_cast<ReturnStmt*>(s)->value))
        t = types.lref(t); // an lvalue as `auto&&` deduces it (`return this->member;`: T&); `auto` decays it (deducedShape)
      // Inside a for constexpr body of an instantiation, before the loop binds its variable,
      // a return that depends on it gives no type (the per-element walk does).
      bool open = forConstexprDepth > 0 && activeSubst && t && (types.isDependent(t) || impl.containsAuto(t));
      if (!returnFrames.empty() && !returnFrames.back() && t && t->isKnown() && !open)
        returnFrames.back() = t;
      break;
    }
    case NodeKind::TryStmt:
    {
      auto* t = static_cast<TryStmt*>(s);
      walkStmt(t->body);
      for (const TryStmt::Handler& h : t->handlers)
      {
        size_t mark = locals.mark();
        if (h.param)
          walkParam(static_cast<ParamDecl*>(h.param));
        walkStmt(h.body);
        locals.pop(mark);
      }
      break;
    }
    default: break;
  }
}

auto GlobalScope::Impl::Walker::walkExprs(Span<Expr*> exprs) -> void
{
  for (Expr* e : exprs)
    walkExpr(e);
}

auto GlobalScope::Impl::Walker::walkExpr(Expr* e) -> const Ty*
{
  if (!e)
    return nullptr;
  struct Current
  {
    Expr*& slot;
    Expr* saved;
    Current(Expr*& slot, Expr* e) : slot(slot), saved(slot) { slot = e; }
    ~Current() { slot = saved; }
  } guard(current, e);
  switch (e->kind)
  {
    case NodeKind::NameExpr: return record(e, resolveName(static_cast<NameExpr*>(e)));
    case NodeKind::CallerExpr: return record(e, types.unknown()); // typed by the parameter it defaults
    case NodeKind::NameofExpr:
      walkExpr(static_cast<NameofExpr*>(e)->operand);
      return record(e, types.pointer(types.qualified(types.charType(), true, false)));
    case NodeKind::NoexceptExpr:
    {
      auto* n = static_cast<NoexceptExpr*>(e);
      walkExpr(n->operand);
      if (!inGenericTemplate())
      {
        std::string unsupported;
        bool throws = canThrow(n->operand, unsupported);
        if (unsupported.empty())
          n->value = throws ? 0 : 1;
        else if (file && !probing)
          impl.reportCheckerError(file, e->token, "the noexcept operator does not decide " + unsupported + " yet");
      }
      return record(e, types.boolType());
    }
    case NodeKind::LiteralExpr: return record(e, typeOfLiteral(static_cast<LiteralExpr*>(e)));
    case NodeKind::ThisExpr: return record(e, thisType ? thisType : unknown(Why::Unsupported));
    case NodeKind::HeldExpr: return record(e, static_cast<HeldExpr*>(e)->holder->ty);
    case NodeKind::CallExpr: return record(e, typeOfCall(static_cast<CallExpr*>(e)));
    case NodeKind::IndexExpr:
    {
      auto* i = static_cast<IndexExpr*>(e);
      checkPostfixAfterParenthesisedType(i->base, i->token);
      const Ty* base = walkExpr(i->base);
      const Ty* firstIndex = nullptr;
      // `p[Integer]` names a typed union's alternative, which no lookup finds: a lone name as the
      // index is judged undeclared once the subscript had its say (judged: its base is known).
      struct TagLater
      {
        Walker& w;
        NameExpr* tag;
        const NameExpr* saved;
        bool judged = false;
        ~TagLater()
        {
          w.undeclaredLater = saved;
          if (tag && judged && !tag->resolved)
            w.reportUndeclared(tag->name, tag->token);
        }
      } later{*this,
              i->args.size() == 1 && i->args[0]->kind == NodeKind::NameExpr && static_cast<NameExpr*>(i->args[0])->name.parts.size() == 1
                ? static_cast<NameExpr*>(i->args[0]) : nullptr,
              undeclaredLater};
      undeclaredLater = later.tag;
      for (Expr* a : i->args)
      {
        const Ty* at = walkExpr(a);
        if (a == i->args[0])
          firstIndex = at;
      }
      undeclaredLater = later.saved;
      const Ty* b = types.stripRef(base);
      if (!b || !b->isKnown())
        return record(e, types.unknown());
      // `2[a]`, `oi[arr]`: the built-in subscript takes the pointer on either side ([expr.sub] 1). The
      // node keeps one shape for all who read it (in a template's own walk it stays as written).
      if (const Ty* pointer = firstIndex && i->args.size() == 1 ? types.unqualified(types.stripRef(firstIndex)) : nullptr;
          pointer && (pointer->kind == TyKind::Pointer || pointer->kind == TyKind::Array) && reversedSubscript(b, base))
      {
        later.judged = true;
        if (ownsNodes())
        {
          std::swap(i->base, i->args[0]);
          i->indexFirst = !i->indexFirst;
        }
        return record(e, types.lref(pointer->inner));
      }
      if (b->kind == TyKind::Pointer || b->kind == TyKind::Array)
      {
        later.judged = true;
        return record(e, types.lref(b->inner));
      }
      if (b->kind == TyKind::Param)
        return record(e, b);
      later.judged = true;
      if (b->kind == TyKind::Class)
      {
        if (ClassDecl* u = typedUnionOf(b); u && i->args.size() == 1)
        {
          // `p[Property::Integer]`: the alternative the enumerator names (design.md 8.5); `p[a]`
          // with a descriptor of alternatives(U): the one it describes.
          std::optional<int64_t> v;
          size_t index = 0;
          if (const Ty* it = firstIndex ? types.stripRef(firstIndex) : nullptr; it && it->kind == TyKind::Member)
          {
            if (alternativeOf(it, index) != u)
              return record(e, unknown(Why::Operator)); // a descriptor of another union, or of a member
            v = int64_t(index);
          }
          else if (firstIndex && types.isDependent(firstIndex))
            return record(e, types.unknown()); // `u[a]` before the loop binds a
          else if (EnumeratorDecl* en = alternativeNamed(u, i->args[0]))
          {
            // `p[Integer]`: an alternative's name as written in the union (design.md 8.5).
            auto* n = static_cast<NameExpr*>(i->args[0]);
            n->resolved = en;
            record(n, impl.tagTypeOf(u, b));
            v = impl.constOfDecl(en, ctx(), 0);
          }
          else
            v = impl.constEval(i->args[0], ctx());
          if (!v || *v < 0 || size_t(*v) >= u->alternatives.size())
            return record(e, unknown(Why::Operator));
          Subst s = b->args.empty() ? Subst{} : impl.substFor(b);
          const Ty* alt = impl.declaredTypeOf(u->alternatives[size_t(*v)], s.pairs.empty() ? nullptr : &s, 0);
          if (!alt || !alt->isKnown())
            return record(e, types.unknown());
          return record(e, types.lref(b->isConst ? types.qualified(alt, true, false) : alt));
        }
        if (types.isDependent(b))
          return record(e, types.unknown());
        MemberResult r = memberOf(b, impl.opSymbols[uint32_t(P::LBracket)], nullptr, i->token, false);
        if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
        {
          bool ambiguous;
          if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypesOf(i->args), &r.subst, b, ambiguous))
            return record(e, returnTypeOfCall(f, &r.subst, i->args, nullptr));
        }
        return record(e, unknown(Why::Operator));
      }
      return record(e, unknown(Why::Operator));
    }
    case NodeKind::MemberExpr: return record(e, typeOfMember(static_cast<MemberExpr*>(e)));
    case NodeKind::UnaryExpr: return record(e, typeOfUnary(static_cast<UnaryExpr*>(e)));
    case NodeKind::BinaryExpr: return record(e, typeOfBinary(static_cast<BinaryExpr*>(e)));
    case NodeKind::ConditionalExpr:
    {
      auto* c = static_cast<ConditionalExpr*>(e);
      const Ty* cond = walkExpr(c->cond);
      const Ty* then = walkExpr(c->then);
      if (c->else_ && c->else_->kind == NodeKind::DefaultValueExpr)
      {
        // `a?->b`: a value of b's type, whatever b is (a reference gives a copy).
        const Ty* value = then ? types.unqualified(types.stripRef(then)) : nullptr;
        if (value && value->kind == TyKind::Array && value->inner)
          value = types.pointer(value->inner);
        record(c->else_, value);
        return record(e, value);
      }
      const Ty* other = walkExpr(c->else_);
      if (!c->then)
        return record(e, cond); // Elvis
      const Ty* a = types.stripRef(then);
      const Ty* b = types.stripRef(other);
      // A branch of unknown type (`self(self, n - 1)` through a generic lambda's `auto&& self`): no
      // guess from the other branch unless that is a class (the one the unknown converts to); a
      // scalar one beside it would be wrong for a wider type (`n <= 1 ? 1 : ...` typed int cut a long
      // long), and codegen takes the type both branches have as each instance emits them.
      if (!a || !b || !a->isKnown())
        return record(e, b && b->isKnown() && b->kind == TyKind::Class ? other : types.unknown());
      if (!b->isKnown())
        return record(e, a->kind == TyKind::Class ? then : types.unknown());
      // `c ? throw x : value`: a throw-expression's branch leaves, and the conditional is the other
      // branch, its type and value category ([expr.cond] 2); both throw: void.
      if (Expr* beside = branchBesideThrow(c))
        return record(e, beside == c->then ? then : other);
      // `target ? &target[i] : 0`: a literal 0 beside a pointer would be that pointer (design.md 4).
      noteZeroNull(c->else_, a->kind == TyKind::Array ? types.decay(a) : a);
      noteZeroNull(c->then, b->kind == TyKind::Array ? types.decay(b) : b);
      // `info ? info->buffer : nullptr`: the other branch's type, an array decayed to a pointer.
      if (a->kind == TyKind::Nullptr)
        return record(e, b->kind == TyKind::Array ? types.decay(b) : other);
      if (b->kind == TyKind::Nullptr)
        return record(e, a->kind == TyKind::Array ? types.decay(a) : then);
      // Two arithmetic or unscoped enumeration operands of two types: the usual arithmetic
      // conversions ([expr.cond]/7.2): `c ? U200 : 0.1f` is a float, `c ? small : -7` an int.
      auto arithmeticOrEnum = [](const Ty* t) { return t->isArithmetic() || (t->kind == TyKind::Enum && !static_cast<EnumDecl*>(t->decl)->isClass); };
      if (arithmeticOrEnum(a) && arithmeticOrEnum(b) && types.unqualified(a) != types.unqualified(b))
        return record(e, arithmetic(operandType(c->then, a), operandType(c->else_, b))); // (`no ? u3 : -1`: an int, u3 a bit-field)
      // `on ? "true" : "false"`, `c ? buffer : fallback`, `c ? derived : base`: arrays decay to
      // pointers, and two pointers meet at their composite pointer type.
      if (a != b && (a->kind == TyKind::Array || a->kind == TyKind::Pointer) && (b->kind == TyKind::Array || b->kind == TyKind::Pointer))
        if (const Ty* composite = impl.compositePointer(types.decay(a), types.decay(b)))
          return record(e, composite);
      // Two functions, one noexcept: an lvalue of the other's type, the noexcept one's reference converting.
      if (a != b && a->kind == TyKind::Function && (impl.losesNoexcept(a, b) || impl.losesNoexcept(b, a)))
        return record(e, types.lref(types.withNoexcept(a, false)));
      // `cond ? "_" : someString`: the class branch's type; the other converts to it. Unless only
      // the class converts to the other (`c ? modifier : 0.0` with `operator double()`): then the
      // other's type, as C++ tries each way.
      if ((a->kind == TyKind::Class) != (b->kind == TyKind::Class))
      {
        const Ty* object = a->kind == TyKind::Class ? then : other;
        const Ty* value = a->kind == TyKind::Class ? other : then;
        const Ty* cls = types.unqualified(types.stripRef(object));
        const Ty* plain = types.unqualified(types.stripRef(value));
        if (plain->kind != TyKind::Pointer && plain->kind != TyKind::Array &&
            impl.conversionRank(object, plain) < Impl::NotViable && !(impl.conversionRank(value, cls) < Impl::NotViable))
          return record(e, plain);
        return record(e, cls);
      }
      // Two classes: the one the other converts to, as C++ tries each way (`first && second ?
      // Undo(first->start, second->end) : first` with first an Optional<Undo> is an Optional<Undo>).
      if (a->kind == TyKind::Class && b->kind == TyKind::Class && types.unqualified(a) != types.unqualified(b))
      {
        // One a base of the other (`negate ? unary : expression` with a UnaryExpression, which
        // derives from NoiseExpression, and a NoiseExpression): the derived one converts to the
        // base, the base never to the derived through a converting constructor (C++ [expr.cond]
        // 4.3.1). Two lvalues, the base qualified at least as the derived, make an lvalue of the
        // base (4.1).
        bool aBase = impl.isBaseClass(types.unqualified(a), types.unqualified(b));
        if (aBase || impl.isBaseClass(types.unqualified(b), types.unqualified(a)))
        {
          const Ty* base = aBase ? a : b;
          const Ty* derived = aBase ? b : a;
          bool lvalues = argumentType(c->then, then, types)->kind == TyKind::LRef && argumentType(c->else_, other, types)->kind == TyKind::LRef;
          bool binds = (base->isConst || !derived->isConst) && (base->isVolatile || !derived->isVolatile);
          return record(e, lvalues && binds ? types.lref(base) : types.unqualified(base));
        }
        bool toB = impl.conversionRank(then, types.unqualified(b)) < Impl::NotViable;
        bool toA = impl.conversionRank(other, types.unqualified(a)) < Impl::NotViable;
        if (toB != toA)
          return record(e, types.unqualified(toB ? b : a));
      }
      return record(e, then);
    }
    case NodeKind::CastExpr:
    {
      auto* c = static_cast<CastExpr*>(e);
      walkType(c->type);
      const Ty* operand = walkExpr(c->operand);
      const Ty* t = typeOf(c->type);
      // `(double (*)(double))twice`: an overloaded function's name takes the overload of the cast's
      // type ([over.over] 1, an explicit conversion), as an initializer's does; it took the first one
      bindTemplateAddresses(c->operand, t);
      operand = c->operand && c->operand->ty ? c->operand->ty : operand;
      return record(e, probedCast(t, operand));
    }
    case NodeKind::NamedCastExpr:
    {
      auto* c = static_cast<NamedCastExpr*>(e);
      walkType(c->type);
      const Ty* operand = walkExpr(c->operand);
      const Ty* t = typeOf(c->type);
      if (c->castKind == Kw::static_cast_)
      {
        bindTemplateAddresses(c->operand, t); // (`static_cast<long long (*)(long long)>(twice)`: as the C-style cast above)
        operand = c->operand && c->operand->ty ? c->operand->ty : operand;
      }
      checkCastConst(c, operand, t); // `static_cast<S&&>(s)` of a const s
      return record(e, c->castKind == Kw::static_cast_ ? probedCast(t, operand) : t);
    }
    case NodeKind::SizeofExpr:
    {
      auto* z = static_cast<SizeofExpr*>(e);
      walkType(z->type);
      walkExpr(z->operand);
      // `sizeof(s->items)` of a flexible array member (or of `extern int table[];`): an array
      // without a length has no size ([expr.sizeof] 1), never 0.
      if (!z->isPack && z->sizeofKind != Kw::alignof_)
        if (VariableDecl* v = unboundedArray(z->operand ? static_cast<Node*>(z->operand) : z->type))
          impl.reportCheckerError(file, e->token, "sizeof of " + std::string(symbols.name(v->name.last().symbol)) +
                                                      ", an array without a length (a flexible array member), which has no size");
      return record(e, types.sizeType());
    }
    case NodeKind::NewExpr:
    {
      auto* n = static_cast<NewExpr*>(e);
      walkExprs(n->placement);
      if (n->placement.size() == 1)
        if (Decl* constant = constObjectPlaced(n->placement[0]))
        {
          uint32_t name = constant->kind == NodeKind::ParamDecl ? static_cast<ParamDecl*>(constant)->name
                                                                 : static_cast<VariableDecl*>(constant)->name.last().symbol;
          impl.reportCheckerError(file, n->token, "a placement new over " + (name == UINT32_MAX ? std::string("an object") : "'" + std::string(symbols.name(name)) + "'") +
                                                      ", which is declared const: a const object is never replaced (design.md 21)");
        }
      walkType(n->type);
      walkExprs(n->args);
      const Ty* t = typeOf(n->type);
      bool array = t->kind == TyKind::Array;
      const Ty* wholeArray = array ? t : nullptr;
      if (array)
        t = t->inner;
      else if (impl.isUndeducedClassTemplate(t))
      {
        // `new DeleteHelper(old.release())`: the class's arguments from a constructor (CTAD); `new Pair{a, b}`:
        // from what the braces hold, or an aggregate's members.
        Span<Expr*> args = n->args;
        if (n->braced && args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr)
          args = static_cast<BraceInitExpr*>(args[0])->args;
        std::vector<const Ty*> argTypes = argTypesOf(args);
        if (const Ty* deduced = impl.deduceClassArgs(t, argTypes))
          t = deduced;
        else if (const Ty* aggregate = n->braced ? impl.deduceAggregateArgs(t, argTypes) : nullptr)
          t = aggregate;
      }
      if (!n->braced && !n->args.empty())
        bindOverloadSetsOfConstruction(t, n->args);
      if (impl.holdsText(t) && !inGenericTemplate())
        reportTextError(n->token, impl.isTextClosure(t) ? "a lambda capturing a Text cannot be stored (design.md 14)" : textStored("allocated"));
      if (!n->braced && array && !n->args.empty())
        checkParenthesizedArray(n->args, wholeArray, n->token); // `new int[3](8, 9)` (C++20)
      else if (!n->braced)
      {
        checkConstruction(t, n->args, n->token);
        packText(n->args, textConstructor(t, n->args), n->token); // `new Problem("bad ", point)`
      }
      else if (n->args.size() == 1 && n->args[0] && n->args[0]->kind == NodeKind::BraceInitExpr)
      {
        if (auto* list = static_cast<BraceInitExpr*>(n->args[0]); list->designators.empty() && !list->text)
          checkListNarrowing(list->args, t); // `new int{1.5}`
      }
      else
        checkListNarrowing(n->args, t);
      chooseAllocation(n, t, array);
      return record(e, t->isKnown() ? types.pointer(t) : t);
    }
    case NodeKind::DeleteExpr:
    {
      auto* d = static_cast<DeleteExpr*>(e);
      walkExpr(d->operand);
      d->deallocator = nullptr;
      const Ty* p = d->operand->ty ? types.unqualified(types.stripRef(d->operand->ty)) : nullptr;
      if (!d->global && p && p->kind == TyKind::Pointer && p->inner && types.unqualified(p->inner)->kind == TyKind::Class)
      {
        bool declared = false;
        const Ty* cls = types.unqualified(p->inner);
        d->deallocator = usualDeallocation(cls, d->token, d->sizedDeallocation, declared, d->isArray);
        std::string name = d->isArray ? "operator delete[]" : "operator delete";
        if (!d->deallocator && declared)
          reportHere(d->token, std::string(d->isArray ? "the delete[] of " : "the delete of a ") + types.toString(cls, &symbols) + " finds its class's " + name +
                                   ", and none of them is the usual one, " + name + "(void*) or " + name + "(void*, size_t)");
      }
      return record(e, types.voidType());
    }
    case NodeKind::LambdaExpr:
    {
      auto* l = static_cast<LambdaExpr*>(e);
      bool generic = !l->templateParams.empty(); // a template: its call operator is typed per call
      std::vector<LocalStack::Item> environment;
      if (generic)
        environment = locals.items; // what the body can see, kept for typing it for each call
      size_t mark = locals.mark();
      for (const LambdaCapture& cap : l->captures)
        if (cap.decl)
          walkLocalDecl(cap.decl); // init-capture: a variable of the closure, typed from its initializer
        else if (Decl* captured = cap.isThis ? nullptr : locals.find(cap.symbol); captured && captured->when)
        {
          bool undecided = false;
          whenUse(cap.symbol, l->token, captured, undecided); // (design.md 19: a use of the local)
        }
      // Its captures by copy as its body sees them: const where the lambda is not `mutable`, as its
      // operator() is ([expr.prim.lambda.closure] 12), so that writing one is an error as in C++ (the
      // parser dropped `mutable`, and every copy was writable:
      // language/lambda_copy_capture_is_const_unless_mutable); a mutable one's as its variable is
      // (over a const one of a lambda around it). An item over each, its type the copy's.
      size_t copiesMark = constCopyCaptures.size();
      {
        auto variable = [](Decl* d) {
          return d && (d->kind == NodeKind::ParamDecl || (d->kind == NodeKind::VariableDecl && !(d->flags & (DF_Static | DF_Typedef))));
        };
        // The copy's type for the item at i: its variable's, a reference's referent, const unless mutable.
        auto copyType = [&](size_t i) -> const Ty* {
          const Ty* t = locals.items[i].type ? locals.items[i].type : impl.declaredTypeOf(locals.items[i].decl, activeSubst, 0);
          if (!t || !t->isKnown() || impl.containsAuto(t))
            return nullptr;
          t = types.stripRef(t);
          if (t->kind == TyKind::Array || t->kind == TyKind::Pack)
            return nullptr; // (an array's elements, a pack's: not told here)
          return l->isMutable ? t : types.qualified(t, true, t->isVolatile);
        };
        auto copy = [&](size_t i) {
          if (const Ty* t = copyType(i))
          {
            LocalStack::Item item = locals.items[i];
            item.type = t;
            locals.items.push_back(item);
            if (generic)
              environment.push_back(item);
            if (!l->isMutable)
              constCopyCaptures.push_back(item.decl);
          }
        };
        auto outer = [&](uint32_t symbol) -> std::optional<size_t> {
          for (size_t i = std::min(mark, locals.items.size()); i-- > 0;)
            if (locals.items[i].symbol == symbol)
              return variable(locals.items[i].decl) ? std::optional<size_t>(i) : std::nullopt;
          return std::nullopt;
        };
        std::vector<uint32_t> named;
        for (const LambdaCapture& cap : l->captures)
        {
          if (cap.isThis)
            continue;
          named.push_back(cap.symbol);
          if (cap.decl)
          {
            // `[k = 1]`: the closure's own variable, const too unless mutable (`[&k = x]` refers).
            for (size_t i = locals.items.size(); i-- > mark;)
              if (locals.items[i].decl == cap.decl)
              {
                if (const Ty* t = cap.byRef ? nullptr : copyType(i))
                {
                  locals.items[i].type = t;
                  if (!l->isMutable)
                    constCopyCaptures.push_back(cap.decl);
                }
                break;
              }
          }
          else if (!cap.byRef)
            if (std::optional<size_t> i = outer(cap.symbol))
              copy(*i);
        }
        if (l->defaultCapture == 1) // `[=]`: every variable around it the body names, by copy
          for (size_t i = std::min(mark, locals.items.size()); i-- > 0;)
          {
            uint32_t symbol = locals.items[i].symbol;
            if (std::find(named.begin(), named.end(), symbol) != named.end())
              continue;
            named.push_back(symbol); // (the innermost of the name only)
            if (variable(locals.items[i].decl))
              copy(i);
          }
      }
      pushTemplateParams(l->templateParams);
      std::vector<const Ty*> params;
      for (Decl* p : l->params)
      {
        auto* pd = static_cast<ParamDecl*>(p);
        walkParam(pd);
        const Ty* t = impl.declaredTypeOf(p, activeSubst, 0);
        // As this walk sees the type, its locals included: `const Layers::value_type&` with
        // `using Layers = ...;` in a function around the lambda, which the declaration's own
        // context does not see, nor the template parameters of a lambda around this one
        // (`[&](ChooseButton<IDType>*)` in `[&]<class IDType>(...)`, which it read as a draft
        // class). The parameter's uses take it from the local.
        if (const Ty* walked = pd->type ? typeOf(pd->type) : nullptr;
            walked && walked->isKnown() && !impl.containsAuto(walked) && !Impl::mentionsUnknown(walked) &&
            (((!t || !t->isKnown() || types.isDependent(t)) && !types.isDependent(walked)) || (t && Impl::mentionsUnknown(t))))
        {
          t = walked->kind == TyKind::Array ? types.decay(walked) : walked;
          if (!locals.items.empty() && locals.items.back().decl == p)
            locals.items.back().type = t;
        }
        params.push_back(t);
      }
      walkType(l->returnType);
      const Ty* result = nullptr;
      if (l->returnType)
        result = typeOf(l->returnType);
      returnFrames.push_back(nullptr);
      decltypeReturns.push_back(l->returnType && l->returnType->kind == NodeKind::AutoType && static_cast<AutoType*>(l->returnType)->isDecltypeAuto);
      walkStmt(l->body);
      markTextCapture(l, mark);
      const Ty* deduced = returnFrames.back();
      returnFrames.pop_back();
      decltypeReturns.pop_back();
      if (!result)
        result = deduced ? types.decay(deduced) : types.voidType();
      else if (impl.containsAuto(result))
        result = deduced ? impl.deducedShape(result, deduced) : types.voidType(); // `-> decltype(auto)`, `-> auto&`
      if (generic && (!result->isKnown() || types.isDependent(result)))
        result = nullptr; // deduced per call
      locals.pop(mark);
      constCopyCaptures.resize(copiesMark);
      // In a template, the lambda is a different type in each instantiation (its body differs).
      // The mark telling them apart is a hash of the arguments as printed (with their classes'
      // full paths: two classes named LineInfo are two arguments), in parameter-name order, so
      // that it (and every name printed with it) is the same in every run. A body typed again
      // with no arguments at all (a function's `auto` return deduced on demand) is no
      // instantiation: its lambda is the one of the plain walk.
      int64_t instance = 0;
      if (activeSubst && !activeSubst->pairs.empty())
      {
        std::vector<std::string> printed;
        for (auto& [d, ty] : activeSubst->pairs)
          printed.push_back(std::string(symbols.name(static_cast<TemplateParamDecl*>(d)->name)) + "=" + types.toString(ty, &symbols, true));
        std::sort(printed.begin(), printed.end());
        uint64_t h = 1469598103934665603ull;
        for (const std::string& p : printed)
          for (unsigned char c : p + ";")
            h = (h ^ c) * 1099511628211ull;
        instance = int64_t(h ? h : 1);
      }
      const Ty* lambdaTy = types.lambda(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, l->closure, instance);
      if (generic)
      {
        Impl::LambdaEnv env;
        env.lambda = l;
        env.locals = std::move(environment);
        env.scope = scope;
        env.thisType = thisType;
        if (activeSubst)
          env.subst = *activeSubst;
        env.tokens = &tokens;
        env.text = text;
        env.file = file;
        env.depth = instantiationDepth;
        std::lock_guard lock(impl.lambdaEnvMutex);
        impl.lambdaEnvs[lambdaTy] = std::move(env);
      }
      return record(e, lambdaTy);
    }
    case NodeKind::BraceInitExpr:
      walkExprs(static_cast<BraceInitExpr*>(e)->args);
      if (static_cast<BraceInitExpr*>(e)->text)
        if (const Ty* text = impl.textType())
          return record(e, text); // a call's arguments packed for its Text parameter (packText)
      return record(e, unknown(Why::Unsupported));
    case NodeKind::ThrowExpr:
      walkExpr(static_cast<ThrowExpr*>(e)->operand);
      return record(e, types.voidType());
    case NodeKind::FunctionalCastExpr:
    {
      auto* c = static_cast<FunctionalCastExpr*>(e);
      walkType(c->type);
      const Ty* operand = nullptr;
      for (Expr* a : c->args)
      {
        const Ty* at = walkExpr(a);
        if (a == c->args[0])
          operand = at;
      }
      const Ty* t = typeOf(c->type);
      if (t->kind == TyKind::Auto && c->args.size() == 1 && operand)
        t = types.decay(operand); // auto(x)
      if (c->args.size() == 1 && !c->braced)
        t = probedCast(t, operand); // `bool(instance)` in a requirement
      if (impl.isUndeducedClassTemplate(t))
      {
        // `Pair(a, b)`, `Span(p, n)`, `Pair{a, b}` in an expression: the class's arguments from a
        // constructor, or from an aggregate's members (CTAD).
        Span<Expr*> args = c->args;
        if (c->braced && args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr)
          args = static_cast<BraceInitExpr*>(args[0])->args;
        std::vector<const Ty*> argTypes = argTypesOf(args);
        if (const Ty* deduced = impl.deduceClassArgs(t, argTypes))
          t = deduced;
        else if (const Ty* aggregate = c->braced ? impl.deduceAggregateArgs(t, argTypes) : nullptr)
          t = aggregate;
      }
      if (!c->args.empty())
      {
        // (`NamedFunction{set}`: the list's elements, for the aggregate's members or the constructor.)
        Span<Expr*> args = c->args;
        if (c->braced && args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr)
        {
          bindTemplateAddresses(args[0], t);
          args = static_cast<BraceInitExpr*>(args[0])->args;
        }
        bindOverloadSetsOfConstruction(t, args);
      }
      if (!c->braced)
      {
        checkConstruction(t, c->args, c->token);
        packText(c->args, textConstructor(t, c->args), c->token); // `Problem("bad ", point)`, `Problem problem("bad ", point);`
      }
      else if (c->args.size() == 1 && c->args[0] && c->args[0]->kind == NodeKind::BraceInitExpr)
      {
        if (auto* list = static_cast<BraceInitExpr*>(c->args[0]); list->designators.empty() && !list->text)
          checkListNarrowing(list->args, t); // `int{1.5}`, `Point{1.5, 2}`
      }
      else
        checkListNarrowing(c->args, t);
      return record(e, t);
    }
    case NodeKind::TypeidExpr:
    {
      auto* t = static_cast<TypeidExpr*>(e);
      walkNode(t->operand);
      if (t->operand && uint8_t(t->operand->kind) <= uint8_t(NodeKind::PackType))
      {
        // typeid(Widget), or typeid(q) with q a variable the parser read as a type name.
        auto* n = t->operand->kind == NodeKind::NamedType ? static_cast<NamedType*>(t->operand) : nullptr;
        Lookup l = n ? impl.lookupQualified(n->name, scope, &locals, 0, activeSubst) : Lookup{};
        if (l.decl && (l.decl->kind == NodeKind::VariableDecl || l.decl->kind == NodeKind::ParamDecl) && !(l.decl->flags & DF_Typedef))
        {
          t->operandDecl = l.decl;
          t->operandType = impl.declaredTypeOf(l.decl, activeSubst, 0);
        }
        else
          t->operandType = typeOf(static_cast<Type*>(t->operand));
      }
      if (Decl* info = impl.typeInfoDecl())
        return record(e, types.classType(info, {})); // the library's TypeInfo; the code generator fills in the name
      return record(e, unknown(Why::Unsupported));
    }
    case NodeKind::FoldExpr:
    {
      auto* f = static_cast<FoldExpr*>(e);
      const Ty* l = walkExpr(f->lhs);
      const Ty* r = walkExpr(f->rhs);
      if (f->op == uint32_t(P::Comma))
        return record(e, types.voidType());
      const Ty* pattern = l ? l : r;
      const Ty* init = l && r ? (l->kind == TyKind::Pack ? r : l) : nullptr;
      if (pattern && pattern->kind == TyKind::Pack && pattern->args.empty())
      {
        // An empty pack: a binary fold is its initial value (`(xs + ... + 0)` of `[](auto... xs)`
        // called with none: an int, which was void), a unary && or || true or false.
        if (init && init->kind != TyKind::Pack)
          return record(e, init);
        if (f->op == uint32_t(P::AndAnd) || f->op == uint32_t(P::OrOr))
          return record(e, types.boolType());
      }
      if (pattern && pattern->kind == TyKind::Pack)
        pattern = pattern->args.empty() ? types.voidType() : pattern->args[0];
      return record(e, pattern ? pattern : types.unknown());
    }
    case NodeKind::RequiresExpr:
    {
      // `requires { t.save(); }`: true when every requirement is well-typed for the types at
      // hand, probed on a scratch walker so that the misses count in no statistic.
      auto* r = static_cast<RequiresExpr*>(e);
      ResolveStats scratch;
      Walker probe(impl, scratch, tokens, text);
      probe.scope = scope;
      probe.locals = locals;
      probe.activeSubst = activeSubst;
      probe.thisType = thisType;
      probe.probing = true;
      for (Decl* p : r->params)
      {
        // As the walk sees its type (`requires(const T& value)` in `[]<class T>(T) { ... }`, or
        // with `using T = L::ThePrototypeType;` in the lambda's body): the declaration's own
        // context sees neither a lambda's template parameters nor its body's aliases.
        auto* pd = static_cast<ParamDecl*>(p);
        probe.walkParam(pd);
        if (const Ty* walked = pd->type ? probe.typeOf(pd->type) : nullptr;
            walked && walked->isKnown() && !impl.containsAuto(walked) && !Impl::mentionsUnknown(walked))
          probe.locals.items.back().type = walked->kind == TyKind::Array ? types.decay(walked) : walked;
      }
      // Each requirement on a copy of its own, its names' access checked from where the
      // requires-expression is (Impl::namesOnlyAccessible): the function walked, its class and
      // the classes around it, as the instance it is (self).
      std::vector<Scope*> context;
      if (bodyOf)
        impl.accessContext(bodyOf, context);
      for (Scope* s = scope; s; s = s->parent)
        if (s->kind == ScopeKind::Class)
          context.push_back(s);
      const Ty* self = nullptr;
      if (activeSubst && bodyOf)
        if (Decl* cls = impl.owningClassOf(bodyOf); cls && cls->kind == NodeKind::ClassDecl)
          self = impl.substitute(impl.selfType(cls), *activeSubst);
      InstanceCopier copier(impl.instanceArena());
      bool satisfied = true;
      for (Type* t : r->typeRequirements)
      {
        Type* copy = copier.typeAlone(t);
        probe.walkType(copy); // (its names resolved, for their access)
        if (const Ty* ty = probe.typeOf(copy); !ty || !ty->isKnown() || (!impl.types.isDependent(ty) && !impl.namesOnlyAccessible(copy, bodyOf, context, self)))
          satisfied = false;
      }
      std::vector<const Ty*> resultTypes;
      bool dependent = false;
      for (Expr* requirement : r->requirements)
      {
        Expr* copy = copier.expression(requirement);
        const Ty* ty = probe.walkExpr(copy);
        if (!ty || !ty->isKnown() || probe.namesFunctionsWithoutTarget(copy) ||
            (!impl.types.isDependent(ty) && !impl.namesOnlyAccessible(copy, bodyOf, context, self)))
          satisfied = false;
        dependent = dependent || (ty && impl.types.isDependent(ty));
        resultTypes.push_back(ty ? argumentType(copy, ty, impl.types) : nullptr);
      }
      // The compound requirements' result constraints and the nested requirements (requirementsExtraHold).
      if (satisfied && !dependent && (!r->nested.empty() || std::any_of(r->resultConstraints.begin(), r->resultConstraints.end(), [](Type* t) { return t != nullptr; })))
      {
        Decl* owner = bodyOf ? static_cast<Decl*>(bodyOf) : nullptr;
        Subst none;
        if (owner)
          if (std::optional<bool> extra = impl.requirementsExtraHold(r, resultTypes, owner, activeSubst ? *activeSubst : none); extra && !*extra)
            satisfied = false;
      }
      r->satisfied = satisfied;
      return record(e, types.boolType());
    }
    case NodeKind::ParenExpr:
      return record(e, walkExpr(static_cast<ParenExpr*>(e)->inner));
    default:
      return record(e, unknown(Why::Unsupported));
  }
}
