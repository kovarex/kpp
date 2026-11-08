// struct Emitter's members: exceptions off Windows (the Itanium model); throw, try and catch; the
// entry; their declarations in ItaniumExceptions.hpp, part of the struct's body in
// Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05 (codegen.cpp split, stage 2),
// moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::itaniumSlots() -> void
{
  if (cleanupPads.emplace("itanium.slots", "").second)
    allocas += "  %eh.exc = alloca ptr\n  %eh.sel = alloca i32\n";
}

auto codegen::Emitter::itaniumResume() -> std::string
{
  auto found = cleanupPads.find("itanium.resume");
  if (found != cleanupPads.end())
    return found->second;
  itaniumSlots();
  std::string block = newLabel("eh.resume"), exc = newTemp(), sel = newTemp(), pair = newTemp(), full = newTemp();
  cleanupPads["itanium.resume"] = block;
  ehBlocks += block + ":\n  " + exc + " = load ptr, ptr %eh.exc\n  " + sel + " = load i32, ptr %eh.sel\n  " + pair +
              " = insertvalue { ptr, i32 } poison, ptr " + exc + ", 0\n  " + full + " = insertvalue { ptr, i32 } " + pair + ", i32 " + sel +
              ", 1\n  resume { ptr, i32 } " + full + "\n";
  return block;
}

auto codegen::Emitter::itaniumTerminatePad() -> std::string
{
  auto found = cleanupPads.find("itanium.terminate");
  if (found != cleanupPads.end())
    return found->second;
  markEH();
  std::string block = newLabel("eh.terminate"), lp = newTemp();
  cleanupPads["itanium.terminate"] = block;
  ehBlocks += block + ":\n  " + lp + " = landingpad { ptr, i32 } filter [0 x ptr] zeroinitializer\n  unreachable\n";
  return block;
}

auto codegen::Emitter::emittingNoexcept() -> bool
{
  static const TemplateArgs none;
  if (noexceptDestructorHelper || (bodyNow && bodyNow == noexceptLambdaBody))
    return true;
  if (!currentFunction || !bodyNow || bodyNow != currentFunction->body)
    return false;
  // A destructor is noexcept as its class says, with no specifier too ([except.spec] 8).
  if ((currentFunction->flags & DF_Destructor) && currentClassTy && isClass(currentClassTy))
    return !destructionMayThrow(currentClassTy, currentFunction);
  return (currentFunction->flags & DF_Noexcept) && global.isNoexcept(currentFunction, currentArgs ? *currentArgs : none);
}

auto codegen::Emitter::itaniumChain(const std::vector<Object>& objects, size_t from, const std::string& next) -> std::string
{
  if (from == objects.size())
    return next.empty() ? itaniumResume() : next;
  std::string key = "itanium.chain|" + next;
  for (size_t i = from; i < objects.size(); ++i)
    key += "|" + objects[i].addr + (objects[i].freeMemory ? "~" + objects[i].freeCall : "") + (objects[i].exception ? "!" : "") +
           (objects[i].builtFlag.empty() ? "" : "?" + objects[i].builtFlag); // (one built for sure is another pad)
  auto found = cleanupPads.find(key);
  if (found != cleanupPads.end())
    return found->second;
  std::string rest = itaniumChain(objects, from + 1, next);
  std::string block = newLabel("eh.cleanup");
  cleanupPads[key] = block;
  // The block is emitted aside, after the function body.
  std::string savedFn = std::move(fn), savedBlock = currentBlock;
  bool savedTerminated = blockTerminated, savedCleanup = itaniumCleanup;
  fn.clear();
  startBlock(block);
  itaniumCleanup = true;
  destroyObject(objects[from]);
  itaniumCleanup = savedCleanup;
  emitBranch(rest);
  ehBlocks += fn;
  fn = std::move(savedFn);
  currentBlock = savedBlock;
  blockTerminated = savedTerminated;
  return block;
}

auto codegen::Emitter::itaniumLandingPad(const std::string& head) -> std::string
{
  std::string clauses;
  for (size_t i = ehFrames.size(); i-- > 0;)
    if (!ehFrames[i].isHandler)
      for (const std::string& type : ehFrames[i].catchTypes)
        clauses += " catch ptr " + type;
  if (emittingNoexcept())
    clauses += " filter [0 x ptr] zeroinitializer";
  std::string key = "itanium.pad|" + head + "|" + clauses;
  auto found = cleanupPads.find(key);
  if (found != cleanupPads.end())
    return found->second;
  markEH();
  itaniumSlots();
  std::string block = newLabel("eh.pad"), lp = newTemp(), exc = newTemp(), sel = newTemp();
  cleanupPads[key] = block;
  ehBlocks += block + ":\n  " + lp + " = landingpad { ptr, i32 } cleanup" + clauses + "\n  " + exc + " = extractvalue { ptr, i32 } " + lp +
              ", 0\n  store ptr " + exc + ", ptr %eh.exc\n  " + sel + " = extractvalue { ptr, i32 } " + lp + ", 1\n  store i32 " + sel +
              ", ptr %eh.sel\n";
  if (!emittingNoexcept())
  {
    ehBlocks += "  br label %" + head + "\n";
    return block;
  }
  // A noexcept function's pad: its empty filter, which the personality ends the program at in the
  // search (`why` 2), and here a selector of that filter's going to abort, which never runs. Without
  // it the pad of a function with nothing to destroy was `landingpad; resume`, which LLVM's optimizer
  // deletes, its filter with it: inlined into a caller's try, the caller's catch took the exception
  // (a release build off Windows: language/noexcept_lambda_terminates and nine others).
  std::string negative = newTemp(), stop = newLabel("eh.noexcept");
  if (declaredNames.claim("@abort"))
    decls += "declare void @abort()\n";
  ehBlocks += "  " + negative + " = icmp slt i32 " + sel + ", 0\n  br i1 " + negative + ", label %" + stop + ", label %" + head + "\n" + stop +
              ":\n  call void @abort()\n  unreachable\n";
  return block;
}

auto codegen::Emitter::itaniumObjects() -> std::vector<Object>
{
  size_t limit = ehFrames.empty() ? 0 : ehFrames.back().scopeDepth;
  std::vector<Object> objects;
  for (size_t i = statementTemps.size(); i-- > 0;)
    objects.push_back(statementTemps[i]);
  for (size_t s = scopes.size(); s-- > limit;)
    for (size_t i = scopes[s].size(); i-- > 0;)
      objects.push_back(scopes[s][i]);
  return objects;
}

auto codegen::Emitter::itaniumUnwindDestination() -> std::string
{
  if (itaniumCleanup)
    return itaniumTerminatePad();
  std::vector<Object> objects = itaniumObjects();
  std::string next;
  if (!ehFrames.empty())
  {
    next = ehFrames.back().dispatchLabel;
    if (!ehFrames.back().isHandler)
      ehFrames.back().used = true;
  }
  if (objects.empty() && next.empty() && !emittingNoexcept())
    return "";
  return itaniumLandingPad(itaniumChain(objects, 0, next));
}

