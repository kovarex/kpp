/* minizip's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): built
   once with the C library and once converted with it (the C build with zlib's C build, zlib.lib;
   the converted one with the converted zlib, the scoped library it uses), it must print the same bytes.
   It runs:
   - unzip.h's, zip.h's and ioapi.h's macros, the converted minizip's constants (interface());
   - a zip written with zipOpen64, files stored and deflated at levels 1, 6 and 9 of pseudo-random
     sizes and contents (one empty), with extra fields, comments and a global comment, the bytes
     of the archive printed as its length and CRC; then read back with unzip: every entry's info,
     its data in pieces of pseudo-random sizes checked against what was written, unzLocateFile,
     file positions and the global comment;
   - every zip file the list file names (the tests' saves): each entry's name, sizes, method,
     CRC, and its data read in pieces and checked against that CRC.

     minizip LISTFILE > output.txt (harness.cmd minizip builds and compares both) */
#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* zipOpen64 and unzOpen64 take the path wide on Windows (as the game passes it). */
static const wchar_t* wide(const char* path)
{
  static wchar_t buffers[2][4096];
  static int next = 0;
  wchar_t* out = buffers[next++ % 2];
  mbstowcs(out, path, 4095);
  out[4095] = 0;
  return out;
}

static unsigned long long state = 0x9E3779B97F4A7C15ull;
static unsigned int nextRandom(void)
{
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (unsigned int)(state >> 16);
}

/* An entry read in pieces: its CRC, its bytes in *data when data is given (malloc'd), the count. */
static uint32_t readEntry(unzFile zip, unsigned char** data, uint64_t* count, int* rc)
{
  unsigned char buffer[70000];
  uint32_t crc = crc32(0, Z_NULL, 0);
  uint64_t total = 0, capacity = 0;
  if (data)
    *data = NULL;
  *rc = unzOpenCurrentFile(zip);
  if (*rc != UNZ_OK)
  {
    *count = 0;
    return 0;
  }
  for (;;)
  {
    uint32_t piece = 1 + nextRandom() % sizeof(buffer);
    int got = unzReadCurrentFile(zip, buffer, piece);
    if (got < 0)
    {
      *rc = got;
      break;
    }
    if (got == 0)
      break;
    crc = crc32(crc, buffer, (uInt)got);
    if (data)
    {
      if (total + got > capacity)
      {
        capacity = (total + got) * 2;
        *data = (unsigned char*)realloc(*data, (size_t)capacity);
      }
      memcpy(*data + total, buffer, got);
    }
    total += got;
  }
  int closed = unzCloseCurrentFile(zip);
  if (*rc == UNZ_OK)
    *rc = closed;
  *count = total;
  return crc;
}

