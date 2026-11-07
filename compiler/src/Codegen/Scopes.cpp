// struct Emitter's members: temporaries and scopes; their declarations in Scopes.hpp, part of the
// struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05 (codegen.cpp
// split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::destroyTemps() -> void
{
  for (size_t i = statementTemps.size(); i-- > 0;)
  {
    Object o = statementTemps[i];
    statementTemps.resize(i);
    destroyOnExit(o);
  }
  statementTemps.clear();
}

auto codegen::Emitter::destroyTempsFrom(size_t mark) -> void
{
  for (size_t i = statementTemps.size(); i-- > mark;)
  {
    Object o = statementTemps[i];
    statementTemps.resize(i);
    destroyOnExit(o);
  }
  statementTemps.resize(mark);
}

auto codegen::Emitter::keepOperandTemps(size_t mark) -> void
{
  for (size_t i = mark; i < statementTemps.size(); ++i)
  {
    Object& o = statementTemps[i];
    if (!o.builtFlag.empty() || o.unwindOnly || o.exception)
      continue; // a nested operand's (its own flag says), or no temporary of this one
    o.builtFlag = "%v" + std::to_string(temp++);
    allocas += "  " + o.builtFlag + " = alloca i1\n  store i1 false, ptr " + o.builtFlag + "\n";
    fn += "  store i1 true, ptr " + o.builtFlag + "\n";
  }
}

auto codegen::Emitter::destroyObject(const Object& o) -> void
{
  if (!o.builtFlag.empty())
  {
    // A temporary of an operand that may not have run (keepOperandTemps): destroyed only when it
    // did, its flag false again (a loop's next pass runs the expression anew).
    Object built = o;
    built.builtFlag.clear();
    std::string ran = newTemp(), builtL = newLabel("temp.built"), endL = newLabel("temp.end");
    fn += "  " + ran + " = load i1, ptr " + o.builtFlag + "\n";
    fn += "  br i1 " + ran + ", label %" + builtL + ", label %" + endL + "\n";
    blockTerminated = true;
    startBlock(builtL);
    fn += "  store i1 false, ptr " + o.builtFlag + "\n";
    destroyObject(built);
    emitBranch(endL);
    startBlock(endL);
    return;
  }
  if (o.exception)
    fn += "  call void @kpp_eh_end(ptr " + o.addr + ")" + bundle() + "\n";
  else if (o.freeMemory && !o.freeCall.empty())
    fn += "  call " + o.freeCall + bundle() + "\n";
  else if (o.freeMemory)
  {
    ensureMalloc();
    fn += "  call void @free(ptr " + o.addr + ")" + bundle() + "\n";
  }
  else if (!o.guardReset.empty())
    fn += "  store atomic i8 0, ptr " + o.guardReset + " release, align 1\n";
  else if (!o.countSlot.empty())
  {
    // The elements an array built in a loop has so far (InlineParts::builtCounted), the last first.
    const Ty* t = types.unqualified(types.stripRef(o.ty));
    std::string index = "%v" + std::to_string(temp++);
    allocas += "  " + index + " = alloca i64\n";
    std::string count = newTemp();
    fn += "  " + count + " = load i64, ptr " + o.countSlot + "\n";
    fn += "  store i64 " + count + ", ptr " + index + "\n";
    std::string loopL = newLabel("unbuild.loop"), bodyL = newLabel("unbuild.body"), endL = newLabel("unbuild.end");
    emitBranch(loopL);
    startBlock(loopL);
    std::string i = newTemp(), done = newTemp();
    fn += "  " + i + " = load i64, ptr " + index + "\n";
    fn += "  " + done + " = icmp eq i64 " + i + ", 0\n";
    fn += "  br i1 " + done + ", label %" + endL + ", label %" + bodyL + "\n";
    startBlock(bodyL);
    std::string previous = newTemp(), element = newTemp();
    fn += "  " + previous + " = sub i64 " + i + ", 1\n";
    fn += "  store i64 " + previous + ", ptr " + index + "\n";
    fn += "  " + element + " = getelementptr inbounds " + llType(t, nullptr) + ", ptr " + o.addr + ", i64 0, i64 " + previous + "\n";
    destroy(element, t->inner, nullptr);
    emitBranch(loopL);
    startBlock(endL);
  }
  else
    destroy(o.addr, o.ty, nullptr);
}

auto codegen::Emitter::destroyOnExit(const Object& o) -> void
{
  if (o.unwindOnly)
    return;
  if (!keptOnReturn.empty() && o.addr == keptOnReturn && !o.exception && !o.freeMemory)
  {
    // The named return, left by its return: the result, destroyed only when a destructor the
    // return runs after it throws (ReturnStmt).
    Object result = o;
    result.unwindOnly = true;
    statementTemps.push_back(result);
    return;
  }
  destroyObject(o);
}

auto codegen::Emitter::subStatement(Stmt* s) -> void
{
  if (!s || s->kind == NodeKind::BlockStmt)
  {
    statement(s);
    return;
  }
  pushScope();
  statement(s);
  popScope();
}

auto codegen::Emitter::debugScopeEnd(size_t scope) -> void
{
  if (scopes[scope].empty() || !scopeEnds[scope] || !sh.debugInfo)
    return;
  size_t lastLine = fn.size() < 2 ? std::string::npos : fn.rfind('\n', fn.size() - 2);
  if (lastLine == std::string::npos || fn.compare(lastLine + 1, 4, "  ;@") != 0)
    debugLineAt(scopeEnds[scope]);
}