auto codegen::Emitter::typeinfoFor(const Ty* t, Node* at) -> std::string
{
  t = types.unqualified(types.stripRef(t));
  auto lock = sharedLock(); // the descriptors emitted, the library's Exception's
  std::string display = types.toString(t, &symbols), list;
  std::string name = isClass(t) ? "@kpp.ti." + structFor(t, at).name.substr(8) : "@\"kpp.ti." + display + "\""; // one per scalar type, pointers by pointee
  if (emittedShared[name])
    return name;
  size_t count = 0;
  // The list is all this type converts to (a handler's search goes no further): what an object of
  // it is an unambiguous public base of ([except.handle] 3), one list for the whole hierarchy.
  auto convertsTo = [&](const Ty* to, const std::string& offset)
  { list += (count++ ? ", " : "") + std::string("{ ptr, i64 } { ptr ") + typeinfoFor(to, at) + ", i64 " + offset + " }"; };
  if (isClass(t))
  {
    const StructInfo& info = structFor(t, at);
    std::string structName = info.name;
    std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
    std::vector<const Ty*> fieldTypes = info.fieldTypes;
    for (const BaseSubobject& base : handlerBases(t, at))
      convertsTo(base.cls, base.offset);
    if (display == "Exception" && exceptionTypeinfo.empty())
    {
      exceptionTypeinfo = name;
      // Its message, its first data member, is a String: the last of its bytes is the length of a
      // small one or the large mark (String.kpp's markIndex), wherever String's sizes put it; the
      // member is where the layout puts it, after the vtable pointer (Exception is polymorphic, its
      // what() virtual, as std::exception's).
      if (!info.fieldDecls.empty() && bases.empty())
        if (auto field = info.fieldIndex.find(info.fieldDecls[0]); field != info.fieldIndex.end() && field->second < fieldTypes.size() &&
                                                                    types.toString(fieldTypes[field->second], &symbols) == "String")
        {
          exceptionMessageMark = sizeOf(fieldTypes[field->second], at) - 1;
          exceptionMessageOffset = "ptrtoint (ptr getelementptr (" + structName + ", ptr null, i32 0, i32 " + std::to_string(field->second) + ") to i64)";
        }
    }
  }
  else if (t->kind == TyKind::Pointer && t->inner && t->inner->kind != TyKind::Function)
  {
    // What a thrown pointer converts to ([except.handle] 3.3), in the order searched: its pointee
    // const, void* (the same address, before any base's), a pointer to each base of a class it
    // points to, at the base's offset (until 2026-11-05 none: only a handler of its own type took it),
    // each also as a pointer to const.
    const Ty* pointee = t->inner;
    bool isConst = pointee->isConst, isVolatile = pointee->isVolatile;
    auto andConst = [&](const Ty* to, const std::string& offset)
    {
      if (!isConst)
        convertsTo(types.pointer(types.qualified(to, false, isVolatile)), offset);
      convertsTo(types.pointer(types.qualified(to, true, isVolatile)), offset);
    };
    if (!isConst)
      convertsTo(types.pointer(types.qualified(pointee, true, isVolatile)), "0");
    if (types.unqualified(pointee)->kind != TyKind::Void)
      andConst(types.voidType(), "0");
    const Ty* cls = types.unqualified(pointee);
    if (isClass(cls) && !declaredOnly(cls))
      for (const BaseSubobject& base : handlerBases(cls, at))
        andConst(base.cls, base.offset);
  }
  else if (t->kind == TyKind::Pointer && t->inner->isNoexcept())
    convertsTo(types.pointer(types.withNoexcept(t->inner, false)), "0"); // (a function pointer conversion, [except.handle] 3.3)
  else if (t->kind == TyKind::MemberPointer && t->inner && t->inner->kind == TyKind::Function && t->inner->isNoexcept() && !t->args.empty())
    convertsTo(types.memberPointer(types.withNoexcept(t->inner, false), t->args[0]), "0");
  emittedShared[name] = true;
  bool was = emittingShared;
  emittingShared = true;
  std::string str = cStringConstant(display);
  emittingShared = was;
  std::string arrayType = "[" + std::to_string(count) + " x { ptr, i64 }]";
  shared += sharedComdat(name) + name + " = linkonce_odr constant { ptr, i64, " + arrayType + " } { ptr " + str + ", i64 " +
            std::to_string(count) + ", " + arrayType + " " + (count ? "[" + list + "]" : std::string("zeroinitializer")) + " }, comdat\n";
  return name;
}

auto codegen::Emitter::handlerTypeinfo(const Ty* t, Node* at) -> std::string
{
  const Ty* pointer = types.unqualified(types.stripRef(t));
  if (pointer->kind != TyKind::Pointer && pointer->kind != TyKind::MemberPointer)
    return typeinfoFor(t, at);
  bool exact = t->isReference() && !types.stripRef(t)->isConst;
  std::string real = typeinfoFor(pointer, at), display = types.toString(pointer, &symbols);
  std::string name = "@\"kpp.ti." + std::string(exact ? "exact." : "convert.") + display + "\"";
  auto lock = sharedLock();
  if (emittedShared[name])
    return name;
  emittedShared[name] = true;
  bool was = emittingShared;
  emittingShared = true;
  std::string str = cStringConstant(display);
  emittingShared = was;
  shared += sharedComdat(name) + name + " = linkonce_odr constant { ptr, i64, ptr } { ptr " + str + ", i64 " + (exact ? "-1" : "-2") + ", ptr " + real +
            " }, comdat\n";
  return name;
}

auto codegen::Emitter::implicitDestructor(const Ty* t, Node* at) -> std::string
{
  std::string thunk = destructorThunk(t, at);
  if (thunk != "null")
    return thunk;
  auto lock = sharedLock();
  if (!emittedShared["@kpp.nodtor"])
  {
    emittedShared["@kpp.nodtor"] = true;
    shared += sharedComdat("@kpp.nodtor") + "define linkonce_odr void @kpp.nodtor(ptr %o) comdat {\nentry:\n  ret void\n}\n";
  }
  return "@kpp.nodtor";
}

auto codegen::Emitter::destructorThunk(const Ty* t, Node* at) -> std::string
{
  if (!needsDestruction(t, at))
    return "null";
  // (an array's: `thread_local Tracked pair[2];` destroyed at its thread's exit)
  std::string name = isClass(t) ? "@kpp.dtor." + structFor(t, at).name.substr(8) : "@\"kpp.dtor." + types.toString(t, &symbols) + "\"";
  auto lock = sharedLock(); // held while the thunk is emitted: one emitter writes it
  if (emittedShared[name])
    return name;
  emittedShared[name] = true;
  FunctionState saved = saveState();
  std::string savedOverride = funcletOverride;
  const Ty* savedHelperFor = destructorHelperFor;
  destructorHelperFor = types.stripRef(t); // (its own body spells its destruction out)
  funcletOverride.clear();
  resetFunctionState();
  choicesRecorded = false;
  try
  {
    noexceptDestructorHelper = isClass(t) && !destructionMayThrow(t, at);
    destroy("%o", t, at);
    fn += "  ret void\n";
    shared += sharedComdat(name) + "define linkonce_odr void " + name + "(ptr %o) comdat " + (usesEH ? "personality ptr " + personality() + " " : std::string()) +
              "{\nentry:\n" + allocas + fn + ehBlocks + "}\n";
  }
  catch (Unsupported&)
  {
    restoreState(saved);
    funcletOverride = savedOverride;
    destructorHelperFor = savedHelperFor;
    ++sh.failuresRemembered; // (marked emitted: its later uses do not fail)
    throw;
  }
  restoreState(saved);
  funcletOverride = savedOverride;
  destructorHelperFor = savedHelperFor;
  return name;
}