static void writeAndRead(const char* path)
{
  enum { entries = 12 };
  unsigned char* contents[entries];
  uint32_t sizes[entries];
  char names[entries][32];
  zipFile zip = zipOpen64(wide(path), APPEND_STATUS_CREATE);
  if (!zip)
  {
    printf("zipOpen64 failed\n");
    return;
  }
  for (int k = 0; k < entries; ++k)
  {
    sizes[k] = k == 3 ? 0 : 1 + nextRandom() % (k % 4 == 0 ? 300000 : 20000);
    contents[k] = (unsigned char*)malloc(sizes[k] + 1);
    for (uint32_t b = 0; b < sizes[k]; ++b)
      contents[k][b] = (unsigned char)(k % 3 == 0 ? nextRandom() : "entity inserter belt\n"[(b + nextRandom() % 3) % 21]);
    snprintf(names[k], sizeof(names[k]), "dir%d/entry-%d.dat", k % 3, k);
    zip_fileinfo info;
    memset(&info, 0, sizeof(info));
    info.dos_date = 0x5A215C2F + k;
    info.external_fa = 0x20;
    static const int levels[] = {0, 1, 6, 9};
    int level = levels[k % 4];
    char extra[8] = {0x55, 0x54, 4, 0, 1, 2, 3, (char)k};
    int rc = zipOpenNewFileInZip(zip, names[k], &info, extra, sizeof(extra), extra, sizeof(extra), k % 2 ? "an entry's comment" : NULL,
                                 level ? Z_DEFLATED : 0, level);
    uint32_t written = 0;
    while (written < sizes[k])
    {
      uint32_t piece = 1 + nextRandom() % 9000;
      if (piece > sizes[k] - written)
        piece = sizes[k] - written;
      rc |= zipWriteInFileInZip(zip, contents[k] + written, piece);
      written += piece;
    }
    rc |= zipCloseFileInZip(zip);
    if (rc != ZIP_OK)
      printf("writing %s: %d\n", names[k], rc);
  }
  int closed = zipClose(zip, "the archive's comment");

  FILE* f = fopen(path, "rb");
  if (!f)
  {
    printf("written: %s not there\n", path);
    return;
  }
  fseek(f, 0, SEEK_END);
  long length = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char* archive = (unsigned char*)malloc(length);
  fread(archive, 1, length, f);
  fclose(f);
  printf("written: close %d, %ld bytes, crc %08lx\n", closed, length, crc32(0, archive, (uInt)length));
  free(archive);

  int error = 0;
  unzFile in = unzOpen64(wide(path), &error);
  if (!in)
  {
    printf("unzOpen64 failed: %d\n", error);
    return;
  }
  unz_global_info64 global;
  unzGetGlobalInfo64(in, &global);
  char comment[64] = "";
  int commentLength = unzGetGlobalComment(in, comment, sizeof(comment));
  printf("read: error %d, %llu entries, comment %d \"%s\"\n", error, (unsigned long long)global.number_entry, commentLength, comment);
  int k = 0;
  unz64_file_pos positions[entries];
  for (int rc = unzGoToFirstFile(in, NULL, 0, NULL); rc == UNZ_OK; rc = unzGoToNextFile(in, NULL, 0, NULL), ++k)
  {
    unz_file_info64 info;
    char name[64], entryComment[64];
    unsigned char extra[32];
    unzGetCurrentFileInfo64(in, &info, name, sizeof(name), extra, sizeof(extra), entryComment, sizeof(entryComment));
    if (k < entries)
      unzGetFilePos64(in, &positions[k]);
    unsigned char* data;
    uint64_t count;
    int readRc;
    uint32_t crc = readEntry(in, &data, &count, &readRc);
    int same = k < entries && count == sizes[k] && (count == 0 || memcmp(data, contents[k], (size_t)count) == 0);
    printf("  %s: method %u, %llu -> %llu, crc %08x %08x, date %08x, extra %u, comment \"%s\", read %d, %s\n", name, info.compression_method,
           (unsigned long long)info.uncompressed_size, (unsigned long long)info.compressed_size, info.crc, crc, info.dos_date,
           info.size_file_extra, info.size_file_comment ? entryComment : "", readRc, same ? "same" : "DIFFERENT");
    free(data);
  }
  int located = unzLocateFile(in, "dir2/entry-5.dat", NULL);
  unz_file_info64 info;
  char name[64];
  unzGetCurrentFileInfo64(in, &info, name, sizeof(name), NULL, 0, NULL, 0);
  int moved = unzGoToFilePos64(in, &positions[1]);
  char second[64];
  unzGetCurrentFileInfo64(in, &info, second, sizeof(second), NULL, 0, NULL, 0);
  printf("locate %d %s, position %d %s, close %d\n", located, name, moved, second, unzClose(in));
  remove(path);
  for (int e = 0; e < entries; ++e)
    free(contents[e]);
}

