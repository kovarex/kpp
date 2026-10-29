/* The internals of the game's Lua (libraries/Lua: Lua 5.2 with Wube's changes) that the game's C++
   reaches into, imported as C: TValue and its accessors (lobject.h), lua_State and global_State
   (lstate.h), Table and Node (ltable.h), the collector's marks (lgc.h), the type names (ltm.h), and
   index2addr, which lapi.c defines and the game's lapi.h declares with C++'s `extern "C"`. Their
   macros (hvalue, uvalue, ttisnil, setobj2s, gval, ...) are expanded where the converted code uses
   them (overrides/expand-macros.txt, `included`).
   The game compiles Lua as C++. lobject.h's one C++ word, `class LuaBindableObject*` in Udata, is
   read here as a struct pointer, which has the same layout (tests/lua_internals_smoke.kpp checks
   the sizes and offsets against the C++ build's, lua_internals_smoke.cpp). The internal functions
   and data lua.lib has under C++'s names get their C names in native/lua_internals.c.
   This header goes when Lua itself is converted (docs/path-to-factorio.md, "Lua's internals"). */
#ifndef KPP_LUA_INTERNALS_H
#define KPP_LUA_INTERNALS_H
#include "lua.h"
#define class struct
typedef struct Node Node; /* lobject.h's Node names itself without `struct`, as C++ allows */
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
#include "lgc.h"
#include "ltm.h"
#undef class
TValue* index2addr(lua_State* L, int idx);
#endif
