# Writes harness/glad.c, glad's differential harness driver, from glad.h's function pointers and
# GLAD_GL_ flags: python -I harness/glad.py (from programs/factorio).
import re, io
h = io.open(r'C:/Factorio/libraries/glad/include/glad/glad.h', encoding='utf-8', errors='replace').read()
pointers = re.findall(r'^GLAPI PFN\w+ (glad_\w+);', h, re.M)
flags = re.findall(r'^GLAPI int (GLAD_GL_\w+);', h, re.M)
out = []
w = out.append
w('''/* glad's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): built once
   with the C library and once converted with it, it must print the same bytes. glad loads OpenGL's
   functions through a loader; this one is fake, so no GL context is needed: glGetString,
   glGetIntegerv and glGetStringi report a GL version and extensions, every other name gets an
   address of its own. For GL versions from 1.1 to 4.6 (and an ES string), with and without
   extensions, it prints glad's version, every GLAD_GL_ flag, and which of its %d function pointers
   it loaded (a count and a hash of the addresses by name).
   Written by harness/glad.py from glad.h's lists (python -I harness/glad.py): edit that, not this.

     glad > output.txt (harness.cmd glad builds and compares both) */
#include <glad/glad.h>
#include <stdio.h>
#include <string.h>
''' % len(pointers))
w('static const char* version = "";')
w('static const char* extensions[64];')
w('static int extensionCount = 0;')
w('static char extensionString[4096];')
w('static char marks[65536];')
w('static const GLubyte* APIENTRY fakeGetString(GLenum name)')
w('{')
w('  if (name == GL_VERSION)')
w('    return (const GLubyte*)version;')
w('  if (name == GL_EXTENSIONS)')
w('    return (const GLubyte*)extensionString;')
w('  return (const GLubyte*)"fake";')
w('}')
w('static void APIENTRY fakeGetIntegerv(GLenum name, GLint* data)')
w('{')
w('  if (name == GL_NUM_EXTENSIONS)')
w('    *data = extensionCount;')
w('  else if (name == GL_MAJOR_VERSION)')
w('    *data = version[0] - \'0\';')
w('  else if (name == GL_MINOR_VERSION)')
w('    *data = version[2] - \'0\';')
w('  else')
w('    *data = 0;')
w('}')
w('static const GLubyte* APIENTRY fakeGetStringi(GLenum name, GLuint index)')
w('{')
w('  return name == GL_EXTENSIONS && index < (GLuint)extensionCount ? (const GLubyte*)extensions[index] : NULL;')
w('}')
w('static unsigned hashName(const char* name)')
w('{')
w('  unsigned h = 2166136261u;')
w('  for (; *name; ++name)')
w('    h = (h ^ (unsigned char)*name) * 16777619u;')
w('  return h;')
w('}')
w('static void* loader(const char* name)')
w('{')
w('  if (strcmp(name, "glGetString") == 0)')
w('    return (void*)fakeGetString;')
w('  if (strcmp(name, "glGetIntegerv") == 0)')
w('    return (void*)fakeGetIntegerv;')
w('  if (strcmp(name, "glGetStringi") == 0)')
w('    return (void*)fakeGetStringi;')
w('  if (hashName(name) % 7 == 0)')
w('    return NULL; /* (a driver without the function) */')
w('  return &marks[hashName(name) % sizeof(marks)];')
w('}')
w('static void scenario(const char* gl, const char* const* names, int count)')
w('{')
w('  version = gl;')
w('  extensionCount = count;')
w('  extensionString[0] = 0;')
w('  for (int k = 0; k < count; ++k)')
w('  {')
w('    extensions[k] = names[k];')
w('    strcat(extensionString, names[k]);')
w('    strcat(extensionString, " ");')
w('  }')
w('  int loaded = gladLoadGLLoader(loader);')
w('  printf("GL \\"%s\\" with %d extensions: load %d, version %d.%d\\n", gl, count, loaded, GLVersion.major, GLVersion.minor);')
w('  unsigned long long hash = 1469598103934665603ull;')
w('  int set = 0;')
w('  const char* p;')
for name in pointers:
    w('  p = (const char*)%s;' % name)
    w('  set += p != NULL;')
    w('  hash = (hash ^ (unsigned long long)(p >= marks && p < marks + sizeof(marks) ? p - marks + 1 : p ? 99999 : 0)) * 1099511628211ull;')
w('  printf("  %d of %d pointers set, hash %016llx\\n", set, ' + str(len(pointers)) + ', hash);')
w('  int on = 0;')
for flag in flags:
    w('  if (%s)' % flag)
    w('  {')
    w('    printf("  %s\\n");' % flag)
    w('    ++on;')
    w('  }')
w('  printf("  %d of ' + str(len(flags)) + ' flags\\n", on);')
w('}')
w('''
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
''')
io.open(r'C:/kpp-sandbox/worktrees/convert/programs/factorio/harness/glad.c', 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
print(len(pointers), len(flags))
