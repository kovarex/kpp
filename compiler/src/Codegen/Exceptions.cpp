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
  std::string cont = newLabel("invoke.cont");
  fn += "  " + prefix + "invoke " + callee + bundle() + " to label %" + cont + " unwind label %" + dest + "\n";
  startBlock(cont);
  // A nop at the return address, as MSVC puts one after a call in a __try: __C_specific_handler's
  // scope entry for this call ends at its return address + 1, so where -O2 lays the next invoke
  // right after this one (the Control Flow Optimizer folding the jump between them), their
  // entries would share a byte and a throw from this call run the next one's cleanups too (a
  // double free in LuaHelper::parseLocalisedString's loop). It touches only memory nothing else
  // reaches (#0, memory(inaccessiblemem: readwrite), in every module's prefix): still a side
  // effect, never removed or merged, but no barrier to the program's loads and stores, which may
  // move across it once the call is inlined (any instruction between two invokes keeps their
  // entries apart).
  fn += "  call void asm sideeffect \"nop\", \"\"() #0" + bundle() + "\n"; // (in a funclet, as its own)
}

auto codegen::Emitter::unwindDestination() -> std::string
{
  if (!sh.target.windows())
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
  std::string rest = cleanupChain(objects, from + 1, next, parent);
  std::string padLabel = newLabel("cleanup"), pad = newTemp();
  cleanupPads[key] = padLabel;
  // The pad is emitted aside, after the function body.
  std::string savedFn = std::move(fn), savedBlock = currentBlock, savedOverride = funcletOverride;
  bool savedTerminated = blockTerminated;
  fn.clear();
  startBlock(padLabel);
  fn += "  " + pad + " = cleanuppad within " + (parent.empty() ? "none" : parent) + " []\n";
  funcletOverride = pad;
  destroyObject(objects[from]);
  funcletOverride = savedOverride;
  fn += "  cleanupret from " + pad + " unwind " + (rest.empty() ? "to caller" : "label %" + rest) + "\n";
  ehBlocks += fn;
  fn = std::move(savedFn);
  currentBlock = savedBlock;
  blockTerminated = savedTerminated;
  return padLabel;
}
