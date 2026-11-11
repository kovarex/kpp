// GlobalScope::Impl::Walker::checkFlow (its declaration in Walker.hpp): the checks that follow a body's paths, after the
// walk typed it (kovarex, 2026-11-11, items 2 and 3 of thread D's survey of missing checks), each an error:
// - A local read before anything is assigned to it ("ok" to "zero becomes the official rule, and a read before any
//   assignment that the compiler can prove is an error"). kpp zeroes a local declared without an initializer, so such a
//   read is defined, the zero; in C++ it is undefined, and in either it is nearly always a mistake: `int x = 0;` says
//   the zero is meant. Proven: on every path from the declaration to the read, nothing that may assign the local comes
//   first. A read only some paths reach unassigned is not judged (C++ compilers' "may be used uninitialized"): its value
//   is the zero kpp gives it.
// - An integer division or remainder by a constant 0, and a constant index outside a built-in array, where they run: a
//   branch a constant decides away is not judged (`if (shards != 0) h % shards` with a constexpr 0).
#include "GlobalScopeImpl.hpp"
#include "../Ast/AstWalk.hpp"
#include <algorithm>
#include <iterator>

namespace
{
// The followed locals certainly unassigned at a point of a body (their indices, sorted), and whether the point can be
// reached at all (nothing after a return is judged).
struct Flow
{
  bool reachable = true;
  std::vector<uint32_t> unassigned;

  bool has(uint32_t i) const { return std::binary_search(this->unassigned.begin(), this->unassigned.end(), i); }
  void add(uint32_t i)
  {
    auto at = std::lower_bound(this->unassigned.begin(), this->unassigned.end(), i);
    if (at == this->unassigned.end() || *at != i)
      this->unassigned.insert(at, i);
  }
  void remove(uint32_t i)
  {
    auto at = std::lower_bound(this->unassigned.begin(), this->unassigned.end(), i);
    if (at != this->unassigned.end() && *at == i)
      this->unassigned.erase(at);
  }
  // Where another path comes in: unassigned only where it is on both; a path no one reaches brings nothing.
  void join(const Flow& other)
  {
    if (!other.reachable)
      return;
    if (!this->reachable)
    {
      *this = other;
      return;
    }
    std::vector<uint32_t> both;
    std::set_intersection(this->unassigned.begin(), this->unassigned.end(), other.unassigned.begin(), other.unassigned.end(), std::back_inserter(both));
    this->unassigned = std::move(both);
  }
  static Flow none()
  {
    Flow f;
    f.reachable = false;
    return f;
  }
};

// How what is given to a call, or bound to a reference, is taken: read (by value, by const reference) or possibly
// written (by another reference, or by what the walk did not decide).
enum class Taken : uint8_t
{
  Read,
  Written,
};

Expr* strip(Expr* e)
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  return e;
}

// How a parameter or a variable of type t takes what it is given.
Taken takenBy(const Ty* t)
{
  if (!t || !t->isKnown() || t->kind == TyKind::Pack || t->kind == TyKind::Auto)
    return Taken::Written;
  if (t->kind == TyKind::LRef)
    return t->inner && t->inner->isConst ? Taken::Read : Taken::Written;
  if (t->kind == TyKind::RRef)
    return Taken::Written; // (a forwarding reference binds an lvalue as T&)
  return Taken::Read;
}

// Whether a reference of type t (null: not known) may assign what it is bound to: a declared one, a cast to one.
bool bindsWritable(const Ty* t) { return !t || (t->isReference() && takenBy(t) == Taken::Written); }

bool nested(const Node* n) { return n->kind == NodeKind::FunctionDecl || n->kind == NodeKind::ClassDecl; }
}

// (a member of the Walker, which may name GlobalScope's Impl)
struct GlobalScope::Impl::Walker::FlowChecks
{
public:
  explicit FlowChecks(Walker& w) : w(w) {}

  // One body, a function's or a lambda's (a lambda's locals are its own; what it captures is the enclosing body's).
  void run(Span<Decl*> params, Stmt* body)
  {
    if (!body || this->contains(body, {NodeKind::ErrorStmt, NodeKind::ErrorExpr}))
      return; // (what the parser could not read)
    // A goto or a label: the paths are not the statements', so no read is judged; what a constant decides still is.
    this->judgeReads = !this->contains(body, {NodeKind::GotoStmt, NodeKind::LabelStmt});
    for (Decl* p : params)
      if (p && p->kind == NodeKind::ParamDecl)
        this->visible.emplace_back(static_cast<ParamDecl*>(p)->name, UINT32_MAX);
    Flow flow;
    this->stmt(body, flow);
  }

  std::vector<LambdaExpr*> lambdas; // the lambdas the body makes, each judged on its own after it

private:
  Walker& w;
  std::unordered_map<const Decl*, uint32_t> index; // a followed local's
  std::vector<VariableDecl*> locals;                // by index
  std::vector<bool> reported;
  // The names in scope, innermost last (symbol, index; UINT32_MAX: a local or parameter not followed): what a capture
  // `[x]` names.
  std::vector<std::pair<uint32_t, uint32_t>> visible;
  std::vector<std::vector<Flow>*> exits; // where a break goes: the innermost loop's or switch's paths out, last
  bool judgeReads = true;
  Expr* addressOf = nullptr; // the operand of the `&` being walked: `&a[3]`, the pointer past an array's end

  // Whether the body (not a nested function's, class's or lambda's) has a node of one of these kinds.
  bool contains(Stmt* body, std::initializer_list<NodeKind> kinds)
  {
    bool found = false;
    forEachNodeExcept(body, [&](Node* n)
    {
      for (NodeKind k : kinds)
        found = found || n->kind == k;
    }, [&](Node* n) { return nested(n) || n->kind == NodeKind::LambdaExpr; });
    return found;
  }