auto codegen::Emitter::emitThrow(ThrowExpr* t) -> void
{
  usesExceptions = true;
  if (!t->operand)
  {
    const EHFrame* handler = nullptr;
    for (size_t i = ehFrames.size(); i-- > 0 && !handler;)
      if (ehFrames[i].isHandler)
        handler = &ehFrames[i];
    std::string slot;
    if (handler)
      slot = handler->exceptionSlot;
    else
    {
      // In a function a handler calls (an exception dispatcher's `try { throw; } catch ...`):
      // the exception that handler holds, kpp.eh.current; with none, a wrong program, stopped.
      slot = newTemp();
      fn += "  " + slot + " = load ptr, ptr @kpp.eh.current\n";
      if (declaredNames.claim("@llvm.trap"))
        decls += "declare void @llvm.trap()\n";
      std::string none = newTemp(), okL = newLabel("rethrow"), noneL = newLabel("rethrow.none");
      fn += "  " + none + " = icmp eq ptr " + slot + ", null\n";
      fn += "  br i1 " + none + ", label %" + noneL + ", label %" + okL + "\n";
      startBlock(noneL);
      fn += "  call void @llvm.trap()\n  unreachable\n";
      blockTerminated = true;
      startBlock(okL);
    }
    destroyTemps();
    emitCall("", "void @kpp_eh_rethrow(ptr " + slot + ")");
  }
  else
  {
    const Ty* ty = t->operand->ty ? types.unqualified(types.stripRef(t->operand->ty)) : nullptr;
    if (!ty || !ty->isKnown() || types.isDependent(ty))
      unsupported(t, "throwing a value of unknown type");
    if (ty->kind == TyKind::Array)
      ty = types.pointer(ty->inner);
    else if (ty->kind == TyKind::Function)
      ty = types.pointer(ty); // `throw f;`: a pointer to the function ([except.throw] 3), which handlers of the pointer take
    ensureMalloc();
    std::string obj = newTemp();
    emitCall(obj + " = ", "ptr @malloc(i64 " + std::to_string(sizeOf(ty, t)) + ")", false);
    // `throw m;` with m a local, as `return m;`: C++'s implicit move ([class.copy.elision] 3), when no
    // try block lies between the throw and m's scope (m declared inside the innermost try's body that
    // holds the throw, or outside every try). Until 2026-11-08 a copy.
    const Ty* movable = nullptr;
    if (isClass(ty) && implicitlyMovableLocal(t->operand, movable) && classKey(movable) == classKey(ty))
    {
      Expr* e = t->operand;
      while (e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      size_t declared = locals[static_cast<NameExpr*>(e)->resolved].scopeDepth;
      for (size_t i = ehFrames.size(); i-- > 0;)
        if (!ehFrames[i].isHandler)
        {
          if (declared <= ehFrames[i].scopeDepth)
            movable = nullptr;
          break;
        }
    }
    else
      movable = nullptr;
    if (movable)
      moveConstruct(obj, lvalue(t->operand), ty, t);
    else if (isClass(ty))
      initializeFrom(obj, ty, t->operand, t);
    else
      store(convert(rvalue(t->operand), ty, t), obj, ty, t);
    std::string ti = typeinfoFor(ty, t), dtor = destructorThunk(ty, t);
    destroyTemps();
    emitCall("", "void @kpp_throw(ptr " + obj + ", ptr " + ti + ", ptr " + dtor + ")");
  }
  // No `unreachable` here, on purpose: the code after a throw stays, so that the optimizer never
  // proves a function noreturn. The SEH scope tables need an instruction after every throwing
  // call; a catch handler placed right at a call's return address would make the runtime skip
  // the cleanups between them, taking the unwind for a jump within the scope.
}

auto codegen::Emitter::constructorTryOf(FunctionDecl* f) -> TryStmt*
{
  if (!f->body || f->body->kind != NodeKind::BlockStmt)
    return nullptr;
  auto* b = static_cast<BlockStmt*>(f->body);
  if (b->stmts.size() != 1 || b->stmts[0]->kind != NodeKind::TryStmt || !static_cast<TryStmt*>(b->stmts[0])->constructorTry)
    return nullptr;
  return static_cast<TryStmt*>(b->stmts[0]);
}

auto codegen::Emitter::tryBody(TryStmt* t) -> void
{
  if (!t->constructorTry || !currentFunction || !(currentFunction->flags & DF_Constructor))
  {
    statement(t->body);
    return;
  }
  // A constructor's function-try-block: its member initializers inside the try, in a scope of
  // their own, so that an exception destroys the subobjects made before the handler runs.
  pushScope();
  initializeMembers(thisPointer(currentFunction), currentClassTy, currentFunction->ctorInits, currentFunction, true);
  destroyTemps();
  statement(t->body);
  popScope();
}

auto codegen::Emitter::tryStatement(TryStmt* t) -> void
{
  usesExceptions = true;
  if (!sh.target.windows())
  {
    itaniumTry(t);
    return;
  }
  markEH();
  std::string endL = newLabel("try.end"), dispatchL = newLabel("catch.dispatch"), handlerL = newLabel("catch.pad");
  ehFrames.push_back({false, dispatchL, currentFunclet(), "", "", scopes.size()});
  tryBody(t);
  emitBranch(endL);
  EHFrame frame = ehFrames.back();
  ehFrames.pop_back();
  if (!frame.used)
  {
    startBlock(endL); // nothing in the body can throw to the handlers
    return;
  }
  // The filter function, named by the function and where the try is written (the same in every
  // run, whatever order the functions are emitted in).
  std::string owner = currentFunctionName.size() > 2 && currentFunctionName[1] == '"' ? currentFunctionName.substr(2, currentFunctionName.size() - 3)
                                                                                      : currentFunctionName.substr(currentFunctionName.empty() ? 0 : 1);
  std::string filter = "@\"kpp.filter." + owner + "." + std::to_string(t->token) + "\"";
  std::string ft = "define i32 " + filter + "(ptr %pointers, ptr %frame) {\nentry:\n"
                   "  %rec = load ptr, ptr %pointers\n  %code = load i32, ptr %rec\n"
                   "  %tip = getelementptr inbounds i8, ptr %rec, i64 40\n  %ti = load ptr, ptr %tip\n"
                   "  %is = icmp eq i32 %code, -536870911\n  br i1 %is, label %check0, label %no\n";
  for (size_t i = 0; i < t->handlers.size(); ++i)
  {
    std::string n = std::to_string(i), nextL = i + 1 < t->handlers.size() ? "check" + std::to_string(i + 1) : "no";
    ft += "check" + n + ":\n";
    if (!t->handlers[i].param)
    {
      ft += "  call void @kpp_eh_push(ptr %rec, i32 " + n + ", i64 0)\n  ret i32 1\n";
      break;
    }
    const Ty* pt = declType(t->handlers[i].param);
    if (!pt || !pt->isKnown())
      unsupported(t->handlers[i].param, "a catch parameter of unknown type");
    ft += "  %off" + n + " = call i64 @kpp_eh_offset(ptr %ti, ptr " + handlerTypeinfo(pt, t) + ")\n"
          "  %ok" + n + " = icmp sge i64 %off" + n + ", 0\n  br i1 %ok" + n + ", label %yes" + n + ", label %" + nextL + "\n"
          "yes" + n + ":\n  call void @kpp_eh_push(ptr %rec, i32 " + n + ", i64 %off" + n + ")\n  ret i32 1\n";
  }
  ft += "no:\n  ret i32 0\n}\n\n";
  addDefinition(filter, ft);
  // The dispatch and the handlers.
  std::string outer = unwindDestination(); // objects outside the try, then the enclosing try
  startBlock(dispatchL);
  std::string cs = newTemp(), pad = newTemp();
  fn += "  " + cs + " = catchswitch within " + (frame.parentFunclet.empty() ? "none" : frame.parentFunclet) + " [label %" + handlerL +
        "] unwind " + (outer.empty() ? "to caller" : "label %" + outer) + "\n";
  startBlock(handlerL);
  fn += "  " + pad + " = catchpad within " + cs + " [ptr " + filter + "]\n";
  std::string slot = "%v" + std::to_string(temp++);
  allocas += "  " + slot + " = alloca [6 x i64]\n"; // object, typeinfo, destructor, handler, adjusted object, the handler around this one (kpp.eh.current)
  ehFrames.push_back({true, outer, frame.parentFunclet, pad, slot, scopes.size()});
  fn += "  call void @kpp_eh_begin(ptr " + slot + ")" + bundle() + "\n";
  std::vector<std::string> handlerLabels;
  for (size_t i = 0; i < t->handlers.size(); ++i)
    handlerLabels.push_back(newLabel("catch.body"));
  if (t->handlers.size() == 1)
    emitBranch(handlerLabels[0]);
  else
  {
    std::string hp = newTemp(), h = newTemp();
    fn += "  " + hp + " = getelementptr inbounds i8, ptr " + slot + ", i64 24\n  " + h + " = load i64, ptr " + hp + "\n";
    fn += "  switch i64 " + h + ", label %" + handlerLabels[0] + " [";
    for (size_t i = 1; i < t->handlers.size(); ++i)
      fn += " i64 " + std::to_string(i) + ", label %" + handlerLabels[i];
    fn += " ]\n";
    blockTerminated = true;
  }
  catchHandlers(t, slot, handlerLabels, pad, endL);
  ehFrames.pop_back();
  startBlock(endL);
}

auto codegen::Emitter::catchHandlers(TryStmt* t, const std::string& slot, const std::vector<std::string>& handlerLabels, const std::string& pad, const std::string& endL) -> void
{
  for (size_t i = 0; i < handlerLabels.size(); ++i)
  {
    const TryStmt::Handler& h = t->handlers[i];
    startBlock(handlerLabels[i]);
    pushScope();
    scopes.back().push_back({slot, nullptr, true});
    uint32_t outerDebugBlock = debugBlock;
    if (h.param)
    {
      openDebugBlock(h.param); // (the parameter is its handler's, as a block's variable)
      debugLine(h.param);
      const Ty* pt = declType(h.param);
      std::string ap = newTemp(), obj = newTemp();
      fn += "  " + ap + " = getelementptr inbounds i8, ptr " + slot + ", i64 32\n  " + obj + " = load ptr, ptr " + ap + "\n";
      const Ty* pointer = types.unqualified(types.stripRef(pt));
      if (pointer->kind == TyKind::Pointer && !(pt->isReference() && !types.stripRef(pt)->isConst))
      {
        // A pointer handler by value or by const reference: the thrown pointer converted to its type
        // ([except.handle] 3.3), moved by the offset the match found (the adjusted object's distance
        // from the object, which holds the pointer), a null pointer staying null; a const reference
        // binds the converted pointer, as a temporary. (A non-const reference matched its own type
        // alone: it binds the exception object, below.)
        std::string object = newTemp(), value = newTemp(), from = newTemp(), to = newTemp(), offset = newTemp(), moved = newTemp(),
                    isNull = newTemp(), converted = newTemp();
        fn += "  " + object + " = load ptr, ptr " + slot + "\n  " + value + " = load ptr, ptr " + object + "\n";
        fn += "  " + from + " = ptrtoint ptr " + object + " to i64\n  " + to + " = ptrtoint ptr " + obj + " to i64\n";
        fn += "  " + offset + " = sub i64 " + to + ", " + from + "\n  " + moved + " = getelementptr i8, ptr " + value + ", i64 " + offset + "\n";
        fn += "  " + isNull + " = icmp eq ptr " + value + ", null\n  " + converted + " = select i1 " + isNull + ", ptr null, ptr " + moved + "\n";
        std::string v = "%v" + std::to_string(temp++);
        allocas += "  " + v + " = alloca ptr\n";
        fn += "  store ptr " + converted + ", ptr " + v + "\n";
        if (pt->isReference())
        {
          std::string ref = "%v" + std::to_string(temp++);
          allocas += "  " + ref + " = alloca ptr\n";
          fn += "  store ptr " + v + ", ptr " + ref + "\n";
          locals[h.param] = {ref, pt, true};
        }
        else
          locals[h.param] = {v, pt, false};
      }
      else if (pt->isReference())
      {
        std::string ref = "%v" + std::to_string(temp++);
        allocas += "  " + ref + " = alloca ptr\n";
        fn += "  store ptr " + obj + ", ptr " + ref + "\n";
        locals[h.param] = {ref, pt, true};
      }
      else if (isClass(pt))
      {
        std::string copy = newObjectSlot(pt, h.param);
        copyConstruct(copy, obj, pt, h.param);
        locals[h.param] = {copy, pt, false};
        if (needsDestruction(pt, h.param))
          scopes.back().push_back({copy, pt});
      }
      else
      {
        std::string v = "%v" + std::to_string(temp++), loaded = newTemp();
        allocas += "  " + v + " = alloca " + llType(pt, h.param) + "\n";
        fn += "  " + loaded + " = load " + llType(pt, h.param) + ", ptr " + obj + "\n  store " + llType(pt, h.param) + " " + loaded + ", ptr " + v + "\n";
        locals[h.param] = {v, pt, false};
      }
      locals[h.param].scopeDepth = scopes.size();
      if (uint32_t name = static_cast<ParamDecl*>(h.param)->name; name != UINT32_MAX)
        debugVariable(sym(name), h.param, locals[h.param].addr, pt);
    }
    statement(h.body);
    popScope();
    debugBlock = outerDebugBlock;
    if (!blockTerminated && pad.empty())
      emitBranch(endL);
    else if (!blockTerminated)
    {
      fn += "  catchret from " + pad + " to label %" + endL + "\n";
      blockTerminated = true;
    }
  }
}

auto codegen::Emitter::itaniumTry(TryStmt* t) -> void
{
  std::string endL = newLabel("try.end"), dispatchL = newLabel("catch.dispatch");
  EHFrame frame{false, dispatchL, "", "", "", scopes.size()};
  for (const TryStmt::Handler& h : t->handlers)
  {
    if (!h.param)
    {
      frame.catchTypes.push_back("null");
      break; // (a handler after catch (...) is never reached)
    }
    const Ty* pt = declType(h.param);
    if (!pt || !pt->isKnown())
      unsupported(h.param, "a catch parameter of unknown type");
    frame.catchTypes.push_back(handlerTypeinfo(pt, t));
  }
  ehFrames.push_back(frame);
  tryBody(t);
  emitBranch(endL);
  frame = ehFrames.back();
  ehFrames.pop_back();
  if (!frame.used)
  {
    startBlock(endL); // nothing in the body can throw to the handlers
    return;
  }
  // Where the exception goes on when no handler takes it: the objects since the enclosing try or
  // handler, then its dispatch (or where it goes), else the caller.
  std::vector<Object> objects = itaniumObjects();
  std::string next = ehFrames.empty() ? std::string() : ehFrames.back().dispatchLabel;
  if (!ehFrames.empty() && !ehFrames.back().isHandler)
    ehFrames.back().used = true;
  std::string outer = itaniumChain(objects, 0, next);
  itaniumSlots();
  if (declaredNames.claim("@llvm.eh.typeid.for.p0"))
    decls += "declare i32 @llvm.eh.typeid.for.p0(ptr)\n";
  startBlock(dispatchL);
  std::string sel = newTemp();
  fn += "  " + sel + " = load i32, ptr %eh.sel\n";
  std::string slot = "%v" + std::to_string(temp++);
  allocas += "  " + slot + " = alloca [7 x i64]\n"; // object, typeinfo, destructor, handler, adjusted object, the handler around this one, the exception's header
  std::vector<std::string> handlerLabels, enterLabels;
  for (size_t i = 0; i < frame.catchTypes.size(); ++i)
  {
    handlerLabels.push_back(newLabel("catch.body"));
    enterLabels.push_back(newLabel("catch.enter"));
  }
  for (size_t i = 0; i < frame.catchTypes.size(); ++i)
  {
    std::string id = newTemp(), is = newTemp(), nextL = i + 1 < frame.catchTypes.size() ? newLabel("catch.next") : outer;
    fn += "  " + id + " = call i32 @llvm.eh.typeid.for.p0(ptr " + frame.catchTypes[i] + ")\n  " + is + " = icmp eq i32 " + sel + ", " + id +
          "\n  br i1 " + is + ", label %" + enterLabels[i] + ", label %" + nextL + "\n";
    blockTerminated = true;
    startBlock(enterLabels[i]);
    std::string exc = newTemp();
    fn += "  " + exc + " = load ptr, ptr %eh.exc\n  call void @kpp_eh_enter(ptr " + slot + ", ptr " + exc + ", i32 " + std::to_string(i) +
          ", ptr " + frame.catchTypes[i] + ")\n";
    emitBranch(handlerLabels[i]);
    if (nextL != outer)
      startBlock(nextL);
  }
  ehFrames.push_back({true, outer, "", "", slot, scopes.size()});
  catchHandlers(t, slot, handlerLabels, "", endL);
  ehFrames.pop_back();
  startBlock(endL);
}

auto codegen::Emitter::exceptionMessage(const std::string& format) -> std::string
{
  if (exceptionTypeinfo.empty() || !exceptionMessageMark)
    return "  br label %end\n";
  return "  %off = call i64 @kpp_eh_offset(ptr %typeinfo, ptr " + exceptionTypeinfo + ")\n  %isExc = icmp sge i64 %off, 0\n"
         "  br i1 %isExc, label %msg, label %end\n"
         "msg:\n  %ex = getelementptr inbounds i8, ptr %object, i64 %off\n"
         "  %ep = getelementptr inbounds i8, ptr %ex, i64 " + exceptionMessageOffset + "\n"
         "  %markp = getelementptr inbounds i8, ptr %ep, i64 " + std::to_string(exceptionMessageMark) + "\n  %mark = load i8, ptr %markp\n"
         "  %large = icmp eq i8 %mark, -128\n  br i1 %large, label %heap, label %print\n"
         "heap:\n  %heapChars = load ptr, ptr %ep\n  %nodata = icmp eq ptr %heapChars, null\n  br i1 %nodata, label %end, label %print\n"
         "print:\n  %data = phi ptr [ %ep, %msg ], [ %heapChars, %heap ]\n"
         "  %r2 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + format + ", ptr %data)\n  br label %end\n";
}

