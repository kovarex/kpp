// struct Emitter's members: exceptions, Windows' model; their declarations in Exceptions.hpp, part
// of the struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05
// (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::currentFunclet() -> std::string
{
  if (!funcletOverride.empty())
    return funcletOverride;
  for (size_t i = ehFrames.size(); i-- > 0;)
    if (ehFrames[i].isHandler)
      return ehFrames[i].funclet;
  return "";
}

auto codegen::Emitter::bundle() -> std::string
{
  std::string f = currentFunclet();
  return f.empty() ? "" : " [ \"funclet\"(token " + f + ") ]";
}

auto codegen::Emitter::markEH() -> void
{
  usesEH = true;
  if (handlerDeclared)
    return;
  handlerDeclared = true;
  if (declaredNames.claim(personality()))
    decls += "declare i32 " + personality() + "(...)\n";
}

auto codegen::Emitter::emitCall(const std::string& prefix, const std::string& callee, bool mayThrow) -> void
{
  std::string dest = mayThrow && funcletOverride.empty() ? unwindDestination() : std::string();
  if (dest.empty())
  {
    fn += "  " + prefix + "call " + callee + bundle() + "\n";
    return;
  }
  markEH();
  // A nop before it (Windows' own exceptions): a call that may throw and has nothing to clean up is a
  // plain call, and when it comes right before this invoke, its return address is where this one's
  // scope entry begins, so a throw from it ran this invoke's cleanups: a member whose constructor
  // threw was destroyed when the constructor's body then called a function that may throw (E's
  // battery, 2026-11-06, classes/member_whose_constructor_throws_is_not_destroyed). However the
  // blocks are laid out, a call before an invoke now returns to this nop, outside the entry.
  if (sh.target.seh())
    fn += "  call void asm sideeffect \"nop\", \"\"() #0" + bundle() + "\n";
  std::string cont = newLabel("invoke.cont");
  fn += "  " + prefix + "invoke " + callee + bundle() + " to label %" + cont + " unwind label %" + dest + "\n";
  startBlock(cont);
  // And a nop at the return address, as MSVC puts one after a call in a __try: __C_specific_handler's
  // scope entry for this call ends at its return address + 1, and that byte must be no other block's.
  // Where -O2 lays the next invoke right after this one (the Control Flow Optimizer folding the jump
  // between them), their entries would share a byte and a throw from this call run the next one's
  // cleanups too (a double free in LuaHelper::parseLocalisedString's loop). And where this call does
  // not return (a throw, an allocation's failure) nothing of its own block follows it: the block laid
  // out next may be a handler's, and __C_specific_handler runs no cleanup of a scope whose entry the
  // unwind's target lies in, so with the handler's first byte inside this call's entry the objects
  // of the try block were not destroyed before the handler ran. That happened when this nop was
  // dropped as covered by the one before the next invoke (d55ff8e62, 2026-11-24: the differential
  // suite's life/5 and life/6 in a release build, an array's elements left alive by a constructor
  // that threw in a new-expression beside them). The nop touches only memory nothing else reaches
  // (#0, memory(inaccessiblemem: readwrite), in every module's prefix): still a side effect, never
  // removed or merged, but no barrier to the program's loads and stores, which may move across it
  // once the call is inlined. Not written where exceptions are Itanium's (until 2026-11-24 it was,
  // on every target): their call-site tables have no such byte.
  if (sh.target.seh())
    fn += "  call void asm sideeffect \"nop\", \"\"() #0" + bundle() + "\n"; // (in a funclet, as its own)
}

