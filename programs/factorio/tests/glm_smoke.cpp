// The C++ twin of glm_smoke.kpp, with the game's glm 0.9.9.0; its output is glm_smoke.expected.
// From a VS developer prompt in programs\factorio:
//   clang-cl /O2 /MT /std:c++17 /I C:\Factorio\libraries tests\glm_smoke.cpp
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>
#include <stdio.h>
#include <string.h>

static void print2(const char* name, glm::vec2 v) { printf("%s %a %a\n", name, v.x, v.y); }
static void print3(const char* name, glm::vec3 v) { printf("%s %a %a %a\n", name, v.x, v.y, v.z); }
static void print4(const char* name, glm::vec4 v) { printf("%s %a %a %a %a\n", name, v.x, v.y, v.z, v.w); }
static void printMatrix(const char* name, const glm::mat4& m)
{
  float values[16];
  memcpy(values, glm::value_ptr(m), sizeof(values));
  printf("%s", name);
  for (int i = 0; i < 16; ++i)
    printf(" %a", values[i]);
  printf("\n");
}

static float smoothFloor(float a, float tightness)
{
  return float(a - 0.5 - glm::atan((-tightness * glm::sin(6.28318 * a)) / (1 - tightness * glm::cos(6.28318 * a))) / 3.14159);
}
static glm::vec2 smoothFloor(glm::vec2 a, float tightness)
{
  return a - glm::vec2(0.5) - glm::atan((-tightness * glm::sin(glm::vec2(6.28318) * a)) / (glm::vec2(1) - tightness * glm::cos(glm::vec2(6.28318) * a))) / glm::vec2(3.14159);
}
static glm::vec3 permute(glm::vec3 x0, glm::vec3 p)
{
  const glm::vec3 x1 = glm::mod(x0 * p.y, p.x);
  return floor(glm::mod((x1 + p.z) * x0, p.x));
}
static float taylorInvSqrt(float r) { return float(0.83666002653408 + 0.7 * 0.85373472095314 - 0.85373472095314 * r); }
static glm::vec2 simplexProject(glm::vec2 coord_xy) { return coord_xy + glm::dot(coord_xy, glm::vec2(0.366025403784438597)); }
static glm::vec2 simplexUnproject(glm::vec2 coord_uv) { return coord_uv - glm::dot(coord_uv, glm::vec2(0.211324865405187134)); }
static glm::vec3 lessThanV3(glm::vec3 const& x, glm::vec3 const& y)
{
  glm::vec3 result;
  for (glm::length_t i = 0; i < 3; ++i)
    result[i] = x[i] < y[i];
  return result;
}
static float simplexNoise2(glm::vec2 v)
{
  const glm::vec3 noiseParamXYZ = glm::vec3(289.0, 34.0, 1.0);
  const float noiseParamW = 7.0f;
  glm::vec2 i = glm::floor(simplexProject(v));
  glm::vec2 x0 = v - simplexUnproject(i);
  glm::vec2 i1 = (x0.x > x0.y) ? glm::vec2(1.0, 0.0) : glm::vec2(0.0, 1.0);
  glm::vec2 x1 = x0 - simplexUnproject(i1);
  glm::vec2 x2 = x0 - simplexUnproject(glm::vec2(1.0, 1.0));
  i = glm::mod(i, noiseParamXYZ.x);
  glm::vec3 p = permute(permute(i.y + glm::vec3(0.0, i1.y, 1.0), noiseParamXYZ) + i.x + glm::vec3(0.0, i1.x, 1.0), noiseParamXYZ);
  glm::vec3 x = glm::fract(p / noiseParamW);
  glm::vec3 h = glm::vec3(0.5) - glm::abs(x);
  const glm::vec3 D = glm::vec3(0.0, 0.5, 2.0) * glm::vec3(3.14159265358979312);
  glm::vec3 sx = glm::vec3(lessThanV3(x, glm::vec3(D.x))) * glm::vec3(2.0) - glm::vec3(1.0);
  glm::vec3 sh = glm::vec3(lessThanV3(h, glm::vec3(D.x)));
  glm::vec3 a0 = x + sx * sh;
  glm::vec2 p0 = glm::vec2(a0.x, h.x);
  glm::vec2 p1 = glm::vec2(a0.y, h.y);
  glm::vec2 p2 = glm::vec2(a0.z, h.z);
  p0 *= taylorInvSqrt(glm::dot(p0, p0));
  p1 *= taylorInvSqrt(glm::dot(p1, p1));
  p2 *= taylorInvSqrt(glm::dot(p2, p2));
  glm::vec3 g = glm::vec3(2.0) * glm::vec3(glm::dot(p0, x0), glm::dot(p1, x1), glm::dot(p2, x2));
  glm::vec3 m = glm::max(glm::vec3(0.5) - glm::vec3(glm::dot(x0, x0), dot(x1, x1), dot(x2, x2)), glm::vec3(0.0));
  m = m * m;
  return float(0.5 + 43.2 * glm::dot(m * m, g));
}