static void zipFileListed(const char* path)
{
  int error = 0;
  unzFile in = unzOpen64(wide(path), &error);
  if (!in)
  {
    printf("%s: unzOpen64 %d\n", path, error);
    return;
  }
  unz_global_info64 global;
  unzGetGlobalInfo64(in, &global);
  printf("%s: %llu entries\n", path, (unsigned long long)global.number_entry);
  for (int rc = unzGoToFirstFile(in, NULL, 0, NULL); rc == UNZ_OK; rc = unzGoToNextFile(in, NULL, 0, NULL))
  {
    unz_file_info64 info;
    char name[512];
    unzGetCurrentFileInfo64(in, &info, name, sizeof(name), NULL, 0, NULL, 0);
    uint64_t count;
    int readRc;
    uint32_t crc = readEntry(in, NULL, &count, &readRc);
    printf("  %s: method %u, %llu -> %llu, crc %08x, read %llu %08x %d%s\n", name, info.compression_method, (unsigned long long)info.uncompressed_size,
           (unsigned long long)info.compressed_size, info.crc, (unsigned long long)count, crc, readRc, crc == info.crc ? "" : " CRC DIFFERS");
  }
  unzClose(in);
}

/* unzip.h's, zip.h's and ioapi.h's macros, which the converted minizip has as constants of its scope
   (design.md 16): each constant's value, size and signedness. */
static void constant(const char* name, long long value, size_t size, int isSigned)
{
  printf("%s = %lld, %u bytes, %s\n", name, value, (unsigned)size, isSigned ? "signed" : "unsigned");
}

