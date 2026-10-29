// Triangulation.cpp's jc_voronoi, built for the converted game (the game's copy of the header
// calls std::floor and std::ceil, so this is C++; its functions are extern "C"). In double
// precision, as kpp_jc_voronoi.h gives the import; its atan2 the game's deterministic
// Math::atan2, which the converted Triangulation exports to C
// (overrides/src/Logistics/Triangulation.kpp). The game's Math::fabs and Math::sqrt are the C
// library's, and its distance the larger of the two magnitudes.
#include <cmath>

extern "C" double kppMathAtan2(double y, double x);

#define JC_VORONOI_IMPLEMENTATION
#define JCV_REAL_TYPE double
#define JCV_FABS std::fabs
#define JCV_ATAN2 kppMathAtan2
#define JCV_SQRT std::sqrt
#define JCV_DISTANCE(dx, dy) std::fmax(std::fabs(dx), std::fabs(dy))
#include <jc_voronoi/jc_voronoi.h>
