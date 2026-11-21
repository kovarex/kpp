// cppinput: a #define constant inside an `extern "C" {` block of a source file (TraceLog's Codec.cpp, its copy of
// LZ4's lz4.h): the file's own, written private before the block, where no declaration is private
extern "C" {

#define VERSION_RELEASE 2 /* for tweaks */

int release()
{
  return VERSION_RELEASE;
}

}

int main()
{
  return release() == 2 ? 0 : 1;
}
