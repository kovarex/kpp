# Unique pointer: a built-in type or the library class?

kovarex asked (2026-10-29) whether the unique pointer should be part of the language, with syntax
like `A^ b;`, instead of the library class `UniquePtr<T>` (docs/stdlib/uniqueptr.md). This is a
research note: nothing in the language, the compiler or the library was changed. It records what
Factorio uses, what the class costs kppc today, what clang pays for `std::unique_ptr`, what a
built-in would have to mean, the syntax options, how other languages do it, and a recommendation.

Measured on PC-PRACOVNA (Ryzen 9 9950X, 32 threads) with kppc built from master `89058a6`, on a
fresh conversion of the game (the headline inputs of path-to-factorio.md: CommonUtil, Agui,
cxxopts, Lua and `src/`, 4,102 files with the stdlib, 0.99 M lines). The scripts are in
`compiler/bench/uniqueptr/`; each command is given with its numbers.

## Answer

**Keep `UniquePtr<T>` a library class. If the short spelling is wanted, make `T^` a spelling of
`UniquePtr<T>`, not a new kind of type.** The reasons, each measured below:

- The class costs kppc about 1% of the build: UniquePtr and makeUnique together are 1.5 to 2.2% of
  the front end's instantiation CPU (1.8% of all typed expressions), 1.3% of codegen and 1.1% of
  the IR bytes (1.4% of its instructions). That is about 0.03 s of the front end's wall time and
  0.11 s of codegen on the whole game. A built-in cannot save more than that.
- What clang pays for `std::unique_ptr` (3.1% of its front end, 147 class instantiations in every
  TU, each distinct `unique_ptr<T>` instantiated again in 52 TUs on average) is a cost of
  instantiating per translation unit. kppc instantiates each `UniquePtr<T>` once, which already
  removed that.
- The one large effect is debug-build run time: reading through a UniquePtr at -O0 is 2.5 to 2.7
  times slower than through a plain pointer, because `->` and `bool` are calls. That is true of
  every small library accessor (Vector's `[]`, Optional's `*`, String's `size()`), and it goes away
  the same way for all of them: kppc inlining trivial functions when it emits IR (design.md
  section 2, option (4), already preferred). With `[[forceinline]]` and the instantiation in the
  calling module the ratio drops to 1.2.
- A new type kind is the most expensive thing to build: every place that knows pointers or classes
  must learn it (79 sites name `TyKind::Pointer`, 113 `case TyKind::` labels, 245 tests for a
  class type, in the resolver, codegen and types), plus deduction, traits, layout, destruction and
  cleanups, containers, reflection and the converter. And it would still not cover what Factorio
  needs: custom deleters (12 uses) and arrays (89) stay library classes, so there would be two ways
  to own an object.
- What only a built-in can do (covariant `clone()` returns, an ownership-moving `dynamic_cast`, a
  null niche in `Optional<A^>`) is not used by the converted code: Factorio has 17 `clone()`
  functions, 0 ownership-moving casts and 0 `optional<unique_ptr>`.
- Other languages point the same way (section 5): no current language with destructors has a
  built-in sole-owner pointer type. Rust had one (`~T`) and replaced it with the library's `Box<T>`
  before 1.0, and Nim's `owned ref` gave way to ARC/ORC. What languages add instead is syntax
  around library types: Swift's `T?` for Optional, Cpp2's `new<T>(args)` for make_unique, Mojo's
  `x^` for a move.

`T^` as a spelling is a parser and printer change with nothing behind it: the type stays
`UniquePtr<T>`, so overloads, templates, traits, the specification and the C++ guarantees stay as
they are. Whether to have it is a question of reading, for kovarex (section 3 has the cases).

Found on the way, two wrong-code bugs outside this thread's area (probes in
`compiler/bench/uniqueptr/`):

- `UniquePtr<T, D>` compiles and ignores D: the library's UniquePtr has one parameter, and the
  extra template argument raises no error. In `probe_deleter_ignored.kpp` a deleter that counts its
  calls is called 0 times; the object is deleted with `delete` instead. Factorio's 12
  custom-deleter uses convert to exactly this (CustomGuiElementDeleter calls `destroySafe`,
  HandleDeleter `CloseHandle`, PcloseDeleter `pclose`). Needs an error for too many template
  arguments and a deleter parameter in the library.
