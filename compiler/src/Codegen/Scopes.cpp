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

auto codegen::Emitter::destroyObject(const Object& o) -> void
{
  if (o.exception)
    fn += "  call void @kpp_eh_end(ptr " + o.addr + ")" + bundle() + "\n";
  else if (o.freeMemory)
  {
    ensureMalloc();
    fn += "  call void @free(ptr " + o.addr + ")" + bundle() + "\n";
  }
  else
    destroy(o.addr, o.ty, nullptr);
}

auto codegen::Emitter::destroyOnExit(const Object& o) -> void
{
  if (!o.unwindOnly && !(!keptOnReturn.empty() && o.addr == keptOnReturn && !o.exception && !o.freeMemory))
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

auto codegen::Emitter::popScope() -> void
{
  if (!blockTerminated)
    for (size_t i = scopes.back().size(); i-- > 0;)
    {
      Object o = scopes.back()[i];
      scopes.back().resize(i); // (destroyed: a destructor that throws unwinds through those before it)
      destroyOnExit(o);
    }
  scopes.pop_back();
}

auto codegen::Emitter::unwindRange(size_t low, size_t high) -> void
{
  std::vector<std::vector<Object>> open(scopes.begin() + low, scopes.begin() + high);
  for (size_t s = high; s-- > low;)
    for (size_t i = scopes[s].size(); i-- > 0;)
    {
      Object o = scopes[s][i];
      scopes[s].resize(i);
      destroyOnExit(o);
    }
  std::move(open.begin(), open.end(), scopes.begin() + low);
}

auto codegen::Emitter::leaveScopes(size_t depth) -> void
{
  std::vector<EHFrame> saved = ehFrames;
  size_t top = scopes.size();
  for (size_t i = ehFrames.size(); i-- > 0;)
  {
    if (!ehFrames[i].isHandler)
      continue;
    if (ehFrames[i].scopeDepth < depth)
      break; // the target is inside this handler
    unwindRange(ehFrames[i].scopeDepth, top);
    top = ehFrames[i].scopeDepth;
    if (!ehFrames[i].funclet.empty()) // (off Windows a handler is plain code: nothing to leave)
    {
      std::string next = newLabel("catchret");
      fn += "  catchret from " + ehFrames[i].funclet + " to label %" + next + "\n";
      startBlock(next);
    }
    ehFrames.resize(i);
  }
  unwindRange(depth, top);
  ehFrames = saved;
}
