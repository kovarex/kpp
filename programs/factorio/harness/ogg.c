/* ogg's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): built once
   with the C library and once converted with it, it must print the same bytes. It runs:
   - the bit packers, both orders (oggpack_*, oggpackB_*): writes of every width from 0 to 32,
     copies, truncations and alignments, then reads, looks and advances back over the buffer and
     past its end;
   - framing: packets of pseudo-random sizes (around the 255-byte lacing edges too) through
     ogg_stream_packetin and ogg_stream_pageout, pageout_fill and flush, every page's fields and
     bytes printed, then the pages through ogg_sync in pieces of pseudo-random sizes, back into
     packets, compared with what went in;
   - every .ogg file the list file names (one path per line), read through ogg_sync in pieces and
     decoded into pages and packets: one line per file with its counts and a hash of everything.

     ogg LISTFILE > output.txt (harness.cmd ogg builds and compares both) */
#include <ogg/ogg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long state = 0x9E3779B97F4A7C15ull;
static unsigned int nextRandom(void)
{
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (unsigned int)(state >> 16);
}

/* FNV-1a over bytes, continued from hash. */
static unsigned long long hashBytes(unsigned long long hash, const unsigned char* bytes, long length)
{
  long i;
  for (i = 0; i < length; ++i)
  {
    hash ^= bytes[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}
static unsigned long long hashValue(unsigned long long hash, long long value)
{
  unsigned char bytes[8];
  int i;
  for (i = 0; i < 8; ++i)
    bytes[i] = (unsigned char)(value >> (8 * i));
  return hashBytes(hash, bytes, 8);
}

/* One bit packer, LSb first (big = 0) or MSb first (big = 1). */
static void packer(int big, int round)
{
  oggpack_buffer b;
  unsigned char source[64];
  unsigned long long hash = 0xCBF29CE484222325ull;
  long i, writes = 200 + round * 300;
  int k;
  if (big)
    oggpackB_writeinit(&b);
  else
    oggpack_writeinit(&b);
  for (i = 0; i < writes; ++i)
  {
    unsigned int choice = nextRandom() % 40;
    if (choice < 33)
    {
      int bits = (int)choice;
      unsigned long value = ((unsigned long)nextRandom() << 16) ^ nextRandom();
      if (bits < 32)
        value &= (1ul << bits) - 1;
      if (big)
        oggpackB_write(&b, value, bits);
      else
        oggpack_write(&b, value, bits);
    }
    else if (choice < 34)
    {
      /* (Rare and short: an aligned copy that has to grow the buffer clears it, as this ogg's
         oggpack_writecopy_helper tests `endbyte + bytes + BUFFER_INCREMENT > storage` for an error,
         and every later write of the round then writes nothing.) */
      long bits = (long)(nextRandom() % 128);
      for (k = 0; k < (int)sizeof(source); ++k)
        source[k] = (unsigned char)nextRandom();
      if (big)
        oggpackB_writecopy(&b, source, bits);
      else
        oggpack_writecopy(&b, source, bits);
    }
    else if (choice < 37)
    {
      if (big)
        oggpackB_writealign(&b);
      else
        oggpack_writealign(&b);
    }
    else
    {
      long now = big ? oggpackB_bits(&b) : oggpack_bits(&b);
      long keep = now - (long)(nextRandom() % 64);
      if (keep < 0)
        keep = 0;
      if (big)
        oggpackB_writetrunc(&b, keep);
      else
        oggpack_writetrunc(&b, keep);
    }
  }
  {
    long bytes = big ? oggpackB_bytes(&b) : oggpack_bytes(&b);
    long bits = big ? oggpackB_bits(&b) : oggpack_bits(&b);
    unsigned char* buffer = big ? oggpackB_get_buffer(&b) : oggpack_get_buffer(&b);
    unsigned char* copy = (unsigned char*)malloc(bytes + 1);
    oggpack_buffer r;
    unsigned long long readHash = 0xCBF29CE484222325ull;
    memcpy(copy, buffer, bytes);
    hash = hashBytes(hash, buffer, bytes);
    printf("pack %s round %d: %ld writes, %ld bytes, %ld bits, check %d, buffer %016llx\n", big ? "B" : "L", round, writes, bytes, bits,
           big ? oggpackB_writecheck(&b) : oggpack_writecheck(&b), hash);
    if (big)
      oggpackB_readinit(&r, copy, (int)bytes);
    else
      oggpack_readinit(&r, copy, (int)bytes);
    for (i = 0; i < writes + 50; ++i)
    {
      unsigned int choice = nextRandom() % 6;
      int bits = (int)(nextRandom() % 33);
      long value;
      if (choice == 0)
        value = big ? oggpackB_look(&r, bits) : oggpack_look(&r, bits);
      else if (choice == 1)
      {
        value = big ? oggpackB_look1(&r) : oggpack_look1(&r);
        if (big)
          oggpackB_adv1(&r);
        else
          oggpack_adv1(&r);
      }
      else if (choice == 2)
      {
        value = big ? oggpackB_look(&r, bits) : oggpack_look(&r, bits);
        if (big)
          oggpackB_adv(&r, bits);
        else
          oggpack_adv(&r, bits);
      }
      else if (choice == 3)
        value = big ? oggpackB_read1(&r) : oggpack_read1(&r);
      else
        value = big ? oggpackB_read(&r, bits) : oggpack_read(&r, bits);
      readHash = hashValue(readHash, value);
      readHash = hashValue(readHash, big ? oggpackB_bits(&r) : oggpack_bits(&r));
    }
    printf("unpack %s round %d: at %ld bits, %ld bytes, reads %016llx\n", big ? "B" : "L", round, big ? oggpackB_bits(&r) : oggpack_bits(&r),
           big ? oggpackB_bytes(&r) : oggpack_bytes(&r), readHash);
    free(copy);
  }
  if (big)
  {
    oggpackB_reset(&b);
    printf("reset B: %ld bytes\n", oggpackB_bytes(&b));
    oggpackB_writeclear(&b);
  }
  else
  {
    oggpack_reset(&b);
    printf("reset L: %ld bytes\n", oggpack_bytes(&b));
    oggpack_writeclear(&b);
  }
}

static void printPage(const char* what, ogg_page* page)
{
  unsigned long long hash = hashBytes(0xCBF29CE484222325ull, page->header, page->header_len);
  hash = hashBytes(hash, page->body, page->body_len);
  printf("%s: header %ld body %ld version %d continued %d bos %d eos %d granule %lld serial %d page %ld packets %d bytes %016llx\n", what,
         page->header_len, page->body_len, ogg_page_version(page), ogg_page_continued(page), ogg_page_bos(page), ogg_page_eos(page),
         (long long)ogg_page_granulepos(page), ogg_page_serialno(page), ogg_page_pageno(page), ogg_page_packets(page), hash);
}

/* Packets through a stream into pages, the pages through ogg_sync back into packets. */
static void framing(int round)
{
  int packets = 20 + round * 40;
  long sizes[400];
  unsigned char* data[400];
  unsigned char* stream = NULL;
  long streamLength = 0;
  ogg_stream_state out, in;
  ogg_sync_state sync;
  ogg_page page;
  ogg_packet packet;
  int i, pages = 0, matched = 0, got = 0;
  long fed = 0;
  ogg_stream_init(&out, 1000 + round);
  for (i = 0; i < packets; ++i)
  {
    unsigned int kind = nextRandom() % 5;
    long size = kind == 0 ? (long)(nextRandom() % 600) : kind == 1 ? 255 * (long)(1 + nextRandom() % 4) - 1 + (long)(nextRandom() % 3)
              : kind == 2 ? (long)(nextRandom() % 70000) : (long)(nextRandom() % 4000);
    long k;
    sizes[i] = size;
    data[i] = (unsigned char*)malloc(size + 1);
    for (k = 0; k < size; ++k)
      data[i][k] = (unsigned char)nextRandom();
    packet.packet = data[i];
    packet.bytes = size;
    packet.b_o_s = i == 0;
    packet.e_o_s = i == packets - 1;
    packet.granulepos = (ogg_int64_t)i * 1024 + (nextRandom() % 7 == 0 ? -1 : 0);
    packet.packetno = i;
    ogg_stream_packetin(&out, &packet);
    for (;;)
    {
      unsigned int how = nextRandom() % 4;
      int result = how == 0 ? ogg_stream_flush(&out, &page) : how == 1 ? ogg_stream_pageout_fill(&out, &page, (int)(nextRandom() % 9000))
                                                           : ogg_stream_pageout(&out, &page);
      if (!result)
        break;
      ++pages;
      printPage("page", &page);
      stream = (unsigned char*)realloc(stream, streamLength + page.header_len + page.body_len);
      memcpy(stream + streamLength, page.header, page.header_len);
      memcpy(stream + streamLength + page.header_len, page.body, page.body_len);
      streamLength += page.header_len + page.body_len;
    }
  }
  while (ogg_stream_flush(&out, &page))
  {
    ++pages;
    printPage("flushed", &page);
    stream = (unsigned char*)realloc(stream, streamLength + page.header_len + page.body_len);
    memcpy(stream + streamLength, page.header, page.header_len);
    memcpy(stream + streamLength + page.header_len, page.body, page.body_len);
    streamLength += page.header_len + page.body_len;
  }
  printf("framing round %d: %d packets, %d pages, %ld bytes, eos %d, check %d\n", round, packets, pages, streamLength, ogg_stream_eos(&out),
         ogg_stream_check(&out));
  ogg_stream_clear(&out);

  ogg_sync_init(&sync);
  ogg_stream_init(&in, 1000 + round);
  for (;;)
  {
    long piece = 1 + (long)(nextRandom() % 5000);
    char* buffer;
    if (piece > streamLength - fed)
      piece = streamLength - fed;
    if (piece > 0)
    {
      buffer = ogg_sync_buffer(&sync, piece);
      memcpy(buffer, stream + fed, piece);
      ogg_sync_wrote(&sync, piece);
      fed += piece;
    }
    while (ogg_sync_pageout(&sync, &page) == 1)
    {
      ogg_stream_pagein(&in, &page);
      while (ogg_stream_packetout(&in, &packet) == 1)
      {
        if (got < packets && packet.bytes == sizes[got] && memcmp(packet.packet, data[got], sizes[got]) == 0)
          ++matched;
        else
          printf("packet %d differs: %ld bytes\n", got, packet.bytes);
        printf("packet %d: %ld bytes bos %ld eos %ld granule %lld no %lld\n", got, packet.bytes, packet.b_o_s, packet.e_o_s,
               (long long)packet.granulepos, (long long)packet.packetno);
        ++got;
      }
    }
    if (fed >= streamLength)
      break;
  }
  printf("deframing round %d: %d of %d packets back, %d match, eos %d\n", round, got, packets, matched, ogg_stream_eos(&in));
  ogg_stream_clear(&in);
  ogg_sync_clear(&sync);
  for (i = 0; i < packets; ++i)
    free(data[i]);
  free(stream);
}

/* A file through ogg_sync and the streams it holds. */
static void file(const char* path)
{
  FILE* f = fopen(path, "rb");
  ogg_sync_state sync;
  ogg_stream_state streams[8];
  int serials[8];
  int streamCount = 0;
  ogg_page page;
  ogg_packet packet;
  long pages = 0, packets = 0, skipped = 0, bytes = 0;
  unsigned long long hash = 0xCBF29CE484222325ull;
  int i;
  if (!f)
  {
    printf("%s: cannot open\n", path);
    return;
  }
  ogg_sync_init(&sync);
  for (;;)
  {
    long piece = 1 + (long)(nextRandom() % 16384);
    char* buffer = ogg_sync_buffer(&sync, piece);
    long n = (long)fread(buffer, 1, piece, f);
    long seek;
    ogg_sync_wrote(&sync, n);
    bytes += n;
    while ((seek = ogg_sync_pageseek(&sync, &page)) != 0)
    {
      int s;
      if (seek < 0)
      {
        skipped += -seek;
        continue;
      }
      ++pages;
      hash = hashBytes(hash, page.header, page.header_len);
      hash = hashBytes(hash, page.body, page.body_len);
      for (s = 0; s < streamCount && serials[s] != ogg_page_serialno(&page); ++s)
        ;
      if (s == streamCount && streamCount < 8)
      {
        serials[s] = ogg_page_serialno(&page);
        ogg_stream_init(&streams[s], serials[s]);
        ++streamCount;
      }
      if (s < streamCount)
      {
        hash = hashValue(hash, ogg_stream_pagein(&streams[s], &page));
        while (ogg_stream_packetout(&streams[s], &packet) != 0)
        {
          ++packets;
          hash = hashBytes(hash, packet.packet, packet.bytes);
          hash = hashValue(hash, packet.granulepos);
          hash = hashValue(hash, packet.packetno);
          hash = hashValue(hash, packet.b_o_s * 2 + packet.e_o_s);
        }
      }
    }
    if (n == 0)
      break;
  }
  fclose(f);
  printf("%s: %ld bytes, %ld pages, %ld packets, %ld skipped, %d streams, %016llx\n", path, bytes, pages, packets, skipped, streamCount, hash);
  for (i = 0; i < streamCount; ++i)
    ogg_stream_clear(&streams[i]);
  ogg_sync_clear(&sync);
}

int main(int argc, char** argv)
{
  int round;
  char line[4096];
  FILE* list;
  for (round = 0; round < 6; ++round)
  {
    packer(0, round);
    packer(1, round);
    framing(round);
  }
  if (argc < 2)
    return 0;
  list = fopen(argv[1], "r");
  if (!list)
  {
    printf("cannot open %s\n", argv[1]);
    return 1;
  }
  while (fgets(line, sizeof(line), list))
  {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
    if (n > 0)
      file(line);
  }
  fclose(list);
  return 0;
}