auto codegen::Emitter::popScope() -> void
{
  if (!blockTerminated)
  {
    debugScopeEnd(scopes.size() - 1);
    for (size_t i = scopes.back().size(); i-- > 0;)
    {
      Object o = scopes.back()[i];
      scopes.back().resize(i); // (destroyed: a destructor that throws unwinds through those before it)
      destroyOnExit(o);
    }
  }
  scopes.pop_back();
  scopeEnds.pop_back();
  if (!scopeIds.empty())
    scopeIds.pop_back();
  if (!scopeInits.empty())
    scopeInits.pop_back();
}

auto codegen::Emitter::unwindRange(size_t low, size_t high) -> void
{
  std::vector<std::vector<Object>> open(scopes.begin() + low, scopes.begin() + high);
  destroyRange(low, high);
  std::move(open.begin(), open.end(), scopes.begin() + low);
}

auto codegen::Emitter::destroyRange(size_t low, size_t high) -> void
{
  for (size_t s = high; s-- > low;)
  {
    debugScopeEnd(s);
    for (size_t i = scopes[s].size(); i-- > 0;)
    {
      Object o = scopes[s][i];
      scopes[s].resize(i);
      destroyOnExit(o);
    }
  }
}

auto codegen::Emitter::leaveScopes(size_t depth, size_t sharedKept) -> void
{
  // Each try or handler the jump leaves is left once the objects of its own scopes are destroyed:
  // an object outside a try is destroyed outside it, so what its destructor throws is not the try's
  // to catch (until 2026-11-07 every frame but a handler's stayed open, and `D d; try { return 1; }
  // catch (...) {}` caught d's exception in the function and destroyed d a second time). The scopes
  // stay emptied until the end, so a destructor that throws unwinds through the objects not yet
  // destroyed alone. A frame's `used` (a call unwinds to its dispatch) outlives the restore.
  std::vector<EHFrame> saved = ehFrames;
  std::vector<std::vector<Object>> open(scopes.begin() + (depth > 0 ? depth - 1 : 0), scopes.end());
  auto keepUsed = [&] {
    for (size_t k = 0; k < ehFrames.size(); ++k)
      saved[k].used = saved[k].used || ehFrames[k].used;
  };
  size_t top = scopes.size();
  for (size_t i = ehFrames.size(); i-- > 0;)
  {
    if (ehFrames[i].scopeDepth < depth)
      break; // the target is inside this try or handler
    destroyRange(ehFrames[i].scopeDepth, top);
    top = ehFrames[i].scopeDepth;
    if (ehFrames[i].isHandler && !ehFrames[i].funclet.empty()) // (off Windows a handler is plain code: nothing to leave)
    {
      std::string next = newLabel("catchret");
      fn += "  catchret from " + ehFrames[i].funclet + " to label %" + next + "\n";
      startBlock(next);
    }
    keepUsed();
    ehFrames.resize(i);
  }
  destroyRange(depth, top);
  if (depth > 0 && sharedKept != size_t(-1))
  {
    std::vector<Object>& shared = scopes[depth - 1];
    while (shared.size() > sharedKept)
    {
      Object o = shared.back();
      shared.pop_back();
      destroyOnExit(o);
    }
  }
  keepUsed();
  ehFrames = saved;
  std::move(open.begin(), open.end(), scopes.begin() + (depth > 0 ? depth - 1 : 0));
}

auto codegen::Emitter::leaveForward(std::vector<GotoStop>& stops) -> void
{
  // Until 2026-11-07 a trampoline at the label destroyed the objects there, inside the label's tries
  // and handlers: a destructor that threw went to the label's handlers, past the objects the jump
  // had not destroyed yet, and a goto out of a catch handler branched out of its funclet, which
  // LLVM's Windows exception preparation dropped (the goto did nothing).
  std::vector<EHFrame> saved = ehFrames;
  std::vector<std::vector<Object>> open = scopes;
  auto keepUsed = [&] {
    for (size_t k = 0; k < ehFrames.size(); ++k)
      saved[k].used = saved[k].used || ehFrames[k].used;
  };
  auto leaveFrames = [&](size_t depth) {
    while (!ehFrames.empty() && ehFrames.back().scopeDepth >= depth)
    {
      if (ehFrames.back().isHandler && !ehFrames.back().funclet.empty())
      {
        std::string next = newLabel("catchret");
        fn += "  catchret from " + ehFrames.back().funclet + " to label %" + next + "\n";
        startBlock(next);
      }
      keepUsed();
      ehFrames.pop_back();
    }
  };
  auto stopHere = [&](size_t low, size_t high) {
    GotoStop stop{low, high, newLabel("goto.stop"), newLabel("goto.leave")};
    fn += "  br label %" + stop.stop + "\n";
    blockTerminated = true;
    startBlock(stop.next);
    stops.push_back(std::move(stop));
  };
  size_t depth = scopes.size();
  leaveFrames(depth);
  size_t high = depth;
  for (; depth > 0; --depth)
  {
    if (scopes[depth - 1].empty() && (ehFrames.empty() || ehFrames.back().scopeDepth < depth - 1))
      continue; // (leaving the scope does nothing: the same stop)
    stopHere(depth, high);
    high = depth - 1;
    destroyRange(depth - 1, depth);
    leaveFrames(depth - 1);
  }
  stopHere(0, high);
  fn += "  unreachable\n";
  blockTerminated = true;
  keepUsed();
  ehFrames = saved;
  scopes = std::move(open);
}