int main()
{
  glm::vec2 a(1.25f, -3.5f);
  glm::vec2 b = glm::vec2(0.5 * a.y, 2);
  print2("a+b", a + b);
  print2("a*b/3", a * b / 3.0f);
  print2("2-a", 2.0f - a);
  print2("normalize", glm::normalize(b - a));
  b -= glm::normalize(b - a) * 0.2f;
  print2("b", b);
  glm::vec3 c(0.1f, 2.7f, -1.3f);
  print3("floor", glm::floor(c));
  print3("fract", glm::fract(c));
  print3("mod", glm::mod(c * 7.0f, 2.5f));
  print3("abs max", glm::max(glm::abs(c) - glm::vec3(1.0f), glm::vec3(0.0f)));
  print2("sin cos atan", glm::atan(glm::sin(a) / glm::cos(a)));
  printf("dot %a %a\n", glm::dot(a, b), glm::dot(c, c));
  printf("scalars %a %a %a\n", smoothFloor(2.3f, 0.7f), glm::fract(-1.75f), glm::pow(0.37f, 2.2f));
  print2("smoothFloor", smoothFloor(glm::vec2(2.3f, -0.6f), 0.4f));
  for (int k = 0; k < 4; ++k)
    printf("noise %a\n", simplexNoise2(glm::vec2(0.37f * k - 1.0f, 1.9f - 0.61f * k)));

  glm::mat4 transform(1.0f);
  {
    using namespace glm;
    mat4 spriteAtlasToUv(1.0f);
    spriteAtlasToUv = translate(spriteAtlasToUv, vec3(0.25f, 0.5f, 0.0f));
    spriteAtlasToUv = scale(spriteAtlasToUv, vec3(0.125f, 0.0625f, 1.0f));
    spriteAtlasToUv = translate(spriteAtlasToUv, vec3(0.5f, 0.5f, 0.0f));
    spriteAtlasToUv = inverse(spriteAtlasToUv);
    mat4 maskTransformation(1.0f);
    maskTransformation = translate(maskTransformation, vec3(3.0f - 1.5f, 7.25f - 2.0f, 0.0f));
    maskTransformation = scale(maskTransformation, vec3(1.5f * 32.0f, 0.75f * 64.0f, 1.0f));
    maskTransformation = inverse(maskTransformation);
    vec3 maskTranslation = maskTransformation * vec4(0, 0, 0, 1);
    print3("maskTranslation", maskTranslation);
    mat4 spriteUvToMaskUv(1.0f);
    spriteUvToMaskUv = translate(spriteUvToMaskUv, maskTranslation);
    spriteUvToMaskUv = scale(spriteUvToMaskUv, vec3((32.0f * 1.25f) / (32.0f * 1.5f), (64.0f * 0.5f) / (64.0f * 0.75f), 1.0f));
    transform = spriteUvToMaskUv * spriteAtlasToUv;
  }
  printMatrix("transform", transform);
  glm::mat4 rotation = glm::mat4(1.0f);
  rotation = glm::rotate(rotation, 0.37f, glm::vec3(0.0f, 1.0f, 0.0f));
  rotation = glm::rotate(rotation, -0.21f, glm::vec3(0.0f, 0.0f, 1.0f));
  rotation = glm::rotate(rotation, 1.3f, glm::vec3(1.0f, 0.0f, 0.0f));
  printMatrix("rotation", rotation);
  glm::mat4 view = glm::scale(glm::vec3(48.0f, 48.0f, 1));
  view = glm::translate(view, glm::vec3(-12.5f, 7.75f, 0));
  printMatrix("view", view);
  glm::mat4 projection = glm::ortho(0.0f, 1920.0f, 1080.0f, 0.0f);
  glm::mat4 model = glm::translate(glm::vec3{960.0f + 15.0f, 540.0f - 4.0f, 0.0f}) * glm::scale(glm::vec3{30.0f, 8.0f, 0.0f});
  printMatrix("mvp", projection * view * model * rotation);
  print4("mvp*v", projection * view * glm::vec4(1.0f, 2.0f, 0.0f, 1.0f));
  return 0;
}