  // Whether e is made only of what a constant is made of: literals, enumerators, variables of a namespace (or a class's
  // static ones), operators, casts, sizeof a type and calls of constexpr functions on them. Only such an expression is
  // evaluated here, after the walk: a local's name, a member's or a name the walk did not resolve is not one (the walk's
  // scopes are gone, and the evaluator would look there).
  bool constantShaped(Expr* e, int depth = 0)
  {
    e = strip(e);
    if (!e || depth > 64)
      return false;
    switch (e->kind)
    {
      case NodeKind::LiteralExpr:
        return true;
      case NodeKind::NameExpr:
      {
        Decl* d = static_cast<NameExpr*>(e)->resolved;
        if (!d)
          return false;
        if (d->kind == NodeKind::EnumeratorDecl)
          return true;
        return d->kind == NodeKind::VariableDecl &&
               (!d->parent || d->parent->kind == NodeKind::NamespaceDecl || (d->parent->kind == NodeKind::ClassDecl && (d->flags & DF_Static)));
      }
      case NodeKind::UnaryExpr:
      {
        auto* u = static_cast<UnaryExpr*>(e);
        P op = P(u->op);
        return !u->isKeyword && u->call == OperatorCall::Builtin && op != P::PlusPlus && op != P::MinusMinus && op != P::Amp && op != P::Star &&
               this->constantShaped(u->operand, depth + 1);
      }
      case NodeKind::BinaryExpr:
      {
        auto* b = static_cast<BinaryExpr*>(e);
        return b->call == OperatorCall::Builtin && (b->isKeyword || (!Walker::isAssignmentOp(P(b->op)) && P(b->op) != P::Comma)) &&
               this->constantShaped(b->lhs, depth + 1) && this->constantShaped(b->rhs, depth + 1);
      }
      case NodeKind::ConditionalExpr:
      {
        auto* c = static_cast<ConditionalExpr*>(e);
        return c->then && this->constantShaped(c->cond, depth + 1) && this->constantShaped(c->then, depth + 1) && this->constantShaped(c->else_, depth + 1);
      }
      case NodeKind::CastExpr: return this->constantShaped(static_cast<CastExpr*>(e)->operand, depth + 1);
      case NodeKind::NamedCastExpr: return this->constantShaped(static_cast<NamedCastExpr*>(e)->operand, depth + 1);
      case NodeKind::FunctionalCastExpr:
        for (Expr* a : static_cast<FunctionalCastExpr*>(e)->args)
          if (!this->constantShaped(a, depth + 1))
            return false;
        return static_cast<FunctionalCastExpr*>(e)->args.size() == 1;
      case NodeKind::SizeofExpr: return static_cast<SizeofExpr*>(e)->type && !static_cast<SizeofExpr*>(e)->operand;
      case NodeKind::CallExpr:
      {
        auto* c = static_cast<CallExpr*>(e);
        Expr* callee = strip(c->callee);
        Decl* g = callee && callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(callee)->resolved : nullptr;
        if (!g || g->kind != NodeKind::FunctionDecl || !(g->flags & (DF_Constexpr | DF_Consteval)))
          return false;
        for (Expr* a : c->args)
          if (!this->constantShaped(a, depth + 1))
            return false;
        return true;
      }
      default:
        return false;
    }
  }

  // e's value when it is a constant, else none.
  std::optional<int64_t> valueOf(Expr* e)
  {
    if (!e || !this->constantShaped(e))
      return std::nullopt;
    return this->w.impl.constEval(e, this->w.ctx());
  }

  // A condition's value when it is a constant (`shards != 0` of a constexpr shards), else none.
  std::optional<bool> constant(Node* c)
  {
    if (!c || uint8_t(c->kind) < uint8_t(NodeKind::NameExpr))
      return std::nullopt;
    std::optional<int64_t> v = this->valueOf(static_cast<Expr*>(c));
    return v ? std::optional<bool>(*v != 0) : std::nullopt;
  }

  void report(Expr* at, const std::string& message) { this->w.impl.reportCheckerError(this->w.file, strip(at)->token, message); }

  // An integer `/` or `%` (or `/=`, `%=`) whose divisor is a constant 0.
  void divisor(BinaryExpr* b, const char* op, const Flow& flow)
  {
    if (!flow.reachable || b->call != OperatorCall::Builtin || !b->ty || !b->ty->isKnown())
      return;
    const Ty* t = this->w.types.unqualified(this->w.types.stripRef(b->ty));
    if (t->kind != TyKind::Int && t->kind != TyKind::Char && t->kind != TyKind::Bool && t->kind != TyKind::Enum)
      return; // (a floating division by 0 is IEEE's infinity or NaN)
    if (std::optional<int64_t> v = this->valueOf(b->rhs); v && *v == 0)
      this->report(b->rhs, std::string("division by zero: the divisor of this integer `") + op + "` is the constant 0");
  }

  // Whether e names a class's last data member (C's flexible array: `int data[1];` at a struct's end, indexed past it in
  // memory allocated larger, as clang takes every trailing array).
  bool trailingMember(Expr* e)
  {
    e = strip(e);
    Decl* m = e && e->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(e)->resolved : nullptr;
    if (!m || m->kind != NodeKind::VariableDecl || !m->parent || m->parent->kind != NodeKind::ClassDecl)
      return false;
    Decl* last = nullptr;
    for (Decl* d : membersRead(static_cast<ClassDecl*>(m->parent)))
      if (d->kind == NodeKind::VariableDecl && !(d->flags & (DF_Static | DF_Typedef)))
        last = d;
    return last == m;
  }

