// cppinput: a C cast of a C cast, each to a type named by an identifier (Unreal's Utf8Iterator.cpp:
// `const uint32 Cont1 = (uint32)(uint8)Pos[1];`)
typedef unsigned int uint32;
typedef unsigned char uint8;

int main()
{
  const char text[] = {'a', (char)0xC3, 0};
  const char* pos = text;
  const uint32 code = (uint32)(uint8)pos[1];
  const uint32 flipped = (uint32)(uint8)~code;
  return code == 0xC3 && flipped == 0x3C ? 0 : 1;
}