- `delete p` with `p == nullptr` crashes when the class has a non-trivial destructor
  (`probe_delete_null.kpp`): codegen.cpp's DeleteExpr destroys and frees without a null check, so
  a virtual destructor is called through a null vtable pointer (0xC0000005). C++ makes it a no-op,
  and `src` has 246 plain `delete x;` statements (92 of them `delete this->x;`) written under that
  rule; the library's UniquePtr tests for null itself, which is why converted unique_ptr code did
  not show it.

## 1. What was measured

### 1.1 Factorio's use of `std::unique_ptr`

`python -X utf8 -I compiler\bench\uniqueptr\survey_up.py C:\Factorio`: a scan of `src`, `tests`,
`libraries/CommonUtil` and `libraries/Agui` (8,056 files) with comments and string literals
stripped. The template arguments are read as spelled, so a T is counted once per spelling.

| | count |
|---|---:|
| `std::unique_ptr<...>` written | **4,064**, in 1,124 files |
| distinct T | 731 as spelled (967 distinct instantiations reach kppc's codegen, 1.3 below) |
| most used T | agui::Widget 977, ElementImageSet 115, Color 110, MemoryBitmap 86, LuaEventFilters 72, Entity 70, BlueprintRecord 66 |
| `std::make_unique<T>(...)` | 990 (434 distinct T), 13 of them `make_unique<T[]>` |
| `.reset(new ...)` | 742 (any smart pointer) |
| `std::unique_ptr<T>(new ...)` | 37 |
| `.release()` | 197 at most (the count includes locks' `release()`) |
| `std::unique_ptr<T[]>` | 89: float 51, char 15, uint8_t 11, others 12 |
| `std::unique_ptr<T, D>` (custom deleter) | **12**, 6 deleters: EntityToBeBuiltDeleter 7 (LatencyState), CustomGuiElementDeleter 1, HandleDeleter 1 (tests), PcloseDeleter 1 (tests), FreeDeleter 1 (Agui's UniqueCPtr), HeapArray's own Deleter 1 |
| CommonUtil's own `UniquePointer<T>` | 332, plus 14 `ThrowingUniquePointer` (a deleter that may throw) and 1 custom deleter (ChartDeleter) |
| `std::unique_ptr<const T>` | 50 |
| `const std::unique_ptr<T>&` / `std::unique_ptr<T>&` / `&&` / `*` | 269 / 160 / 17 / 23 |
| inside another template's arguments | 316: vector 252, map 22, pair 9, array 6, deque 3, FlatMap 3, set 2 (both with comparators on the pointee), unordered_map, SmallVector and list 1 each, 16 in function templates' and std::function's arguments |
| T forward-declared in the same file (incomplete there) | 575 uses in 242 files |
| `dynamic_cast<D*>(p.get())` / `static_cast<D*>(p.get())` | 36 / 80 |
| `virtual std::unique_ptr<Base> clone()` and overrides | 17 |
| `get_deleter()`, `std::hash<unique_ptr>`, `optional<unique_ptr>`, `variant` with a unique_ptr, a set or map ordered by the pointer | 0 |

### 1.2 What the converted game does with UniquePtr

`python -X utf8 -I compiler\bench\uniqueptr\ir_calls.py full.ll` on the IR of the headline command
with `--keep-going` (which assembles the IR despite the 23,548 error lines; functions that failed
are missing, so these are lower bounds). At -O0 every use in the source is one call:

| member | call sites |
|---|---:|
| destructor | **42,133**, of which 31,827 in exception cleanup funclets |
| `operator->` | 4,420 |
| default constructor | 2,521 |
| `release()` | 2,018 (1,425 in ValueToWidget::make, which hands widgets to Agui) |
| `operator*` | 1,517 |
| `operator bool` / `operator!` | 1,347 / 399 |
| `get()` | 1,108 |
| `UniquePtr(T*)` (adopting a raw pointer) / `reset(T*)` | 907 / 595 |
| move constructor / move assignment | 889 / 431 |
| from `UniquePtr<Derived>&&` (derived to base) | 612 (170 distinct pairs) |
| `reset()` | 347 |
| from `nullptr` / `== nullptr`, `!= nullptr` / `= nullptr` | 66 / 33 / 2 |
| derived-to-base assignment / `==` with a plain pointer / `swap` | 9 / 2 / 0 |
| **all members** | **59,360**, plus 635 calls of makeUnique |

### 1.3 What the class costs kppc

A scratch copy of kppc (copy `compiler\`, run `python -X utf8 -I
compiler\bench\uniqueptr\prof_patch.py <copy>\src` and build the copy; the patch is never
committed) records, per template, the exclusive CPU time and typed expressions of the front end's
instantiation checks (nested instantiations are charged to their own template) and the time
codegen spends emitting its functions. Run on the headline command with `--keep-going`; the
profile is written to `%KPROF_OUT%`. Two runs (the second on a busier machine); the IR columns
from `python -X utf8 -I compiler\bench\uniqueptr\ir_stats.py full.ll`:

| template | instantiations | front-end CPU (share) | typed expressions (share) | codegen (share) | functions in the IR | IR bytes (share) |
|---|---:|---:|---:|---:|---:|---:|
| **UniquePtr** | 1,438 | 0.47 to 0.53 s (1.3 to 1.9%) | 120,578 (1.6%) | 0.080 s (1.0%) | 5,086 for 967 T | 2.74 MB (0.90%) |
| **makeUnique** | 1,625 | 0.07 to 0.08 s (0.2%) | 13,139 (0.17%) | 0.027 s (0.3%) | 557 | 0.64 MB (0.21%) |
| UniqueArray | 11 | 0.004 s | 1,188 | 0.001 s | 48 for 10 T | 0.03 MB |
| CommonUtil's UniquePointer | 568 | 0.16 to 0.23 s (0.6%) | 16,125 (0.2%) | 0.018 s (0.2%) | 903 | |
| for scale: Vector | 6,888 | 4.7 to 5.3 s (13 to 20%) | 2,118,665 (28%) | 0.38 s (4.5%) | 20,057 | 18.1 MB (5.9%) |
| for scale: `move` | 14,014 | 0.06 to 0.08 s (0.2%) | 28,028 (0.4%) | 0.05 s (0.6%) | 6,066 | 1.14 MB (0.37%) |
| all | 15,821 class + 58,460 function | 27.3 / 37.2 s exclusive, of 30.7 / 42.1 s resolve CPU | 7.56 M | 8.2 / 8.5 s | 159,798 | 306 MB in bodies |

Instantiation checks are 89% of the front end's resolve CPU, and UniquePtr is a small part of
them: each `UniquePtr<T>` types about 84 expressions (all its members, used or not; codegen needs
5.3 functions per T). In wall time on 32 threads the front end's share is about 0.03 s of 1.5 to
1.7 s, codegen's 0.11 s of 9 to 11 s (codegen ran on one thread at 89058a6; d1ccfb8, the same
day, spreads it over all threads, which shrinks the time but not the share). LLVM's share is
estimated from the IR, 1.1% of the bytes and 1.4% of the instruction lines (UniquePtr and
makeUnique together), because the full game's IR does not compile as one module yet (functions an
error cut short leave `ret ptr` without a value).

### 1.4 The same program with and without the class

`powershell -File compiler\bench\uniqueptr\run_synth.ps1 -N 1000 -Runs 3`: 1,000 classes, each
with an owner class that uses `UniquePtr<Item_i>` in the ten ways Factorio does (a member, a Vector
of owners, makeUnique, `->`, `*`, `bool`, `get`, `reset`, moving in and out), against the same
program with plain pointers and the deletes written out (what a built-in would lower to, without
its unwinding cleanups). 65 files plus the stdlib, debug build, three runs each:

| | wall | resolve | codegen | functions | IR | clang -O0 (64 modules) | link |
|---|---:|---:|---:|---:|---:|---:|---:|
| UniquePtr | 1.37 to 1.50 s | 0.068 s | 0.58 to 0.59 s | 38,503 | 49.6 MB | 0.34 s | 0.28 to 0.37 s |
| plain pointers | 1.03 to 1.23 s | 0.057 to 0.064 s | 0.36 to 0.39 s | 27,503 | 40.4 MB | 0.31 s (once 0.51) | 0.20 to 0.22 s |

The class costs 11 functions per T (ten members and makeUnique), 23% more IR and about 0.3 s here,
in a program that does nothing else. In the game the same costs are the 1% of 1.3.

### 1.5 Run time in debug and release builds

`compiler/bench/uniqueptr/deref.kpp`, built with `kppc --exe` (debug) and `--release`: 1,000
owners in a Vector read as `if (p) sum += p->value` 20 M times, 2 M owners made and destroyed,
200 x 1,000 owners moved between Vectors, each against the same loop on `Node*`. Four runs:

| | debug: UniquePtr | debug: Node* | ratio | release |
|---|---:|---:|---:|---|
| read | 59.5 to 62.1 ms | 22.5 to 24.2 ms | **2.5 to 2.7x** | 5.5 to 6.2 ms against 5.4 to 5.7 ms: equal within the noise |
| make and destroy | 43.9 to 50.6 ms | 39.5 to 44.2 ms | 1.1x | both optimised away |
| move | 2.2 ms | 1.5 ms | 1.4x | 0.2 against 0.1 ms |

The debug cost is call overhead. With `[[forceinline]]` on `get`, `->`, `*`, `bool` and `!` in a
scratch copy of the stdlib the read is still 2.60x in a normal build (the instantiation is emitted
into another module, and -O0 inlines only within a module) and **1.20x** with `--modules 1`
(without the attribute, `--modules 1` gives 2.14x). So an inliner that sees the callee removes most
of it, and that is the compiler work design.md section 2 lists as option (4). How much of the
game's debug frame time this is cannot be measured until the game runs.

### 1.6 What clang pays for `std::unique_ptr`

`compiler\bench\uniqueptr\clang_trace.cmd 320 1 16 OUTDIR`: clang-cl 19 `/Zs` (the flags of
compiler/bench/clang_syntax_only.ps1, no PCH, no unity build) with `-ftime-trace` at granularity 0
on 320 random `.cpp` files of `src` (308 compiled). The "unique_ptr family" is the class, its
members, `default_delete`, the `_Compressed_pair` that holds the deleter and `make_unique`;
exclusive times, nested instantiations of other templates excluded:

- **3.13% of clang's front-end time** (8.2% of its instantiation time, 75 ms per TU under the
  trace); the std traits its constraints ask about `unique_ptr`/`default_delete` add 0.45%.
- **147 `unique_ptr<T>` class instantiations per TU** (up to 498) and 558 member and deleter
  function instantiations. The sample has 869 distinct `unique_ptr<T>`, and each was instantiated
  in **52 TUs on average**; the most repeated (GuiGameControllerHandler) in 260 of the 308.
- At `/Od` (`clang_ir.cmd 96 1 16 OUTDIR`, 91 TUs compiled to IR): 13 unique_ptr functions per TU,
  1.9% of the 673 functions a TU defines and 1.2% of its IR, as `linkonce_odr` copies that the
  linker throws away again.
- For all 3,230 `.cpp` of `src`: about 474 k class instantiations and 41 k emitted unique_ptr
  functions where kppc has 1,438 instantiations and 5,643 functions; about 110 s of CPU (3.1% of
  the measured untraced 1.09 s per TU). The real build repeats less, since FASTBuild compiles
  about 100 files per unity object with a PCH.

clang treats `std::unique_ptr` as an ordinary template (libc++ only offers an opt-in
`[[clang::trivial_abi]]` variant that is passed in a register; kppc passes a `UniquePtr<T>`
argument by address, as clang does by default). What compilers do special-case are the smallest
and most used library functions: clang (since 15) treats `std::move`, `std::forward`,
`std::move_if_noexcept`, `std::as_const` and `std::addressof` as builtins that are never
instantiated, and MSVC's library marks `std::move` and `std::forward` `[[msvc::intrinsic]]`, for
compile time and for debug builds. In kppc `move` and `forward` are 17,662 instantiations and
7,853 functions (0.47% of the IR bytes, half of what UniquePtr is), called for example 1,174 times
as `move<UniquePtr<T>>`.

## 2. What a built-in would have to mean

What docs/stdlib/uniqueptr.md specifies (which follows C++), with Factorio's use beside it. A
built-in would move these rules from the library's specification into design.md and into the
compiler.

1. **Ownership.** The owner deletes its object (`delete`, so through a virtual destructor when it
   is a derived object) when it is destroyed, assigned or reset; `reset(q)` stores q before
   deleting the old object. 42,133 destructor calls, three quarters on exception paths: a built-in
   needs everything a class with a destructor gets (cleanup funclets, temporaries, member
   destruction order, containers' `relocate`), so it cannot be "just a pointer" in the code
   generator.
2. **Null.** An owner can be empty: default construction (2,521), `nullptr` (66), tests (1,347
   `bool`, 399 `!`, 33 comparisons). A moved-from owner is empty and converted code relies on that,
   so `A^` must be nullable; Rust's non-null `Box` with `Option` for the empty case does not
   convert.
3. **Move only.** Copying is an error; 889 move constructions, 431 move assignments, implicit moves
   on return.
4. **Derived to base.** An owner of Derived converts to an owner of Base, and of T to const T, from
   an rvalue (612 calls). Deleting through a Base without a virtual destructor is undefined in C++;
   either form can turn it into a compile error at the conversion (a constraint with the
   isPolymorphic trait in the class, a rule in a built-in).
5. **Adopting and releasing.** `UniquePtr(T*)`, `reset(T*)` (1,502 calls) take a raw pointer from
   `new`; `release()` (2,018) hands the object to a raw-pointer owner (entities to the map, widgets
   to Agui). `new T` stays a `T*`, so a built-in needs spellings for both.
6. **Borrowing.** `get()` (1,108 calls), also inside `dynamic_cast<D*>(p.get())` (36) and
   `static_cast<D*>(p.get())` (80). A built-in could convert an lvalue owner to `T*` implicitly and
   refuse a temporary owner (it would be deleted at the end of the statement): that removes most
   `.get()` calls but is not C++'s rule (and kpp is explicit by default). The class could offer the
   same with a conversion operator for lvalues only, so this is a choice, not a reason.
7. **Custom deleters.** 12 uses, 6 deleters (1.1), plus CommonUtil's UniquePointer with
   ChartDeleter and the throwing variant. A built-in has no place for a deleter type, so these need
   a library class whatever is chosen.
8. **Arrays.** 89 `unique_ptr<T[]>` convert to the library's UniqueArray<T>, which keeps the count
   so that the elements' destructors run. Not worth a built-in form.
9. **Containers.** Owners live in Vector (252), Map (22), Pair, Array, Deque, FlatMap, Set and
   HashMap. Containers move-construct elements and destroy the old ones (design.md section 4), so
   a built-in needs placement `new` and an explicit destructor call on a non-class type (a C++
   pseudo-destructor that must not be a no-op), and answers from every trait templates ask
   (isTriviallyCopyable, isMoveConstructible, isNothrowMoveConstructible...).
10. **Optional.** Factorio has no `optional<unique_ptr>`. A built-in could give `Optional<A^>` the
    null pointer as its empty state (8 bytes instead of 16); nothing needs it.
11. **dynamic_cast.** Only observing casts on `get()`. A built-in could add
    `dynamic_cast<Derived^>(move(p))` (ownership moves on success, p keeps it on failure); no
    Factorio code would use it.
12. **Serialisation.** SerialiserHelper and DeserialiserHelper dispatch on `const
    std::unique_ptr<T>&` and `std::unique_ptr<T>&` overloads (save `*target`, load
    `target.reset(new T(input, args...))`), next to the ones for CommonUtil's UniquePointer and
    OnHeap. Deduction must match `T^` as it matches `T*` and `UniquePtr<T>`. Reflection-based save
    and load (design.md 8.3) asks of `m.type` whether it is an owner: a built-in trait (traitValue,
    like the others) answers for either form.
13. **Reflection and names.** `members(T)` lists owner members, `m.type` names them, `typeid` and
    diagnostics print them; a built-in needs its own printed form everywhere.
14. **Incomplete types.** C++'s rule that `unique_ptr<T>`'s destructor needs T complete (575 uses
    in 242 files with T forward-declared there, each needing an out-of-line destructor) does not
    exist in kpp, which has no forward declarations. Neither form has to deal with it.
15. **Size and C.** 8 bytes, the layout of a `T*` (C++ guarantees nothing but every implementation
    does this for the default deleter). An owner in an `extern "C"` signature would need a rule
    (refused, since C has no destructors).
16. **CommonUtil's UniquePointer** (346 uses) is a converted class of the game's own; it stays a
    class in every option (or a later cleanup maps it to UniquePtr; its `emplace(args)` is
    `reset(new T(args))`).

## 3. Syntax options

Real call sites, as the converter writes them today (src/CraftingQueue.kpp,
src/Controller/ControllerManager.kpp, src/AI/Command.kpp):

```cpp
using QueueType = Vector<UniquePtr<CraftingOrder>>;
void enqueueOne(UniquePtr<CraftingOrder> order);
for (const UniquePtr<CraftingOrder>& order : this->queue)
UniquePtr<Controller> physicalController;
this->physicalController.reset(new CharacterController(character, this->remoteController->player));
this->pausedController->getMap().toBeDeleted.push_back(this->pausedController.release());

UniquePtr<Command> CompoundCommand::clone()
{
  auto cloneCommand = makeUnique<CompoundCommand>(this->structureType);
  for (const auto& command : this->commands)
    cloneCommand->appendCommand(command->clone());
  return UniquePtr<Command>(::move(cloneCommand));
}
```

The same with `T^` as a spelling of `UniquePtr<T>` (option B below as sugar): only the types
change, the members and the explicit conversion keep the class's name.

```cpp
using QueueType = Vector<CraftingOrder^>;
void enqueueOne(CraftingOrder^ order);
for (CraftingOrder^ const& order : this->queue)
Controller^ physicalController;
this->physicalController.reset(new CharacterController(character, this->remoteController->player));
this->pausedController->getMap().toBeDeleted.push_back(this->pausedController.release());

Command^ CompoundCommand::clone()
{
  auto cloneCommand = makeUnique<CompoundCommand>(this->structureType);
  for (const auto& command : this->commands)
    cloneCommand->appendCommand(command->clone());
  return UniquePtr<Command>(::move(cloneCommand));
}
```

As a built-in type kind the last three statements need new spellings (adopting a `new`, releasing,
converting explicitly), and `clone()` could return `CompoundCommand^`, overriding by covariance.

| C++ | A: `UniquePtr<T>` (today) | B: `T^` | C: `own T*` |
|---|---|---|---|
| `std::unique_ptr<Widget>` | `UniquePtr<Widget>` | `Widget^` | `own Widget*` |
| `std::vector<std::unique_ptr<Widget>>` | `Vector<UniquePtr<Widget>>` | `Vector<Widget^>` | `Vector<own Widget*>` |
| `const std::unique_ptr<Widget>&` (269) | `const UniquePtr<Widget>&` | `Widget^ const&` | `own Widget* const&` |
| `std::unique_ptr<const Widget>` (50) | `UniquePtr<const Widget>` | `const Widget^` | `own const Widget*` |
| `virtual std::unique_ptr<Command> clone()` | `virtual UniquePtr<Command> clone()` | `virtual Command^ clone()` | `virtual own Command* clone()` |
| `std::make_unique<Label>(text)` | `makeUnique<Label>(text)` | the same | the same |
| `std::unique_ptr<Widget>(new Label(text))` (37) | `UniquePtr<Widget>(new Label(text))` | `UniquePtr<Widget>(...)` as sugar; a built-in needs a form | needs a form |
| `p.get()`, `p.release()`, `p.reset(q)` | the same | the same as sugar; built-in members otherwise | built-in members |
| `std::unique_ptr<T[]>`, `std::unique_ptr<T, D>` | `UniqueArray<T>`, `UniquePtr<T, D>` | the same | the same |

**A. `UniquePtr<T>`**, today. The converter only renames. Long in nested types
(`Vector<UniquePtr<CraftingOrder>>`, `HashMap<ForceID, UniquePtr<SurfaceCargoLogistics>>`), but it
is the shape every C++ programmer knows, `const` sits where C++ puts it, and nothing new is to
learn.

**B. `T^`**, a declarator next to `*`, `&` and `&&` (C++/CLI writes its handles this way, Pascal
`^T`). Shortest, and it reads as "a kind of pointer", which it is (`->` and `*` work).
- Parsing without name lookup: a statement that starts with a name followed by `^` and an
  identifier is a declaration, by the same rule as `A* b;` (an expression `a ^ b;` would be
  useless). Where a type is expected (template arguments, `static_cast<...>`, parameter lists), a
  `^` followed by `>`, `,`, `)`, `&`, `*` or `const` can only be the declarator, since an XOR
  needs an operand after it. In an expression, `A^(x)` is `A ^ (x)`, so there is no functional
  cast with `^` (C++ has the same gap for `T*(x)`): explicit construction keeps a named form,
  `UniquePtr<A>(new B)` if `^` is sugar.
- `const` follows the pointer rule: `const T^` owns a const T and a const owner is `T^ const`, the
  opposite of how C++ code writes `const std::unique_ptr<T>&` (269 times). The converter writes the
  right one; a person who writes `const T^&` for a const owner gets a compile error (an lvalue
  owner does not convert to an owner of const T without a move), not a different program.
- Section 8's spellings reuse C++'s own forms (constexpr and union in new places, calls like
  `members(T)`, member access like `m.name`) rather than new punctuation, with one exception
  (`object.[m]`). `^` is a new declarator, but in the family C++ already has.

**C. `own T*`**, a keyword before a pointer declarator. Reads as English and keeps the `*`. `own`
is free (0 identifiers in Factorio's code; `owner` has 3,644 and could not be the keyword). But the
qualifier belongs to the pointer while it stands where the pointee's qualifiers stand: `own const
T*`, `const own T*` and `own T* const` all need a rule, and C++'s place for a pointer's qualifier
is after the `*` (`T* const`), where `Vector<CraftingOrder* own>` reads worse.

**D. `unique T*`.** As C, but `unique` is the name of an algorithm (std::unique, which the
converter maps to the library's `unique`; the word appears 26 times in Factorio's code): as a
keyword it takes the name, as a contextual keyword one word means two things. And "unique" says
less than "own".

**E. `UniquePtr<T>` that the compiler implements** (no syntax change; the compiler already knows
library classes by name: InitializerList, TypeInfo, strong_ordering). Combines with A or B.

How each converts: A and E rename (done). B and C rewrite types: `std::unique_ptr<X>` to `X^`,
`const std::unique_ptr<X>&` to `X^ const&`, `std::unique_ptr<const X>` to `const X^`, a nested
`std::unique_ptr<X>` in template arguments the same way. Arrays and deleters stay `UniqueArray<T>`
and `UniquePtr<T, D>` in every option.

## 4. The ways to build it, against the numbers

| | plain template (today) | class the compiler implements (E) | new type kind (B or C as a built-in) | `T^` as a spelling of `UniquePtr<T>` |
|---|---|---|---|---|
| build time saved on the game | none (it costs ~1%) | ~1%: no member instantiation, no UniquePtr functions | ~1%, the same | none |
| debug read through an owner | 2.5 to 2.7x a plain pointer (measured) | 1x expected (a plain load) | 1x expected | as today; 1.2x measured with an inliner that sees the callee |
| semantics | the library's, specified in docs/stdlib | the class's, but written twice: in the stdlib and in the compiler's lowering, which must agree | new rules in design.md for all of section 2 | the class's |
| compiler work | none | known-class lookup, typing of ~20 members by rule, inline lowering in codegen, a fallback for `&UniquePtr<T>::reset` and generic code | a TyKind and its cases in types, resolver, codegen and converter: deduction (`T^` patterns), overload ranking (owner to owner, nullptr, maybe to `T*`), traits, layout, destruction and cleanups, moves, deleted copies, placement new in containers, reflection, printing, C interop | the parser's declarator loop and the type printer; the converter's output |
| still needs library classes for | | deleters, arrays | deleters, arrays | deleters, arrays |
| what only it gives | | covariant returns could be allowed by rule | covariant `clone()`, ownership-moving dynamic_cast, null niche in Optional, register passing | the short spelling |

The general mechanisms give most of E's and the built-in's benefit to every small template, not
only to UniquePtr, which is why they come first in the recommendation:

1. **Inlining trivial functions when kppc emits IR** (design.md section 2, option (4)). UniquePtr's
   accessors are 8,791 of the call sites in 1.2 alone, and the same holds for Vector's `[]`,
   `size()`, `begin()`/`end()` and String's `c_str()`, each called thousands of times in the
   game's IR. Measured on UniquePtr: 2.6x to 1.2x in debug.
2. **Instantiating a class template's members when they are used**, as C++ does. Today every
   member of every instantiated class is copied and typed (design.md, "Instantiations own their
   typed bodies": 350 k of the copies); UniquePtr types all of its about 20 members per T, and the
   game needs 5.3.
3. **`move` and `forward` as builtins**, as clang and MSVC treat them: 17,662 instantiations and
   7,853 functions, more functions than UniquePtr and makeUnique together (5,643), though smaller
   ones.

## 5. How other languages do it

kovarex asked for this comparison too. From the languages' own documentation (sources below):

| language | sole owner of a heap object | built in or library | empty state |
|---|---|---|---|
| C++ | `std::unique_ptr<T>` | library class, instantiated in every TU that uses it; `std::move` and `std::forward` are compiler builtins in clang and MSVC (1.6) | nullable, moved-from is null |
| Rust | `Box<T>` | library type that the compiler knows (a lang item: moving out with `*b`, `Box<dyn Trait>`); until 2014 a built-in `~T`, removed for `Box` | never null; `Option<Box<T>>` is one pointer |
| Swift | none: class references are shared (ARC); a noncopyable value type (`~Copyable`, Swift 5.9) with `consume`, `borrowing` and `consuming` makes a value uniquely owned | the language, as a property of a type, not a pointer | |
| C#, Java, Go | none, garbage collected | | C#'s `int?` is a spelling of the library's `Nullable<int>` |
| C++/CLI | `T^` is a handle to a garbage-collected object (made by `gcnew`), not an owner | built in | nullable |
| Objective-C | none: ARC, with ownership qualifiers on pointers (`__strong`, `__weak`, `__unsafe_unretained`) | built in (qualifiers) | nullable |
| D | `std.typecons.Unique!T` (garbage collection by default) | library | nullable |
| Nim | `owned ref T`, proposed 2019 as "very much like C++'s unique_ptr", unowned refs counted in debug builds | built in, an experimental runtime; Nim went to ARC/ORC instead (destructors and moves, `ref` reference-counted) | nil |
| Zig, Odin | none: no destructors; explicit allocators and `defer` | | |
| Mojo | ownership is an argument convention (`var` owns, `mut`, read-only by default); `x^` moves x into a call and ends its lifetime | built in, as a move operator, not a type | |
| Carbon | `T*` is never null; a nullable pointer is `Optional(T*)` | | `Optional` |
| Cpp2 (cppfront) | `std::unique_ptr<T>`, made with `unique.new<T>(args)` or just `new<T>(args)` (`shared.new<T>` for shared_ptr) | library type, new creation syntax | nullable |
| Safe C++ (P3390) | `std2::box<T>`; there `T^` is a borrow (a checked reference, `const T^` a shared one) and `rel x` a destructive move | library type | never empty |

What this says for kpp:

- No current language with destructors makes the sole owner a built-in pointer type. The two that
  did went back: Rust replaced `~T` with `Box<T>` before 1.0 (RFC 59, 2014: `~T` could not take an
  allocator, two spellings did one thing, `Box` documents itself where `~` does not, and sigils
  invite adding them until the compiler is quiet), and Nim left its `owned ref` for ARC/ORC. Rust
  kept compiler support for Box, which is option E. The allocator reason does not apply to kpp
  (allocators are passed at run time, design.md section 4); the reading reasons are the opposite of
  the reason kovarex gives, so this is a judgement of taste, not a fact.
- What languages do add is syntax around library types: spellings (Swift's `T?` and `[T]`, C#'s
  `int?`), creation (Cpp2's `new<T>(args)`) and moves (Mojo's `x^`, Swift's `consume x`, Safe C++'s
  `rel x`). All of these leave the type a library class, as `T^` as sugar would.
- `^` already means three things: a garbage-collected handle (C++/CLI), a borrow (Safe C++) and a
  move (Mojo, in expressions). The Safe C++ meaning is the one nearest kpp: if kpp ever adds checked
  borrows, `^` would be the natural spelling, and it would be taken.
- A non-null owner with Optional for the empty case (Rust, Safe C++, Carbon's pointers) is a
  different design; converted code needs the nullable owner (section 2, item 2), so it could only
  come as a second type.
- A shorter `makeUnique<T>(args)` (Cpp2's `new<T>(args)`) is a separate question from the type's
  spelling; Factorio has 990 make_unique calls and 742 `.reset(new ...)`.

Sources: Rust RFC 59 "remove-tilde" (rust-lang.github.io/rfcs/0059-remove-tilde.html) and the
std::boxed documentation; Swift SE-0390 (noncopyable structs and enums), SE-0366 (`consume`) and
SE-0377 (`borrowing`/`consuming`); Nim, "A new runtime for Nim" (nim-lang.org/araq/ownedrefs.html)
and "Introduction to ARC/ORC in Nim" (nim-lang.org/blog, 2020); the Mojo manual, "Ownership"
(mojolang.org/docs/manual/values/ownership); the Carbon design overview
(github.com/carbon-language/carbon-lang, docs/design/README.md); the cppfront documentation,
"Objects" (hsutter.github.io/cppfront/cpp2/objects); the Safe C++ draft (safecpp.org/draft.html).

## 6. Recommendation and what is left to decide

1. Keep the unique pointer a library class, `UniquePtr<T>`, specified in docs/stdlib/uniqueptr.md
   with std::unique_ptr's guarantees and produced by the converter. No new kind of type.
2. For kovarex: whether to add `T^` as a spelling of `UniquePtr<T>` (option B as sugar). If yes:
   `const T^` owns a const T and `T^ const` is a const owner; `^` without the standard library is
   an error; diagnostics print `T^`; the converter writes `T^` (and `X^ const&` for `const
   std::unique_ptr<X>&`). `UniquePtr<T, D>` and `UniqueArray<T>` keep their names.
3. The speed work that would have been the built-in's reason, done generally (section 4's list,
   for the threads that own codegen, templates and the resolver): emit-time inlining of trivial
   functions, members instantiated on use, `move`/`forward` as builtins.
4. The two bugs found: too many template arguments must be an error, and the library's UniquePtr
   needs the deleter parameter (`UniquePtr<T, D = DefaultDelete<T>>`, std's shape, still 8 bytes
   for the default) for the 12 converted custom deleters; `delete` of a null pointer must do
   nothing.