  // A built-in array's constant index outside it: `a[3]` of 3 elements (but `&a[3]`, the pointer past its end), `a[-1]`.
  void checkIndex(IndexExpr* x, bool addressed, const Flow& flow)
  {
    if (!flow.reachable || x->args.size() != 1 || !x->base->ty || !x->base->ty->isKnown())
      return;
    const Ty* t = this->w.types.unqualified(this->w.types.stripRef(x->base->ty));
    if (t->kind != TyKind::Array || t->count == 0 || this->w.types.isDependent(t) || this->trailingMember(x->base))
      return;
    std::optional<int64_t> v = this->valueOf(x->args[0]);
    int64_t n = int64_t(t->count);
    if (!v || (*v >= 0 && (*v < n || (*v == n && addressed))))
      return;
    this->report(x->args[0], "the constant index " + std::to_string(*v) + " is outside the array's " + std::to_string(n) + " elements (" +
                               this->w.types.toString(t, &this->w.symbols) + ", 0 to " + std::to_string(n - 1) + ")");
  }

  // The followed local a bare name is (through parentheses), or UINT32_MAX.
  uint32_t followed(Expr* e) const
  {
    e = strip(e);
    if (!e || e->kind != NodeKind::NameExpr)
      return UINT32_MAX;
    auto it = this->index.find(static_cast<NameExpr*>(e)->resolved);
    return it == this->index.end() ? UINT32_MAX : it->second;
  }

  // The type a declared variable has (null: not known).
  const Ty* typeOf(VariableDecl* v)
  {
    const Ty* t = v->cachedType.load(std::memory_order_acquire);
    if (!t)
      t = this->w.impl.declaredTypeOf(v, nullptr, 0);
    return t && t->isKnown() ? t : nullptr;
  }

  // A local followed here: a variable of a scalar type declared without an initializer (not static, thread_local,
  // extern or constexpr; not a binding, not one `when` declares).
  bool follows(VariableDecl* v)
  {
    if (v->init || v->name.empty() || v->name.last().symbol == UINT32_MAX || v->when || !v->structuredBindings.empty() || v->tupleBinding ||
        (v->flags & (DF_Static | DF_Extern | DF_Typedef | DF_ThreadLocal | DF_Constexpr)))
      return false;
    const Ty* t = this->typeOf(v);
    if (!t || this->w.types.isDependent(t) || Impl::mentionsUnknown(t) || this->w.impl.containsAuto(t))
      return false;
    switch (t->kind)
    {
      case TyKind::Bool: case TyKind::Char: case TyKind::Int: case TyKind::Float: case TyKind::Enum: case TyKind::Pointer:
      case TyKind::Nullptr: case TyKind::MemberPointer:
        return true;
      default:
        return false;
    }
  }

  // What a call calls: its type (a Function) and the function, when the walk decided one.
  struct Callee
  {
    const Ty* type = nullptr;
    FunctionDecl* function = nullptr;
  };
  Callee calleeOf(Expr* callee)
  {
    callee = strip(callee);
    Decl* g = callee && callee->kind == NodeKind::NameExpr     ? static_cast<NameExpr*>(callee)->resolved
              : callee && callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                               : nullptr;
    if (g && g->kind == NodeKind::FunctionDecl)
    {
      const Ty* ft = this->w.impl.declaredTypeOf(g, nullptr, 0);
      return {ft && ft->kind == TyKind::Function ? ft : nullptr, static_cast<FunctionDecl*>(g)};
    }
    if (g || !callee || !callee->ty || !callee->ty->isKnown())
      return {};
    // (a pointer to a function: its type says; an object called, a functor or a Function, does not)
    const Ty* t = this->w.types.unqualified(this->w.types.stripRef(callee->ty));
    if (t->kind == TyKind::Pointer && t->inner)
      t = this->w.types.unqualified(t->inner);
    return t->kind == TyKind::Function ? Callee{t, nullptr} : Callee{};
  }

  // The followed local a method is called on itself (`d.turn()`, a kpp enum's method), and how: a const method reads it,
  // another may assign it (`this = North;`; a static one, or one the walk did not decide, is taken so too). UINT32_MAX:
  // none.
  uint32_t calledOn(Expr* callee, const Callee& c, Taken& taken)
  {
    callee = strip(callee);
    if (!callee || callee->kind != NodeKind::MemberExpr || static_cast<MemberExpr*>(callee)->op != P::Dot)
      return UINT32_MAX;
    uint32_t i = this->followed(static_cast<MemberExpr*>(callee)->base);
    if (i != UINT32_MAX)
      taken = c.function && (c.function->flags & DF_Const) && !(c.function->flags & DF_Static) ? Taken::Read : Taken::Written;
    return i;
  }

  // How a call takes its argument i.
  Taken argumentTaken(const Callee& c, size_t i)
  {
    if (!c.type)
      return Taken::Written;
    bool packLast = c.function && !c.function->params.empty() && c.function->params[c.function->params.size() - 1]->kind == NodeKind::ParamDecl &&
                    static_cast<ParamDecl*>(c.function->params[c.function->params.size() - 1])->isPack;
    if (i < c.type->args.size() && !(packLast && i + 1 >= c.type->args.size()))
      return takenBy(c.type->args[i]);
    if (packLast && !c.type->args.empty())
      return takenBy(c.type->args[c.type->args.size() - 1]); // (a pack takes the rest as its pattern says: `Args&&...` may write them)
    return c.type->variadic ? Taken::Read : Taken::Written;
  }

  // How an operator takes its operand (0 its left, 1 its right): an operator function's parameter, or the built-in
  // operator's read.
  Taken operandTaken(BinaryExpr* b, int which)
  {
    if (b->call == OperatorCall::Builtin || b->call == OperatorCall::Rewritten)
      return Taken::Read;
    if (b->call != OperatorCall::Function || !b->callee || (b->calleeMember && which == 0))
      return Taken::Written; // (undecided: a template's operand of a type it depends on, `stream >> x`; a member's object)
    const Ty* ft = this->w.impl.declaredTypeOf(b->callee, nullptr, 0);
    size_t i = b->calleeMember ? size_t(which) - 1 : size_t(which);
    return ft && ft->kind == TyKind::Function && i < ft->args.size() ? takenBy(ft->args[i]) : Taken::Written;
  }