static void interface(void)
{
  constant("UNZ_OK", (long long)UNZ_OK, sizeof(UNZ_OK), UNZ_OK * 0 - 1 < 0);
  constant("UNZ_END_OF_LIST_OF_FILE", (long long)UNZ_END_OF_LIST_OF_FILE, sizeof(UNZ_END_OF_LIST_OF_FILE), UNZ_END_OF_LIST_OF_FILE * 0 - 1 < 0);
  constant("UNZ_ERRNO", (long long)UNZ_ERRNO, sizeof(UNZ_ERRNO), UNZ_ERRNO * 0 - 1 < 0);
  constant("UNZ_EOF", (long long)UNZ_EOF, sizeof(UNZ_EOF), UNZ_EOF * 0 - 1 < 0);
  constant("UNZ_PARAMERROR", (long long)UNZ_PARAMERROR, sizeof(UNZ_PARAMERROR), UNZ_PARAMERROR * 0 - 1 < 0);
  constant("UNZ_BADZIPFILE", (long long)UNZ_BADZIPFILE, sizeof(UNZ_BADZIPFILE), UNZ_BADZIPFILE * 0 - 1 < 0);
  constant("UNZ_INTERNALERROR", (long long)UNZ_INTERNALERROR, sizeof(UNZ_INTERNALERROR), UNZ_INTERNALERROR * 0 - 1 < 0);
  constant("UNZ_CRCERROR", (long long)UNZ_CRCERROR, sizeof(UNZ_CRCERROR), UNZ_CRCERROR * 0 - 1 < 0);
  constant("UNZ_BADPASSWORD", (long long)UNZ_BADPASSWORD, sizeof(UNZ_BADPASSWORD), UNZ_BADPASSWORD * 0 - 1 < 0);
  constant("Z_BZIP2ED", (long long)Z_BZIP2ED, sizeof(Z_BZIP2ED), Z_BZIP2ED * 0 - 1 < 0);
  constant("ZIP_OK", (long long)ZIP_OK, sizeof(ZIP_OK), ZIP_OK * 0 - 1 < 0);
  constant("ZIP_EOF", (long long)ZIP_EOF, sizeof(ZIP_EOF), ZIP_EOF * 0 - 1 < 0);
  constant("ZIP_ERRNO", (long long)ZIP_ERRNO, sizeof(ZIP_ERRNO), ZIP_ERRNO * 0 - 1 < 0);
  constant("ZIP_PARAMERROR", (long long)ZIP_PARAMERROR, sizeof(ZIP_PARAMERROR), ZIP_PARAMERROR * 0 - 1 < 0);
  constant("ZIP_BADZIPFILE", (long long)ZIP_BADZIPFILE, sizeof(ZIP_BADZIPFILE), ZIP_BADZIPFILE * 0 - 1 < 0);
  constant("ZIP_INTERNALERROR", (long long)ZIP_INTERNALERROR, sizeof(ZIP_INTERNALERROR), ZIP_INTERNALERROR * 0 - 1 < 0);
  constant("DEF_MEM_LEVEL", (long long)DEF_MEM_LEVEL, sizeof(DEF_MEM_LEVEL), DEF_MEM_LEVEL * 0 - 1 < 0);
  constant("APPEND_STATUS_CREATE", (long long)APPEND_STATUS_CREATE, sizeof(APPEND_STATUS_CREATE), APPEND_STATUS_CREATE * 0 - 1 < 0);
  constant("APPEND_STATUS_CREATEAFTER", (long long)APPEND_STATUS_CREATEAFTER, sizeof(APPEND_STATUS_CREATEAFTER), APPEND_STATUS_CREATEAFTER * 0 - 1 < 0);
  constant("APPEND_STATUS_ADDINZIP", (long long)APPEND_STATUS_ADDINZIP, sizeof(APPEND_STATUS_ADDINZIP), APPEND_STATUS_ADDINZIP * 0 - 1 < 0);
  constant("ZLIB_FILEFUNC_SEEK_CUR", (long long)ZLIB_FILEFUNC_SEEK_CUR, sizeof(ZLIB_FILEFUNC_SEEK_CUR), ZLIB_FILEFUNC_SEEK_CUR * 0 - 1 < 0);
  constant("ZLIB_FILEFUNC_SEEK_END", (long long)ZLIB_FILEFUNC_SEEK_END, sizeof(ZLIB_FILEFUNC_SEEK_END), ZLIB_FILEFUNC_SEEK_END * 0 - 1 < 0);
  constant("ZLIB_FILEFUNC_SEEK_SET", (long long)ZLIB_FILEFUNC_SEEK_SET, sizeof(ZLIB_FILEFUNC_SEEK_SET), ZLIB_FILEFUNC_SEEK_SET * 0 - 1 < 0);
  constant("ZLIB_FILEFUNC_MODE_READ", (long long)ZLIB_FILEFUNC_MODE_READ, sizeof(ZLIB_FILEFUNC_MODE_READ), ZLIB_FILEFUNC_MODE_READ * 0 - 1 < 0);
  constant("ZLIB_FILEFUNC_MODE_WRITE", (long long)ZLIB_FILEFUNC_MODE_WRITE, sizeof(ZLIB_FILEFUNC_MODE_WRITE), ZLIB_FILEFUNC_MODE_WRITE * 0 - 1 < 0);
  constant("ZLIB_FILEFUNC_MODE_READWRITEFILTER", (long long)ZLIB_FILEFUNC_MODE_READWRITEFILTER, sizeof(ZLIB_FILEFUNC_MODE_READWRITEFILTER), ZLIB_FILEFUNC_MODE_READWRITEFILTER * 0 - 1 < 0);
  constant("ZLIB_FILEFUNC_MODE_EXISTING", (long long)ZLIB_FILEFUNC_MODE_EXISTING, sizeof(ZLIB_FILEFUNC_MODE_EXISTING), ZLIB_FILEFUNC_MODE_EXISTING * 0 - 1 < 0);
  constant("ZLIB_FILEFUNC_MODE_CREATE", (long long)ZLIB_FILEFUNC_MODE_CREATE, sizeof(ZLIB_FILEFUNC_MODE_CREATE), ZLIB_FILEFUNC_MODE_CREATE * 0 - 1 < 0);
}

int main(int argc, char** argv)
{
  interface();
  char path[4096] = "minizip-harness.zip";
  if (argc >= 2)
  {
    snprintf(path, sizeof(path), "%s", argv[1]);
    char* slash = strrchr(path, '\\');
    snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? (size_t)(slash + 1 - path) : 0), "%s", "minizip-harness.zip");
  }
  for (int round = 0; round < 3; ++round)
    writeAndRead(path);
  if (argc < 2)
    return 0;
  FILE* list = fopen(argv[1], "r");
  if (!list)
  {
    printf("cannot open %s\n", argv[1]);
    return 1;
  }
  char line[4096];
  while (fgets(line, sizeof(line), list))
  {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
    if (n > 0)
      zipFileListed(line);
  }
  fclose(list);
  return 0;
}
