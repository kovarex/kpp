// cppinput: a #define constant of a source file is that file's own, written private: no other file sees a .cpp
// file's macros (TraceLog's Codec.cpp and Core's lz4hc.cpp, two copies of LZ4, each define LZ4_VERSION_RELEASE)
#define RELEASE 2

int main()
{
  return RELEASE == 2 ? 0 : 1;
}