auto codegen::Emitter::capturedRelease(const std::string& next) -> std::string
{
  if (!declaredNames.contains("@kpp_captured_release"))
    return "";
  return "  %captured = icmp eq ptr %dtor, @kpp_captured_release\n  br i1 %captured, label %unref, label %owned\n"
         "unref:\n  call void @kpp_captured_release(ptr %obj)\n  store ptr null, ptr %out\n  br label %" + next + "\nowned:\n";
}

auto codegen::Emitter::uncaughtChange(int delta) -> std::string
{
  if (!declaredNames.contains("@kpp_uncaught_change"))
    return "";
  return "  call void @kpp_uncaught_change(i32 " + std::to_string(delta) + ")\n";
}

auto codegen::Emitter::terminateHandlerCall() -> std::string
{
  if (!declaredNames.contains("@kpp_terminate_handler"))
    return "";
  return "  %handler = call ptr @kpp_terminate_handler()\n  %unset = icmp eq ptr %handler, null\n  br i1 %unset, label %abort, label %call\n"
         "call:\n  call void %handler()\n  br label %abort\nabort:\n";
}

auto codegen::Emitter::finishExceptionRuntime() -> void
{
  auto declareOnce = [&](const char* name, const std::string& line)
  {
    if (declaredNames.claim(name))
      decls += line + "\n";
  };
  if (usesExceptions && !sh.target.windows())
  {
    // Off Windows, the Itanium model (itaniumUnwindDestination, itaniumTry, main.cpp's
    // itaniumRuntime). kpp_throw puts the object, its descriptor and its destructor after the
    // unwinder's 32-byte header, class "KPP\0KPP\0", and raises it. _Unwind_RaiseException returns
    // only when the search finds no handler: kpp_eh_terminate then reports it, as the personality
    // does for a noexcept function in the way, with the throw site still on the stack (kovarex,
    // 2026-11-03), and aborts. A handler takes the exception into its record (kpp_eh_enter), whose
    // end destroys and frees the object and frees the header.
    ensureMalloc();
    declareOnce("@fprintf", "declare i32 @fprintf(ptr, ptr, ...)");
    declareOnce("@fflush", "declare i32 @fflush(ptr)");
    declareOnce("@abort", "declare void @abort()");
    declareOnce("@_Unwind_RaiseException", "declare i32 @_Unwind_RaiseException(ptr)");
    emittingShared = true;
    std::string uncaught = cStringConstant("unhandled exception of type %s"), throughNoexcept = cStringConstant("exception of type %s thrown through a noexcept function"),
                unlisted = cStringConstant("exception of type %s thrown from a call its function's exception table does not list"),
                foreign = cStringConstant("unhandled exception of another language"), noMemory = cStringConstant("no memory for an exception\n"),
                fmt2 = cStringConstant(": %s"), fmt3 = cStringConstant("\n");
    emittingShared = false;
    std::string err = stderrStream();
    decls += sharedComdat("@kpp.eh.current") + kppEhCurrentLine + "\n";
    shared += baseSearchFunction("@kpp_eh_offset", typeinfoFor(types.nullptrType(), nullptr)); // (a pointer handler takes nullptr)
    std::string message = exceptionMessage(fmt2), handler = terminateHandlerCall();
    shared += sharedComdat("@kpp_throw") +
              std::string(kppThrowHeader) + "\nentry:\n"
              "  %h = call ptr @malloc(i64 64)\n  %none = icmp eq ptr %h, null\n  br i1 %none, label %nomemory, label %raise\n"
              "nomemory:\n  %flushed = call i32 @fflush(ptr null)\n" + err +
              "  %r0 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + noMemory + ")\n  call void @abort()\n  unreachable\n"
              "raise:\n  store i64 21198927785840720, ptr %h\n" // "KPP\0KPP\0"
              "  %cleanup = getelementptr inbounds i8, ptr %h, i64 8\n  store ptr null, ptr %cleanup\n"
              "  %private = getelementptr inbounds i8, ptr %h, i64 16\n  call void @llvm.memset.p0.i64(ptr %private, i8 0, i64 16, i1 false)\n"
              "  %op = getelementptr inbounds i8, ptr %h, i64 32\n  store ptr %object, ptr %op\n"
              "  %tp = getelementptr inbounds i8, ptr %h, i64 40\n  store ptr %typeinfo, ptr %tp\n"
              "  %dp = getelementptr inbounds i8, ptr %h, i64 48\n  store ptr %dtor, ptr %dp\n" + uncaughtChange(1) +
              "  %r = call i32 @_Unwind_RaiseException(ptr %h)\n  call void @kpp_eh_terminate(ptr %h, i32 1)\n  unreachable\n}\n";
    declareOnce("@llvm.memset.p0.i64", "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)");
    shared += sharedComdat("@kpp_eh_terminate") +
              std::string(kppEhTerminateHeader) + "\nentry:\n"
              "  %flushed = call i32 @fflush(ptr null)\n" + // (the program's output first: abort writes no buffer out)
              err +
              "  %class = load i64, ptr %h\n  %isKpp = icmp eq i64 %class, 21198927785840720\n  br i1 %isKpp, label %kpp, label %foreign\n"
              "foreign:\n  %r0 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + foreign + ")\n  br label %end\n"
              "kpp:\n  %op = getelementptr inbounds i8, ptr %h, i64 32\n  %object = load ptr, ptr %op\n"
              "  %tp = getelementptr inbounds i8, ptr %h, i64 40\n  %typeinfo = load ptr, ptr %tp\n"
              "  %isNoexcept = icmp eq i32 %why, 2\n  %isUnlisted = icmp eq i32 %why, 3\n"
              "  %fmt0 = select i1 %isNoexcept, ptr " + throughNoexcept + ", ptr " + uncaught + "\n"
              "  %fmt = select i1 %isUnlisted, ptr " + unlisted + ", ptr %fmt0\n"
              "  %name = load ptr, ptr %typeinfo\n  %r1 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr %fmt, ptr %name)\n" + message +
              "end:\n  %r3 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt3 + ")\n" + handler + "  call void @abort()\n  unreachable\n}\n";
    shared += sharedComdat("@kpp_eh_enter") +
              std::string(kppEhEnterHeader) + "\nentry:\n" + uncaughtChange(-1) +
              "  %op = getelementptr inbounds i8, ptr %h, i64 32\n  %object = load ptr, ptr %op\n"
              "  %tp = getelementptr inbounds i8, ptr %h, i64 40\n  %ti = load ptr, ptr %tp\n"
              "  %dp = getelementptr inbounds i8, ptr %h, i64 48\n  %dtor = load ptr, ptr %dp\n"
              "  %any = icmp eq ptr %target, null\n  br i1 %any, label %join, label %search\n"
              "search:\n  %found = call i64 @kpp_eh_offset(ptr %ti, ptr %target)\n  br label %join\n"
              "join:\n  %offset = phi i64 [ 0, %entry ], [ %found, %search ]\n  %adjusted = getelementptr inbounds i8, ptr %object, i64 %offset\n"
              "  %s1 = getelementptr inbounds i64, ptr %out, i64 1\n  %s2 = getelementptr inbounds i64, ptr %out, i64 2\n"
              "  %s3 = getelementptr inbounds i64, ptr %out, i64 3\n  %s4 = getelementptr inbounds i64, ptr %out, i64 4\n"
              "  %s5 = getelementptr inbounds i64, ptr %out, i64 5\n  %s6 = getelementptr inbounds i64, ptr %out, i64 6\n"
              "  store ptr %object, ptr %out\n  store ptr %ti, ptr %s1\n  store ptr %dtor, ptr %s2\n  %hz = zext i32 %handler to i64\n  store i64 %hz, ptr %s3\n"
              "  store ptr %adjusted, ptr %s4\n  store ptr %h, ptr %s6\n"
              "  %outer = load ptr, ptr @kpp.eh.current\n  store ptr %outer, ptr %s5\n  store ptr %out, ptr @kpp.eh.current\n  ret void\n}\n";
    shared += sharedComdat("@kpp_eh_end") +
              std::string(kppEhEndHeader) + "\nentry:\n"
              "  %op = getelementptr inbounds i8, ptr %out, i64 40\n  %outer = load ptr, ptr %op\n  store ptr %outer, ptr @kpp.eh.current\n"
              "  %obj = load ptr, ptr %out\n  %null = icmp eq ptr %obj, null\n  br i1 %null, label %header, label %live\n"
              "live:\n  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n"
              + capturedRelease("header") +
              "  %nodtor = icmp eq ptr %dtor, null\n  br i1 %nodtor, label %release, label %destroy\n"
              "destroy:\n  call void %dtor(ptr %obj)\n  br label %release\n"
              "release:\n  call void @free(ptr %obj)\n  store ptr null, ptr %out\n  br label %header\n"
              "header:\n  %hp = getelementptr inbounds i8, ptr %out, i64 48\n  %h = load ptr, ptr %hp\n  call void @free(ptr %h)\n"
              "  store ptr null, ptr %hp\n  ret void\n}\n";
    shared += sharedComdat("@kpp_eh_rethrow") +
              std::string(kppEhRethrowHeader) + "\nentry:\n"
              "  %obj = load ptr, ptr %out\n  %tp = getelementptr inbounds i8, ptr %out, i64 8\n  %ti = load ptr, ptr %tp\n"
              "  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n  store ptr null, ptr %out\n"
              "  call void @kpp_throw(ptr %obj, ptr %ti, ptr %dtor)\n  ret void\n}\n";
    // std::current_exception's capture (the library's currentException, core/Error.kpp): the exception
    // of the innermost handler running on this thread (kpp.eh.current's record), its object, descriptor
    // and destructor written to into's three words (zeros when no handler runs). The object passes to
    // the library's registry of captured exceptions, which counts its references: the record's
    // destructor slot then names kpp_captured_release, so that the handler's end (kpp_eh_end) drops the
    // record's reference instead of destroying the object, and a rethrow from an ExceptionPtr throws
    // it so too. The library's kpp_captured_take(object, descriptor, destructor) registers a newly
    // captured one with two references (the record's and the caller's) or takes one more.
    if (declaredNames.contains("@kpp_captured_release") && declaredNames.contains("@kpp_captured_take"))
    shared += sharedComdat("@kpp_current_exception") + std::string(kppCurrentExceptionHeader) + "\nentry:\n"
              "  %s1 = getelementptr inbounds i64, ptr %into, i64 1\n  %s2 = getelementptr inbounds i64, ptr %into, i64 2\n"
              "  store ptr null, ptr %into\n  store ptr null, ptr %s1\n  store ptr null, ptr %s2\n"
              "  %rec = load ptr, ptr @kpp.eh.current\n  %none = icmp eq ptr %rec, null\n  br i1 %none, label %done, label %some\n"
              "some:\n  %obj = load ptr, ptr %rec\n  %gone = icmp eq ptr %obj, null\n  br i1 %gone, label %done, label %take\n"
              "take:\n  %tp = getelementptr inbounds i8, ptr %rec, i64 8\n  %ti = load ptr, ptr %tp\n"
              "  %dp = getelementptr inbounds i8, ptr %rec, i64 16\n  %dtor = load ptr, ptr %dp\n"
              "  call void @kpp_captured_take(ptr %obj, ptr %ti, ptr %dtor)\n  store ptr @kpp_captured_release, ptr %dp\n"
              "  store ptr %obj, ptr %into\n  store ptr %ti, ptr %s1\n  store ptr @kpp_captured_release, ptr %s2\n  br label %done\n"
              "done:\n  ret void\n}\n";
  }
  else if (usesExceptions)
  {
    ensureMalloc();
    declareOnce("@RaiseException", "declare dllimport void @RaiseException(i32, i32, i32, ptr)");
    declareOnce("@fprintf", "declare i32 @fprintf(ptr, ptr, ...)");
    declareOnce("@fflush", "declare i32 @fflush(ptr)");
    declareOnce("@__acrt_iob_func", "declare ptr @__acrt_iob_func(i32)");
    declareOnce("@abort", "declare void @abort()");
    markEH();
    emittingShared = true;
    std::string fmt1 = cStringConstant("unhandled exception of type %s"), fmt2 = cStringConstant(": %s"), fmt3 = cStringConstant("\n");
    emittingShared = false;
    shared += sharedComdat("@kpp.eh.stack") + "@kpp.eh.stack = linkonce_odr thread_local global [160 x i64] zeroinitializer, comdat\n" +
              sharedComdat("@kpp.eh.depth") + "@kpp.eh.depth = linkonce_odr thread_local global i64 0, comdat\n";
    shared += sharedComdat("@kpp_throw") +
              std::string(kppThrowHeader) + "\nentry:\n"
              "  %args = alloca [3 x i64]\n  %a1 = getelementptr inbounds i64, ptr %args, i64 1\n  %a2 = getelementptr inbounds i64, ptr %args, i64 2\n"
              "  store ptr %object, ptr %args\n  store ptr %typeinfo, ptr %a1\n  store ptr %dtor, ptr %a2\n" + uncaughtChange(1) +
              "  call void @RaiseException(i32 -536870911, i32 1, i32 3, ptr %args)\n  ret void\n}\n";
    shared += baseSearchFunction("@kpp_eh_offset", typeinfoFor(types.nullptrType(), nullptr)); // (a pointer handler takes nullptr)
    shared += sharedComdat("@kpp_eh_push") +
              std::string(kppEhPushHeader) + "\nentry:\n"
              "  %d = load i64, ptr @kpp.eh.depth\n  %i = mul i64 %d, 5\n  %slot = getelementptr inbounds i64, ptr @kpp.eh.stack, i64 %i\n"
              "  %info = getelementptr inbounds i8, ptr %record, i64 32\n  %obj = load i64, ptr %info\n"
              "  %tip = getelementptr inbounds i8, ptr %record, i64 40\n  %ti = load i64, ptr %tip\n"
              "  %dp = getelementptr inbounds i8, ptr %record, i64 48\n  %dtor = load i64, ptr %dp\n"
              "  %h = zext i32 %handler to i64\n  %adj = add i64 %obj, %offset\n"
              "  %s1 = getelementptr inbounds i64, ptr %slot, i64 1\n  %s2 = getelementptr inbounds i64, ptr %slot, i64 2\n"
              "  %s3 = getelementptr inbounds i64, ptr %slot, i64 3\n  %s4 = getelementptr inbounds i64, ptr %slot, i64 4\n"
              "  store i64 %obj, ptr %slot\n  store i64 %ti, ptr %s1\n  store i64 %dtor, ptr %s2\n  store i64 %h, ptr %s3\n  store i64 %adj, ptr %s4\n"
              "  %d1 = add i64 %d, 1\n  store i64 %d1, ptr @kpp.eh.depth\n  ret void\n}\n";
    // A handler's record is the current exception while it runs (kpp.eh.current, the record of
    // the handler around it kept in its sixth word): what `throw;` in a function it calls raises.
    decls += sharedComdat("@kpp.eh.current") + kppEhCurrentLine + "\n";
    shared += sharedComdat("@kpp_eh_begin") +
              std::string(kppEhBeginHeader) + "\nentry:\n" + uncaughtChange(-1) +
              "  %d = load i64, ptr @kpp.eh.depth\n  %d1 = sub i64 %d, 1\n  store i64 %d1, ptr @kpp.eh.depth\n"
              "  %i = mul i64 %d1, 5\n  %slot = getelementptr inbounds i64, ptr @kpp.eh.stack, i64 %i\n"
              "  call void @llvm.memcpy.p0.p0.i64(ptr %out, ptr %slot, i64 40, i1 false)\n"
              "  %outer = load ptr, ptr @kpp.eh.current\n  %op = getelementptr inbounds i8, ptr %out, i64 40\n  store ptr %outer, ptr %op\n"
              "  store ptr %out, ptr @kpp.eh.current\n  ret void\n}\n";
    shared += sharedComdat("@kpp_eh_end") +
              std::string(kppEhEndHeader) + "\nentry:\n"
              "  %op = getelementptr inbounds i8, ptr %out, i64 40\n  %outer = load ptr, ptr %op\n  store ptr %outer, ptr @kpp.eh.current\n"
              "  %obj = load ptr, ptr %out\n  %null = icmp eq ptr %obj, null\n  br i1 %null, label %done, label %live\n"
              "live:\n  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n"
              + capturedRelease("done") +
              "  %nodtor = icmp eq ptr %dtor, null\n  br i1 %nodtor, label %release, label %destroy\n"
              "destroy:\n  call void %dtor(ptr %obj)\n  br label %release\n"
              "release:\n  call void @free(ptr %obj)\n  store ptr null, ptr %out\n  br label %done\n"
              "done:\n  ret void\n}\n";
    shared += sharedComdat("@kpp_eh_rethrow") +
              std::string(kppEhRethrowHeader) + "\nentry:\n"
              "  %obj = load ptr, ptr %out\n  %tp = getelementptr inbounds i8, ptr %out, i64 8\n  %ti = load ptr, ptr %tp\n"
              "  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n  store ptr null, ptr %out\n"
              "  call void @kpp_throw(ptr %obj, ptr %ti, ptr %dtor)\n  ret void\n}\n";
    // std::current_exception's capture (the library's currentException, core/Error.kpp): the exception
    // of the innermost handler running on this thread (kpp.eh.current's record), its object, descriptor
    // and destructor written to into's three words (zeros when no handler runs). The object passes to
    // the library's registry of captured exceptions, which counts its references: the record's
    // destructor slot then names kpp_captured_release, so that the handler's end (kpp_eh_end) drops the
    // record's reference instead of destroying the object, and a rethrow from an ExceptionPtr throws
    // it so too. The library's kpp_captured_take(object, descriptor, destructor) registers a newly
    // captured one with two references (the record's and the caller's) or takes one more.
    if (declaredNames.contains("@kpp_captured_release") && declaredNames.contains("@kpp_captured_take"))
    shared += sharedComdat("@kpp_current_exception") + std::string(kppCurrentExceptionHeader) + "\nentry:\n"
              "  %s1 = getelementptr inbounds i64, ptr %into, i64 1\n  %s2 = getelementptr inbounds i64, ptr %into, i64 2\n"
              "  store ptr null, ptr %into\n  store ptr null, ptr %s1\n  store ptr null, ptr %s2\n"
              "  %rec = load ptr, ptr @kpp.eh.current\n  %none = icmp eq ptr %rec, null\n  br i1 %none, label %done, label %some\n"
              "some:\n  %obj = load ptr, ptr %rec\n  %gone = icmp eq ptr %obj, null\n  br i1 %gone, label %done, label %take\n"
              "take:\n  %tp = getelementptr inbounds i8, ptr %rec, i64 8\n  %ti = load ptr, ptr %tp\n"
              "  %dp = getelementptr inbounds i8, ptr %rec, i64 16\n  %dtor = load ptr, ptr %dp\n"
              "  call void @kpp_captured_take(ptr %obj, ptr %ti, ptr %dtor)\n  store ptr @kpp_captured_release, ptr %dp\n"
              "  store ptr %obj, ptr %into\n  store ptr %ti, ptr %s1\n  store ptr @kpp_captured_release, ptr %s2\n  br label %done\n"
              "done:\n  ret void\n}\n";
    shared += sharedComdat("@kpp.eh.filter.any") +
              std::string(kppEhFilterAnyHeader) + "\nentry:\n"
              "  %rec = load ptr, ptr %pointers\n  %code = load i32, ptr %rec\n  %is = icmp eq i32 %code, -536870911\n"
              "  br i1 %is, label %yes, label %no\n"
              // (no handler took it: the search ends the program here, the throw site on the stack)
              "yes:\n  %op = getelementptr inbounds i8, ptr %rec, i64 32\n  %obj = load ptr, ptr %op\n"
              "  %tp = getelementptr inbounds i8, ptr %rec, i64 40\n  %ti = load ptr, ptr %tp\n"
              "  call void @kpp_eh_unhandled(ptr %obj, ptr %ti, i32 1)\n  unreachable\nno:\n  ret i32 0\n}\n";
    std::string message = exceptionMessage(fmt2), handler = terminateHandlerCall();
    emittingShared = true;
    std::string throughNoexcept = cStringConstant("exception of type %s thrown through a noexcept function");
    emittingShared = false;
    shared += sharedComdat("@kpp_eh_unhandled") +
              std::string(kppEhUnhandledHeader) + "\nentry:\n"
              "  %flushed = call i32 @fflush(ptr null)\n" // (the program's output first: abort writes no buffer out)
              "  %err = call ptr @__acrt_iob_func(i32 2)\n  %name = load ptr, ptr %typeinfo\n"
              "  %isNoexcept = icmp eq i32 %why, 2\n  %fmt = select i1 %isNoexcept, ptr " + throughNoexcept + ", ptr " + fmt1 + "\n"
              "  %r1 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr %fmt, ptr %name)\n" + message +
              "end:\n  %r3 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt3 + ")\n" + handler + "  call void @abort()\n  unreachable\n}\n";
    declareOnce("@llvm.memcpy.p0.p0.i64", "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)");
  }
  // On Windows a noexcept function's outermost catchpad (noexceptDispatch): the search, reaching
  // its frame with a kpp exception no handler inside took, reports it and ends the program there,
  // the throw site on the stack (C++'s std::terminate), as off Windows the personality does at the
  // empty filter. Another exception (an access violation) goes on searching. A program that throws
  // nothing has no kpp exception to stop.
  if (usesNoexceptFilter && sh.target.windows())
  {
    // (one append: the shared text is taken definition by definition)
    shared += sharedComdat("@kpp.eh.filter.noexcept") + std::string(kppEhFilterNoexceptHeader) + "\nentry:\n" +
              (usesExceptions ? "  %rec = load ptr, ptr %pointers\n  %code = load i32, ptr %rec\n  %is = icmp eq i32 %code, -536870911\n"
                                "  br i1 %is, label %yes, label %no\n"
                                "yes:\n  %op = getelementptr inbounds i8, ptr %rec, i64 32\n  %obj = load ptr, ptr %op\n"
                                "  %tp = getelementptr inbounds i8, ptr %rec, i64 40\n  %ti = load ptr, ptr %tp\n"
                                "  call void @kpp_eh_unhandled(ptr %obj, ptr %ti, i32 2)\n  unreachable\nno:\n  ret i32 0\n}\n"
                              : "  ret i32 0\n}\n");
  }
  // The program's entry: main, or on Windows the program's wmain when it defines no main (the game's,
  // which makes its wide arguments UTF-8 itself: design.md, "One spelling on every platform"). With
  // both (the game's tests: src's wmain beside the runner's main) main is the entry, as the C++
  // linkers choose it.
  if (!emitEntry("@kpp.main") && sh.target.windows()) // (wmain is Windows' entry alone)
    emitEntry("@kpp.wmain");
}

