#!/usr/bin/env python3
# Every SSE intrinsic the game uses (and its neighbours), on edge values, one source for kpp and clang.
# Usage: gensse.py SEED HEADER [skip-file] > file.   Lines of output name the intrinsic and the input.
import random, struct, sys

seed, header = int(sys.argv[1]), sys.argv[2]
skip = set(open(sys.argv[3]).read().split()) if len(sys.argv) > 3 else set()
r = random.Random(seed)

def f2u(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def d2u(x): return struct.unpack('<Q', struct.pack('<d', x))[0]
F_EDGE = [f2u(x) for x in [0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, -2.5, 3.5, 1e10, -1e10, 2147483648.0, -2147483648.0, 2147483520.0, 3e9, 1e-30, 16777217.0, 0.1, 255.5]] + [0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fa00000, 0x00000001, 0x807fffff, 0x7f7fffff]
D_EDGE = [d2u(x) for x in [0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, -2.5, 1e300, -1e300, 2147483648.0, -2147483649.0, 2147483647.5, 1e-310, 0.1, 3.4e38, 3.5e38, 1e-46]] + [0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0x7ff4000000000000]
I32_EDGE = [0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 0x0000ffff, 0xffff0000, 0x00ff00ff, 0xff00ff00, 0x80808080, 0x7f7f7f7f, 0x80008000, 0x7fff7fff]

def vector():
    k = r.randint(0, 6)
    if k == 0: return [r.choice(F_EDGE) for _ in range(4)]
    if k == 1:
        a, b = r.choice(D_EDGE), r.choice(D_EDGE)
        return [a & 0xffffffff, a >> 32, b & 0xffffffff, b >> 32]
    if k == 2: return [r.choice(I32_EDGE) for _ in range(4)]
    if k == 3: return [r.getrandbits(32) for _ in range(4)]
    if k == 4: return [f2u(r.uniform(-1000, 1000)) for _ in range(4)]
    if k == 5:
        a, b = d2u(r.uniform(-1e6, 1e6)), d2u(r.uniform(-1, 1))
        return [a & 0xffffffff, a >> 32, b & 0xffffffff, b >> 32]
    return [r.randint(0, 65535) | (r.randint(0, 65535) << 16) for _ in range(4)]

N = 48
vs = [vector() for _ in range(N)]

BIN = {
 'i': 'add_epi8 add_epi16 add_epi32 add_epi64 adds_epi8 adds_epi16 adds_epu8 adds_epu16 and_si128 andnot_si128 avg_epu8 avg_epu16 cmpeq_epi8 cmpeq_epi16 cmpeq_epi32 cmpgt_epi8 cmpgt_epi16 cmpgt_epi32 cmplt_epi8 cmplt_epi16 cmplt_epi32 madd_epi16 max_epi16 max_epu8 min_epi16 min_epu8 mul_epu32 mulhi_epi16 mulhi_epu16 mullo_epi16 or_si128 packs_epi16 packs_epi32 packus_epi16 sad_epu8 sub_epi8 sub_epi16 sub_epi32 sub_epi64 subs_epi8 subs_epi16 subs_epu8 subs_epu16 unpackhi_epi8 unpackhi_epi16 unpackhi_epi32 unpackhi_epi64 unpacklo_epi8 unpacklo_epi16 unpacklo_epi32 unpacklo_epi64 xor_si128'.split(),
 'f': 'add_ps sub_ps mul_ps div_ps and_ps or_ps xor_ps andnot_ps max_ps min_ps cmpeq_ps cmpge_ps cmpgt_ps cmple_ps cmplt_ps cmpneq_ps cmpord_ps cmpunord_ps unpackhi_ps unpacklo_ps add_ss sub_ss mul_ss div_ss max_ss min_ss movehl_ps movelh_ps move_ss cmpeq_ss cmplt_ss'.split(),
 'd': 'add_pd sub_pd mul_pd div_pd and_pd or_pd xor_pd andnot_pd max_pd min_pd cmpeq_pd cmpge_pd cmpgt_pd cmple_pd cmplt_pd cmpneq_pd cmpord_pd cmpunord_pd unpackhi_pd unpacklo_pd add_sd sub_sd mul_sd div_sd max_sd min_sd move_sd'.split(),
}
UN = [('sqrt_ps', 'f', 'f'), ('rcp_ps', 'f', 'f'), ('rcp_ss', 'f', 'f'), ('rsqrt_ps', 'f', 'f'), ('rsqrt_ss', 'f', 'f'), ('sqrt_ss', 'f', 'f'), ('sqrt_pd', 'd', 'd'),
      ('cvtps_epi32', 'f', 'i'), ('cvttps_epi32', 'f', 'i'), ('castps_si128', 'f', 'i'), ('cvtpd_epi32', 'd', 'i'), ('cvttpd_epi32', 'd', 'i'), ('castpd_si128', 'd', 'i'),
      ('cvtepi32_ps', 'i', 'f'), ('castsi128_ps', 'i', 'f'), ('cvtepi32_pd', 'i', 'd'), ('castsi128_pd', 'i', 'd'),
      ('cvtps_pd', 'f', 'd'), ('castps_pd', 'f', 'd'), ('cvtpd_ps', 'd', 'f'), ('castpd_ps', 'd', 'f'),
      ('cvtss_f32', 'f', 'sf'), ('cvtsd_f64', 'd', 'sd'), ('cvtsi128_si32', 'i', 's32'), ('cvtsi128_si64', 'i', 's64'), ('movemask_epi8', 'i', 's32'), ('movemask_ps', 'f', 's32'), ('movemask_pd', 'd', 's32'),
      ('cvtss_si32', 'f', 's32'), ('cvttss_si32', 'f', 's32'), ('cvtsd_si32', 'd', 's32'), ('cvttsd_si32', 'd', 's32'), ('cvtss_si64', 'f', 's64'), ('cvttss_si64', 'f', 's64'), ('cvtsd_si64', 'd', 's64'), ('cvttsd_si64', 'd', 's64')]
SHIFT = [('slli_epi16', 'i'), ('slli_epi32', 'i'), ('slli_epi64', 'i'), ('srli_epi16', 'i'), ('srli_epi32', 'i'), ('srli_epi64', 'i'), ('srai_epi16', 'i'), ('srai_epi32', 'i'), ('srli_si128', 'i'), ('slli_si128', 'i')]
COUNTS = [0, 1, 3, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 100, 255]
VSHIFT = ['sll_epi16', 'sll_epi32', 'sll_epi64', 'srl_epi16', 'srl_epi32', 'srl_epi64', 'sra_epi16', 'sra_epi32']
IMM = {'shuffle_epi32': ('i', 256), 'shufflelo_epi16': ('i', 256), 'shufflehi_epi16': ('i', 256)}

out = []
w = out.append
w('extern "C" import "%s";' % header if header != 'cxx' else '')
w('extern "C" int printf(const char* format, ...);')
w('unsigned table[%d][4] = {' % N)
for v in vs: w('  { ' + ', '.join('0x%xu' % x for x in v) + ' },')
w('};')
w('volatile int zero = 0;')
w('__m128i vi(int i) { return _mm_loadu_si128((const __m128i*)table[(i + zero) %% %d]); }' % N)
w('__m128 vf(int i) { return _mm_castsi128_ps(vi(i)); }')
w('__m128d vd(int i) { return _mm_castsi128_pd(vi(i)); }')
w('void show(const char* n, int i, __m128i v) { unsigned x[4]; _mm_storeu_si128((__m128i*)x, v); printf("%s %d %08x %08x %08x %08x\\n", n, i, x[0], x[1], x[2], x[3]); }')
w('void show(const char* n, int i, __m128 v) { show(n, i, _mm_castps_si128(v)); }')
w('void show(const char* n, int i, __m128d v) { show(n, i, _mm_castpd_si128(v)); }')
w('void show(const char* n, int i, float v) { unsigned u; memcpy(&u, &v, 4); printf("%s %d %08x\\n", n, i, u); }')
w('void show(const char* n, int i, double v) { unsigned long long u; memcpy(&u, &v, 8); printf("%s %d %016llx\\n", n, i, u); }')
w('void show(const char* n, int i, int v) { printf("%s %d %d\\n", n, i, v); }')
w('void show(const char* n, int i, long long v) { printf("%s %d %lld\\n", n, i, v); }')
ld = {'i': 'vi', 'f': 'vf', 'd': 'vd'}
fns = []
def test(name, body):
    if name in skip: return
    fns.append('t_' + name)
    w('void t_%s()\n{\n%s\n}' % (name, body))
for kind, names in BIN.items():
    for n in names:
        test(n, '  for (int i = 0; i < %d; i++)\n    show("%s", i, _mm_%s(%s(i), %s(i * 7 + 1)));' % (N, n, n, ld[kind], ld[kind]))
        # the same operands, for compares and min and max
for n, a, res in UN:
    test(n, '  for (int i = 0; i < %d; i++)\n    show("%s", i, _mm_%s(%s(i)));' % (N, n, n, ld[a]))
for n, a in SHIFT:
    lines = []
    for c in COUNTS:
        if n.endswith('si128') and c > 16: continue
        lines.append('  for (int i = 0; i < 12; i++)\n    show("%s/%d", i, _mm_%s(vi(i), %d));' % (n, c, n, c))
    test(n, '\n'.join(lines))
    if not n.endswith('si128'):
        test(n + '_var', '  for (int c : {0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 100})\n    for (int i = 0; i < 12; i++)\n      show("%s/v", i * 1000 + c, _mm_%s(vi(i), c + zero));' % (n, n))
for n in VSHIFT:
    test(n, '  for (int c : {0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 100})\n    for (int i = 0; i < 12; i++)\n      show("%s", i * 1000 + c, _mm_%s(vi(i), _mm_cvtsi32_si128(c + zero)));' % (n, n))
for n, (a, imms) in IMM.items():
    test(n, '\n'.join('  show("%s/%d", 0, _mm_%s(vi(3), %d)); show("%s/%d", 1, _mm_%s(vi(9), %d));' % (n, i, n, i, n, i, n, i) for i in range(0, 256, 5)))
test('shuffle_ps', '\n'.join('  show("shuffle_ps/%d", 0, _mm_shuffle_ps(vf(3), vf(4), %d));' % (i, i) for i in range(0, 256, 3)))
test('shuffle_pd', '\n'.join('  show("shuffle_pd/%d", 0, _mm_shuffle_pd(vd(3), vd(4), %d));' % (i, i) for i in range(4)))
test('insert_epi16', '\n'.join('  show("insert/%d", 0, _mm_insert_epi16(vi(5), 0xabcd + zero, %d)); show("extract/%d", 0, _mm_extract_epi16(vi(5), %d));' % (i, i, i, i) for i in range(8)))
test('scalars', r'''  show("cvtsi32", 0, _mm_cvtsi32_si128(-5 + zero));
  show("cvtsi64", 0, _mm_cvtsi64_si128(-5000000000ll + zero));
  show("set_ss", 0, _mm_set_ss(1.5f + zero));
  show("set_sd", 0, _mm_set_sd(1.5 + zero));
  show("set1_epi8", 0, _mm_set1_epi8((char)(200 + zero)));
  show("set1_epi16", 0, _mm_set1_epi16((short)(40000 + zero)));
  show("set1_epi32", 0, _mm_set1_epi32(-7 + zero));
  show("set1_epi64x", 0, _mm_set1_epi64x(0x123456789abcdefll + zero));
  show("set1_ps", 0, _mm_set1_ps(2.5f + zero));
  show("set1_pd", 0, _mm_set1_pd(2.5 + zero));
  show("set_epi32", 0, _mm_set_epi32(1 + zero, 2, 3, 4));
  show("setr_epi32", 0, _mm_setr_epi32(1 + zero, 2, 3, 4));
  show("set_epi16", 0, _mm_set_epi16(1 + zero, 2, 3, 4, 5, 6, 7, 8));
  show("setr_epi16", 0, _mm_setr_epi16(1 + zero, 2, 3, 4, 5, 6, 7, 8));
  show("set_epi8", 0, _mm_set_epi8(1 + zero, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16));
  show("setr_epi8", 0, _mm_setr_epi8(1 + zero, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16));
  show("set_ps", 0, _mm_set_ps(1.0f + zero, 2, 3, 4));
  show("setr_ps", 0, _mm_setr_ps(1.0f + zero, 2, 3, 4));
  show("set_pd", 0, _mm_set_pd(1.0 + zero, 2));
  show("setr_pd", 0, _mm_setr_pd(1.0 + zero, 2));
  show("setzero_ps", 0, _mm_setzero_ps());
  show("setzero_pd", 0, _mm_setzero_pd());
  show("setzero_si128", 0, _mm_setzero_si128());
  float fs[5] = { 1, 2, 3, 4, 5 }; double ds[3] = { 1, 2, 3 }; unsigned us[6] = { 1, 2, 3, 4, 5, 6 };
  show("load_ss", 0, _mm_load_ss(fs + 1));
  show("loadu_ps", 0, _mm_loadu_ps(fs + 1));
  show("loadu_pd", 0, _mm_loadu_pd(ds + 1));
  show("loadl_epi64", 0, _mm_loadl_epi64((const __m128i*)(us + 1)));
  show("loadu_si128", 0, _mm_loadu_si128((const __m128i*)(us + 1)));
  show("load_sd", 0, _mm_load_sd(ds + 1));
  float of[5] = { 0, 0, 0, 0, 0 }; double od[3] = { 0, 0, 0 }; unsigned ou[6] = { 0, 0, 0, 0, 0, 0 };
  _mm_store_ss(of + 1, vf(4)); _mm_storeu_ps(of, vf(5)); show("store_ss", 0, _mm_loadu_ps(of)); show("store_ss", 1, _mm_load_ss(of + 4));
  _mm_storeu_pd(od + 1, vd(5)); show("storeu_pd", 0, _mm_loadu_pd(od)); show("storeu_pd", 1, _mm_load_sd(od + 2));
  _mm_storeu_si32(ou + 1, vi(5)); _mm_storel_epi64((__m128i*)(ou + 2), vi(6)); show("storeu_si32", 0, _mm_loadu_si128((const __m128i*)ou));
  alignas(16) float af[4]; _mm_store_ps(af, vf(6)); show("store_ps", 0, _mm_loadu_ps(af));
  alignas(16) unsigned au[4]; _mm_store_si128((__m128i*)au, vi(6)); show("store_si128", 0, _mm_load_si128((const __m128i*)au));
  show("load_ps", 0, _mm_load_ps(af));''')
w('int main()\n{')
for f in fns: w('  %s();' % f)
w('  return 0;\n}')
print('\n'.join(out))
