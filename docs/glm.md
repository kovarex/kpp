# glm for Kpp: the subset the game uses

The game's rendering code (11 files: DrawCommandBatch, BlurEffectDrawOrder, StarfieldEffectLogic,
GraphicsInterface, LightningDrawEngine, PrimitiveHelper, RenderParameters, SimplexNoiseUtil,
DebugTools) uses glm 0.9.9.0, a C++ header-only template library. Kpp calls only C, and a header of
templates has nothing to wrap, so the part the game uses is ported to Kpp:
`programs/factorio/overrides/src/API/glm.kpp`, appended to the conversion of the game's
`API/glm.hpp`. Everything is in namespace `glm` with glm's names, so converted code (`glm::vec2`,
`using namespace glm;` then `mat4`, `translate`, ..., `floor(v)` and `dot(a, b)` found through the
argument's namespace) needs no change.

## The rule: glm's results to the bit

Every function computes with glm's formula: the same floating-point operations in the same order
(dot of a vec4 adds `(x + y) + (z + w)`, a matrix times a vector adds `(c0 v0 + c1 v1) + (c2 v2 +
c3 v3)`, inverse is glm's cofactor expansion), and a float function calls the C library's float
function as `std::floor(float)` does in C++ (`floorf`, `sinf`, `powf`, ...), a double one the double
function. `programs/factorio/tests/glm_smoke.kpp` prints the results of vector arithmetic, the
component functions, the game's simplex noise and the game's matrix chains in hex, and its output
equals `glm_smoke.cpp`'s, the same program compiled against glm itself (convert.cmd's glm smoke
test).

## Types

- `vec2`, `vec3`, `vec4`: two, three or four floats `x`, `y`, `z`, `w`, laid out in that order
  (8, 12 and 16 bytes). Constructors: from nothing (zero), `explicit` from one float (every
  component), from each component (any arithmetic type, converted), `vec3(vec2, z)`,
  `vec3(x, vec2)`, `vec4(vec3, w)`, `vec4(vec2, z, w)`, and implicitly from a longer vector (its
  first components: `vec3 t = m * vec4(0, 0, 0, 1);`). `v[i]`: component i.
- `mat4`: four `vec4` columns, `m[c]` the column c and `m[c][r]` its row r, 64 bytes column after
  column (what `memcpy` into a uniform buffer and `value_ptr` see). `mat4()` is zero, `mat4(s)` has
  s on the diagonal, `mat4(c0, c1, c2, c3)` takes the columns.
- `length_t`: int, the type of a component index.

The default vector and matrix are zero, where glm leaves them uninitialized: no correct program
reads them before writing them, so this changes nothing it computes.

## Operations

- Vectors: `+`, `-`, `*`, `/` of two vectors of a size (component by component), of a vector and a
  float on either side, the compound forms `+=`, `-=`, `*=`, `/=` with a vector or a float, unary
  `-` and `+`, `==`, `!=`.
- Matrices: `mat4 * mat4`, `mat4 * vec4`, `mat4 * float`, `==`, `!=`.

## Functions

Each takes a float, a double where noted, or a vector (then component by component):
- `floor`, `fract` (`x - floor(x)`), `abs` (`x >= 0 ? x : -x`), `sin`, `cos`, `atan` (one
  argument), `sqrt` (float and double); `pow(base, exponent)` (float, double, vec2, vec3).
- `mod(x, y)`: `x - y * floor(x / y)`, y a float or a vector of x's size.
- `max(x, y)`: `(x < y) ? y : x`; `min(x, y)`: `(y < x) ? y : x`; y a float or a vector.
- `dot(a, b)`, `length(v)` (vec2, vec3), `normalize(v)`: `v * inversesqrt(dot(v, v))`, and
  `inversesqrt(x)`: `1 / sqrt(x)`.
- `translate(m, v)`, `scale(m, v)`, `rotate(m, angle, axis)` (glm/gtc/matrix_transform: m times
  the transformation, the axis normalized), and `translate(v)`, `scale(v)`, `rotate(angle, axis)`
  (glm/gtx/transform: of the identity).
- `ortho(left, right, bottom, top)`: glm 0.9.9.0's 2D projection, which maps x and y to [-1, 1]
  and leaves z as it is (its `[2][2]` stays 1).
- `inverse(m)`: glm's; a singular m gives infinities, as glm's does.
- `value_ptr(m)` (also of a vector): a pointer to the first float.

Not ported: the other sizes and types (`dvec`, `ivec`, `mat2`, `mat3`), quaternions, swizzles,
the perspective and look-at projections, and every other function: none of them is used.