auto codegen::Emitter::wideArguments() -> std::string
{
  ensureMalloc();
  if (declaredNames.claim("@WideCharToMultiByte"))
    decls += "declare dllimport i32 @WideCharToMultiByte(i32, i32, ptr, i32, ptr, i32, ptr, ptr)\n";
  if (declaredNames.claim("@fprintf"))
    decls += "declare i32 @fprintf(ptr, ptr, ...)\n";
  if (declaredNames.claim("@abort"))
    decls += "declare void @abort()\n";
  emittingShared = true;
  std::string notText = cStringConstant("argument %d holds a lone surrogate, which has no UTF-8 form (main's argv is UTF-8)\n"),
              noMemory = cStringConstant("no memory for main's arguments\n");
  emittingShared = false;
  // Each argument measured, then written after the argc + 1 pointers. CP_UTF8 is 65001; with
  // WC_ERR_INVALID_CHARS (128) the conversion fails on a lone surrogate, which it would otherwise
  // write as U+FFFD, a text that names another file.
  std::string convert = "call i32 @WideCharToMultiByte(i32 65001, i32 128, ptr ";
  return "  %count = sext i32 %argc to i64\n  %slots = add i64 %count, 1\n  %table = shl i64 %slots, 3\n  br label %measure\n"
         "measure:\n  %i = phi i64 [ 0, %entry ], [ %i.next, %measured ]\n  %size = phi i64 [ %table, %entry ], [ %size.next, %measured ]\n"
         "  %measuring = icmp slt i64 %i, %count\n  br i1 %measuring, label %measure.one, label %allocate\n"
         "measure.one:\n  %wp = getelementptr inbounds ptr, ptr %wargv, i64 %i\n  %w = load ptr, ptr %wp\n"
         "  %n = " + convert + "%w, i32 -1, ptr null, i32 0, ptr null, ptr null)\n"
         "  %which.i = trunc i64 %i to i32\n  %bad = icmp slt i32 %n, 1\n  br i1 %bad, label %fail, label %measured\n"
         "measured:\n  %n64 = zext i32 %n to i64\n  %size.next = add i64 %size, %n64\n  %i.next = add i64 %i, 1\n  br label %measure\n"
         "allocate:\n  %argv = call ptr @malloc(i64 %size)\n  %none = icmp eq ptr %argv, null\n  br i1 %none, label %fail, label %fill\n"
         "fail:\n  %message = phi ptr [ " + notText + ", %measure.one ], [ " + noMemory + ", %allocate ]\n"
         "  %which = phi i32 [ %which.i, %measure.one ], [ 0, %allocate ]\n" + stderrStream() +
         "  %r0 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr %message, i32 %which)\n  call void @abort()\n  unreachable\n"
         "fill:\n  %j = phi i64 [ 0, %allocate ], [ %j.next, %fill.one ]\n  %at = phi i64 [ %table, %allocate ], [ %at.next, %fill.one ]\n"
         "  %filling = icmp slt i64 %j, %count\n  br i1 %filling, label %fill.one, label %filled\n"
         "fill.one:\n  %vp = getelementptr inbounds ptr, ptr %wargv, i64 %j\n  %v = load ptr, ptr %vp\n"
         "  %out = getelementptr inbounds i8, ptr %argv, i64 %at\n  %slot = getelementptr inbounds ptr, ptr %argv, i64 %j\n  store ptr %out, ptr %slot\n"
         "  %room64 = sub i64 %size, %at\n  %room = trunc i64 %room64 to i32\n"
         "  %m = " + convert + "%v, i32 -1, ptr %out, i32 %room, ptr null, ptr null)\n"
         "  %m64 = zext i32 %m to i64\n  %at.next = add i64 %at, %m64\n  %j.next = add i64 %j, 1\n  br label %fill\n"
         "filled:\n  %end = getelementptr inbounds ptr, ptr %argv, i64 %count\n  store ptr null, ptr %end\n";
}

