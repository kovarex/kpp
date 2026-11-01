// zstd.h as zstd's own files include it (common/zstd_internal.h, compress/zstdmt_compress.h...):
// with its static linking API, which the converted files call. The converter imports this for
// zstd.h (c-libraries/zstd/c-headers.txt); the game's own import of zstd.h then adds nothing.
#pragma once
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
