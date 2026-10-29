// The C++ twin of lua_internals_smoke.kpp: the layout of the Lua internals the game reaches into and
// the same operations on a real Lua state, through the game's own Lua headers compiled as the game
// compiles them (as C++); its output is lua_internals_smoke.expected. From a VS developer prompt in
// programs\factorio:
//   clang-cl /O2 /MT /std:c++20 /DNDEBUG /I C:\Factorio\libraries /I C:\Factorio\libraries\Lua tests\lua_internals_smoke.cpp /link C:\Factorio\vs2022\Releasex64\lua.lib C:\Factorio\vs2022\Releasex64\trio.lib
#include <Lua/lua.hpp>
#include <Lua/lapi.h>
#include <Lua/lgc.h>
#include <Lua/lobject.h>
#include <Lua/lstate.h>
#include <Lua/ltable.h>
#include <stddef.h>
#include <stdio.h>

static int bindable;

int main()
{
  // The layout: what kpp_lua_internals.h must give the C import, `class LuaBindableObject*` read as
  // a struct pointer included.
  printf("TValue %d tt_ %d\n", (int)sizeof(TValue), (int)offsetof(TValue, tt_));
  printf("Udata %d metatable %d env %d factorioBindableObject %d len %d\n", (int)sizeof(Udata), (int)offsetof(Udata, uv.metatable),
         (int)offsetof(Udata, uv.env), (int)offsetof(Udata, uv.factorioBindableObject), (int)offsetof(Udata, uv.len));
  printf("Table %d array %d node %d sizearray %d firstadded %d lastadded %d\n", (int)sizeof(Table), (int)offsetof(Table, array),
         (int)offsetof(Table, node), (int)offsetof(Table, sizearray), (int)offsetof(Table, firstadded), (int)offsetof(Table, lastadded));
  printf("Node %d i_key %d next %d prev %d\n", (int)sizeof(Node), (int)offsetof(Node, i_key), (int)offsetof(Node, next), (int)offsetof(Node, prev));
  printf("lua_State %d top %d l_G %d\n", (int)sizeof(lua_State), (int)offsetof(lua_State, top), (int)offsetof(lua_State, l_G));

  // A table with an array part and two keys, read through index2addr and the accessors.
  lua_State* L = luaL_newstate();
  lua_createtable(L, 4, 0);
  for (int i = 1; i <= 4; ++i)
  {
    lua_pushnumber(L, i * 10);
    lua_rawseti(L, -2, i);
  }
  lua_pushstring(L, "x");
  lua_pushnumber(L, 7);
  lua_rawset(L, -3);
  lua_pushstring(L, "y");
  lua_pushnumber(L, 8);
  lua_rawset(L, -3);
  Table* table = hvalue(index2addr(L, -1));
  double sum = 0;
  for (int i = 0; i < table->sizearray; ++i)
    if (!ttisnil(&table->array[i]))
      sum += nvalue(&table->array[i]);
  printf("sizearray %d sum %g istable %d\n", table->sizearray, sum, (int)ttistable(index2addr(L, -1)));
  for (Node* node = table->firstadded; node; node = node->next)
    printf("%s=%g ", getstr(rawtsvalue(gkey(node))), nvalue(gval(node)));
  printf("\n");
  printf("luaH_getint 2: %g\n", nvalue(luaH_getint(L, table, 2)));

  // A full userdata and the game's pointer to its bound object.
  lua_newuserdata(L, 0);
  uvalue(index2addr(L, -1))->factorioBindableObject = (LuaBindableObject*)&bindable;
  printf("userdata bound %d payload after header %d\n", uvalue(index2addr(L, -1))->factorioBindableObject == (LuaBindableObject*)&bindable,
         (int)((char*)lua_touserdata(L, -1) - (char*)rawuvalue(index2addr(L, -1))));
  lua_close(L);
  return 0;
}