auto codegen::Emitter::emitEntry(const std::string& mainName) -> bool
{
  std::string declare = declareLines[mainName];
  if (moduleCount <= 1)
  {
    std::string definition = body.fromFirst("define i32 " + mainName + "(");
    if (definition.empty())
      return false;
    declare = definition.substr(0, definition.find(")") + 1);
  }
  if (declare.empty())
    return false;
  bool args = declare.find("(i32, ptr)") != std::string::npos || declare.find("(i32 %p0, ptr %p1)") != std::string::npos;
  // On Windows main's argv is wmain's, as UTF-8: the C runtime's own is in the ANSI code page (U+017E
  // as 9e in 1250, Japanese as `??`), where every other target's is UTF-8, as all of kpp's narrow text
  // is (design.md, "Targets and hosts"). The link then names wmainCRTStartup (driver::wideEntry).
  bool utf8 = args && mainName == "@kpp.main" && sh.target.windows();
  std::string entry = utf8 || mainName == "@kpp.wmain" ? "wmain" : "main";
  std::string call = "i32 " + mainName + (args ? "(i32 %argc, ptr %argv)" : "()");
  // On Windows the program's main runs under a handler whose filter (kpp.eh.filter.any) the search
  // reaches when no handler of the program takes a kpp exception: it reports the exception and ends
  // the program from the search, so the handler itself is never entered. (Off Windows, kpp_throw
  // reports and aborts itself.)
  bool reports = usesExceptions && sh.target.windows();
  std::string code = utf8 ? wideArguments() : "";
  if (!reports)
    code += "  %r = call " + call + "\n  ret i32 %r\n";
  else
    code += "  %r = invoke " + call + " to label %done unwind label %dispatch\n"
            "done:\n  ret i32 %r\n"
            "dispatch:\n  %cs = catchswitch within none [label %handler] unwind to caller\n"
            "handler:\n  %pad = catchpad within %cs [ptr @kpp.eh.filter.any]\n  unreachable\n";
  // With debug information the entry is a function of it too, an artificial one at main's line, each
  // instruction there: what the optimizer inlines into it (main and what main inlines) keeps its lines,
  // inlined at the call. Without, a release build's crash report named every such frame wmain+N with no
  // line (E's finding, 2026-11-07).
  std::pair<std::string, size_t> at;
  if (sh.debugInfo)
  {
    auto lock = sharedLock();
    if (auto it = sh.entryDebug.find(mainName); it != sh.entryDebug.end())
      at = it->second;
  }
  bool described = at.second != 0;
  if (described)
  {
    std::string located;
    for (size_t from = 0; from < code.size();)
    {
      size_t end = std::min(code.find('\n', from), code.size());
      located.append(code, from, end - from);
      if (code.compare(from, 2, "  ") == 0)
        located += ", !dbg " + debugId(1);
      located += '\n';
      from = end + 1;
    }
    code = std::move(located);
  }
  std::string text = "define i32 @" + entry + "(i32 %argc, ptr " + (utf8 ? "%wargv" : "%argv") + ") " +
                     std::string(reports ? "personality ptr @__C_specific_handler " : "") + (described ? "!dbg " + debugId(0) + " " : std::string()) +
                     "{\nentry:\n" + code + "}\n";
  if (described)
    text += debugId(0) + " = distinct !DISubprogram(name: \"" + entry + "\", scope: " + at.first + ", file: " + at.first + ", line: " +
            std::to_string(at.second) + ", type: !2, scopeLine: " + std::to_string(at.second) +
            ", flags: DIFlagArtificial, spFlags: DISPFlagDefinition, unit: !0)\n" + debugId(1) + " = !DILocation(line: " + std::to_string(at.second) +
            ", scope: " + debugId(0) + ")\n";
  text += '\n';
  if (moduleCount <= 1)
    body += text;
  else
  {
    Shared::Definition definition = Shared::definitionOf(text);
    auto lock = sharedLock();
    sh.definitions[definedIn[mainName]].push_back(std::move(definition));
  }
  return true;
}