  // Whether making an object of type t may call a constructor, which may take an argument by reference.
  bool constructs(const Ty* t)
  {
    if (!t || !t->isKnown())
      return true;
    t = this->w.types.unqualified(this->w.types.stripRef(t));
    if (t->kind != TyKind::Class)
      return t->kind == TyKind::Param || t->kind == TyKind::DependentMember;
    ClassDecl* def = this->w.impl.classDefinition(t);
    if (!def)
      return true;
    for (Decl* m : membersRead(def))
      if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
        return true;
      else if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)))
        if (const Ty* mt = this->typeOf(static_cast<VariableDecl*>(m)); !mt || mt->isReference())
          return true; // (an aggregate's reference member binds what initializes it)
    return false;
  }

  // Whether a cast may assign a bare name it is given: to a reference that is not const, or a cast not typed.
  static bool castMayWrite(Expr* cast) { return !cast->ty || !cast->ty->isKnown() || (cast->ty->isReference() && takenBy(cast->ty) == Taken::Written); }

  // The followed local a lambda's capture names: the one its body's use resolves to, else the innermost visible one.
  uint32_t captured(LambdaExpr* l, const LambdaCapture& c)
  {
    uint32_t found = UINT32_MAX;
    forEachNodeExcept(l->body, [&](Node* n)
    {
      if (found != UINT32_MAX || n->kind != NodeKind::NameExpr)
        return;
      auto* x = static_cast<NameExpr*>(n);
      if (x->name.parts.size() == 1 && x->name.parts[0].symbol == c.symbol)
        found = this->followed(x);
    }, nested);
    if (found != UINT32_MAX)
      return found;
    for (size_t k = this->visible.size(); k-- > 0;)
      if (this->visible[k].first == c.symbol)
        return this->visible[k].second;
    return UINT32_MAX;
  }

  // What a lambda takes of the followed locals: those it copies (read where it is made) and those it refers to (it may
  // assign them whenever it runs: assigned from where it is made on).
  void capturesOf(LambdaExpr* l, std::vector<uint32_t>& copied, std::vector<uint32_t>& referred)
  {
    std::unordered_set<uint32_t> explicitly;
    for (const LambdaCapture& c : l->captures)
    {
      if (c.isThis || c.symbol == UINT32_MAX)
        continue;
      if (c.init)
      {
        if (uint32_t i = c.byRef ? this->followed(c.init) : UINT32_MAX; i != UINT32_MAX)
          referred.push_back(i); // `[&r = x]`
        continue;
      }
      uint32_t i = this->captured(l, c);
      if (i == UINT32_MAX)
        continue;
      explicitly.insert(i);
      (c.byRef ? referred : copied).push_back(i);
    }
    if (l->defaultCapture == 0)
      return;
    // `[=]` and `[&]`: what its body names (and a lambda's in it, which captures through it)
    forEachNodeExcept(l->body, [&](Node* n)
    {
      if (n->kind != NodeKind::NameExpr)
        return;
      uint32_t i = this->followed(static_cast<NameExpr*>(n));
      if (i != UINT32_MAX && !explicitly.count(i))
        (l->defaultCapture == 2 ? referred : copied).push_back(i);
    }, nested);
  }

  // Every followed local anything in n may assign, on any path and in any order: what a loop's next pass and a try's
  // handler may find assigned. What the flow below assigns, and more; `everything`: an asm statement.
  void writesIn(Node* n, std::vector<uint32_t>& out, bool& everything)
  {
    if (!n)
      return;
    auto note = [&](uint32_t i)
    {
      if (i != UINT32_MAX)
        out.push_back(i);
    };
    forEachNodeExcept(n, [&](Node* x)
    {
      switch (x->kind)
      {
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(x);
          if (!b->isKeyword && Walker::isAssignmentOp(P(b->op)))
            note(this->followed(b->lhs));
          for (int k = 0; k < 2; ++k)
            if (this->operandTaken(b, k) == Taken::Written)
              note(this->followed(k ? b->rhs : b->lhs));
          break;
        }
        case NodeKind::UnaryExpr:
        {
          auto* u = static_cast<UnaryExpr*>(x);
          P op = P(u->op);
          if (!u->isKeyword && (op == P::PlusPlus || op == P::MinusMinus || op == P::Amp))
            note(this->followed(u->operand));
          break;
        }
        case NodeKind::CallExpr:
        {
          auto* c = static_cast<CallExpr*>(x);
          Callee callee = this->calleeOf(c->callee);
          for (size_t i = 0; i < c->args.size(); ++i)
            if (this->argumentTaken(callee, i) == Taken::Written)
              note(this->followed(c->args[i]));
          Taken object = Taken::Read;
          if (uint32_t i = this->calledOn(c->callee, callee, object); i != UINT32_MAX && object == Taken::Written)
            note(i);
          break;
        }
        case NodeKind::FunctionalCastExpr:
          if (this->constructs(static_cast<Expr*>(x)->ty))
            for (Expr* a : static_cast<FunctionalCastExpr*>(x)->args)
              note(this->followed(a));
          break;
        case NodeKind::BraceInitExpr:
          if (this->constructs(static_cast<Expr*>(x)->ty))
            for (Expr* a : static_cast<BraceInitExpr*>(x)->args)
              note(this->followed(a));
          break;
        case NodeKind::NewExpr:
        {
          auto* ne = static_cast<NewExpr*>(x);
          if (this->constructs(ne->ty && ne->ty->isKnown() && ne->ty->kind == TyKind::Pointer ? ne->ty->inner : nullptr))
            for (Expr* a : ne->args)
              note(this->followed(a));
          break;
        }
        case NodeKind::CastExpr:
          if (castMayWrite(static_cast<Expr*>(x)))
            note(this->followed(static_cast<CastExpr*>(x)->operand));
          break;
        case NodeKind::NamedCastExpr:
          if (castMayWrite(static_cast<Expr*>(x)))
            note(this->followed(static_cast<NamedCastExpr*>(x)->operand));
          break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(x)->decls)
            if (d && d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->init &&
                bindsWritable(this->typeOf(static_cast<VariableDecl*>(d))))
              note(this->followed(static_cast<VariableDecl*>(d)->init));
          break;
        case NodeKind::LambdaExpr:
        {
          std::vector<uint32_t> copied;
          this->capturesOf(static_cast<LambdaExpr*>(x), copied, out);
          for (const LambdaCapture& c : static_cast<LambdaExpr*>(x)->captures)
            if (c.init)
              this->writesIn(c.init, out, everything);
          break;
        }
        case NodeKind::AsmStmt:
        case NodeKind::MacroStmt:
          everything = true;
          break;
        default:
          break;
      }
    }, [&](Node* x) { return x != n && (nested(x) || x->kind == NodeKind::LambdaExpr); });
  }

  void kill(Node* n, Flow& flow)
  {
    std::vector<uint32_t> written;
    bool everything = false;
    this->writesIn(n, written, everything);
    if (everything)
      flow.unassigned.clear();
    for (uint32_t i : written)
      flow.remove(i);
  }

  void read(uint32_t i, uint32_t token, Flow& flow)
  {
    if (i == UINT32_MAX || !this->judgeReads || !flow.reachable || !flow.has(i) || this->reported[i])
      return;
    this->reported[i] = true;
    std::string name(this->w.symbols.name(this->locals[i]->name.last().symbol));
    this->w.impl.reportCheckerError(this->w.file, token,
                                    "the local " + name + " is read before anything is assigned to it (it is still the zero kpp gives it): initialize it where it is declared");
  }

  void declare(VariableDecl* v, Flow& flow)
  {
    if (v->init)
    {
      // A reference that is not const may assign what it is bound to, from here on; a value or a const reference reads it.
      uint32_t bound = this->followed(v->init);
      if (bound != UINT32_MAX && bindsWritable(this->typeOf(v)))
        flow.remove(bound);
      else
        this->expr(v->init, flow);
    }
    uint32_t symbol = v->name.empty() ? UINT32_MAX : v->name.last().symbol;
    if (!this->follows(v))
    {
      this->visible.emplace_back(symbol, UINT32_MAX);
      return;
    }
    auto [it, added] = this->index.emplace(v, uint32_t(this->locals.size()));
    if (added)
    {
      this->locals.push_back(v);
      this->reported.push_back(false);
    }
    flow.add(it->second); // (anew on each pass of a loop the declaration is in: kpp zeroes it there each time)
    this->visible.emplace_back(symbol, it->second);
  }

  // A condition: an expression, or a declaration (`if (auto p = find())`).
  void condition(Node* c, Flow& flow)
  {
    if (!c)
      return;
    if (c->kind == NodeKind::DeclStmt)
      this->stmt(static_cast<Stmt*>(c), flow);
    else if (uint8_t(c->kind) >= uint8_t(NodeKind::NameExpr))
      this->expr(static_cast<Expr*>(c), flow);
  }

  // `while (true)`, `for (;;)`, `do ... while (1)`: left by a break alone.
  bool alwaysTrue(Node* c)
  {
    if (!c)
      return true;
    Expr* e = uint8_t(c->kind) >= uint8_t(NodeKind::NameExpr) ? strip(static_cast<Expr*>(c)) : nullptr;
    return e && e->kind == NodeKind::LiteralExpr &&
           (static_cast<LiteralExpr*>(e)->keyword == Kw::true_ ||
            (static_cast<LiteralExpr*>(e)->literalKind == TokenKind::Number && this->w.spelling(static_cast<LiteralExpr*>(e)) == "1"));
  }

  // A loop: what any pass may assign is taken as assigned at its head, since a pass after the first finds it so (and
  // so throughout the loop: nothing in it assigns anything else). Its paths out: those its body adds (the condition
  // false) and its breaks'.
  template<class Body>
  void loop(Node* whole, Flow& flow, Body body)
  {
    this->kill(whole, flow);
    std::vector<Flow> out;
    this->exits.push_back(&out);
    Flow pass = flow;
    body(pass);
    this->exits.pop_back();
    Flow after = Flow::none();
    for (const Flow& f : out)
      after.join(f);
    flow = after;
  }

  void stmt(Stmt* s, Flow& flow)
  {
    if (!s)
      return;
    if (!this->judgeReads)
      flow.reachable = true; // (a label may lead anywhere)
    size_t scope = this->visible.size();
    switch (s->kind)
    {
      case NodeKind::BlockStmt:
        for (Stmt* c : static_cast<BlockStmt*>(s)->stmts)
          this->stmt(c, flow);
        break;
      case NodeKind::ExprStmt:
        this->expr(static_cast<ExprStmt*>(s)->expr, flow);
        break;
      case NodeKind::DeclStmt:
        for (Decl* d : static_cast<DeclStmt*>(s)->decls)
          if (d && d->kind == NodeKind::VariableDecl)
            this->declare(static_cast<VariableDecl*>(d), flow);
        return; // (its names stay in scope: the block's)
      case NodeKind::IfStmt:
      {
        // An else-if chain link by link, not a recursion per link (a chain may have 100,000).
        Flow after = Flow::none();
        for (auto* i = static_cast<IfStmt*>(s);;)
        {
          this->stmt(i->init, flow);
          Stmt* next = nullptr;
          std::optional<bool> decided = i->isConstexpr && i->constexprKnown >= 0 ? std::optional<bool>(i->constexprKnown == 1)
                                        : i->isConstexpr || i->ifConsteval       ? std::nullopt
                                                                                 : this->constant(i->cond);
          if (!i->isConstexpr && !i->ifConsteval)
            this->condition(i->cond, flow);
          if (decided)
            next = *decided ? i->then : i->else_; // (the branch the walk dropped, or a constant decides away, never runs)
          else
          {
            Flow taken = flow;
            this->stmt(i->then, taken);
            after.join(taken);
            next = i->else_;
          }
          if (next && next->kind == NodeKind::IfStmt)
          {
            i = static_cast<IfStmt*>(next);
            continue;
          }
          this->stmt(next, flow);
          after.join(flow);
          break;
        }
        flow = after;
        break;
      }
      case NodeKind::WhileStmt:
      {
        auto* l = static_cast<WhileStmt*>(s);
        bool endless = this->alwaysTrue(l->cond);
        this->loop(s, flow, [&](Flow& pass)
        {
          this->condition(l->cond, pass);
          if (!endless)
            this->exits.back()->push_back(pass); // (the condition false)
          this->stmt(l->body, pass);
        });
        break;
      }
      case NodeKind::DoStmt:
      {
        auto* l = static_cast<DoStmt*>(s);
        bool endless = this->alwaysTrue(l->cond);
        this->loop(s, flow, [&](Flow& pass)
        {
          Flow start = pass;
          this->stmt(l->body, pass);
          pass.join(start); // (a continue goes to the condition too: with what the start has, as nothing else in a loop assigns)
          this->expr(l->cond, pass);
          if (!endless)
            this->exits.back()->push_back(pass);
        });
        break;
      }
      case NodeKind::ForStmt:
      {
        auto* l = static_cast<ForStmt*>(s);
        this->stmt(l->init, flow);
        bool endless = this->alwaysTrue(l->cond);
        this->loop(s, flow, [&](Flow& pass)
        {
          this->condition(l->cond, pass);
          if (!endless)
            this->exits.back()->push_back(pass);
          Flow start = pass;
          this->stmt(l->body, pass);
          pass.join(start); // (a continue goes to the step)
          this->expr(l->step, pass);
        });
        break;
      }
      case NodeKind::RangeForStmt:
      {
        auto* l = static_cast<RangeForStmt*>(s);
        this->expr(l->range, flow);
        this->loop(s, flow, [&](Flow& pass)
        {
          this->exits.back()->push_back(pass);
          if (l->var && l->var->kind == NodeKind::VariableDecl)
            this->visible.emplace_back(static_cast<VariableDecl*>(l->var)->name.empty() ? UINT32_MAX : static_cast<VariableDecl*>(l->var)->name.last().symbol,
                                       UINT32_MAX);
          this->stmt(l->body, pass);
        });
        break;
      }
      case NodeKind::ForConstexprStmt:
      {
        auto* l = static_cast<ForConstexprStmt*>(s);
        this->loop(s, flow, [&](Flow& pass)
        {
          this->exits.back()->push_back(pass);
          if (l->elementBodies.empty())
          {
            Flow body = pass;
            this->stmt(l->body, body);
          }
          for (Stmt* b : l->elementBodies)
          {
            Flow body = pass;
            this->stmt(b, body);
          }
        });
        break;
      }
      case NodeKind::SwitchStmt:
        this->switchStmt(static_cast<SwitchStmt*>(s), flow);
        break;
      case NodeKind::ReturnStmt:
        this->expr(static_cast<ReturnStmt*>(s)->value, flow);
        flow.reachable = false;
        break;
      case NodeKind::BreakStmt:
        if (!this->exits.empty())
          this->exits.back()->push_back(flow);
        flow.reachable = false;
        break;
      case NodeKind::ContinueStmt:
        flow.reachable = false; // (its loop's head has what any pass assigns assigned)
        break;
      case NodeKind::TryStmt:
      {
        auto* t = static_cast<TryStmt*>(s);
        Flow start = flow;
        this->stmt(t->body, flow);
        Flow after = flow;
        this->kill(t->body, start); // (a handler runs after any part of the block)
        for (const TryStmt::Handler& h : t->handlers)
        {
          size_t handlerScope = this->visible.size();
          Flow handler = start;
          if (h.param && h.param->kind == NodeKind::ParamDecl)
            this->visible.emplace_back(static_cast<ParamDecl*>(h.param)->name, UINT32_MAX);
          this->stmt(h.body, handler);
          after.join(handler);
          this->visible.resize(handlerScope);
        }
        flow = after;
        break;
      }
      case NodeKind::AssertStmt:
        this->expr(static_cast<AssertStmt*>(s)->cond, flow);
        break;
      case NodeKind::AsmStmt:
      case NodeKind::MacroStmt:
        flow.unassigned.clear();
        break;
      default:
        break; // (an empty statement, `fallthrough;`, a label the switch reads)
    }
    this->visible.resize(scope);
  }

  void switchStmt(SwitchStmt* sw, Flow& flow)
  {
    this->stmt(sw->init, flow);
    this->condition(sw->cond, flow);
    Flow head = flow;
    std::vector<Flow> out;
    this->exits.push_back(&out);
    auto* block = sw->body && sw->body->kind == NodeKind::BlockStmt ? static_cast<BlockStmt*>(sw->body) : nullptr;
    bool labelsOnTop = block != nullptr;
    if (block)
      for (Stmt* c : block->stmts)
        forEachNodeExcept(c, [&](Node* n)
        {
          if (n != c && (n->kind == NodeKind::CaseStmt || n->kind == NodeKind::DefaultStmt))
            labelsOnTop = false;
        }, [&](Node* n) { return nested(n) || n->kind == NodeKind::LambdaExpr || n->kind == NodeKind::SwitchStmt; });
    bool hasDefault = false;
    if (!labelsOnTop)
    {
      // (a label inside a statement, Duff's device: any part of the body may run after any other)
      this->kill(sw->body, head);
      Flow body = head;
      this->stmt(sw->body, body);
      out.push_back(body);
    }
    else
    {
      // A case's statements run from the switch's start, or from the case before when it goes on into this one (the
      // C and C++ a conversion reads, kpp's by `fallthrough;`); in kpp's they leave the switch where they end.
      Flow current = Flow::none();
      bool fallsThrough = false;
      for (Stmt* c : block->stmts)
      {
        if (c->kind == NodeKind::CaseStmt || c->kind == NodeKind::DefaultStmt)
        {
          hasDefault = hasDefault || c->kind == NodeKind::DefaultStmt;
          if (current.reachable && !fallsThrough && !sw->cRules)
          {
            out.push_back(current);
            current = Flow::none();
          }
          current.join(head);
          fallsThrough = false;
          continue;
        }
        this->stmt(c, current);
        fallsThrough = c->kind == NodeKind::FallthroughStmt;
      }
      out.push_back(current);
    }
    this->exits.pop_back();
    Flow after = Flow::none();
    if (!hasDefault)
      after.join(head); // (no label taken)
    for (const Flow& f : out)
      after.join(f);
    flow = after;
  }

  // A call's arguments in order (kpp's, left to right), then what the call may assign through a reference to a bare
  // name: `f(x, x)` with an int& and an int reads the second before the call assigns the first.
  void arguments(Span<Expr*> args, const std::function<Taken(size_t)>& taken, Flow& flow)
  {
    std::vector<uint32_t> written;
    for (size_t i = 0; i < args.size(); ++i)
    {
      uint32_t k = this->followed(args[i]);
      if (k != UINT32_MAX && taken(i) == Taken::Written)
        written.push_back(k);
      else
        this->expr(args[i], flow);
    }
    for (uint32_t k : written)
      flow.remove(k);
  }

  void expr(Expr* e, Flow& flow)
  {
    e = strip(e);
    if (!e || !flow.reachable)
      return;
    switch (e->kind)
    {
      case NodeKind::NameExpr:
        this->read(this->followed(e), e->token, flow);
        break;
      case NodeKind::BinaryExpr:
      {
        auto* b = static_cast<BinaryExpr*>(e);
        bool logical = b->isKeyword ? (Kw(b->op) == Kw::and_ || Kw(b->op) == Kw::or_) : (P(b->op) == P::AndAnd || P(b->op) == P::OrOr);
        if (logical && b->call != OperatorCall::Function)
        {
          this->expr(b->lhs, flow);
          bool andOp = b->isKeyword ? Kw(b->op) == Kw::and_ : P(b->op) == P::AndAnd;
          if (std::optional<bool> left = this->constant(b->lhs); left && *left != andOp)
            break; // (`false && x`, `true || x`: x never runs)
          Flow right = flow;
          this->expr(b->rhs, right);
          flow.join(right);
          break;
        }
        if (!b->isKeyword && Walker::isAssignmentOp(P(b->op)) && b->call != OperatorCall::Function)
        {
          // (the right side first, [expr.ass] 1; a compound assignment reads the left before it assigns it)
          this->expr(b->rhs, flow);
          if (P(b->op) == P::SlashAssign || P(b->op) == P::PercentAssign)
            this->divisor(b, P(b->op) == P::SlashAssign ? "/=" : "%=", flow);
          if (uint32_t i = this->followed(b->lhs); i != UINT32_MAX)
          {
            if (P(b->op) != P::Assign)
              this->read(i, strip(b->lhs)->token, flow);
            flow.remove(i);
          }
          else
            this->expr(b->lhs, flow);
          break;
        }
        Expr* operands[2] = {b->lhs, b->rhs};
        this->arguments(Span<Expr*>{operands, 2}, [&](size_t k) { return this->operandTaken(b, int(k)); }, flow);
        if (!b->isKeyword && (P(b->op) == P::Slash || P(b->op) == P::Percent))
          this->divisor(b, P(b->op) == P::Slash ? "/" : "%", flow);
        break;
      }
      case NodeKind::UnaryExpr:
      {
        auto* u = static_cast<UnaryExpr*>(e);
        P op = P(u->op);
        uint32_t i = u->isKeyword || u->call == OperatorCall::Function ? UINT32_MAX : this->followed(u->operand);
        if (i != UINT32_MAX && op == P::Amp)
          flow.remove(i); // (its address: anything may assign it through that)
        else if (i != UINT32_MAX && (op == P::PlusPlus || op == P::MinusMinus))
        {
          this->read(i, strip(u->operand)->token, flow);
          flow.remove(i);
        }
        else if (op == P::Amp && !u->isKeyword && u->call != OperatorCall::Function)
        {
          Expr* outer = this->addressOf;
          this->addressOf = strip(u->operand);
          this->expr(u->operand, flow);
          this->addressOf = outer;
        }
        else
          this->expr(u->operand, flow);
        break;
      }
      case NodeKind::ConditionalExpr:
      {
        auto* c = static_cast<ConditionalExpr*>(e);
        this->expr(c->cond, flow);
        if (!c->then)
        {
          Flow other = flow; // `a ?: b`: b only when a is not set
          this->expr(c->else_, other);
          flow.join(other);
          break;
        }
        if (std::optional<bool> decided = this->constant(c->cond))
        {
          this->expr(*decided ? c->then : c->else_, flow); // (the other never runs)
          break;
        }
        Flow yes = flow;
        this->expr(c->then, yes);
        this->expr(c->else_, flow);
        flow.join(yes);
        break;
      }
      case NodeKind::CastExpr:
      case NodeKind::NamedCastExpr:
      {
        Expr* operand = e->kind == NodeKind::CastExpr ? static_cast<CastExpr*>(e)->operand : static_cast<NamedCastExpr*>(e)->operand;
        uint32_t i = this->followed(operand);
        if (i != UINT32_MAX && e->ty && e->ty->kind == TyKind::Void)
          break; // `(void)x;`: no read (C++'s discarded value), the way to keep a name
        if (i != UINT32_MAX && castMayWrite(e))
          flow.remove(i);
        else
          this->expr(operand, flow);
        break;
      }
      case NodeKind::CallExpr:
      {
        auto* c = static_cast<CallExpr*>(e);
        Expr* callee = strip(c->callee);
        Callee called = this->calleeOf(callee);
        bool conditional = false; // `p?->f(x)`: its arguments only when p is set
        Taken object = Taken::Read;
        uint32_t on = this->calledOn(callee, called, object);
        if (callee && callee->kind == NodeKind::MemberExpr)
        {
          auto* m = static_cast<MemberExpr*>(callee);
          if (on == UINT32_MAX || object == Taken::Read)
            this->expr(m->base, flow);
          this->expr(m->dynamicMember, flow);
          conditional = m->op == P::SafeArrow;
        }
        else if (!called.function)
          this->expr(callee, flow); // (a pointer to a function, an object called: read)
        Flow taken = flow;
        this->arguments(c->args, [&](size_t i) { return this->argumentTaken(called, i); }, taken);
        if (on != UINT32_MAX && object == Taken::Written)
          taken.remove(on);
        if (called.function && this->w.declaredNoReturn(called.function))
          taken.reachable = false;
        if (conditional)
          flow.join(taken);
        else
          flow = taken;
        break;
      }
      case NodeKind::IndexExpr:
      {
        auto* x = static_cast<IndexExpr*>(e);
        bool addressed = e == this->addressOf;
        this->addressOf = nullptr;
        this->expr(x->base, flow);
        for (Expr* a : x->args)
          this->expr(a, flow);
        this->checkIndex(x, addressed, flow);
        break;
      }
      case NodeKind::MemberExpr:
        this->expr(static_cast<MemberExpr*>(e)->base, flow);
        this->expr(static_cast<MemberExpr*>(e)->dynamicMember, flow);
        break;
      case NodeKind::FunctionalCastExpr:
      {
        bool made = this->constructs(e->ty);
        this->arguments(static_cast<FunctionalCastExpr*>(e)->args, [&](size_t) { return made ? Taken::Written : Taken::Read; }, flow);
        break;
      }
      case NodeKind::BraceInitExpr:
      {
        bool made = this->constructs(e->ty);
        this->arguments(static_cast<BraceInitExpr*>(e)->args, [&](size_t) { return made ? Taken::Written : Taken::Read; }, flow);
        break;
      }
      case NodeKind::NewExpr:
      {
        auto* n = static_cast<NewExpr*>(e);
        for (Expr* p : n->placement)
          this->expr(p, flow);
        bool made = this->constructs(n->ty && n->ty->isKnown() && n->ty->kind == TyKind::Pointer ? n->ty->inner : nullptr);
        this->arguments(n->args, [&](size_t) { return made ? Taken::Written : Taken::Read; }, flow);
        break;
      }
      case NodeKind::DeleteExpr:
        this->expr(static_cast<DeleteExpr*>(e)->operand, flow);
        break;
      case NodeKind::ThrowExpr:
        this->expr(static_cast<ThrowExpr*>(e)->operand, flow);
        flow.reachable = false;
        break;
      case NodeKind::LambdaExpr:
      {
        auto* l = static_cast<LambdaExpr*>(e);
        for (const LambdaCapture& c : l->captures)
          if (c.init && !(c.byRef && this->followed(c.init) != UINT32_MAX))
            this->expr(c.init, flow);
        std::vector<uint32_t> copied, referred;
        this->capturesOf(l, copied, referred);
        for (uint32_t i : copied)
          this->read(i, l->token, flow);
        for (uint32_t i : referred)
          flow.remove(i);
        this->lambdas.push_back(l);
        break;
      }
      case NodeKind::FoldExpr:
        this->expr(static_cast<FoldExpr*>(e)->lhs, flow);
        this->expr(static_cast<FoldExpr*>(e)->rhs, flow);
        break;
      case NodeKind::LiteralExpr: case NodeKind::ThisExpr: case NodeKind::CallerExpr: case NodeKind::NameofExpr: case NodeKind::SizeofExpr:
      case NodeKind::NoexceptExpr: case NodeKind::TypeidExpr: case NodeKind::RequiresExpr: case NodeKind::DefaultValueExpr:
      case NodeKind::HeldExpr:
        break; // (none evaluates a local's value: sizeof's, noexcept's and typeid's operand is not evaluated)
      default:
        this->kill(e, flow); // (another expression: what it may assign is assigned, nothing in it judged)
        break;
    }
  }
};

auto GlobalScope::Impl::Walker::checkFlow(FunctionDecl* f) -> void
{
  // Once, in the walk as written (a template's own walk: its instances share its body), reported there itself as
  // checkUnusedNames does.
  if (!f || !f->body || probing || activeSubst || retypingElements || !file || file->isImported)
    return;
  FlowChecks body(*this);
  body.run(f->params, f->body);
  // Each lambda in it on its own, its locals its own (and one in a lambda when that lambda is judged).
  std::vector<LambdaExpr*> lambdas = std::move(body.lambdas);
  for (size_t i = 0; i < lambdas.size(); ++i)
  {
    FlowChecks lambda(*this);
    lambda.run(lambdas[i]->params, lambdas[i]->body);
    lambdas.insert(lambdas.end(), lambda.lambdas.begin(), lambda.lambdas.end());
  }
}
