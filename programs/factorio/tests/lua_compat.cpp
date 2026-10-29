// The C++ twin of lua_compat.kpp: runs lua_compat.lua with the game's Lua as the game builds it
// (lua.lib: Lua compiled as C++, formatting through trio); its output is lua_compat.expected. From a
// VS developer prompt in programs\factorio:
//   clang-cl /O2 /MT /std:c++20 /DNDEBUG /I C:\Factorio\libraries /Fe:kpp\lua_compat_cpp.exe tests\lua_compat.cpp /link C:\Factorio\vs2022\Releasex64\lua.lib C:\Factorio\vs2022\Releasex64\trio.lib
//   kpp\lua_compat_cpp.exe tests\lua_compat.lua > tests\lua_compat.expected
#include <Lua/lua.hpp>
#include <stdio.h>

static char script[1 << 20];

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    fprintf(stderr, "usage: lua_compat script.lua\n");
    return 2;
  }
  FILE* file = fopen(argv[1], "rb");
  if (!file)
  {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  size_t size = fread(script, 1, sizeof(script), file);
  fclose(file);
  lua_State* L = luaL_newstate();
  luaL_openlibs(L, 0); // the debug library as mods get it
  // A fixed chunk name: the error messages the script prints carry it, whatever path it runs from.
  if (luaL_loadbuffer(L, script, size, "=lua_compat") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK)
  {
    printf("error: %s\n", lua_tostring(L, -1));
    return 1;
  }
  lua_close(L);
  return 0;
}
