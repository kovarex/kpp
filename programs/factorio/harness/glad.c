/* glad's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): built once
   with the C library and once converted with it, it must print the same bytes. glad loads OpenGL's
   functions through a loader; this one is fake, so no GL context is needed: glGetString,
   glGetIntegerv and glGetStringi report a GL version and extensions, every other name gets an
   address of its own. For GL versions from 1.1 to 4.6 (and an ES string), with and without
   extensions, it prints glad's version, every GLAD_GL_ flag, and which of its 2584 function pointers
   it loaded (a count and a hash of the addresses by name).
   Written by harness/glad.py from glad.h's lists (python -I harness/glad.py): edit that, not this.

     glad > output.txt (harness.cmd glad builds and compares both) */
#include <glad/glad.h>
#include <stdio.h>
#include <string.h>

static const char* version = "";
static const char* extensions[64];
static int extensionCount = 0;
static char extensionString[4096];
static char marks[65536];
static const GLubyte* APIENTRY fakeGetString(GLenum name)
{
  if (name == GL_VERSION)
    return (const GLubyte*)version;
  if (name == GL_EXTENSIONS)
    return (const GLubyte*)extensionString;
  return (const GLubyte*)"fake";
}
static void APIENTRY fakeGetIntegerv(GLenum name, GLint* data)
{
  if (name == GL_NUM_EXTENSIONS)
    *data = extensionCount;
  else if (name == GL_MAJOR_VERSION)
    *data = version[0] - '0';
  else if (name == GL_MINOR_VERSION)
    *data = version[2] - '0';
  else
    *data = 0;
}
static const GLubyte* APIENTRY fakeGetStringi(GLenum name, GLuint index)
{
  return name == GL_EXTENSIONS && index < (GLuint)extensionCount ? (const GLubyte*)extensions[index] : NULL;
}
static unsigned hashName(const char* name)
{
  unsigned h = 2166136261u;
  for (; *name; ++name)
    h = (h ^ (unsigned char)*name) * 16777619u;
  return h;
}
static void* loader(const char* name)
{
  if (strcmp(name, "glGetString") == 0)
    return (void*)fakeGetString;
  if (strcmp(name, "glGetIntegerv") == 0)
    return (void*)fakeGetIntegerv;
  if (strcmp(name, "glGetStringi") == 0)
    return (void*)fakeGetStringi;
  if (hashName(name) % 7 == 0)
    return NULL; /* (a driver without the function) */
  return &marks[hashName(name) % sizeof(marks)];
}
static void scenario(const char* gl, const char* const* names, int count)
{
  version = gl;
  extensionCount = count;
  extensionString[0] = 0;
  for (int k = 0; k < count; ++k)
  {
    extensions[k] = names[k];
    strcat(extensionString, names[k]);
    strcat(extensionString, " ");
  }
  int loaded = gladLoadGLLoader(loader);
  printf("GL \"%s\" with %d extensions: load %d, version %d.%d\n", gl, count, loaded, GLVersion.major, GLVersion.minor);
  unsigned long long hash = 1469598103934665603ull;
  int set = 0;
  const char* p;
  p = (const char*)glad_glCullFace;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFrontFace;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glHint;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLineWidth;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointSize;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPolygonMode;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glScissor;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexParameterf;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexParameteri;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexImage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexImage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClear;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearColor;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearStencil;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearDepth;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilMask;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorMask;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthMask;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisable;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnable;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFinish;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFlush;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendFunc;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLogicOp;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilFunc;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilOp;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthFunc;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelStoref;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelStorei;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReadBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReadPixels;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBooleanv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetDoublev;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetError;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFloatv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetIntegerv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetString;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexImage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexLevelParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexLevelParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsEnabled;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthRange;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glViewport;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawArrays;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElements;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPolygonOffset;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTexImage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTexImage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTexSubImage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTexSubImage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexSubImage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexSubImage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindTexture;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteTextures;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenTextures;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsTexture;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawRangeElements;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexImage3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexSubImage3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTexSubImage3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glActiveTexture;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSampleCoverage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexImage3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexImage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexImage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexSubImage3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexSubImage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexSubImage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCompressedTexImage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendFuncSeparate;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawArrays;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawElements;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterf;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameteri;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendColor;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendEquation;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenQueries;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteQueries;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsQuery;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginQuery;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndQuery;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryObjectiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryObjectuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteBuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenBuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBufferData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBufferSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBufferSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUnmapBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBufferParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBufferPointerv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendEquationSeparate;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawBuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilOpSeparate;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilFuncSeparate;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilMaskSeparate;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAttachShader;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindAttribLocation;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompileShader;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateProgram;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateShader;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteProgram;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteShader;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDetachShader;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisableVertexAttribArray;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnableVertexAttribArray;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveAttrib;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveUniform;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetAttachedShaders;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetAttribLocation;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramInfoLog;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetShaderiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetShaderInfoLog;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetShaderSource;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformLocation;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribdv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribPointerv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsProgram;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsShader;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLinkProgram;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glShaderSource;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUseProgram;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix2fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix3fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix4fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glValidateProgram;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1s;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1sv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2s;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2sv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3s;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3sv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4Nbv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4Niv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4Nsv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4Nub;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4Nubv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4Nuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4Nusv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4bv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4s;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4sv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4ubv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4usv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribPointer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix2x3fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix3x2fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix2x4fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix4x2fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix3x4fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix4x3fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorMaski;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBooleani_v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetIntegeri_v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnablei;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisablei;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsEnabledi;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginTransformFeedback;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndTransformFeedback;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBufferRange;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBufferBase;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTransformFeedbackVaryings;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTransformFeedbackVarying;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClampColor;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginConditionalRender;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndConditionalRender;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribIPointer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribIiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribIuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI1i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI2i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI3i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI1ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI2ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI3ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI1iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI2iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI3iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI1uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI2uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI3uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4bv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4sv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4ubv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4usv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindFragDataLocation;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFragDataLocation;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexParameterIiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexParameterIuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexParameterIiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexParameterIuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearBufferiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearBufferuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearBufferfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearBufferfi;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetStringi;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsRenderbuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindRenderbuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteRenderbuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenRenderbuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRenderbufferStorage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetRenderbufferParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsFramebuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindFramebuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteFramebuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenFramebuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCheckFramebufferStatus;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTexture1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTexture2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTexture3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferRenderbuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFramebufferAttachmentParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenerateMipmap;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlitFramebuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRenderbufferStorageMultisample;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTextureLayer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapBufferRange;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFlushMappedBufferRange;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindVertexArray;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteVertexArrays;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenVertexArrays;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsVertexArray;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawArraysInstanced;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElementsInstanced;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPrimitiveRestartIndex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyBufferSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformIndices;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveUniformsiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveUniformName;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformBlockIndex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveUniformBlockiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveUniformBlockName;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformBlockBinding;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElementsBaseVertex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawRangeElementsBaseVertex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElementsInstancedBaseVertex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawElementsBaseVertex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProvokingVertex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFenceSync;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsSync;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteSync;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClientWaitSync;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWaitSync;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetInteger64v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSynciv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetInteger64i_v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBufferParameteri64v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTexture;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexImage2DMultisample;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexImage3DMultisample;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultisamplefv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSampleMaski;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindFragDataLocationIndexed;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFragDataIndex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenSamplers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteSamplers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsSampler;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindSampler;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSamplerParameteri;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSamplerParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSamplerParameterf;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSamplerParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSamplerParameterIiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSamplerParameterIuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSamplerParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSamplerParameterIiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSamplerParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSamplerParameterIuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glQueryCounter;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryObjecti64v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryObjectui64v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribDivisor;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribP1ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribP1uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribP2ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribP2uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribP3ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribP3uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribP4ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribP4uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexP2ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexP2uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexP3ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexP3uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexP4ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexP4uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordP1ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordP1uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordP2ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordP2uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordP3ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordP3uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordP4ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordP4uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoordP1ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoordP1uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoordP2ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoordP2uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoordP3ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoordP3uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoordP4ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoordP4uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalP3ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalP3uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorP3ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorP3uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorP4ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorP4uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColorP3ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColorP3uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTbufferMask3DFX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageEnableAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageInsertAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageCallbackAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetDebugMessageLogAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendFuncIndexedAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendFuncSeparateIndexedAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendEquationIndexedAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendEquationSeparateIndexedAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferSamplePositionsfvAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferSamplePositionsfvAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFramebufferParameterfvAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedFramebufferParameterfvAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformi64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribParameteriAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawArraysIndirectAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawElementsIndirectAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenNamesAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteNamesAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsNameAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glQueryObjectParameteruiAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPerfMonitorGroupsAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPerfMonitorCountersAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPerfMonitorGroupStringAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPerfMonitorCounterStringAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPerfMonitorCounterInfoAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenPerfMonitorsAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeletePerfMonitorsAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSelectPerfMonitorCountersAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginPerfMonitorAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndPerfMonitorAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPerfMonitorCounterDataAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSetMultisamplefvAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorageSparseAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorageSparseAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilOpValueAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTessellationFactorAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTessellationModeAMD;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glElementPointerAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElementArrayAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawRangeElementArrayAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawElementArrayAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawRangeElementArrayAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenFencesAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteFencesAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSetFenceAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsFenceAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTestFenceAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFinishFenceAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTestObjectAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFinishObjectAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBufferParameteriAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFlushMappedBufferRangeAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glObjectPurgeableAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glObjectUnpurgeableAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetObjectParameterivAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureRangeAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexParameterPointervAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindVertexArrayAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteVertexArraysAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenVertexArraysAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsVertexArrayAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayRangeAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFlushVertexArrayRangeAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayParameteriAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnableVertexAttribAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisableVertexAttribAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsVertexAttribEnabledAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapVertexAttrib1dAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapVertexAttrib1fAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapVertexAttrib2dAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapVertexAttrib2fAPPLE;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReleaseShaderCompiler;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glShaderBinary;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetShaderPrecisionFormat;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthRangef;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearDepthf;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMemoryBarrierByRegion;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPrimitiveBoundingBoxARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawArraysInstancedBaseInstance;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElementsInstancedBaseInstance;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElementsInstancedBaseVertexBaseInstance;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureHandleARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureSamplerHandleARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeTextureHandleResidentARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeTextureHandleNonResidentARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetImageHandleARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeImageHandleResidentARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeImageHandleNonResidentARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformHandleui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformHandleui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformHandleui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformHandleui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsTextureHandleResidentARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsImageHandleResidentARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL1ui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL1ui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribLui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBufferStorage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateSyncFromCLeventARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearBufferData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearBufferSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearTexImage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearTexSubImage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClipControl;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClampColorARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDispatchCompute;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDispatchComputeIndirect;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDispatchComputeGroupSizeARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyImageSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageControlARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageInsertARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageCallbackARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetDebugMessageLogARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateTransformFeedbacks;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTransformFeedbackBufferBase;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTransformFeedbackBufferRange;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTransformFeedbackiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTransformFeedbacki_v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTransformFeedbacki64_v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateBuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedBufferStorage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedBufferData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedBufferSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyNamedBufferSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearNamedBufferData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearNamedBufferSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapNamedBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapNamedBufferRange;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUnmapNamedBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFlushMappedNamedBufferRange;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedBufferParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedBufferParameteri64v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedBufferPointerv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedBufferSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateFramebuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferRenderbuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferParameteri;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferTexture;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferTextureLayer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferDrawBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferDrawBuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferReadBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInvalidateNamedFramebufferData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInvalidateNamedFramebufferSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearNamedFramebufferiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearNamedFramebufferuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearNamedFramebufferfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearNamedFramebufferfi;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlitNamedFramebuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCheckNamedFramebufferStatus;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedFramebufferParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedFramebufferAttachmentParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateRenderbuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedRenderbufferStorage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedRenderbufferStorageMultisample;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedRenderbufferParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateTextures;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureBufferRange;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorage3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorage2DMultisample;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorage3DMultisample;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureSubImage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureSubImage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureSubImage3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTextureSubImage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTextureSubImage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTextureSubImage3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTextureSubImage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTextureSubImage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTextureSubImage3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameterf;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameteri;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameterIiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameterIuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenerateTextureMipmap;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindTextureUnit;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureImage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCompressedTextureImage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureLevelParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureLevelParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureParameterIiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureParameterIuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateVertexArrays;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisableVertexArrayAttrib;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnableVertexArrayAttrib;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayElementBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexBuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayAttribBinding;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayAttribFormat;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayAttribIFormat;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayAttribLFormat;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayBindingDivisor;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexArrayiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexArrayIndexediv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexArrayIndexed64iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateSamplers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateProgramPipelines;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateQueries;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryBufferObjecti64v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryBufferObjectiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryBufferObjectui64v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryBufferObjectuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawBuffersARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendEquationiARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendEquationSeparateiARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendFunciARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendFuncSeparateiARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawArraysIndirect;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElementsIndirect;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawArraysInstancedARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElementsInstancedARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramStringARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindProgramARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteProgramsARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenProgramsARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParameter4dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParameter4dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParameter4fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParameter4fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParameter4dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParameter4dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParameter4fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParameter4fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramEnvParameterdvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramEnvParameterfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramLocalParameterdvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramLocalParameterfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramStringARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsProgramARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferParameteri;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFramebufferParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramParameteriARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTextureARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTextureLayerARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTextureFaceARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramBinary;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramBinary;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramParameteri;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureSubImage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCompressedTextureSubImage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSpecializeShaderARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix2dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix3dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix4dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix2x3dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix2x4dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix3x2dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix3x4dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix4x2dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix4x3dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformdv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1i64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2i64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3i64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4i64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1i64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2i64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3i64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4i64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1ui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2ui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3ui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4ui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1ui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2ui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3ui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4ui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformi64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformi64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1i64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2i64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3i64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4i64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1i64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2i64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3i64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4i64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1ui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2ui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3ui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4ui64ARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1ui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2ui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3ui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4ui64vARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorTable;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorTableParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorTableParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyColorTable;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetColorTable;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetColorTableParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetColorTableParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorSubTable;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyColorSubTable;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionFilter1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionFilter2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionParameterf;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionParameteri;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyConvolutionFilter1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyConvolutionFilter2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetConvolutionFilter;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetConvolutionParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetConvolutionParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSeparableFilter;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSeparableFilter2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetHistogram;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetHistogramParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetHistogramParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMinmax;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMinmaxParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMinmaxParameteriv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glHistogram;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMinmax;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glResetHistogram;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glResetMinmax;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawArraysIndirectCountARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawElementsIndirectCountARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribDivisorARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetInternalformativ;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetInternalformati64v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInvalidateTexSubImage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInvalidateTexImage;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInvalidateBufferSubData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInvalidateBufferData;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInvalidateFramebuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInvalidateSubFramebuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCurrentPaletteMatrixARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixIndexubvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixIndexusvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixIndexuivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixIndexPointerARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBuffersBase;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBuffersRange;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindTextures;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindSamplers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindImageTextures;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindVertexBuffers;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawArraysIndirect;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawElementsIndirect;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSampleCoverageARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glActiveTextureARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClientActiveTextureARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1iARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1sARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1svARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2iARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2sARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2svARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3iARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3sARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3svARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4iARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4sARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4svARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenQueriesARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteQueriesARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsQueryARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginQueryARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndQueryARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryObjectivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryObjectuivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMaxShaderCompilerThreadsARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterfARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPolygonOffsetClamp;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramInterfaceiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramResourceIndex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramResourceName;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramResourceiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramResourceLocation;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramResourceLocationIndex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetGraphicsResetStatusARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnTexImageARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReadnPixelsARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnCompressedTexImageARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformuivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformdvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnMapdvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnMapfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnMapivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnPixelMapfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnPixelMapuivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnPixelMapusvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnPolygonStippleARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnColorTableARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnConvolutionFilterARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnSeparableFilterARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnHistogramARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnMinmaxARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferSampleLocationsfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferSampleLocationsfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEvaluateDepthValuesARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMinSampleShadingARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUseProgramStages;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glActiveShaderProgram;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateShaderProgramv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindProgramPipeline;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteProgramPipelines;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenProgramPipelines;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsProgramPipeline;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramPipelineiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4i;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4iv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4f;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4ui;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4uiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2x3fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3x2fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2x4fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4x2fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3x4fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4x3fv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2x3dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3x2dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2x4dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4x2dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3x4dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4x3dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glValidateProgramPipeline;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramPipelineInfoLog;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveAtomicCounterBufferiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindImageTexture;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMemoryBarrier;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteObjectARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetHandleARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDetachObjectARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateShaderObjectARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glShaderSourceARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompileShaderARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateProgramObjectARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAttachObjectARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLinkProgramARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUseProgramObjectARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glValidateProgramARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1iARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2iARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3iARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4iARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix2fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix3fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformMatrix4fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetObjectParameterfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetObjectParameterivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetInfoLogARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetAttachedObjectsARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformLocationARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveUniformARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetShaderSourceARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glShaderStorageBlockBinding;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSubroutineUniformLocation;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSubroutineIndex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveSubroutineUniformiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveSubroutineUniformName;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveSubroutineName;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformSubroutinesuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformSubroutineuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramStageiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedStringARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteNamedStringARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompileShaderIncludeARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsNamedStringARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedStringARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedStringivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBufferPageCommitmentARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedBufferPageCommitmentEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedBufferPageCommitmentARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexPageCommitmentARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPatchParameteri;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPatchParameterfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureBarrier;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexBufferARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexBufferRange;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexImage3DARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexImage2DARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexImage1DARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexSubImage3DARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexSubImage2DARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTexSubImage1DARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCompressedTexImageARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorage1D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorage2D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorage3D;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorage2DMultisample;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorage3DMultisample;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureView;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindTransformFeedback;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteTransformFeedbacks;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenTransformFeedbacks;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsTransformFeedback;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPauseTransformFeedback;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glResumeTransformFeedback;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawTransformFeedback;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawTransformFeedbackStream;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginQueryIndexed;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndQueryIndexed;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryIndexediv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawTransformFeedbackInstanced;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawTransformFeedbackStreamInstanced;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLoadTransposeMatrixfARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLoadTransposeMatrixdARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultTransposeMatrixfARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultTransposeMatrixdARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL1d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL2d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL3d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL4d;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL1dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL2dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL3dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL4dv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribLPointer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribLdv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindVertexBuffer;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribFormat;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribIFormat;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribLFormat;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribBinding;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexBindingDivisor;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWeightbvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWeightsvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWeightivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWeightfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWeightdvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWeightubvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWeightusvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWeightuivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWeightPointerARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexBlendARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBufferARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteBuffersARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenBuffersARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsBufferARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBufferDataARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBufferSubDataARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBufferSubDataARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapBufferARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUnmapBufferARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBufferParameterivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBufferPointervARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1sARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1svARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2sARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2svARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3sARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3svARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4NbvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4NivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4NsvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4NubARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4NubvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4NuivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4NusvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4bvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4sARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4svARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4ubvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4uivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4usvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribPointerARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnableVertexAttribArrayARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisableVertexAttribArrayARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribdvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribfvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribPointervARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindAttribLocationARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveAttribARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetAttribLocationARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glViewportArrayv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glViewportIndexedf;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glViewportIndexedfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glScissorArrayv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glScissorIndexed;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glScissorIndexedv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthRangeArrayv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthRangeIndexed;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFloati_v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetDoublei_v;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2iARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2sARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2svARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3dARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3dvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3fARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3fvARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3iARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3ivARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3sARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3svARB;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawBuffersATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glElementPointerATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElementArrayATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawRangeElementArrayATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexBumpParameterivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexBumpParameterfvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexBumpParameterivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexBumpParameterfvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenFragmentShadersATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindFragmentShaderATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteFragmentShaderATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginFragmentShaderATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndFragmentShaderATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPassTexCoordATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSampleMapATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorFragmentOp1ATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorFragmentOp2ATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorFragmentOp3ATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAlphaFragmentOp1ATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAlphaFragmentOp2ATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAlphaFragmentOp3ATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSetFragmentShaderConstantATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapObjectBufferATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUnmapObjectBufferATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPNTrianglesiATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPNTrianglesfATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilOpSeparateATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilFuncSeparateATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNewObjectBufferATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsObjectBufferATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUpdateObjectBufferATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetObjectBufferfvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetObjectBufferivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFreeObjectBufferATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glArrayObjectATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetArrayObjectfvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetArrayObjectivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVariantArrayObjectATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVariantArrayObjectfvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVariantArrayObjectivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribArrayObjectATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribArrayObjectfvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribArrayObjectivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream1sATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream1svATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream1iATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream1ivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream1fATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream1fvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream1dATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream1dvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream2sATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream2svATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream2iATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream2ivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream2fATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream2fvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream2dATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream2dvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream3sATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream3svATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream3iATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream3ivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream3fATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream3fvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream3dATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream3dvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream4sATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream4svATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream4iATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream4ivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream4fATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream4fvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream4dATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexStream4dvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalStream3bATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalStream3bvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalStream3sATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalStream3svATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalStream3iATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalStream3ivATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalStream3fATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalStream3fvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalStream3dATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalStream3dvATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClientActiveVertexStreamATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexBlendEnviATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexBlendEnvfATI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEGLImageTargetTexStorageEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEGLImageTargetTextureStorageEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformBufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformBufferSizeEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendColorEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendEquationSeparateEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendFuncSeparateEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendEquationEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorSubTableEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyColorSubTableEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLockArraysEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUnlockArraysEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionFilter1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionFilter2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionParameterfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionParameteriEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyConvolutionFilter1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyConvolutionFilter2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetConvolutionFilterEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetConvolutionParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetConvolutionParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSeparableFilterEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSeparableFilter2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangent3bEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangent3bvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangent3dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangent3dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangent3fEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangent3fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangent3iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangent3ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangent3sEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangent3svEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormal3bEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormal3bvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormal3dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormal3dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormal3fEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormal3fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormal3iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormal3ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormal3sEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormal3svEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTangentPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBinormalPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTexImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTexImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTexSubImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTexSubImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTexSubImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCullParameterdvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCullParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLabelObjectEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetObjectLabelEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInsertEventMarkerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPushGroupMarkerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPopGroupMarkerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthBoundsEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixLoadfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixLoaddEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixMultfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixMultdEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixLoadIdentityEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixRotatefEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixRotatedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixScalefEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixScaledEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixTranslatefEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixTranslatedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixFrustumEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixOrthoEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixPopEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixPushEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClientAttribDefaultEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPushClientAttribDefaultEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameterfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameteriEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureSubImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureSubImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTextureImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTextureImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTextureSubImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTextureSubImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureImageEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureLevelParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureLevelParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureSubImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyTextureSubImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindMultiTextureEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoordPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexEnvfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexEnvfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexEnviEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexEnvivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexGendEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexGendvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexGenfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexGenfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexGeniEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexGenivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexEnvfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexEnvivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexGendvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexGenfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexGenivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexParameteriEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexParameterfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexSubImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexSubImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyMultiTexImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyMultiTexImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyMultiTexSubImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyMultiTexSubImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexImageEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexLevelParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexLevelParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexSubImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyMultiTexSubImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnableClientStateIndexedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisableClientStateIndexedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFloatIndexedvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetDoubleIndexedvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPointerIndexedvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnableIndexedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisableIndexedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsEnabledIndexedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetIntegerIndexedvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBooleanIndexedvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTextureImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTextureImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTextureImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTextureSubImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTextureSubImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedTextureSubImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCompressedTextureImageEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedMultiTexImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedMultiTexImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedMultiTexImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedMultiTexSubImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedMultiTexSubImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompressedMultiTexSubImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCompressedMultiTexImageEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixLoadTransposefEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixLoadTransposedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixMultTransposefEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixMultTransposedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedBufferDataEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedBufferSubDataEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapNamedBufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUnmapNamedBufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedBufferParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedBufferPointervEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedBufferSubDataEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1fEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2fEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3fEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4fEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2x3fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3x2fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2x4fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4x2fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3x4fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4x3fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureBufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexBufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameterIivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureParameterIuivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureParameterIivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureParameterIuivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexParameterIivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexParameterIuivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexParameterIivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultiTexParameterIuivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParameters4fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParameterI4iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParameterI4ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParametersI4ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParameterI4uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParameterI4uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParametersI4uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedProgramLocalParameterIivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedProgramLocalParameterIuivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnableClientStateiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisableClientStateiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFloati_vEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetDoublei_vEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPointeri_vEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramStringEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParameter4dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParameter4dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParameter4fEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedProgramLocalParameter4fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedProgramLocalParameterdvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedProgramLocalParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedProgramivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedProgramStringEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedRenderbufferStorageEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedRenderbufferParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedRenderbufferStorageMultisampleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedRenderbufferStorageMultisampleCoverageEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCheckNamedFramebufferStatusEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferTexture1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferTexture2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferTexture3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferRenderbufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedFramebufferAttachmentParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenerateTextureMipmapEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenerateMultiTexMipmapEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferDrawBufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferDrawBuffersEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferReadBufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFramebufferParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedCopyBufferSubDataEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferTextureEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferTextureLayerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferTextureFaceEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureRenderbufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexRenderbufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayColorOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayEdgeFlagOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayIndexOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayNormalOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayTexCoordOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayMultiTexCoordOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayFogCoordOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArraySecondaryColorOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexAttribOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexAttribIOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnableVertexArrayEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisableVertexArrayEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnableVertexArrayAttribEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisableVertexArrayAttribEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexArrayIntegervEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexArrayPointervEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexArrayIntegeri_vEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexArrayPointeri_vEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapNamedBufferRangeEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFlushMappedNamedBufferRangeEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedBufferStorageEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearNamedBufferDataEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearNamedBufferSubDataEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferParameteriEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedFramebufferParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform1dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform2dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform3dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniform4dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2x3dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix2x4dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3x2dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix3x4dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4x2dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformMatrix4x3dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureBufferRangeEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorage2DMultisampleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorage3DMultisampleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayBindVertexBufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexAttribFormatEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexAttribIFormatEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexAttribLFormatEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexAttribBindingEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexBindingDivisorEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexAttribLOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexturePageCommitmentEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayVertexAttribDivisorEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorMaskIndexedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawArraysInstancedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawElementsInstancedEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawRangeElementsEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBufferStorageExternalEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedBufferStorageExternalEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogCoordfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogCoordfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogCoorddEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogCoorddvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogCoordPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlitFramebufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRenderbufferStorageMultisampleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsRenderbufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindRenderbufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteRenderbuffersEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenRenderbuffersEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRenderbufferStorageEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetRenderbufferParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsFramebufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindFramebufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteFramebuffersEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenFramebuffersEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCheckFramebufferStatusEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTexture1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTexture2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTexture3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferRenderbufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFramebufferAttachmentParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenerateMipmapEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramParameteriEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParameters4fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParameters4fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUniformuivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindFragDataLocationEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFragDataLocationEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform1uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform2uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform3uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniform4uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetHistogramEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetHistogramParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetHistogramParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMinmaxEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMinmaxParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMinmaxParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glHistogramEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMinmaxEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glResetHistogramEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glResetMinmaxEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIndexFuncEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIndexMaterialEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glApplyTextureEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureLightEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureMaterialEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUnsignedBytevEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetUnsignedBytei_vEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteMemoryObjectsEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsMemoryObjectEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateMemoryObjectsEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMemoryObjectParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMemoryObjectParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorageMem2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorageMem2DMultisampleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorageMem3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorageMem3DMultisampleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBufferStorageMemEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorageMem2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorageMem2DMultisampleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorageMem3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorageMem3DMultisampleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedBufferStorageMemEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexStorageMem1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureStorageMem1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImportMemoryFdEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImportMemoryWin32HandleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImportMemoryWin32NameEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawArraysEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawElementsEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSampleMaskEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSamplePatternEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorTableEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetColorTableEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetColorTableParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetColorTableParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelTransformParameteriEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelTransformParameterfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelTransformParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelTransformParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPixelTransformParameterivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPixelTransformParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPolygonOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPolygonOffsetClampEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProvokingVertexEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRasterSamplesEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3bEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3bvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3fEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3fvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3sEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3svEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3ubEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3ubvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3usEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3usvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColorPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenSemaphoresEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteSemaphoresEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsSemaphoreEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSemaphoreParameterui64vEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSemaphoreParameterui64vEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWaitSemaphoreEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSignalSemaphoreEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImportSemaphoreFdEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImportSemaphoreWin32HandleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImportSemaphoreWin32NameEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUseShaderProgramEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glActiveProgramEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateShaderProgramEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glActiveShaderProgramEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindProgramPipelineEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateShaderProgramvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteProgramPipelinesEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenProgramPipelinesEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramPipelineInfoLogEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramPipelineivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsProgramPipelineEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUseProgramStagesEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glValidateProgramPipelineEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferFetchBarrierEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindImageTextureEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMemoryBarrierEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilClearTagEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glActiveStencilFaceEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexSubImage1DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexSubImage2DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexSubImage3DEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTextureLayerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexBufferEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexParameterIivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexParameterIuivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexParameterIivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexParameterIuivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearColorIiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearColorIuiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAreTexturesResidentEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindTextureEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteTexturesEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenTexturesEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsTextureEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPrioritizeTexturesEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureNormalEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryObjecti64vEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetQueryObjectui64vEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginTransformFeedbackEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndTransformFeedbackEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBufferRangeEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBufferOffsetEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBufferBaseEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTransformFeedbackVaryingsEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTransformFeedbackVaryingEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glArrayElementEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawArraysEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEdgeFlagPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPointervEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIndexPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL1dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL2dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL3dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL4dEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL1dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL2dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL3dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL4dvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribLPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribLdvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginVertexShaderEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndVertexShaderEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindVertexShaderEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenVertexShadersEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteVertexShaderEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glShaderOp1EXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glShaderOp2EXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glShaderOp3EXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSwizzleEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWriteMaskEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInsertComponentEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glExtractComponentEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenSymbolsEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSetInvariantEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSetLocalConstantEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVariantbvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVariantsvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVariantivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVariantfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVariantdvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVariantubvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVariantusvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVariantuivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVariantPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEnableVariantClientStateEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDisableVariantClientStateEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindLightParameterEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindMaterialParameterEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindTexGenParameterEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindTextureUnitParameterEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindParameterEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsVariantEnabledEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVariantBooleanvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVariantIntegervEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVariantFloatvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVariantPointervEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetInvariantBooleanvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetInvariantIntegervEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetInvariantFloatvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetLocalConstantBooleanvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetLocalConstantIntegervEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetLocalConstantFloatvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexWeightfEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexWeightfvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexWeightPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAcquireKeyedMutexWin32EXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReleaseKeyedMutexWin32EXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowRectanglesEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImportSyncEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFrameTerminatorGREMEDY;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStringMarkerGREMEDY;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImageTransformParameteriHP;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImageTransformParameterfHP;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImageTransformParameterivHP;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glImageTransformParameterfvHP;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetImageTransformParameterivHP;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetImageTransformParameterfvHP;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiModeDrawArraysIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiModeDrawElementsIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFlushStaticDataIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorPointerListIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColorPointerListIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEdgeFlagPointerListIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogCoordPointerListIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIndexPointerListIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalPointerListIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordPointerListIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexPointerListIBM;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendFuncSeparateINGR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glApplyFramebufferAttachmentCMAAINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSyncTextureINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUnmapTexture2DINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapTexture2DINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexPointervINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalPointervINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorPointervINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordPointervINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginPerfQueryINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreatePerfQueryINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeletePerfQueryINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndPerfQueryINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFirstPerfQueryIdINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNextPerfQueryIdINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPerfCounterInfoINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPerfQueryDataINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPerfQueryIdByNameINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPerfQueryInfoINTEL;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendBarrierKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageControl;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageInsert;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageCallback;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetDebugMessageLog;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPushDebugGroup;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPopDebugGroup;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glObjectLabel;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetObjectLabel;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glObjectPtrLabel;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetObjectPtrLabel;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPointerv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageControlKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageInsertKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDebugMessageCallbackKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetDebugMessageLogKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPushDebugGroupKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPopDebugGroupKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glObjectLabelKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetObjectLabelKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glObjectPtrLabelKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetObjectPtrLabelKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPointervKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMaxShaderCompilerThreadsKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetGraphicsResetStatus;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReadnPixels;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformfv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformuiv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetGraphicsResetStatusKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReadnPixelsKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformfvKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformivKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetnUniformuivKHR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glResizeBuffersMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2dMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2dvMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2fMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2fvMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2iMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2ivMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2sMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos2svMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3dMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3dvMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3fMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3fvMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3iMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3ivMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3sMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos3svMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos4dMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos4dvMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos4fMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos4fvMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos4iMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos4ivMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos4sMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWindowPos4svMESA;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginConditionalRenderNVX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndConditionalRenderNVX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLGPUNamedBufferSubDataNVX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLGPUCopyImageSubDataNVX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLGPUInterlockNVX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAlphaToCoverageDitherControlNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawArraysIndirectBindlessNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawElementsIndirectBindlessNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawArraysIndirectBindlessCountNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiDrawElementsIndirectBindlessCountNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureHandleNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTextureSamplerHandleNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeTextureHandleResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeTextureHandleNonResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetImageHandleNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeImageHandleResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeImageHandleNonResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformHandleui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformHandleui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformHandleui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformHandleui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsTextureHandleResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsImageHandleResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendParameteriNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendBarrierNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glViewportPositionWScaleNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateStatesNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteStatesNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsStateNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStateCaptureNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCommandHeaderNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetStageIndexNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawCommandsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawCommandsAddressNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawCommandsStatesNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawCommandsStatesAddressNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCreateCommandListsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteCommandListsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsCommandListNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glListDrawCommandsStatesClientNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCommandListSegmentsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCompileCommandListNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCallCommandListNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginConditionalRenderNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndConditionalRenderNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSubpixelPrecisionBiasNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConservativeRasterParameterfNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConservativeRasterParameteriNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyImageSubDataNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthRangedNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearDepthdNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthBoundsdNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawTextureNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawVkImageNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVkProcAddrNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWaitVkSemaphoreNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSignalVkSemaphoreNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSignalVkFenceNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapControlPointsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMapControlPointsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMapParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMapParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMapAttribParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMapAttribParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEvalMapsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMultisamplefvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSampleMaskIndexedNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexRenderbufferNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteFencesNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenFencesNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsFenceNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTestFenceNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFenceivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFinishFenceNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSetFenceNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentCoverageColorNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramNamedParameter4fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramNamedParameter4fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramNamedParameter4dNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramNamedParameter4dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramNamedParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramNamedParameterdvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCoverageModulationTableNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCoverageModulationTableNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCoverageModulationNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRenderbufferStorageMultisampleCoverageNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramVertexLimitNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTextureEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTextureFaceEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRenderGpuMaskNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastBufferSubDataNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastCopyBufferSubDataNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastCopyImageSubDataNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastBlitFramebufferNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastFramebufferSampleLocationsfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastBarrierNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastWaitSyncNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastGetQueryObjectivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastGetQueryObjectuivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastGetQueryObjecti64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMulticastGetQueryObjectui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParameterI4iNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParameterI4ivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParametersI4ivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParameterI4uiNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParameterI4uivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramLocalParametersI4uivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParameterI4iNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParameterI4ivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParametersI4ivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParameterI4uiNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParameterI4uivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramEnvParametersI4uivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramLocalParameterIivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramLocalParameterIuivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramEnvParameterIivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramEnvParameterIuivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramSubroutineParametersuivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramSubroutineParameteruivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex2hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex2hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex3hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex3hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex4hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex4hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormal3hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormal3hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor3hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor3hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor4hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor4hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord1hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord1hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord3hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord3hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord4hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord4hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogCoordhNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogCoordhvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColor3hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexWeighthNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexWeighthvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4hNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs1hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs2hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs3hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs4hvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetInternalformatSampleivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenOcclusionQueriesNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteOcclusionQueriesNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsOcclusionQueryNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginOcclusionQueryNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndOcclusionQueryNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetOcclusionQueryivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetOcclusionQueryuivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramBufferParametersfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramBufferParametersIivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramBufferParametersIuivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenPathsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeletePathsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsPathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathCommandsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathCoordsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathSubCommandsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathSubCoordsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathStringNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathGlyphsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathGlyphRangeNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glWeightPathsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyPathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInterpolatePathsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTransformPathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathParameteriNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathParameterfNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathDashArrayNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathStencilFuncNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathStencilDepthOffsetNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilFillPathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilStrokePathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilFillPathInstancedNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilStrokePathInstancedNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathCoverDepthFuncNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCoverFillPathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCoverStrokePathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCoverFillPathInstancedNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCoverStrokePathInstancedNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathCommandsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathCoordsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathDashArrayNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathMetricsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathMetricRangeNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathSpacingNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsPointInFillPathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsPointInStrokePathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathLengthNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointAlongPathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixLoad3x2fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixLoad3x3fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixLoadTranspose3x3fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixMult3x2fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixMult3x3fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMatrixMultTranspose3x3fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilThenCoverFillPathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilThenCoverStrokePathNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilThenCoverFillPathInstancedNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStencilThenCoverStrokePathInstancedNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathGlyphIndexRangeNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathGlyphIndexArrayNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathMemoryGlyphIndexArrayNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramPathFragmentInputGenNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramResourcefvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathColorGenNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathTexGenNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPathFogGenNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathColorGenivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathColorGenfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathTexGenivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPathTexGenfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelDataRangeNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFlushPixelDataRangeNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameteriNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPresentFrameKeyedNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPresentFrameDualFillNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVideoivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVideouivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVideoi64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVideoui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPrimitiveRestartNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPrimitiveRestartIndexNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glQueryResourceNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenQueryResourceTagNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteQueryResourceTagNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glQueryResourceTagNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCombinerParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCombinerParameterfNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCombinerParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCombinerParameteriNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCombinerInputNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCombinerOutputNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFinalCombinerInputNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCombinerInputParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCombinerInputParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCombinerOutputParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCombinerOutputParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFinalCombinerInputParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFinalCombinerInputParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCombinerStageParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetCombinerStageParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferSampleLocationsfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNamedFramebufferSampleLocationsfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glResolveDepthValuesNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeBufferResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeBufferNonResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsBufferResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeNamedBufferResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMakeNamedBufferNonResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsNamedBufferResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetBufferParameterui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetNamedBufferParameterui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetIntegerui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glUniformui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramUniformui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureBarrierNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexImage2DMultisampleCoverageNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexImage3DMultisampleCoverageNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureImage2DMultisampleNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureImage3DMultisampleNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureImage2DMultisampleCoverageNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureImage3DMultisampleCoverageNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginTransformFeedbackNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndTransformFeedbackNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTransformFeedbackAttribsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBufferRangeNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBufferOffsetNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindBufferBaseNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTransformFeedbackVaryingsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glActiveVaryingNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVaryingLocationNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetActiveVaryingNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTransformFeedbackVaryingNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTransformFeedbackStreamAttribsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindTransformFeedbackNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteTransformFeedbacksNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenTransformFeedbacksNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsTransformFeedbackNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPauseTransformFeedbackNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glResumeTransformFeedbackNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawTransformFeedbackNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVDPAUInitNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVDPAUFiniNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVDPAURegisterVideoSurfaceNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVDPAURegisterOutputSurfaceNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVDPAUIsSurfaceNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVDPAUUnregisterSurfaceNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVDPAUGetSurfaceivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVDPAUSurfaceAccessNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVDPAUMapSurfacesNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVDPAUUnmapSurfacesNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFlushVertexArrayRangeNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexArrayRangeNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL1i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL2i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL3i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL4i64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL1i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL2i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL3i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL4i64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL1ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL2ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL3ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL4ui64NV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL1ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL2ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL3ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribL4ui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribLi64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribLui64vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribLFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBufferAddressRangeNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormalFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIndexFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoordFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEdgeFlagFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSecondaryColorFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogCoordFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribIFormatNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetIntegerui64i_vNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAreProgramsResidentNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindProgramNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteProgramsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glExecuteProgramNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenProgramsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramParameterdvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetProgramStringNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTrackMatrixivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribdvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribPointervNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsProgramNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLoadProgramNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramParameter4dNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramParameter4dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramParameter4fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramParameter4fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramParameters4dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glProgramParameters4fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRequestResidentProgramsNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTrackMatrixNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribPointerNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1dNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1sNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib1svNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2dNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2sNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib2svNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3dNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3sNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib3svNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4dNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4fNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4sNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4svNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4ubNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttrib4ubvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs1dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs1fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs1svNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs2dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs2fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs2svNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs3dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs3fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs3svNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs4dvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs4fvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs4svNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribs4ubvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI1iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI2iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI3iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4iEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI1uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI2uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI3uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4uiEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI1ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI2ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI3ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4ivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI1uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI2uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI3uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4uivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4bvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4svEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4ubvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribI4usvEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertexAttribIPointerEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribIivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVertexAttribIuivEXT;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBeginVideoCaptureNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindVideoCaptureStreamBufferNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBindVideoCaptureStreamTextureNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEndVideoCaptureNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVideoCaptureivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVideoCaptureStreamivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVideoCaptureStreamfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetVideoCaptureStreamdvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVideoCaptureNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVideoCaptureStreamParameterivNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVideoCaptureStreamParameterfvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVideoCaptureStreamParameterdvNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glViewportSwizzleNV;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord1bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord1bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord3bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord3bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord4bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord4bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex2bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex2bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex3bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex3bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex4bOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex4bvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAlphaFuncxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearColorxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearDepthxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClipPlanexOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor4xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthRangexOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFrustumxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetClipPlanexOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFixedvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexEnvxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexParameterxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLightModelxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLightModelxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLightxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLightxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLineWidthxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLoadMatrixxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMaterialxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMaterialxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultMatrixxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormal3xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glOrthoxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointSizexOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPolygonOffsetxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRotatexOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glScalexOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexEnvxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexEnvxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexParameterxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexParameterxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTranslatexOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetLightxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMaterialxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSampleCoveragexOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAccumxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBitmapxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glBlendColorxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearAccumxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor3xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor3xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor4xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionParameterxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glConvolutionParameterxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEvalCoord1xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEvalCoord1xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEvalCoord2xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glEvalCoord2xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFeedbackBufferxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetConvolutionParameterxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetHistogramParameterxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetLightxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMapxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetMaterialxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPixelMapxv;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexGenxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexLevelParameterxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIndexxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIndexxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLoadTransposeMatrixxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMap1xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMap2xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapGrid1xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMapGrid2xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultTransposeMatrixxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord1xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord2xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord3xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glMultiTexCoord4xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormal3xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPassThroughxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelMapx;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelStorex;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelTransferxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelZoomxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPrioritizeTexturesxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRasterPos2xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRasterPos2xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRasterPos3xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRasterPos3xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRasterPos4xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRasterPos4xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRectxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glRectxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord1xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord1xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord3xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord3xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord4xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord4xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexGenxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexGenxvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex2xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex2xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex3xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex3xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex4xOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glVertex4xvOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glQueryMatrixxOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClearDepthfOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glClipPlanefOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDepthRangefOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFrustumfOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetClipPlanefOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glOrthofOES;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFramebufferTextureMultiviewOVR;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glHintPGI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDetailTexFuncSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetDetailTexFuncSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFogFuncSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFogFuncSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSampleMaskSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSamplePatternSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelTexGenParameteriSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelTexGenParameterivSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelTexGenParameterfSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelTexGenParameterfvSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPixelTexGenParameterivSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetPixelTexGenParameterfvSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterfSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPointParameterfvSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSharpenTexFuncSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetSharpenTexFuncSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexImage4DSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexSubImage4DSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTextureColorMaskSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetTexFilterFuncSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexFilterFuncSGIS;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glAsyncMarkerSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFinishAsyncSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPollAsyncSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGenAsyncMarkersSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeleteAsyncMarkersSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIsAsyncMarkerSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFlushRasterSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentColorMaterialSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentLightfSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentLightfvSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentLightiSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentLightivSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentLightModelfSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentLightModelfvSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentLightModeliSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentLightModelivSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentMaterialfSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentMaterialfvSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentMaterialiSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFragmentMaterialivSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFragmentLightfvSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFragmentLightivSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFragmentMaterialfvSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetFragmentMaterialivSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLightEnviSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFrameZoomSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glIglooInterfaceSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetInstrumentsSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glInstrumentsBufferSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPollInstrumentsSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReadInstrumentsSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStartInstrumentsSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glStopInstrumentsSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetListParameterfvSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetListParameterivSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glListParameterfSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glListParameterfvSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glListParameteriSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glListParameterivSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glPixelTexGenSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeformationMap3dSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeformationMap3fSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDeformSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glLoadIdentityDeformationMapSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReferencePlaneSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSpriteParameterfSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSpriteParameterfvSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSpriteParameteriSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glSpriteParameterivSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTagSampleBufferSGIX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorTableSGI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorTableParameterfvSGI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColorTableParameterivSGI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glCopyColorTableSGI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetColorTableSGI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetColorTableParameterfvSGI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGetColorTableParameterivSGI;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glFinishTextureSUNX;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGlobalAlphaFactorbSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGlobalAlphaFactorsSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGlobalAlphaFactoriSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGlobalAlphaFactorfSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGlobalAlphaFactordSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGlobalAlphaFactorubSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGlobalAlphaFactorusSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glGlobalAlphaFactoruiSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glDrawMeshArraysSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeusSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeubSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuivSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeusvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeubvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodePointerSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor4ubVertex2fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor4ubVertex2fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor4ubVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor4ubVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormal3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glNormal3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor4fNormal3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glColor4fNormal3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord4fVertex4fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord4fVertex4fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2fColor4ubVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2fColor4ubVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2fColor3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2fColor3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2fNormal3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2fNormal3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2fColor4fNormal3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord2fColor4fNormal3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord4fColor4fNormal3fVertex4fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glTexCoord4fColor4fNormal3fVertex4fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiColor4ubVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiColor4ubVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiColor3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiColor3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiNormal3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiNormal3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiColor4fNormal3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiColor4fNormal3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiTexCoord2fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiTexCoord2fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiTexCoord2fNormal3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiTexCoord2fNormal3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  p = (const char*)glad_glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fvSUN;
  set += p != NULL;
  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;
  printf("  %d of %d pointers set, hash %016llx\n", set, 2584, hash);
  int on = 0;
  if (GLAD_GL_VERSION_1_0)
  {
    printf("  GLAD_GL_VERSION_1_0\n");
    ++on;
  }
  if (GLAD_GL_VERSION_1_1)
  {
    printf("  GLAD_GL_VERSION_1_1\n");
    ++on;
  }
  if (GLAD_GL_VERSION_1_2)
  {
    printf("  GLAD_GL_VERSION_1_2\n");
    ++on;
  }
  if (GLAD_GL_VERSION_1_3)
  {
    printf("  GLAD_GL_VERSION_1_3\n");
    ++on;
  }
  if (GLAD_GL_VERSION_1_4)
  {
    printf("  GLAD_GL_VERSION_1_4\n");
    ++on;
  }
  if (GLAD_GL_VERSION_1_5)
  {
    printf("  GLAD_GL_VERSION_1_5\n");
    ++on;
  }
  if (GLAD_GL_VERSION_2_0)
  {
    printf("  GLAD_GL_VERSION_2_0\n");
    ++on;
  }
  if (GLAD_GL_VERSION_2_1)
  {
    printf("  GLAD_GL_VERSION_2_1\n");
    ++on;
  }
  if (GLAD_GL_VERSION_3_0)
  {
    printf("  GLAD_GL_VERSION_3_0\n");
    ++on;
  }
  if (GLAD_GL_VERSION_3_1)
  {
    printf("  GLAD_GL_VERSION_3_1\n");
    ++on;
  }
  if (GLAD_GL_VERSION_3_2)
  {
    printf("  GLAD_GL_VERSION_3_2\n");
    ++on;
  }
  if (GLAD_GL_VERSION_3_3)
  {
    printf("  GLAD_GL_VERSION_3_3\n");
    ++on;
  }
  if (GLAD_GL_3DFX_multisample)
  {
    printf("  GLAD_GL_3DFX_multisample\n");
    ++on;
  }
  if (GLAD_GL_3DFX_tbuffer)
  {
    printf("  GLAD_GL_3DFX_tbuffer\n");
    ++on;
  }
  if (GLAD_GL_3DFX_texture_compression_FXT1)
  {
    printf("  GLAD_GL_3DFX_texture_compression_FXT1\n");
    ++on;
  }
  if (GLAD_GL_AMD_blend_minmax_factor)
  {
    printf("  GLAD_GL_AMD_blend_minmax_factor\n");
    ++on;
  }
  if (GLAD_GL_AMD_conservative_depth)
  {
    printf("  GLAD_GL_AMD_conservative_depth\n");
    ++on;
  }
  if (GLAD_GL_AMD_debug_output)
  {
    printf("  GLAD_GL_AMD_debug_output\n");
    ++on;
  }
  if (GLAD_GL_AMD_depth_clamp_separate)
  {
    printf("  GLAD_GL_AMD_depth_clamp_separate\n");
    ++on;
  }
  if (GLAD_GL_AMD_draw_buffers_blend)
  {
    printf("  GLAD_GL_AMD_draw_buffers_blend\n");
    ++on;
  }
  if (GLAD_GL_AMD_framebuffer_sample_positions)
  {
    printf("  GLAD_GL_AMD_framebuffer_sample_positions\n");
    ++on;
  }
  if (GLAD_GL_AMD_gcn_shader)
  {
    printf("  GLAD_GL_AMD_gcn_shader\n");
    ++on;
  }
  if (GLAD_GL_AMD_gpu_shader_half_float)
  {
    printf("  GLAD_GL_AMD_gpu_shader_half_float\n");
    ++on;
  }
  if (GLAD_GL_AMD_gpu_shader_int16)
  {
    printf("  GLAD_GL_AMD_gpu_shader_int16\n");
    ++on;
  }
  if (GLAD_GL_AMD_gpu_shader_int64)
  {
    printf("  GLAD_GL_AMD_gpu_shader_int64\n");
    ++on;
  }
  if (GLAD_GL_AMD_interleaved_elements)
  {
    printf("  GLAD_GL_AMD_interleaved_elements\n");
    ++on;
  }
  if (GLAD_GL_AMD_multi_draw_indirect)
  {
    printf("  GLAD_GL_AMD_multi_draw_indirect\n");
    ++on;
  }
  if (GLAD_GL_AMD_name_gen_delete)
  {
    printf("  GLAD_GL_AMD_name_gen_delete\n");
    ++on;
  }
  if (GLAD_GL_AMD_occlusion_query_event)
  {
    printf("  GLAD_GL_AMD_occlusion_query_event\n");
    ++on;
  }
  if (GLAD_GL_AMD_performance_monitor)
  {
    printf("  GLAD_GL_AMD_performance_monitor\n");
    ++on;
  }
  if (GLAD_GL_AMD_pinned_memory)
  {
    printf("  GLAD_GL_AMD_pinned_memory\n");
    ++on;
  }
  if (GLAD_GL_AMD_query_buffer_object)
  {
    printf("  GLAD_GL_AMD_query_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_AMD_sample_positions)
  {
    printf("  GLAD_GL_AMD_sample_positions\n");
    ++on;
  }
  if (GLAD_GL_AMD_seamless_cubemap_per_texture)
  {
    printf("  GLAD_GL_AMD_seamless_cubemap_per_texture\n");
    ++on;
  }
  if (GLAD_GL_AMD_shader_atomic_counter_ops)
  {
    printf("  GLAD_GL_AMD_shader_atomic_counter_ops\n");
    ++on;
  }
  if (GLAD_GL_AMD_shader_ballot)
  {
    printf("  GLAD_GL_AMD_shader_ballot\n");
    ++on;
  }
  if (GLAD_GL_AMD_shader_explicit_vertex_parameter)
  {
    printf("  GLAD_GL_AMD_shader_explicit_vertex_parameter\n");
    ++on;
  }
  if (GLAD_GL_AMD_shader_gpu_shader_half_float_fetch)
  {
    printf("  GLAD_GL_AMD_shader_gpu_shader_half_float_fetch\n");
    ++on;
  }
  if (GLAD_GL_AMD_shader_image_load_store_lod)
  {
    printf("  GLAD_GL_AMD_shader_image_load_store_lod\n");
    ++on;
  }
  if (GLAD_GL_AMD_shader_stencil_export)
  {
    printf("  GLAD_GL_AMD_shader_stencil_export\n");
    ++on;
  }
  if (GLAD_GL_AMD_shader_trinary_minmax)
  {
    printf("  GLAD_GL_AMD_shader_trinary_minmax\n");
    ++on;
  }
  if (GLAD_GL_AMD_sparse_texture)
  {
    printf("  GLAD_GL_AMD_sparse_texture\n");
    ++on;
  }
  if (GLAD_GL_AMD_stencil_operation_extended)
  {
    printf("  GLAD_GL_AMD_stencil_operation_extended\n");
    ++on;
  }
  if (GLAD_GL_AMD_texture_gather_bias_lod)
  {
    printf("  GLAD_GL_AMD_texture_gather_bias_lod\n");
    ++on;
  }
  if (GLAD_GL_AMD_texture_texture4)
  {
    printf("  GLAD_GL_AMD_texture_texture4\n");
    ++on;
  }
  if (GLAD_GL_AMD_transform_feedback3_lines_triangles)
  {
    printf("  GLAD_GL_AMD_transform_feedback3_lines_triangles\n");
    ++on;
  }
  if (GLAD_GL_AMD_transform_feedback4)
  {
    printf("  GLAD_GL_AMD_transform_feedback4\n");
    ++on;
  }
  if (GLAD_GL_AMD_vertex_shader_layer)
  {
    printf("  GLAD_GL_AMD_vertex_shader_layer\n");
    ++on;
  }
  if (GLAD_GL_AMD_vertex_shader_tessellator)
  {
    printf("  GLAD_GL_AMD_vertex_shader_tessellator\n");
    ++on;
  }
  if (GLAD_GL_AMD_vertex_shader_viewport_index)
  {
    printf("  GLAD_GL_AMD_vertex_shader_viewport_index\n");
    ++on;
  }
  if (GLAD_GL_APPLE_aux_depth_stencil)
  {
    printf("  GLAD_GL_APPLE_aux_depth_stencil\n");
    ++on;
  }
  if (GLAD_GL_APPLE_client_storage)
  {
    printf("  GLAD_GL_APPLE_client_storage\n");
    ++on;
  }
  if (GLAD_GL_APPLE_element_array)
  {
    printf("  GLAD_GL_APPLE_element_array\n");
    ++on;
  }
  if (GLAD_GL_APPLE_fence)
  {
    printf("  GLAD_GL_APPLE_fence\n");
    ++on;
  }
  if (GLAD_GL_APPLE_float_pixels)
  {
    printf("  GLAD_GL_APPLE_float_pixels\n");
    ++on;
  }
  if (GLAD_GL_APPLE_flush_buffer_range)
  {
    printf("  GLAD_GL_APPLE_flush_buffer_range\n");
    ++on;
  }
  if (GLAD_GL_APPLE_object_purgeable)
  {
    printf("  GLAD_GL_APPLE_object_purgeable\n");
    ++on;
  }
  if (GLAD_GL_APPLE_rgb_422)
  {
    printf("  GLAD_GL_APPLE_rgb_422\n");
    ++on;
  }
  if (GLAD_GL_APPLE_row_bytes)
  {
    printf("  GLAD_GL_APPLE_row_bytes\n");
    ++on;
  }
  if (GLAD_GL_APPLE_specular_vector)
  {
    printf("  GLAD_GL_APPLE_specular_vector\n");
    ++on;
  }
  if (GLAD_GL_APPLE_texture_range)
  {
    printf("  GLAD_GL_APPLE_texture_range\n");
    ++on;
  }
  if (GLAD_GL_APPLE_transform_hint)
  {
    printf("  GLAD_GL_APPLE_transform_hint\n");
    ++on;
  }
  if (GLAD_GL_APPLE_vertex_array_object)
  {
    printf("  GLAD_GL_APPLE_vertex_array_object\n");
    ++on;
  }
  if (GLAD_GL_APPLE_vertex_array_range)
  {
    printf("  GLAD_GL_APPLE_vertex_array_range\n");
    ++on;
  }
  if (GLAD_GL_APPLE_vertex_program_evaluators)
  {
    printf("  GLAD_GL_APPLE_vertex_program_evaluators\n");
    ++on;
  }
  if (GLAD_GL_APPLE_ycbcr_422)
  {
    printf("  GLAD_GL_APPLE_ycbcr_422\n");
    ++on;
  }
  if (GLAD_GL_ARB_ES2_compatibility)
  {
    printf("  GLAD_GL_ARB_ES2_compatibility\n");
    ++on;
  }
  if (GLAD_GL_ARB_ES3_1_compatibility)
  {
    printf("  GLAD_GL_ARB_ES3_1_compatibility\n");
    ++on;
  }
  if (GLAD_GL_ARB_ES3_2_compatibility)
  {
    printf("  GLAD_GL_ARB_ES3_2_compatibility\n");
    ++on;
  }
  if (GLAD_GL_ARB_ES3_compatibility)
  {
    printf("  GLAD_GL_ARB_ES3_compatibility\n");
    ++on;
  }
  if (GLAD_GL_ARB_arrays_of_arrays)
  {
    printf("  GLAD_GL_ARB_arrays_of_arrays\n");
    ++on;
  }
  if (GLAD_GL_ARB_base_instance)
  {
    printf("  GLAD_GL_ARB_base_instance\n");
    ++on;
  }
  if (GLAD_GL_ARB_bindless_texture)
  {
    printf("  GLAD_GL_ARB_bindless_texture\n");
    ++on;
  }
  if (GLAD_GL_ARB_blend_func_extended)
  {
    printf("  GLAD_GL_ARB_blend_func_extended\n");
    ++on;
  }
  if (GLAD_GL_ARB_buffer_storage)
  {
    printf("  GLAD_GL_ARB_buffer_storage\n");
    ++on;
  }
  if (GLAD_GL_ARB_cl_event)
  {
    printf("  GLAD_GL_ARB_cl_event\n");
    ++on;
  }
  if (GLAD_GL_ARB_clear_buffer_object)
  {
    printf("  GLAD_GL_ARB_clear_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_ARB_clear_texture)
  {
    printf("  GLAD_GL_ARB_clear_texture\n");
    ++on;
  }
  if (GLAD_GL_ARB_clip_control)
  {
    printf("  GLAD_GL_ARB_clip_control\n");
    ++on;
  }
  if (GLAD_GL_ARB_color_buffer_float)
  {
    printf("  GLAD_GL_ARB_color_buffer_float\n");
    ++on;
  }
  if (GLAD_GL_ARB_compatibility)
  {
    printf("  GLAD_GL_ARB_compatibility\n");
    ++on;
  }
  if (GLAD_GL_ARB_compressed_texture_pixel_storage)
  {
    printf("  GLAD_GL_ARB_compressed_texture_pixel_storage\n");
    ++on;
  }
  if (GLAD_GL_ARB_compute_shader)
  {
    printf("  GLAD_GL_ARB_compute_shader\n");
    ++on;
  }
  if (GLAD_GL_ARB_compute_variable_group_size)
  {
    printf("  GLAD_GL_ARB_compute_variable_group_size\n");
    ++on;
  }
  if (GLAD_GL_ARB_conditional_render_inverted)
  {
    printf("  GLAD_GL_ARB_conditional_render_inverted\n");
    ++on;
  }
  if (GLAD_GL_ARB_conservative_depth)
  {
    printf("  GLAD_GL_ARB_conservative_depth\n");
    ++on;
  }
  if (GLAD_GL_ARB_copy_buffer)
  {
    printf("  GLAD_GL_ARB_copy_buffer\n");
    ++on;
  }
  if (GLAD_GL_ARB_copy_image)
  {
    printf("  GLAD_GL_ARB_copy_image\n");
    ++on;
  }
  if (GLAD_GL_ARB_cull_distance)
  {
    printf("  GLAD_GL_ARB_cull_distance\n");
    ++on;
  }
  if (GLAD_GL_ARB_debug_output)
  {
    printf("  GLAD_GL_ARB_debug_output\n");
    ++on;
  }
  if (GLAD_GL_ARB_depth_buffer_float)
  {
    printf("  GLAD_GL_ARB_depth_buffer_float\n");
    ++on;
  }
  if (GLAD_GL_ARB_depth_clamp)
  {
    printf("  GLAD_GL_ARB_depth_clamp\n");
    ++on;
  }
  if (GLAD_GL_ARB_depth_texture)
  {
    printf("  GLAD_GL_ARB_depth_texture\n");
    ++on;
  }
  if (GLAD_GL_ARB_derivative_control)
  {
    printf("  GLAD_GL_ARB_derivative_control\n");
    ++on;
  }
  if (GLAD_GL_ARB_direct_state_access)
  {
    printf("  GLAD_GL_ARB_direct_state_access\n");
    ++on;
  }
  if (GLAD_GL_ARB_draw_buffers)
  {
    printf("  GLAD_GL_ARB_draw_buffers\n");
    ++on;
  }
  if (GLAD_GL_ARB_draw_buffers_blend)
  {
    printf("  GLAD_GL_ARB_draw_buffers_blend\n");
    ++on;
  }
  if (GLAD_GL_ARB_draw_elements_base_vertex)
  {
    printf("  GLAD_GL_ARB_draw_elements_base_vertex\n");
    ++on;
  }
  if (GLAD_GL_ARB_draw_indirect)
  {
    printf("  GLAD_GL_ARB_draw_indirect\n");
    ++on;
  }
  if (GLAD_GL_ARB_draw_instanced)
  {
    printf("  GLAD_GL_ARB_draw_instanced\n");
    ++on;
  }
  if (GLAD_GL_ARB_enhanced_layouts)
  {
    printf("  GLAD_GL_ARB_enhanced_layouts\n");
    ++on;
  }
  if (GLAD_GL_ARB_explicit_attrib_location)
  {
    printf("  GLAD_GL_ARB_explicit_attrib_location\n");
    ++on;
  }
  if (GLAD_GL_ARB_explicit_uniform_location)
  {
    printf("  GLAD_GL_ARB_explicit_uniform_location\n");
    ++on;
  }
  if (GLAD_GL_ARB_fragment_coord_conventions)
  {
    printf("  GLAD_GL_ARB_fragment_coord_conventions\n");
    ++on;
  }
  if (GLAD_GL_ARB_fragment_layer_viewport)
  {
    printf("  GLAD_GL_ARB_fragment_layer_viewport\n");
    ++on;
  }
  if (GLAD_GL_ARB_fragment_program)
  {
    printf("  GLAD_GL_ARB_fragment_program\n");
    ++on;
  }
  if (GLAD_GL_ARB_fragment_program_shadow)
  {
    printf("  GLAD_GL_ARB_fragment_program_shadow\n");
    ++on;
  }
  if (GLAD_GL_ARB_fragment_shader)
  {
    printf("  GLAD_GL_ARB_fragment_shader\n");
    ++on;
  }
  if (GLAD_GL_ARB_fragment_shader_interlock)
  {
    printf("  GLAD_GL_ARB_fragment_shader_interlock\n");
    ++on;
  }
  if (GLAD_GL_ARB_framebuffer_no_attachments)
  {
    printf("  GLAD_GL_ARB_framebuffer_no_attachments\n");
    ++on;
  }
  if (GLAD_GL_ARB_framebuffer_object)
  {
    printf("  GLAD_GL_ARB_framebuffer_object\n");
    ++on;
  }
  if (GLAD_GL_ARB_framebuffer_sRGB)
  {
    printf("  GLAD_GL_ARB_framebuffer_sRGB\n");
    ++on;
  }
  if (GLAD_GL_ARB_geometry_shader4)
  {
    printf("  GLAD_GL_ARB_geometry_shader4\n");
    ++on;
  }
  if (GLAD_GL_ARB_get_program_binary)
  {
    printf("  GLAD_GL_ARB_get_program_binary\n");
    ++on;
  }
  if (GLAD_GL_ARB_get_texture_sub_image)
  {
    printf("  GLAD_GL_ARB_get_texture_sub_image\n");
    ++on;
  }
  if (GLAD_GL_ARB_gl_spirv)
  {
    printf("  GLAD_GL_ARB_gl_spirv\n");
    ++on;
  }
  if (GLAD_GL_ARB_gpu_shader5)
  {
    printf("  GLAD_GL_ARB_gpu_shader5\n");
    ++on;
  }
  if (GLAD_GL_ARB_gpu_shader_fp64)
  {
    printf("  GLAD_GL_ARB_gpu_shader_fp64\n");
    ++on;
  }
  if (GLAD_GL_ARB_gpu_shader_int64)
  {
    printf("  GLAD_GL_ARB_gpu_shader_int64\n");
    ++on;
  }
  if (GLAD_GL_ARB_half_float_pixel)
  {
    printf("  GLAD_GL_ARB_half_float_pixel\n");
    ++on;
  }
  if (GLAD_GL_ARB_half_float_vertex)
  {
    printf("  GLAD_GL_ARB_half_float_vertex\n");
    ++on;
  }
  if (GLAD_GL_ARB_imaging)
  {
    printf("  GLAD_GL_ARB_imaging\n");
    ++on;
  }
  if (GLAD_GL_ARB_indirect_parameters)
  {
    printf("  GLAD_GL_ARB_indirect_parameters\n");
    ++on;
  }
  if (GLAD_GL_ARB_instanced_arrays)
  {
    printf("  GLAD_GL_ARB_instanced_arrays\n");
    ++on;
  }
  if (GLAD_GL_ARB_internalformat_query)
  {
    printf("  GLAD_GL_ARB_internalformat_query\n");
    ++on;
  }
  if (GLAD_GL_ARB_internalformat_query2)
  {
    printf("  GLAD_GL_ARB_internalformat_query2\n");
    ++on;
  }
  if (GLAD_GL_ARB_invalidate_subdata)
  {
    printf("  GLAD_GL_ARB_invalidate_subdata\n");
    ++on;
  }
  if (GLAD_GL_ARB_map_buffer_alignment)
  {
    printf("  GLAD_GL_ARB_map_buffer_alignment\n");
    ++on;
  }
  if (GLAD_GL_ARB_map_buffer_range)
  {
    printf("  GLAD_GL_ARB_map_buffer_range\n");
    ++on;
  }
  if (GLAD_GL_ARB_matrix_palette)
  {
    printf("  GLAD_GL_ARB_matrix_palette\n");
    ++on;
  }
  if (GLAD_GL_ARB_multi_bind)
  {
    printf("  GLAD_GL_ARB_multi_bind\n");
    ++on;
  }
  if (GLAD_GL_ARB_multi_draw_indirect)
  {
    printf("  GLAD_GL_ARB_multi_draw_indirect\n");
    ++on;
  }
  if (GLAD_GL_ARB_multisample)
  {
    printf("  GLAD_GL_ARB_multisample\n");
    ++on;
  }
  if (GLAD_GL_ARB_multitexture)
  {
    printf("  GLAD_GL_ARB_multitexture\n");
    ++on;
  }
  if (GLAD_GL_ARB_occlusion_query)
  {
    printf("  GLAD_GL_ARB_occlusion_query\n");
    ++on;
  }
  if (GLAD_GL_ARB_occlusion_query2)
  {
    printf("  GLAD_GL_ARB_occlusion_query2\n");
    ++on;
  }
  if (GLAD_GL_ARB_parallel_shader_compile)
  {
    printf("  GLAD_GL_ARB_parallel_shader_compile\n");
    ++on;
  }
  if (GLAD_GL_ARB_pipeline_statistics_query)
  {
    printf("  GLAD_GL_ARB_pipeline_statistics_query\n");
    ++on;
  }
  if (GLAD_GL_ARB_pixel_buffer_object)
  {
    printf("  GLAD_GL_ARB_pixel_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_ARB_point_parameters)
  {
    printf("  GLAD_GL_ARB_point_parameters\n");
    ++on;
  }
  if (GLAD_GL_ARB_point_sprite)
  {
    printf("  GLAD_GL_ARB_point_sprite\n");
    ++on;
  }
  if (GLAD_GL_ARB_polygon_offset_clamp)
  {
    printf("  GLAD_GL_ARB_polygon_offset_clamp\n");
    ++on;
  }
  if (GLAD_GL_ARB_post_depth_coverage)
  {
    printf("  GLAD_GL_ARB_post_depth_coverage\n");
    ++on;
  }
  if (GLAD_GL_ARB_program_interface_query)
  {
    printf("  GLAD_GL_ARB_program_interface_query\n");
    ++on;
  }
  if (GLAD_GL_ARB_provoking_vertex)
  {
    printf("  GLAD_GL_ARB_provoking_vertex\n");
    ++on;
  }
  if (GLAD_GL_ARB_query_buffer_object)
  {
    printf("  GLAD_GL_ARB_query_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_ARB_robust_buffer_access_behavior)
  {
    printf("  GLAD_GL_ARB_robust_buffer_access_behavior\n");
    ++on;
  }
  if (GLAD_GL_ARB_robustness)
  {
    printf("  GLAD_GL_ARB_robustness\n");
    ++on;
  }
  if (GLAD_GL_ARB_robustness_isolation)
  {
    printf("  GLAD_GL_ARB_robustness_isolation\n");
    ++on;
  }
  if (GLAD_GL_ARB_sample_locations)
  {
    printf("  GLAD_GL_ARB_sample_locations\n");
    ++on;
  }
  if (GLAD_GL_ARB_sample_shading)
  {
    printf("  GLAD_GL_ARB_sample_shading\n");
    ++on;
  }
  if (GLAD_GL_ARB_sampler_objects)
  {
    printf("  GLAD_GL_ARB_sampler_objects\n");
    ++on;
  }
  if (GLAD_GL_ARB_seamless_cube_map)
  {
    printf("  GLAD_GL_ARB_seamless_cube_map\n");
    ++on;
  }
  if (GLAD_GL_ARB_seamless_cubemap_per_texture)
  {
    printf("  GLAD_GL_ARB_seamless_cubemap_per_texture\n");
    ++on;
  }
  if (GLAD_GL_ARB_separate_shader_objects)
  {
    printf("  GLAD_GL_ARB_separate_shader_objects\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_atomic_counter_ops)
  {
    printf("  GLAD_GL_ARB_shader_atomic_counter_ops\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_atomic_counters)
  {
    printf("  GLAD_GL_ARB_shader_atomic_counters\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_ballot)
  {
    printf("  GLAD_GL_ARB_shader_ballot\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_bit_encoding)
  {
    printf("  GLAD_GL_ARB_shader_bit_encoding\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_clock)
  {
    printf("  GLAD_GL_ARB_shader_clock\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_draw_parameters)
  {
    printf("  GLAD_GL_ARB_shader_draw_parameters\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_group_vote)
  {
    printf("  GLAD_GL_ARB_shader_group_vote\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_image_load_store)
  {
    printf("  GLAD_GL_ARB_shader_image_load_store\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_image_size)
  {
    printf("  GLAD_GL_ARB_shader_image_size\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_objects)
  {
    printf("  GLAD_GL_ARB_shader_objects\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_precision)
  {
    printf("  GLAD_GL_ARB_shader_precision\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_stencil_export)
  {
    printf("  GLAD_GL_ARB_shader_stencil_export\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_storage_buffer_object)
  {
    printf("  GLAD_GL_ARB_shader_storage_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_subroutine)
  {
    printf("  GLAD_GL_ARB_shader_subroutine\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_texture_image_samples)
  {
    printf("  GLAD_GL_ARB_shader_texture_image_samples\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_texture_lod)
  {
    printf("  GLAD_GL_ARB_shader_texture_lod\n");
    ++on;
  }
  if (GLAD_GL_ARB_shader_viewport_layer_array)
  {
    printf("  GLAD_GL_ARB_shader_viewport_layer_array\n");
    ++on;
  }
  if (GLAD_GL_ARB_shading_language_100)
  {
    printf("  GLAD_GL_ARB_shading_language_100\n");
    ++on;
  }
  if (GLAD_GL_ARB_shading_language_420pack)
  {
    printf("  GLAD_GL_ARB_shading_language_420pack\n");
    ++on;
  }
  if (GLAD_GL_ARB_shading_language_include)
  {
    printf("  GLAD_GL_ARB_shading_language_include\n");
    ++on;
  }
  if (GLAD_GL_ARB_shading_language_packing)
  {
    printf("  GLAD_GL_ARB_shading_language_packing\n");
    ++on;
  }
  if (GLAD_GL_ARB_shadow)
  {
    printf("  GLAD_GL_ARB_shadow\n");
    ++on;
  }
  if (GLAD_GL_ARB_shadow_ambient)
  {
    printf("  GLAD_GL_ARB_shadow_ambient\n");
    ++on;
  }
  if (GLAD_GL_ARB_sparse_buffer)
  {
    printf("  GLAD_GL_ARB_sparse_buffer\n");
    ++on;
  }
  if (GLAD_GL_ARB_sparse_texture)
  {
    printf("  GLAD_GL_ARB_sparse_texture\n");
    ++on;
  }
  if (GLAD_GL_ARB_sparse_texture2)
  {
    printf("  GLAD_GL_ARB_sparse_texture2\n");
    ++on;
  }
  if (GLAD_GL_ARB_sparse_texture_clamp)
  {
    printf("  GLAD_GL_ARB_sparse_texture_clamp\n");
    ++on;
  }
  if (GLAD_GL_ARB_spirv_extensions)
  {
    printf("  GLAD_GL_ARB_spirv_extensions\n");
    ++on;
  }
  if (GLAD_GL_ARB_stencil_texturing)
  {
    printf("  GLAD_GL_ARB_stencil_texturing\n");
    ++on;
  }
  if (GLAD_GL_ARB_sync)
  {
    printf("  GLAD_GL_ARB_sync\n");
    ++on;
  }
  if (GLAD_GL_ARB_tessellation_shader)
  {
    printf("  GLAD_GL_ARB_tessellation_shader\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_barrier)
  {
    printf("  GLAD_GL_ARB_texture_barrier\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_border_clamp)
  {
    printf("  GLAD_GL_ARB_texture_border_clamp\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_buffer_object)
  {
    printf("  GLAD_GL_ARB_texture_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_buffer_object_rgb32)
  {
    printf("  GLAD_GL_ARB_texture_buffer_object_rgb32\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_buffer_range)
  {
    printf("  GLAD_GL_ARB_texture_buffer_range\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_compression)
  {
    printf("  GLAD_GL_ARB_texture_compression\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_compression_bptc)
  {
    printf("  GLAD_GL_ARB_texture_compression_bptc\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_compression_rgtc)
  {
    printf("  GLAD_GL_ARB_texture_compression_rgtc\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_cube_map)
  {
    printf("  GLAD_GL_ARB_texture_cube_map\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_cube_map_array)
  {
    printf("  GLAD_GL_ARB_texture_cube_map_array\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_env_add)
  {
    printf("  GLAD_GL_ARB_texture_env_add\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_env_combine)
  {
    printf("  GLAD_GL_ARB_texture_env_combine\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_env_crossbar)
  {
    printf("  GLAD_GL_ARB_texture_env_crossbar\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_env_dot3)
  {
    printf("  GLAD_GL_ARB_texture_env_dot3\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_filter_anisotropic)
  {
    printf("  GLAD_GL_ARB_texture_filter_anisotropic\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_filter_minmax)
  {
    printf("  GLAD_GL_ARB_texture_filter_minmax\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_float)
  {
    printf("  GLAD_GL_ARB_texture_float\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_gather)
  {
    printf("  GLAD_GL_ARB_texture_gather\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_mirror_clamp_to_edge)
  {
    printf("  GLAD_GL_ARB_texture_mirror_clamp_to_edge\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_mirrored_repeat)
  {
    printf("  GLAD_GL_ARB_texture_mirrored_repeat\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_multisample)
  {
    printf("  GLAD_GL_ARB_texture_multisample\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_non_power_of_two)
  {
    printf("  GLAD_GL_ARB_texture_non_power_of_two\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_query_levels)
  {
    printf("  GLAD_GL_ARB_texture_query_levels\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_query_lod)
  {
    printf("  GLAD_GL_ARB_texture_query_lod\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_rectangle)
  {
    printf("  GLAD_GL_ARB_texture_rectangle\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_rg)
  {
    printf("  GLAD_GL_ARB_texture_rg\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_rgb10_a2ui)
  {
    printf("  GLAD_GL_ARB_texture_rgb10_a2ui\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_stencil8)
  {
    printf("  GLAD_GL_ARB_texture_stencil8\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_storage)
  {
    printf("  GLAD_GL_ARB_texture_storage\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_storage_multisample)
  {
    printf("  GLAD_GL_ARB_texture_storage_multisample\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_swizzle)
  {
    printf("  GLAD_GL_ARB_texture_swizzle\n");
    ++on;
  }
  if (GLAD_GL_ARB_texture_view)
  {
    printf("  GLAD_GL_ARB_texture_view\n");
    ++on;
  }
  if (GLAD_GL_ARB_timer_query)
  {
    printf("  GLAD_GL_ARB_timer_query\n");
    ++on;
  }
  if (GLAD_GL_ARB_transform_feedback2)
  {
    printf("  GLAD_GL_ARB_transform_feedback2\n");
    ++on;
  }
  if (GLAD_GL_ARB_transform_feedback3)
  {
    printf("  GLAD_GL_ARB_transform_feedback3\n");
    ++on;
  }
  if (GLAD_GL_ARB_transform_feedback_instanced)
  {
    printf("  GLAD_GL_ARB_transform_feedback_instanced\n");
    ++on;
  }
  if (GLAD_GL_ARB_transform_feedback_overflow_query)
  {
    printf("  GLAD_GL_ARB_transform_feedback_overflow_query\n");
    ++on;
  }
  if (GLAD_GL_ARB_transpose_matrix)
  {
    printf("  GLAD_GL_ARB_transpose_matrix\n");
    ++on;
  }
  if (GLAD_GL_ARB_uniform_buffer_object)
  {
    printf("  GLAD_GL_ARB_uniform_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_ARB_vertex_array_bgra)
  {
    printf("  GLAD_GL_ARB_vertex_array_bgra\n");
    ++on;
  }
  if (GLAD_GL_ARB_vertex_array_object)
  {
    printf("  GLAD_GL_ARB_vertex_array_object\n");
    ++on;
  }
  if (GLAD_GL_ARB_vertex_attrib_64bit)
  {
    printf("  GLAD_GL_ARB_vertex_attrib_64bit\n");
    ++on;
  }
  if (GLAD_GL_ARB_vertex_attrib_binding)
  {
    printf("  GLAD_GL_ARB_vertex_attrib_binding\n");
    ++on;
  }
  if (GLAD_GL_ARB_vertex_blend)
  {
    printf("  GLAD_GL_ARB_vertex_blend\n");
    ++on;
  }
  if (GLAD_GL_ARB_vertex_buffer_object)
  {
    printf("  GLAD_GL_ARB_vertex_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_ARB_vertex_program)
  {
    printf("  GLAD_GL_ARB_vertex_program\n");
    ++on;
  }
  if (GLAD_GL_ARB_vertex_shader)
  {
    printf("  GLAD_GL_ARB_vertex_shader\n");
    ++on;
  }
  if (GLAD_GL_ARB_vertex_type_10f_11f_11f_rev)
  {
    printf("  GLAD_GL_ARB_vertex_type_10f_11f_11f_rev\n");
    ++on;
  }
  if (GLAD_GL_ARB_vertex_type_2_10_10_10_rev)
  {
    printf("  GLAD_GL_ARB_vertex_type_2_10_10_10_rev\n");
    ++on;
  }
  if (GLAD_GL_ARB_viewport_array)
  {
    printf("  GLAD_GL_ARB_viewport_array\n");
    ++on;
  }
  if (GLAD_GL_ARB_window_pos)
  {
    printf("  GLAD_GL_ARB_window_pos\n");
    ++on;
  }
  if (GLAD_GL_ATI_draw_buffers)
  {
    printf("  GLAD_GL_ATI_draw_buffers\n");
    ++on;
  }
  if (GLAD_GL_ATI_element_array)
  {
    printf("  GLAD_GL_ATI_element_array\n");
    ++on;
  }
  if (GLAD_GL_ATI_envmap_bumpmap)
  {
    printf("  GLAD_GL_ATI_envmap_bumpmap\n");
    ++on;
  }
  if (GLAD_GL_ATI_fragment_shader)
  {
    printf("  GLAD_GL_ATI_fragment_shader\n");
    ++on;
  }
  if (GLAD_GL_ATI_map_object_buffer)
  {
    printf("  GLAD_GL_ATI_map_object_buffer\n");
    ++on;
  }
  if (GLAD_GL_ATI_meminfo)
  {
    printf("  GLAD_GL_ATI_meminfo\n");
    ++on;
  }
  if (GLAD_GL_ATI_pixel_format_float)
  {
    printf("  GLAD_GL_ATI_pixel_format_float\n");
    ++on;
  }
  if (GLAD_GL_ATI_pn_triangles)
  {
    printf("  GLAD_GL_ATI_pn_triangles\n");
    ++on;
  }
  if (GLAD_GL_ATI_separate_stencil)
  {
    printf("  GLAD_GL_ATI_separate_stencil\n");
    ++on;
  }
  if (GLAD_GL_ATI_text_fragment_shader)
  {
    printf("  GLAD_GL_ATI_text_fragment_shader\n");
    ++on;
  }
  if (GLAD_GL_ATI_texture_env_combine3)
  {
    printf("  GLAD_GL_ATI_texture_env_combine3\n");
    ++on;
  }
  if (GLAD_GL_ATI_texture_float)
  {
    printf("  GLAD_GL_ATI_texture_float\n");
    ++on;
  }
  if (GLAD_GL_ATI_texture_mirror_once)
  {
    printf("  GLAD_GL_ATI_texture_mirror_once\n");
    ++on;
  }
  if (GLAD_GL_ATI_vertex_array_object)
  {
    printf("  GLAD_GL_ATI_vertex_array_object\n");
    ++on;
  }
  if (GLAD_GL_ATI_vertex_attrib_array_object)
  {
    printf("  GLAD_GL_ATI_vertex_attrib_array_object\n");
    ++on;
  }
  if (GLAD_GL_ATI_vertex_streams)
  {
    printf("  GLAD_GL_ATI_vertex_streams\n");
    ++on;
  }
  if (GLAD_GL_EXT_422_pixels)
  {
    printf("  GLAD_GL_EXT_422_pixels\n");
    ++on;
  }
  if (GLAD_GL_EXT_EGL_image_storage)
  {
    printf("  GLAD_GL_EXT_EGL_image_storage\n");
    ++on;
  }
  if (GLAD_GL_EXT_abgr)
  {
    printf("  GLAD_GL_EXT_abgr\n");
    ++on;
  }
  if (GLAD_GL_EXT_bgra)
  {
    printf("  GLAD_GL_EXT_bgra\n");
    ++on;
  }
  if (GLAD_GL_EXT_bindable_uniform)
  {
    printf("  GLAD_GL_EXT_bindable_uniform\n");
    ++on;
  }
  if (GLAD_GL_EXT_blend_color)
  {
    printf("  GLAD_GL_EXT_blend_color\n");
    ++on;
  }
  if (GLAD_GL_EXT_blend_equation_separate)
  {
    printf("  GLAD_GL_EXT_blend_equation_separate\n");
    ++on;
  }
  if (GLAD_GL_EXT_blend_func_separate)
  {
    printf("  GLAD_GL_EXT_blend_func_separate\n");
    ++on;
  }
  if (GLAD_GL_EXT_blend_logic_op)
  {
    printf("  GLAD_GL_EXT_blend_logic_op\n");
    ++on;
  }
  if (GLAD_GL_EXT_blend_minmax)
  {
    printf("  GLAD_GL_EXT_blend_minmax\n");
    ++on;
  }
  if (GLAD_GL_EXT_blend_subtract)
  {
    printf("  GLAD_GL_EXT_blend_subtract\n");
    ++on;
  }
  if (GLAD_GL_EXT_clip_volume_hint)
  {
    printf("  GLAD_GL_EXT_clip_volume_hint\n");
    ++on;
  }
  if (GLAD_GL_EXT_cmyka)
  {
    printf("  GLAD_GL_EXT_cmyka\n");
    ++on;
  }
  if (GLAD_GL_EXT_color_subtable)
  {
    printf("  GLAD_GL_EXT_color_subtable\n");
    ++on;
  }
  if (GLAD_GL_EXT_compiled_vertex_array)
  {
    printf("  GLAD_GL_EXT_compiled_vertex_array\n");
    ++on;
  }
  if (GLAD_GL_EXT_convolution)
  {
    printf("  GLAD_GL_EXT_convolution\n");
    ++on;
  }
  if (GLAD_GL_EXT_coordinate_frame)
  {
    printf("  GLAD_GL_EXT_coordinate_frame\n");
    ++on;
  }
  if (GLAD_GL_EXT_copy_texture)
  {
    printf("  GLAD_GL_EXT_copy_texture\n");
    ++on;
  }
  if (GLAD_GL_EXT_cull_vertex)
  {
    printf("  GLAD_GL_EXT_cull_vertex\n");
    ++on;
  }
  if (GLAD_GL_EXT_debug_label)
  {
    printf("  GLAD_GL_EXT_debug_label\n");
    ++on;
  }
  if (GLAD_GL_EXT_debug_marker)
  {
    printf("  GLAD_GL_EXT_debug_marker\n");
    ++on;
  }
  if (GLAD_GL_EXT_depth_bounds_test)
  {
    printf("  GLAD_GL_EXT_depth_bounds_test\n");
    ++on;
  }
  if (GLAD_GL_EXT_direct_state_access)
  {
    printf("  GLAD_GL_EXT_direct_state_access\n");
    ++on;
  }
  if (GLAD_GL_EXT_draw_buffers2)
  {
    printf("  GLAD_GL_EXT_draw_buffers2\n");
    ++on;
  }
  if (GLAD_GL_EXT_draw_instanced)
  {
    printf("  GLAD_GL_EXT_draw_instanced\n");
    ++on;
  }
  if (GLAD_GL_EXT_draw_range_elements)
  {
    printf("  GLAD_GL_EXT_draw_range_elements\n");
    ++on;
  }
  if (GLAD_GL_EXT_external_buffer)
  {
    printf("  GLAD_GL_EXT_external_buffer\n");
    ++on;
  }
  if (GLAD_GL_EXT_fog_coord)
  {
    printf("  GLAD_GL_EXT_fog_coord\n");
    ++on;
  }
  if (GLAD_GL_EXT_framebuffer_blit)
  {
    printf("  GLAD_GL_EXT_framebuffer_blit\n");
    ++on;
  }
  if (GLAD_GL_EXT_framebuffer_multisample)
  {
    printf("  GLAD_GL_EXT_framebuffer_multisample\n");
    ++on;
  }
  if (GLAD_GL_EXT_framebuffer_multisample_blit_scaled)
  {
    printf("  GLAD_GL_EXT_framebuffer_multisample_blit_scaled\n");
    ++on;
  }
  if (GLAD_GL_EXT_framebuffer_object)
  {
    printf("  GLAD_GL_EXT_framebuffer_object\n");
    ++on;
  }
  if (GLAD_GL_EXT_framebuffer_sRGB)
  {
    printf("  GLAD_GL_EXT_framebuffer_sRGB\n");
    ++on;
  }
  if (GLAD_GL_EXT_geometry_shader4)
  {
    printf("  GLAD_GL_EXT_geometry_shader4\n");
    ++on;
  }
  if (GLAD_GL_EXT_gpu_program_parameters)
  {
    printf("  GLAD_GL_EXT_gpu_program_parameters\n");
    ++on;
  }
  if (GLAD_GL_EXT_gpu_shader4)
  {
    printf("  GLAD_GL_EXT_gpu_shader4\n");
    ++on;
  }
  if (GLAD_GL_EXT_histogram)
  {
    printf("  GLAD_GL_EXT_histogram\n");
    ++on;
  }
  if (GLAD_GL_EXT_index_array_formats)
  {
    printf("  GLAD_GL_EXT_index_array_formats\n");
    ++on;
  }
  if (GLAD_GL_EXT_index_func)
  {
    printf("  GLAD_GL_EXT_index_func\n");
    ++on;
  }
  if (GLAD_GL_EXT_index_material)
  {
    printf("  GLAD_GL_EXT_index_material\n");
    ++on;
  }
  if (GLAD_GL_EXT_index_texture)
  {
    printf("  GLAD_GL_EXT_index_texture\n");
    ++on;
  }
  if (GLAD_GL_EXT_light_texture)
  {
    printf("  GLAD_GL_EXT_light_texture\n");
    ++on;
  }
  if (GLAD_GL_EXT_memory_object)
  {
    printf("  GLAD_GL_EXT_memory_object\n");
    ++on;
  }
  if (GLAD_GL_EXT_memory_object_fd)
  {
    printf("  GLAD_GL_EXT_memory_object_fd\n");
    ++on;
  }
  if (GLAD_GL_EXT_memory_object_win32)
  {
    printf("  GLAD_GL_EXT_memory_object_win32\n");
    ++on;
  }
  if (GLAD_GL_EXT_misc_attribute)
  {
    printf("  GLAD_GL_EXT_misc_attribute\n");
    ++on;
  }
  if (GLAD_GL_EXT_multi_draw_arrays)
  {
    printf("  GLAD_GL_EXT_multi_draw_arrays\n");
    ++on;
  }
  if (GLAD_GL_EXT_multisample)
  {
    printf("  GLAD_GL_EXT_multisample\n");
    ++on;
  }
  if (GLAD_GL_EXT_packed_depth_stencil)
  {
    printf("  GLAD_GL_EXT_packed_depth_stencil\n");
    ++on;
  }
  if (GLAD_GL_EXT_packed_float)
  {
    printf("  GLAD_GL_EXT_packed_float\n");
    ++on;
  }
  if (GLAD_GL_EXT_packed_pixels)
  {
    printf("  GLAD_GL_EXT_packed_pixels\n");
    ++on;
  }
  if (GLAD_GL_EXT_paletted_texture)
  {
    printf("  GLAD_GL_EXT_paletted_texture\n");
    ++on;
  }
  if (GLAD_GL_EXT_pixel_buffer_object)
  {
    printf("  GLAD_GL_EXT_pixel_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_EXT_pixel_transform)
  {
    printf("  GLAD_GL_EXT_pixel_transform\n");
    ++on;
  }
  if (GLAD_GL_EXT_pixel_transform_color_table)
  {
    printf("  GLAD_GL_EXT_pixel_transform_color_table\n");
    ++on;
  }
  if (GLAD_GL_EXT_point_parameters)
  {
    printf("  GLAD_GL_EXT_point_parameters\n");
    ++on;
  }
  if (GLAD_GL_EXT_polygon_offset)
  {
    printf("  GLAD_GL_EXT_polygon_offset\n");
    ++on;
  }
  if (GLAD_GL_EXT_polygon_offset_clamp)
  {
    printf("  GLAD_GL_EXT_polygon_offset_clamp\n");
    ++on;
  }
  if (GLAD_GL_EXT_post_depth_coverage)
  {
    printf("  GLAD_GL_EXT_post_depth_coverage\n");
    ++on;
  }
  if (GLAD_GL_EXT_provoking_vertex)
  {
    printf("  GLAD_GL_EXT_provoking_vertex\n");
    ++on;
  }
  if (GLAD_GL_EXT_raster_multisample)
  {
    printf("  GLAD_GL_EXT_raster_multisample\n");
    ++on;
  }
  if (GLAD_GL_EXT_rescale_normal)
  {
    printf("  GLAD_GL_EXT_rescale_normal\n");
    ++on;
  }
  if (GLAD_GL_EXT_secondary_color)
  {
    printf("  GLAD_GL_EXT_secondary_color\n");
    ++on;
  }
  if (GLAD_GL_EXT_semaphore)
  {
    printf("  GLAD_GL_EXT_semaphore\n");
    ++on;
  }
  if (GLAD_GL_EXT_semaphore_fd)
  {
    printf("  GLAD_GL_EXT_semaphore_fd\n");
    ++on;
  }
  if (GLAD_GL_EXT_semaphore_win32)
  {
    printf("  GLAD_GL_EXT_semaphore_win32\n");
    ++on;
  }
  if (GLAD_GL_EXT_separate_shader_objects)
  {
    printf("  GLAD_GL_EXT_separate_shader_objects\n");
    ++on;
  }
  if (GLAD_GL_EXT_separate_specular_color)
  {
    printf("  GLAD_GL_EXT_separate_specular_color\n");
    ++on;
  }
  if (GLAD_GL_EXT_shader_framebuffer_fetch)
  {
    printf("  GLAD_GL_EXT_shader_framebuffer_fetch\n");
    ++on;
  }
  if (GLAD_GL_EXT_shader_framebuffer_fetch_non_coherent)
  {
    printf("  GLAD_GL_EXT_shader_framebuffer_fetch_non_coherent\n");
    ++on;
  }
  if (GLAD_GL_EXT_shader_image_load_formatted)
  {
    printf("  GLAD_GL_EXT_shader_image_load_formatted\n");
    ++on;
  }
  if (GLAD_GL_EXT_shader_image_load_store)
  {
    printf("  GLAD_GL_EXT_shader_image_load_store\n");
    ++on;
  }
  if (GLAD_GL_EXT_shader_integer_mix)
  {
    printf("  GLAD_GL_EXT_shader_integer_mix\n");
    ++on;
  }
  if (GLAD_GL_EXT_shadow_funcs)
  {
    printf("  GLAD_GL_EXT_shadow_funcs\n");
    ++on;
  }
  if (GLAD_GL_EXT_shared_texture_palette)
  {
    printf("  GLAD_GL_EXT_shared_texture_palette\n");
    ++on;
  }
  if (GLAD_GL_EXT_sparse_texture2)
  {
    printf("  GLAD_GL_EXT_sparse_texture2\n");
    ++on;
  }
  if (GLAD_GL_EXT_stencil_clear_tag)
  {
    printf("  GLAD_GL_EXT_stencil_clear_tag\n");
    ++on;
  }
  if (GLAD_GL_EXT_stencil_two_side)
  {
    printf("  GLAD_GL_EXT_stencil_two_side\n");
    ++on;
  }
  if (GLAD_GL_EXT_stencil_wrap)
  {
    printf("  GLAD_GL_EXT_stencil_wrap\n");
    ++on;
  }
  if (GLAD_GL_EXT_subtexture)
  {
    printf("  GLAD_GL_EXT_subtexture\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture)
  {
    printf("  GLAD_GL_EXT_texture\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture3D)
  {
    printf("  GLAD_GL_EXT_texture3D\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_array)
  {
    printf("  GLAD_GL_EXT_texture_array\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_buffer_object)
  {
    printf("  GLAD_GL_EXT_texture_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_compression_latc)
  {
    printf("  GLAD_GL_EXT_texture_compression_latc\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_compression_rgtc)
  {
    printf("  GLAD_GL_EXT_texture_compression_rgtc\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_compression_s3tc)
  {
    printf("  GLAD_GL_EXT_texture_compression_s3tc\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_cube_map)
  {
    printf("  GLAD_GL_EXT_texture_cube_map\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_env_add)
  {
    printf("  GLAD_GL_EXT_texture_env_add\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_env_combine)
  {
    printf("  GLAD_GL_EXT_texture_env_combine\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_env_dot3)
  {
    printf("  GLAD_GL_EXT_texture_env_dot3\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_filter_anisotropic)
  {
    printf("  GLAD_GL_EXT_texture_filter_anisotropic\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_filter_minmax)
  {
    printf("  GLAD_GL_EXT_texture_filter_minmax\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_integer)
  {
    printf("  GLAD_GL_EXT_texture_integer\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_lod_bias)
  {
    printf("  GLAD_GL_EXT_texture_lod_bias\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_mirror_clamp)
  {
    printf("  GLAD_GL_EXT_texture_mirror_clamp\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_object)
  {
    printf("  GLAD_GL_EXT_texture_object\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_perturb_normal)
  {
    printf("  GLAD_GL_EXT_texture_perturb_normal\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_sRGB)
  {
    printf("  GLAD_GL_EXT_texture_sRGB\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_sRGB_decode)
  {
    printf("  GLAD_GL_EXT_texture_sRGB_decode\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_shared_exponent)
  {
    printf("  GLAD_GL_EXT_texture_shared_exponent\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_snorm)
  {
    printf("  GLAD_GL_EXT_texture_snorm\n");
    ++on;
  }
  if (GLAD_GL_EXT_texture_swizzle)
  {
    printf("  GLAD_GL_EXT_texture_swizzle\n");
    ++on;
  }
  if (GLAD_GL_EXT_timer_query)
  {
    printf("  GLAD_GL_EXT_timer_query\n");
    ++on;
  }
  if (GLAD_GL_EXT_transform_feedback)
  {
    printf("  GLAD_GL_EXT_transform_feedback\n");
    ++on;
  }
  if (GLAD_GL_EXT_vertex_array)
  {
    printf("  GLAD_GL_EXT_vertex_array\n");
    ++on;
  }
  if (GLAD_GL_EXT_vertex_array_bgra)
  {
    printf("  GLAD_GL_EXT_vertex_array_bgra\n");
    ++on;
  }
  if (GLAD_GL_EXT_vertex_attrib_64bit)
  {
    printf("  GLAD_GL_EXT_vertex_attrib_64bit\n");
    ++on;
  }
  if (GLAD_GL_EXT_vertex_shader)
  {
    printf("  GLAD_GL_EXT_vertex_shader\n");
    ++on;
  }
  if (GLAD_GL_EXT_vertex_weighting)
  {
    printf("  GLAD_GL_EXT_vertex_weighting\n");
    ++on;
  }
  if (GLAD_GL_EXT_win32_keyed_mutex)
  {
    printf("  GLAD_GL_EXT_win32_keyed_mutex\n");
    ++on;
  }
  if (GLAD_GL_EXT_window_rectangles)
  {
    printf("  GLAD_GL_EXT_window_rectangles\n");
    ++on;
  }
  if (GLAD_GL_EXT_x11_sync_object)
  {
    printf("  GLAD_GL_EXT_x11_sync_object\n");
    ++on;
  }
  if (GLAD_GL_GREMEDY_frame_terminator)
  {
    printf("  GLAD_GL_GREMEDY_frame_terminator\n");
    ++on;
  }
  if (GLAD_GL_GREMEDY_string_marker)
  {
    printf("  GLAD_GL_GREMEDY_string_marker\n");
    ++on;
  }
  if (GLAD_GL_HP_convolution_border_modes)
  {
    printf("  GLAD_GL_HP_convolution_border_modes\n");
    ++on;
  }
  if (GLAD_GL_HP_image_transform)
  {
    printf("  GLAD_GL_HP_image_transform\n");
    ++on;
  }
  if (GLAD_GL_HP_occlusion_test)
  {
    printf("  GLAD_GL_HP_occlusion_test\n");
    ++on;
  }
  if (GLAD_GL_HP_texture_lighting)
  {
    printf("  GLAD_GL_HP_texture_lighting\n");
    ++on;
  }
  if (GLAD_GL_IBM_cull_vertex)
  {
    printf("  GLAD_GL_IBM_cull_vertex\n");
    ++on;
  }
  if (GLAD_GL_IBM_multimode_draw_arrays)
  {
    printf("  GLAD_GL_IBM_multimode_draw_arrays\n");
    ++on;
  }
  if (GLAD_GL_IBM_rasterpos_clip)
  {
    printf("  GLAD_GL_IBM_rasterpos_clip\n");
    ++on;
  }
  if (GLAD_GL_IBM_static_data)
  {
    printf("  GLAD_GL_IBM_static_data\n");
    ++on;
  }
  if (GLAD_GL_IBM_texture_mirrored_repeat)
  {
    printf("  GLAD_GL_IBM_texture_mirrored_repeat\n");
    ++on;
  }
  if (GLAD_GL_IBM_vertex_array_lists)
  {
    printf("  GLAD_GL_IBM_vertex_array_lists\n");
    ++on;
  }
  if (GLAD_GL_INGR_blend_func_separate)
  {
    printf("  GLAD_GL_INGR_blend_func_separate\n");
    ++on;
  }
  if (GLAD_GL_INGR_color_clamp)
  {
    printf("  GLAD_GL_INGR_color_clamp\n");
    ++on;
  }
  if (GLAD_GL_INGR_interlace_read)
  {
    printf("  GLAD_GL_INGR_interlace_read\n");
    ++on;
  }
  if (GLAD_GL_INTEL_blackhole_render)
  {
    printf("  GLAD_GL_INTEL_blackhole_render\n");
    ++on;
  }
  if (GLAD_GL_INTEL_conservative_rasterization)
  {
    printf("  GLAD_GL_INTEL_conservative_rasterization\n");
    ++on;
  }
  if (GLAD_GL_INTEL_fragment_shader_ordering)
  {
    printf("  GLAD_GL_INTEL_fragment_shader_ordering\n");
    ++on;
  }
  if (GLAD_GL_INTEL_framebuffer_CMAA)
  {
    printf("  GLAD_GL_INTEL_framebuffer_CMAA\n");
    ++on;
  }
  if (GLAD_GL_INTEL_map_texture)
  {
    printf("  GLAD_GL_INTEL_map_texture\n");
    ++on;
  }
  if (GLAD_GL_INTEL_parallel_arrays)
  {
    printf("  GLAD_GL_INTEL_parallel_arrays\n");
    ++on;
  }
  if (GLAD_GL_INTEL_performance_query)
  {
    printf("  GLAD_GL_INTEL_performance_query\n");
    ++on;
  }
  if (GLAD_GL_KHR_blend_equation_advanced)
  {
    printf("  GLAD_GL_KHR_blend_equation_advanced\n");
    ++on;
  }
  if (GLAD_GL_KHR_blend_equation_advanced_coherent)
  {
    printf("  GLAD_GL_KHR_blend_equation_advanced_coherent\n");
    ++on;
  }
  if (GLAD_GL_KHR_context_flush_control)
  {
    printf("  GLAD_GL_KHR_context_flush_control\n");
    ++on;
  }
  if (GLAD_GL_KHR_debug)
  {
    printf("  GLAD_GL_KHR_debug\n");
    ++on;
  }
  if (GLAD_GL_KHR_no_error)
  {
    printf("  GLAD_GL_KHR_no_error\n");
    ++on;
  }
  if (GLAD_GL_KHR_parallel_shader_compile)
  {
    printf("  GLAD_GL_KHR_parallel_shader_compile\n");
    ++on;
  }
  if (GLAD_GL_KHR_robust_buffer_access_behavior)
  {
    printf("  GLAD_GL_KHR_robust_buffer_access_behavior\n");
    ++on;
  }
  if (GLAD_GL_KHR_robustness)
  {
    printf("  GLAD_GL_KHR_robustness\n");
    ++on;
  }
  if (GLAD_GL_KHR_texture_compression_astc_hdr)
  {
    printf("  GLAD_GL_KHR_texture_compression_astc_hdr\n");
    ++on;
  }
  if (GLAD_GL_KHR_texture_compression_astc_ldr)
  {
    printf("  GLAD_GL_KHR_texture_compression_astc_ldr\n");
    ++on;
  }
  if (GLAD_GL_KHR_texture_compression_astc_sliced_3d)
  {
    printf("  GLAD_GL_KHR_texture_compression_astc_sliced_3d\n");
    ++on;
  }
  if (GLAD_GL_MESAX_texture_stack)
  {
    printf("  GLAD_GL_MESAX_texture_stack\n");
    ++on;
  }
  if (GLAD_GL_MESA_pack_invert)
  {
    printf("  GLAD_GL_MESA_pack_invert\n");
    ++on;
  }
  if (GLAD_GL_MESA_program_binary_formats)
  {
    printf("  GLAD_GL_MESA_program_binary_formats\n");
    ++on;
  }
  if (GLAD_GL_MESA_resize_buffers)
  {
    printf("  GLAD_GL_MESA_resize_buffers\n");
    ++on;
  }
  if (GLAD_GL_MESA_shader_integer_functions)
  {
    printf("  GLAD_GL_MESA_shader_integer_functions\n");
    ++on;
  }
  if (GLAD_GL_MESA_tile_raster_order)
  {
    printf("  GLAD_GL_MESA_tile_raster_order\n");
    ++on;
  }
  if (GLAD_GL_MESA_window_pos)
  {
    printf("  GLAD_GL_MESA_window_pos\n");
    ++on;
  }
  if (GLAD_GL_MESA_ycbcr_texture)
  {
    printf("  GLAD_GL_MESA_ycbcr_texture\n");
    ++on;
  }
  if (GLAD_GL_NVX_blend_equation_advanced_multi_draw_buffers)
  {
    printf("  GLAD_GL_NVX_blend_equation_advanced_multi_draw_buffers\n");
    ++on;
  }
  if (GLAD_GL_NVX_conditional_render)
  {
    printf("  GLAD_GL_NVX_conditional_render\n");
    ++on;
  }
  if (GLAD_GL_NVX_gpu_memory_info)
  {
    printf("  GLAD_GL_NVX_gpu_memory_info\n");
    ++on;
  }
  if (GLAD_GL_NVX_linked_gpu_multicast)
  {
    printf("  GLAD_GL_NVX_linked_gpu_multicast\n");
    ++on;
  }
  if (GLAD_GL_NV_alpha_to_coverage_dither_control)
  {
    printf("  GLAD_GL_NV_alpha_to_coverage_dither_control\n");
    ++on;
  }
  if (GLAD_GL_NV_bindless_multi_draw_indirect)
  {
    printf("  GLAD_GL_NV_bindless_multi_draw_indirect\n");
    ++on;
  }
  if (GLAD_GL_NV_bindless_multi_draw_indirect_count)
  {
    printf("  GLAD_GL_NV_bindless_multi_draw_indirect_count\n");
    ++on;
  }
  if (GLAD_GL_NV_bindless_texture)
  {
    printf("  GLAD_GL_NV_bindless_texture\n");
    ++on;
  }
  if (GLAD_GL_NV_blend_equation_advanced)
  {
    printf("  GLAD_GL_NV_blend_equation_advanced\n");
    ++on;
  }
  if (GLAD_GL_NV_blend_equation_advanced_coherent)
  {
    printf("  GLAD_GL_NV_blend_equation_advanced_coherent\n");
    ++on;
  }
  if (GLAD_GL_NV_blend_minmax_factor)
  {
    printf("  GLAD_GL_NV_blend_minmax_factor\n");
    ++on;
  }
  if (GLAD_GL_NV_blend_square)
  {
    printf("  GLAD_GL_NV_blend_square\n");
    ++on;
  }
  if (GLAD_GL_NV_clip_space_w_scaling)
  {
    printf("  GLAD_GL_NV_clip_space_w_scaling\n");
    ++on;
  }
  if (GLAD_GL_NV_command_list)
  {
    printf("  GLAD_GL_NV_command_list\n");
    ++on;
  }
  if (GLAD_GL_NV_compute_program5)
  {
    printf("  GLAD_GL_NV_compute_program5\n");
    ++on;
  }
  if (GLAD_GL_NV_conditional_render)
  {
    printf("  GLAD_GL_NV_conditional_render\n");
    ++on;
  }
  if (GLAD_GL_NV_conservative_raster)
  {
    printf("  GLAD_GL_NV_conservative_raster\n");
    ++on;
  }
  if (GLAD_GL_NV_conservative_raster_dilate)
  {
    printf("  GLAD_GL_NV_conservative_raster_dilate\n");
    ++on;
  }
  if (GLAD_GL_NV_conservative_raster_pre_snap)
  {
    printf("  GLAD_GL_NV_conservative_raster_pre_snap\n");
    ++on;
  }
  if (GLAD_GL_NV_conservative_raster_pre_snap_triangles)
  {
    printf("  GLAD_GL_NV_conservative_raster_pre_snap_triangles\n");
    ++on;
  }
  if (GLAD_GL_NV_conservative_raster_underestimation)
  {
    printf("  GLAD_GL_NV_conservative_raster_underestimation\n");
    ++on;
  }
  if (GLAD_GL_NV_copy_depth_to_color)
  {
    printf("  GLAD_GL_NV_copy_depth_to_color\n");
    ++on;
  }
  if (GLAD_GL_NV_copy_image)
  {
    printf("  GLAD_GL_NV_copy_image\n");
    ++on;
  }
  if (GLAD_GL_NV_deep_texture3D)
  {
    printf("  GLAD_GL_NV_deep_texture3D\n");
    ++on;
  }
  if (GLAD_GL_NV_depth_buffer_float)
  {
    printf("  GLAD_GL_NV_depth_buffer_float\n");
    ++on;
  }
  if (GLAD_GL_NV_depth_clamp)
  {
    printf("  GLAD_GL_NV_depth_clamp\n");
    ++on;
  }
  if (GLAD_GL_NV_draw_texture)
  {
    printf("  GLAD_GL_NV_draw_texture\n");
    ++on;
  }
  if (GLAD_GL_NV_draw_vulkan_image)
  {
    printf("  GLAD_GL_NV_draw_vulkan_image\n");
    ++on;
  }
  if (GLAD_GL_NV_evaluators)
  {
    printf("  GLAD_GL_NV_evaluators\n");
    ++on;
  }
  if (GLAD_GL_NV_explicit_multisample)
  {
    printf("  GLAD_GL_NV_explicit_multisample\n");
    ++on;
  }
  if (GLAD_GL_NV_fence)
  {
    printf("  GLAD_GL_NV_fence\n");
    ++on;
  }
  if (GLAD_GL_NV_fill_rectangle)
  {
    printf("  GLAD_GL_NV_fill_rectangle\n");
    ++on;
  }
  if (GLAD_GL_NV_float_buffer)
  {
    printf("  GLAD_GL_NV_float_buffer\n");
    ++on;
  }
  if (GLAD_GL_NV_fog_distance)
  {
    printf("  GLAD_GL_NV_fog_distance\n");
    ++on;
  }
  if (GLAD_GL_NV_fragment_coverage_to_color)
  {
    printf("  GLAD_GL_NV_fragment_coverage_to_color\n");
    ++on;
  }
  if (GLAD_GL_NV_fragment_program)
  {
    printf("  GLAD_GL_NV_fragment_program\n");
    ++on;
  }
  if (GLAD_GL_NV_fragment_program2)
  {
    printf("  GLAD_GL_NV_fragment_program2\n");
    ++on;
  }
  if (GLAD_GL_NV_fragment_program4)
  {
    printf("  GLAD_GL_NV_fragment_program4\n");
    ++on;
  }
  if (GLAD_GL_NV_fragment_program_option)
  {
    printf("  GLAD_GL_NV_fragment_program_option\n");
    ++on;
  }
  if (GLAD_GL_NV_fragment_shader_interlock)
  {
    printf("  GLAD_GL_NV_fragment_shader_interlock\n");
    ++on;
  }
  if (GLAD_GL_NV_framebuffer_mixed_samples)
  {
    printf("  GLAD_GL_NV_framebuffer_mixed_samples\n");
    ++on;
  }
  if (GLAD_GL_NV_framebuffer_multisample_coverage)
  {
    printf("  GLAD_GL_NV_framebuffer_multisample_coverage\n");
    ++on;
  }
  if (GLAD_GL_NV_geometry_program4)
  {
    printf("  GLAD_GL_NV_geometry_program4\n");
    ++on;
  }
  if (GLAD_GL_NV_geometry_shader4)
  {
    printf("  GLAD_GL_NV_geometry_shader4\n");
    ++on;
  }
  if (GLAD_GL_NV_geometry_shader_passthrough)
  {
    printf("  GLAD_GL_NV_geometry_shader_passthrough\n");
    ++on;
  }
  if (GLAD_GL_NV_gpu_multicast)
  {
    printf("  GLAD_GL_NV_gpu_multicast\n");
    ++on;
  }
  if (GLAD_GL_NV_gpu_program4)
  {
    printf("  GLAD_GL_NV_gpu_program4\n");
    ++on;
  }
  if (GLAD_GL_NV_gpu_program5)
  {
    printf("  GLAD_GL_NV_gpu_program5\n");
    ++on;
  }
  if (GLAD_GL_NV_gpu_program5_mem_extended)
  {
    printf("  GLAD_GL_NV_gpu_program5_mem_extended\n");
    ++on;
  }
  if (GLAD_GL_NV_gpu_shader5)
  {
    printf("  GLAD_GL_NV_gpu_shader5\n");
    ++on;
  }
  if (GLAD_GL_NV_half_float)
  {
    printf("  GLAD_GL_NV_half_float\n");
    ++on;
  }
  if (GLAD_GL_NV_internalformat_sample_query)
  {
    printf("  GLAD_GL_NV_internalformat_sample_query\n");
    ++on;
  }
  if (GLAD_GL_NV_light_max_exponent)
  {
    printf("  GLAD_GL_NV_light_max_exponent\n");
    ++on;
  }
  if (GLAD_GL_NV_multisample_coverage)
  {
    printf("  GLAD_GL_NV_multisample_coverage\n");
    ++on;
  }
  if (GLAD_GL_NV_multisample_filter_hint)
  {
    printf("  GLAD_GL_NV_multisample_filter_hint\n");
    ++on;
  }
  if (GLAD_GL_NV_occlusion_query)
  {
    printf("  GLAD_GL_NV_occlusion_query\n");
    ++on;
  }
  if (GLAD_GL_NV_packed_depth_stencil)
  {
    printf("  GLAD_GL_NV_packed_depth_stencil\n");
    ++on;
  }
  if (GLAD_GL_NV_parameter_buffer_object)
  {
    printf("  GLAD_GL_NV_parameter_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_NV_parameter_buffer_object2)
  {
    printf("  GLAD_GL_NV_parameter_buffer_object2\n");
    ++on;
  }
  if (GLAD_GL_NV_path_rendering)
  {
    printf("  GLAD_GL_NV_path_rendering\n");
    ++on;
  }
  if (GLAD_GL_NV_path_rendering_shared_edge)
  {
    printf("  GLAD_GL_NV_path_rendering_shared_edge\n");
    ++on;
  }
  if (GLAD_GL_NV_pixel_data_range)
  {
    printf("  GLAD_GL_NV_pixel_data_range\n");
    ++on;
  }
  if (GLAD_GL_NV_point_sprite)
  {
    printf("  GLAD_GL_NV_point_sprite\n");
    ++on;
  }
  if (GLAD_GL_NV_present_video)
  {
    printf("  GLAD_GL_NV_present_video\n");
    ++on;
  }
  if (GLAD_GL_NV_primitive_restart)
  {
    printf("  GLAD_GL_NV_primitive_restart\n");
    ++on;
  }
  if (GLAD_GL_NV_query_resource)
  {
    printf("  GLAD_GL_NV_query_resource\n");
    ++on;
  }
  if (GLAD_GL_NV_query_resource_tag)
  {
    printf("  GLAD_GL_NV_query_resource_tag\n");
    ++on;
  }
  if (GLAD_GL_NV_register_combiners)
  {
    printf("  GLAD_GL_NV_register_combiners\n");
    ++on;
  }
  if (GLAD_GL_NV_register_combiners2)
  {
    printf("  GLAD_GL_NV_register_combiners2\n");
    ++on;
  }
  if (GLAD_GL_NV_robustness_video_memory_purge)
  {
    printf("  GLAD_GL_NV_robustness_video_memory_purge\n");
    ++on;
  }
  if (GLAD_GL_NV_sample_locations)
  {
    printf("  GLAD_GL_NV_sample_locations\n");
    ++on;
  }
  if (GLAD_GL_NV_sample_mask_override_coverage)
  {
    printf("  GLAD_GL_NV_sample_mask_override_coverage\n");
    ++on;
  }
  if (GLAD_GL_NV_shader_atomic_counters)
  {
    printf("  GLAD_GL_NV_shader_atomic_counters\n");
    ++on;
  }
  if (GLAD_GL_NV_shader_atomic_float)
  {
    printf("  GLAD_GL_NV_shader_atomic_float\n");
    ++on;
  }
  if (GLAD_GL_NV_shader_atomic_float64)
  {
    printf("  GLAD_GL_NV_shader_atomic_float64\n");
    ++on;
  }
  if (GLAD_GL_NV_shader_atomic_fp16_vector)
  {
    printf("  GLAD_GL_NV_shader_atomic_fp16_vector\n");
    ++on;
  }
  if (GLAD_GL_NV_shader_atomic_int64)
  {
    printf("  GLAD_GL_NV_shader_atomic_int64\n");
    ++on;
  }
  if (GLAD_GL_NV_shader_buffer_load)
  {
    printf("  GLAD_GL_NV_shader_buffer_load\n");
    ++on;
  }
  if (GLAD_GL_NV_shader_buffer_store)
  {
    printf("  GLAD_GL_NV_shader_buffer_store\n");
    ++on;
  }
  if (GLAD_GL_NV_shader_storage_buffer_object)
  {
    printf("  GLAD_GL_NV_shader_storage_buffer_object\n");
    ++on;
  }
  if (GLAD_GL_NV_shader_thread_group)
  {
    printf("  GLAD_GL_NV_shader_thread_group\n");
    ++on;
  }
  if (GLAD_GL_NV_shader_thread_shuffle)
  {
    printf("  GLAD_GL_NV_shader_thread_shuffle\n");
    ++on;
  }
  if (GLAD_GL_NV_stereo_view_rendering)
  {
    printf("  GLAD_GL_NV_stereo_view_rendering\n");
    ++on;
  }
  if (GLAD_GL_NV_tessellation_program5)
  {
    printf("  GLAD_GL_NV_tessellation_program5\n");
    ++on;
  }
  if (GLAD_GL_NV_texgen_emboss)
  {
    printf("  GLAD_GL_NV_texgen_emboss\n");
    ++on;
  }
  if (GLAD_GL_NV_texgen_reflection)
  {
    printf("  GLAD_GL_NV_texgen_reflection\n");
    ++on;
  }
  if (GLAD_GL_NV_texture_barrier)
  {
    printf("  GLAD_GL_NV_texture_barrier\n");
    ++on;
  }
  if (GLAD_GL_NV_texture_compression_vtc)
  {
    printf("  GLAD_GL_NV_texture_compression_vtc\n");
    ++on;
  }
  if (GLAD_GL_NV_texture_env_combine4)
  {
    printf("  GLAD_GL_NV_texture_env_combine4\n");
    ++on;
  }
  if (GLAD_GL_NV_texture_expand_normal)
  {
    printf("  GLAD_GL_NV_texture_expand_normal\n");
    ++on;
  }
  if (GLAD_GL_NV_texture_multisample)
  {
    printf("  GLAD_GL_NV_texture_multisample\n");
    ++on;
  }
  if (GLAD_GL_NV_texture_rectangle)
  {
    printf("  GLAD_GL_NV_texture_rectangle\n");
    ++on;
  }
  if (GLAD_GL_NV_texture_rectangle_compressed)
  {
    printf("  GLAD_GL_NV_texture_rectangle_compressed\n");
    ++on;
  }
  if (GLAD_GL_NV_texture_shader)
  {
    printf("  GLAD_GL_NV_texture_shader\n");
    ++on;
  }
  if (GLAD_GL_NV_texture_shader2)
  {
    printf("  GLAD_GL_NV_texture_shader2\n");
    ++on;
  }
  if (GLAD_GL_NV_texture_shader3)
  {
    printf("  GLAD_GL_NV_texture_shader3\n");
    ++on;
  }
  if (GLAD_GL_NV_transform_feedback)
  {
    printf("  GLAD_GL_NV_transform_feedback\n");
    ++on;
  }
  if (GLAD_GL_NV_transform_feedback2)
  {
    printf("  GLAD_GL_NV_transform_feedback2\n");
    ++on;
  }
  if (GLAD_GL_NV_uniform_buffer_unified_memory)
  {
    printf("  GLAD_GL_NV_uniform_buffer_unified_memory\n");
    ++on;
  }
  if (GLAD_GL_NV_vdpau_interop)
  {
    printf("  GLAD_GL_NV_vdpau_interop\n");
    ++on;
  }
  if (GLAD_GL_NV_vertex_array_range)
  {
    printf("  GLAD_GL_NV_vertex_array_range\n");
    ++on;
  }
  if (GLAD_GL_NV_vertex_array_range2)
  {
    printf("  GLAD_GL_NV_vertex_array_range2\n");
    ++on;
  }
  if (GLAD_GL_NV_vertex_attrib_integer_64bit)
  {
    printf("  GLAD_GL_NV_vertex_attrib_integer_64bit\n");
    ++on;
  }
  if (GLAD_GL_NV_vertex_buffer_unified_memory)
  {
    printf("  GLAD_GL_NV_vertex_buffer_unified_memory\n");
    ++on;
  }
  if (GLAD_GL_NV_vertex_program)
  {
    printf("  GLAD_GL_NV_vertex_program\n");
    ++on;
  }
  if (GLAD_GL_NV_vertex_program1_1)
  {
    printf("  GLAD_GL_NV_vertex_program1_1\n");
    ++on;
  }
  if (GLAD_GL_NV_vertex_program2)
  {
    printf("  GLAD_GL_NV_vertex_program2\n");
    ++on;
  }
  if (GLAD_GL_NV_vertex_program2_option)
  {
    printf("  GLAD_GL_NV_vertex_program2_option\n");
    ++on;
  }
  if (GLAD_GL_NV_vertex_program3)
  {
    printf("  GLAD_GL_NV_vertex_program3\n");
    ++on;
  }
  if (GLAD_GL_NV_vertex_program4)
  {
    printf("  GLAD_GL_NV_vertex_program4\n");
    ++on;
  }
  if (GLAD_GL_NV_video_capture)
  {
    printf("  GLAD_GL_NV_video_capture\n");
    ++on;
  }
  if (GLAD_GL_NV_viewport_array2)
  {
    printf("  GLAD_GL_NV_viewport_array2\n");
    ++on;
  }
  if (GLAD_GL_NV_viewport_swizzle)
  {
    printf("  GLAD_GL_NV_viewport_swizzle\n");
    ++on;
  }
  if (GLAD_GL_OES_byte_coordinates)
  {
    printf("  GLAD_GL_OES_byte_coordinates\n");
    ++on;
  }
  if (GLAD_GL_OES_compressed_paletted_texture)
  {
    printf("  GLAD_GL_OES_compressed_paletted_texture\n");
    ++on;
  }
  if (GLAD_GL_OES_fixed_point)
  {
    printf("  GLAD_GL_OES_fixed_point\n");
    ++on;
  }
  if (GLAD_GL_OES_query_matrix)
  {
    printf("  GLAD_GL_OES_query_matrix\n");
    ++on;
  }
  if (GLAD_GL_OES_read_format)
  {
    printf("  GLAD_GL_OES_read_format\n");
    ++on;
  }
  if (GLAD_GL_OES_single_precision)
  {
    printf("  GLAD_GL_OES_single_precision\n");
    ++on;
  }
  if (GLAD_GL_OML_interlace)
  {
    printf("  GLAD_GL_OML_interlace\n");
    ++on;
  }
  if (GLAD_GL_OML_resample)
  {
    printf("  GLAD_GL_OML_resample\n");
    ++on;
  }
  if (GLAD_GL_OML_subsample)
  {
    printf("  GLAD_GL_OML_subsample\n");
    ++on;
  }
  if (GLAD_GL_OVR_multiview)
  {
    printf("  GLAD_GL_OVR_multiview\n");
    ++on;
  }
  if (GLAD_GL_OVR_multiview2)
  {
    printf("  GLAD_GL_OVR_multiview2\n");
    ++on;
  }
  if (GLAD_GL_PGI_misc_hints)
  {
    printf("  GLAD_GL_PGI_misc_hints\n");
    ++on;
  }
  if (GLAD_GL_PGI_vertex_hints)
  {
    printf("  GLAD_GL_PGI_vertex_hints\n");
    ++on;
  }
  if (GLAD_GL_REND_screen_coordinates)
  {
    printf("  GLAD_GL_REND_screen_coordinates\n");
    ++on;
  }
  if (GLAD_GL_S3_s3tc)
  {
    printf("  GLAD_GL_S3_s3tc\n");
    ++on;
  }
  if (GLAD_GL_SGIS_detail_texture)
  {
    printf("  GLAD_GL_SGIS_detail_texture\n");
    ++on;
  }
  if (GLAD_GL_SGIS_fog_function)
  {
    printf("  GLAD_GL_SGIS_fog_function\n");
    ++on;
  }
  if (GLAD_GL_SGIS_generate_mipmap)
  {
    printf("  GLAD_GL_SGIS_generate_mipmap\n");
    ++on;
  }
  if (GLAD_GL_SGIS_multisample)
  {
    printf("  GLAD_GL_SGIS_multisample\n");
    ++on;
  }
  if (GLAD_GL_SGIS_pixel_texture)
  {
    printf("  GLAD_GL_SGIS_pixel_texture\n");
    ++on;
  }
  if (GLAD_GL_SGIS_point_line_texgen)
  {
    printf("  GLAD_GL_SGIS_point_line_texgen\n");
    ++on;
  }
  if (GLAD_GL_SGIS_point_parameters)
  {
    printf("  GLAD_GL_SGIS_point_parameters\n");
    ++on;
  }
  if (GLAD_GL_SGIS_sharpen_texture)
  {
    printf("  GLAD_GL_SGIS_sharpen_texture\n");
    ++on;
  }
  if (GLAD_GL_SGIS_texture4D)
  {
    printf("  GLAD_GL_SGIS_texture4D\n");
    ++on;
  }
  if (GLAD_GL_SGIS_texture_border_clamp)
  {
    printf("  GLAD_GL_SGIS_texture_border_clamp\n");
    ++on;
  }
  if (GLAD_GL_SGIS_texture_color_mask)
  {
    printf("  GLAD_GL_SGIS_texture_color_mask\n");
    ++on;
  }
  if (GLAD_GL_SGIS_texture_edge_clamp)
  {
    printf("  GLAD_GL_SGIS_texture_edge_clamp\n");
    ++on;
  }
  if (GLAD_GL_SGIS_texture_filter4)
  {
    printf("  GLAD_GL_SGIS_texture_filter4\n");
    ++on;
  }
  if (GLAD_GL_SGIS_texture_lod)
  {
    printf("  GLAD_GL_SGIS_texture_lod\n");
    ++on;
  }
  if (GLAD_GL_SGIS_texture_select)
  {
    printf("  GLAD_GL_SGIS_texture_select\n");
    ++on;
  }
  if (GLAD_GL_SGIX_async)
  {
    printf("  GLAD_GL_SGIX_async\n");
    ++on;
  }
  if (GLAD_GL_SGIX_async_histogram)
  {
    printf("  GLAD_GL_SGIX_async_histogram\n");
    ++on;
  }
  if (GLAD_GL_SGIX_async_pixel)
  {
    printf("  GLAD_GL_SGIX_async_pixel\n");
    ++on;
  }
  if (GLAD_GL_SGIX_blend_alpha_minmax)
  {
    printf("  GLAD_GL_SGIX_blend_alpha_minmax\n");
    ++on;
  }
  if (GLAD_GL_SGIX_calligraphic_fragment)
  {
    printf("  GLAD_GL_SGIX_calligraphic_fragment\n");
    ++on;
  }
  if (GLAD_GL_SGIX_clipmap)
  {
    printf("  GLAD_GL_SGIX_clipmap\n");
    ++on;
  }
  if (GLAD_GL_SGIX_convolution_accuracy)
  {
    printf("  GLAD_GL_SGIX_convolution_accuracy\n");
    ++on;
  }
  if (GLAD_GL_SGIX_depth_pass_instrument)
  {
    printf("  GLAD_GL_SGIX_depth_pass_instrument\n");
    ++on;
  }
  if (GLAD_GL_SGIX_depth_texture)
  {
    printf("  GLAD_GL_SGIX_depth_texture\n");
    ++on;
  }
  if (GLAD_GL_SGIX_flush_raster)
  {
    printf("  GLAD_GL_SGIX_flush_raster\n");
    ++on;
  }
  if (GLAD_GL_SGIX_fog_offset)
  {
    printf("  GLAD_GL_SGIX_fog_offset\n");
    ++on;
  }
  if (GLAD_GL_SGIX_fragment_lighting)
  {
    printf("  GLAD_GL_SGIX_fragment_lighting\n");
    ++on;
  }
  if (GLAD_GL_SGIX_framezoom)
  {
    printf("  GLAD_GL_SGIX_framezoom\n");
    ++on;
  }
  if (GLAD_GL_SGIX_igloo_interface)
  {
    printf("  GLAD_GL_SGIX_igloo_interface\n");
    ++on;
  }
  if (GLAD_GL_SGIX_instruments)
  {
    printf("  GLAD_GL_SGIX_instruments\n");
    ++on;
  }
  if (GLAD_GL_SGIX_interlace)
  {
    printf("  GLAD_GL_SGIX_interlace\n");
    ++on;
  }
  if (GLAD_GL_SGIX_ir_instrument1)
  {
    printf("  GLAD_GL_SGIX_ir_instrument1\n");
    ++on;
  }
  if (GLAD_GL_SGIX_list_priority)
  {
    printf("  GLAD_GL_SGIX_list_priority\n");
    ++on;
  }
  if (GLAD_GL_SGIX_pixel_texture)
  {
    printf("  GLAD_GL_SGIX_pixel_texture\n");
    ++on;
  }
  if (GLAD_GL_SGIX_pixel_tiles)
  {
    printf("  GLAD_GL_SGIX_pixel_tiles\n");
    ++on;
  }
  if (GLAD_GL_SGIX_polynomial_ffd)
  {
    printf("  GLAD_GL_SGIX_polynomial_ffd\n");
    ++on;
  }
  if (GLAD_GL_SGIX_reference_plane)
  {
    printf("  GLAD_GL_SGIX_reference_plane\n");
    ++on;
  }
  if (GLAD_GL_SGIX_resample)
  {
    printf("  GLAD_GL_SGIX_resample\n");
    ++on;
  }
  if (GLAD_GL_SGIX_scalebias_hint)
  {
    printf("  GLAD_GL_SGIX_scalebias_hint\n");
    ++on;
  }
  if (GLAD_GL_SGIX_shadow)
  {
    printf("  GLAD_GL_SGIX_shadow\n");
    ++on;
  }
  if (GLAD_GL_SGIX_shadow_ambient)
  {
    printf("  GLAD_GL_SGIX_shadow_ambient\n");
    ++on;
  }
  if (GLAD_GL_SGIX_sprite)
  {
    printf("  GLAD_GL_SGIX_sprite\n");
    ++on;
  }
  if (GLAD_GL_SGIX_subsample)
  {
    printf("  GLAD_GL_SGIX_subsample\n");
    ++on;
  }
  if (GLAD_GL_SGIX_tag_sample_buffer)
  {
    printf("  GLAD_GL_SGIX_tag_sample_buffer\n");
    ++on;
  }
  if (GLAD_GL_SGIX_texture_add_env)
  {
    printf("  GLAD_GL_SGIX_texture_add_env\n");
    ++on;
  }
  if (GLAD_GL_SGIX_texture_coordinate_clamp)
  {
    printf("  GLAD_GL_SGIX_texture_coordinate_clamp\n");
    ++on;
  }
  if (GLAD_GL_SGIX_texture_lod_bias)
  {
    printf("  GLAD_GL_SGIX_texture_lod_bias\n");
    ++on;
  }
  if (GLAD_GL_SGIX_texture_multi_buffer)
  {
    printf("  GLAD_GL_SGIX_texture_multi_buffer\n");
    ++on;
  }
  if (GLAD_GL_SGIX_texture_scale_bias)
  {
    printf("  GLAD_GL_SGIX_texture_scale_bias\n");
    ++on;
  }
  if (GLAD_GL_SGIX_vertex_preclip)
  {
    printf("  GLAD_GL_SGIX_vertex_preclip\n");
    ++on;
  }
  if (GLAD_GL_SGIX_ycrcb)
  {
    printf("  GLAD_GL_SGIX_ycrcb\n");
    ++on;
  }
  if (GLAD_GL_SGIX_ycrcb_subsample)
  {
    printf("  GLAD_GL_SGIX_ycrcb_subsample\n");
    ++on;
  }
  if (GLAD_GL_SGIX_ycrcba)
  {
    printf("  GLAD_GL_SGIX_ycrcba\n");
    ++on;
  }
  if (GLAD_GL_SGI_color_matrix)
  {
    printf("  GLAD_GL_SGI_color_matrix\n");
    ++on;
  }
  if (GLAD_GL_SGI_color_table)
  {
    printf("  GLAD_GL_SGI_color_table\n");
    ++on;
  }
  if (GLAD_GL_SGI_texture_color_table)
  {
    printf("  GLAD_GL_SGI_texture_color_table\n");
    ++on;
  }
  if (GLAD_GL_SUNX_constant_data)
  {
    printf("  GLAD_GL_SUNX_constant_data\n");
    ++on;
  }
  if (GLAD_GL_SUN_convolution_border_modes)
  {
    printf("  GLAD_GL_SUN_convolution_border_modes\n");
    ++on;
  }
  if (GLAD_GL_SUN_global_alpha)
  {
    printf("  GLAD_GL_SUN_global_alpha\n");
    ++on;
  }
  if (GLAD_GL_SUN_mesh_array)
  {
    printf("  GLAD_GL_SUN_mesh_array\n");
    ++on;
  }
  if (GLAD_GL_SUN_slice_accum)
  {
    printf("  GLAD_GL_SUN_slice_accum\n");
    ++on;
  }
  if (GLAD_GL_SUN_triangle_list)
  {
    printf("  GLAD_GL_SUN_triangle_list\n");
    ++on;
  }
  if (GLAD_GL_SUN_vertex)
  {
    printf("  GLAD_GL_SUN_vertex\n");
    ++on;
  }
  if (GLAD_GL_WIN_phong_shading)
  {
    printf("  GLAD_GL_WIN_phong_shading\n");
    ++on;
  }
  if (GLAD_GL_WIN_specular_fog)
  {
    printf("  GLAD_GL_WIN_specular_fog\n");
    ++on;
  }
  printf("  %d of 600 flags\n", on);
}

int main(void)
{
  static const char* const none[] = {""};
  static const char* const some[] = {"GL_ARB_debug_output", "GL_KHR_debug", "GL_EXT_texture_filter_anisotropic", "GL_ARB_buffer_storage",
                                     "GL_ARB_texture_compression_bptc", "GL_EXT_texture_compression_s3tc", "GL_ARB_clip_control",
                                     "GL_ARB_direct_state_access", "GL_NV_shader_buffer_load", "GL_AMD_pinned_memory", "GL_ARB_bindless_texture",
                                     "GL_ARB_sparse_texture", "GL_EXT_framebuffer_object", "GL_ARB_vertex_array_object"};
  scenario("1.1.0", none, 0);
  scenario("2.1 Mesa 20.0", some, 6);
  scenario("3.3.0 NVIDIA 535.98", some, 14);
  scenario("4.6.0 NVIDIA 535.98", some, 14);
  scenario("4.1 ATI-4.12", some, 3);
  scenario("OpenGL ES 3.2 NVIDIA", some, 2);
  return 0;
}
