// jc_voronoi as Triangulation.cpp configures it before including it: in double precision. The
// converter imports this for <jc_voronoi/jc_voronoi.h>, so the declarations match the
// implementation in native/header_only.c.
#pragma once
#define JCV_REAL_TYPE double
#include <jc_voronoi/jc_voronoi.h>
