// Lua's internal functions that the converted game calls and that the game's lua.lib has under C++'s
// names: the game compiles Lua as C++, where LUAI_FUNC is a plain `extern` (the API functions,
// LUA_API, are `extern "C"`). Each gets a C function of its C name that calls the C++ one, which
// clang's asm label names. LuaRef's luaH_getint (through native/include/kpp_lua_internals.h).
#include "kpp_lua_internals.h"

const TValue* cxx_luaH_getint(lua_State* L, Table* t, int key) __asm__("?luaH_getint@@YAPEBUlua_TValue@@PEAUlua_State@@PEAUTable@@H@Z");
const TValue* luaH_getint(lua_State* L, Table* t, int key) { return cxx_luaH_getint(L, t, key); }

// Data takes the linker's alias instead: ltm.h's objtypename reads luaT_typenames_ (LuaValueSaver's
// messages). It holds while this object is linked in, which luaH_getint's callers make sure of.
#pragma comment(linker, "/alternatename:luaT_typenames_=?luaT_typenames_@@3QBQEBDB")