auto codegen::Emitter::unwindDestination() -> std::string
{
  if (!sh.target.seh())
    return itaniumUnwindDestination();
  size_t limit = ehFrames.empty() ? 0 : ehFrames.back().scopeDepth;
  std::vector<Object> objects;
  for (size_t i = statementTemps.size(); i-- > 0;)
    objects.push_back(statementTemps[i]);
  for (size_t s = scopes.size(); s-- > limit;)
    for (size_t i = scopes[s].size(); i-- > 0;)
      objects.push_back(scopes[s][i]);
  std::string next; // "" = out of the function
  if (!ehFrames.empty() && !ehFrames.back().isHandler)
  {
    next = ehFrames.back().dispatchLabel;
    if (objects.empty())
      return ehFrames.back().used = true, next;
  }
  else if (!ehFrames.empty())
    next = ehFrames.back().dispatchLabel; // out of a handler: on where its catchswitch unwinds (a call
                                          // without objects there unwinds so implicitly)
  // (out of a noexcept function's own body: its outermost catchpad, whose filter ends the program
  // in the search; noexceptDispatch)
  else if (emittingNoexcept() && funcletOverride.empty() && currentFunclet().empty())
  {
    next = noexceptDispatch();
    if (objects.empty())
      return next;
  }
  if (objects.empty())
    return "";
  markEH();
  if (!next.empty() && !ehFrames.empty()) // (not the noexcept function's own catchswitch: no frame)
    ehFrames.back().used = true;
  return cleanupChain(objects, 0, next, currentFunclet());
}

auto codegen::Emitter::noexceptDispatch() -> std::string
{
  // (a key of its own in cleanupPads, the function's EH blocks by key: no destruction state)
  auto found = cleanupPads.find("seh.noexcept");
  if (found != cleanupPads.end())
    return found->second;
  markEH();
  usesNoexceptFilter = true;
  std::string block = newLabel("noexcept.dispatch"), handlerL = newLabel("noexcept.handler"), cs = newTemp(), pad = newTemp();
  cleanupPads["seh.noexcept"] = block;
  ehBlocks += block + ":\n  " + cs + " = catchswitch within none [label %" + handlerL + "] unwind to caller\n" + handlerL + ":\n  " + pad +
              " = catchpad within " + cs + " [ptr @kpp.eh.filter.noexcept]\n  unreachable\n";
  return block;
}

auto codegen::Emitter::cleanupChain(const std::vector<Object>& objects, size_t from, const std::string& next, const std::string& parent) -> std::string
{
  if (from == objects.size())
    return next;
  std::string key = next + "|" + parent;
  for (size_t i = from; i < objects.size(); ++i)
    key += "|" + objects[i].addr + (objects[i].freeMemory ? "~" + objects[i].freeCall : "") + (objects[i].exception ? "!" : "") +
           (objects[i].builtFlag.empty() ? "" : "?" + objects[i].builtFlag); // (one built for sure is another pad)
  auto it = cleanupPads.find(key);
  if (it != cleanupPads.end())
    return it->second;
  // One pad destroys the outermost objects of a chain 64 or 8 at a time (the objects left, counted
  // from the chain's end, so that the chains of a function's invokes share those pads), and the
  // others one each. __C_specific_handler's scope table has an entry for each range of invokes and
  // each pad on its way out, 16 bytes and three relocations: with a pad per object a function's
  // table grew with the square of the objects alive in it (the game's 972,237 entries, 9.2% of its
  // objects' bytes, a quarter of them two constructors', whose chains reach 500 and 200 objects;
  // 2026-11-25). A chain of n objects is n mod 8 + (n div 8) mod 8 + n div 64 pads long now. What
  // a pad destroys, and in which order, is what the chain of single pads destroyed from there:
  // every object of a chain is alive wherever an invoke unwinds to its first pad, and a pad goes
  // on to the pad of the objects it left. docs/codegen-speed.md, section 10.
  size_t left = objects.size() - from;
  size_t count = left % 64 == 0 ? 64 : left % 8 == 0 ? 8 : 1;
  std::string rest = cleanupChain(objects, from + count, next, parent);
  std::string padLabel = newLabel("cleanup"), pad = newTemp();
  cleanupPads[key] = padLabel;
  // The pad is emitted aside, after the function body.
  std::string savedFn = std::move(fn), savedBlock = currentBlock, savedOverride = funcletOverride;
  bool savedTerminated = blockTerminated;
  fn.clear();
  startBlock(padLabel);
  fn += "  " + pad + " = cleanuppad within " + (parent.empty() ? "none" : parent) + " []\n";
  funcletOverride = pad;
  for (size_t i = from; i < from + count; ++i)
    destroyObject(objects[i]);
  funcletOverride = savedOverride;
  fn += "  cleanupret from " + pad + " unwind " + (rest.empty() ? "to caller" : "label %" + rest) + "\n";
  ehBlocks += fn;
  fn = std::move(savedFn);
  currentBlock = savedBlock;
  blockTerminated = savedTerminated;
  return padLabel;
}
