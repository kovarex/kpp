// tests/gdiplus_smoke.kpp's steps against the SDK's gdiplus.h, whose output is the expected one
// (tests/gdiplus_smoke.expected). Build: cl /nologo /EHsc gdiplus_smoke.cpp gdiplus.lib shlwapi.lib ole32.lib
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

using namespace Gdiplus;

int offset(const void* object, const void* member) { return (int)((const char*)member - (const char*)object); }

int encoderClsid(const WCHAR* format, CLSID* clsid)
{
  UINT count = 0;
  UINT size = 0;
  GetImageEncodersSize(&count, &size);
  if (size == 0)
    return -1;
  ImageCodecInfo* codecs = (ImageCodecInfo*)malloc(size);
  GetImageEncoders(count, size, codecs);
  int found = -1;
  for (UINT i = 0; i < count; ++i)
    if (wcscmp(codecs[i].MimeType, format) == 0)
    {
      *clsid = codecs[i].Clsid;
      found = int(i);
    }
  free(codecs);
  return found;
}

void rewindStream(IStream* stream)
{
  LARGE_INTEGER zero = {};
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
}

void printPixels(Bitmap& bitmap)
{
  Rect rect(0, 0, int(bitmap.GetWidth()), int(bitmap.GetHeight()));
  BitmapData data = {};
  Status status = bitmap.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data);
  printf("read lock %d: %ux%u stride %d format %x\n", int(status), data.Width, data.Height, data.Stride, data.PixelFormat);
  for (UINT y = 0; y < data.Height; ++y)
  {
    const unsigned char* row = (const unsigned char*)data.Scan0 + int(y) * data.Stride;
    for (UINT x = 0; x < data.Width; ++x)
      printf(" %02x%02x%02x%02x", row[4 * x + 3], row[4 * x + 2], row[4 * x + 1], row[4 * x]);
    printf("\n");
  }
  printf("read unlock %d\n", int(bitmap.UnlockBits(&data)));
}

int main()
{
  GdiplusStartupInput input;
  Rect r;
  BitmapData data;
  ImageCodecInfo codec;
  EncoderParameter parameter;
  EncoderParameters parameters;
  printf("GdiplusStartupInput %d version %d callback %d suppress %d %d\n", (int)sizeof(GdiplusStartupInput), int(input.GdiplusVersion),
         offset(&input, &input.DebugEventCallback), offset(&input, &input.SuppressBackgroundThread), offset(&input, &input.SuppressExternalCodecs));
  printf("Rect %d BitmapData %d scan0 %d reserved %d\n", (int)sizeof(Rect), (int)sizeof(BitmapData), offset(&data, &data.Scan0), offset(&data, &data.Reserved));
  printf("ImageCodecInfo %d mime %d flags %d pattern %d\n", (int)sizeof(ImageCodecInfo), offset(&codec, &codec.MimeType), offset(&codec, &codec.Flags),
         offset(&codec, &codec.SigPattern));
  printf("EncoderParameter %d value %d EncoderParameters %d parameter %d\n", (int)sizeof(EncoderParameter), offset(&parameter, &parameter.Value),
         (int)sizeof(EncoderParameters), offset(&parameters, &parameters.Parameter[0]));
  printf("formats %x %x size %u %u\n", PixelFormat32bppARGB, PixelFormat24bppRGB, GetPixelFormatSize(PixelFormat32bppARGB), GetPixelFormatSize(PixelFormat24bppRGB));
  printf("quality %08lx-%04x\n", EncoderQuality.Data1, unsigned(EncoderQuality.Data2));

  ULONG_PTR token = 0;
  printf("startup %d\n", int(GdiplusStartup(&token, &input, nullptr)));

  UINT count = 0;
  UINT size = 0;
  GetImageEncodersSize(&count, &size);
  ImageCodecInfo* codecs = (ImageCodecInfo*)malloc(size);
  printf("encoders %d\n", int(GetImageEncoders(count, size, codecs)));
  for (UINT i = 0; i < count; ++i)
    printf("  %ls\n", codecs[i].MimeType);
  free(codecs);

  CLSID png;
  CLSID jpeg;
  printf("png %d jpeg %d\n", encoderClsid(L"image/png", &png) >= 0 ? 1 : 0, encoderClsid(L"image/jpeg", &jpeg) >= 0 ? 1 : 0);

  Bitmap* bitmap = new Bitmap(3, 2, PixelFormat32bppARGB);
  printf("bitmap %d: %ux%u\n", int(bitmap->GetLastStatus()), bitmap->GetWidth(), bitmap->GetHeight());
  Rect rect(0, 0, 3, 2);
  BitmapData written = {};
  printf("write lock %d stride %d\n", int(bitmap->LockBits(&rect, ImageLockModeWrite, PixelFormat32bppARGB, &written)), written.Stride);
  for (int y = 0; y < 2; ++y)
  {
    unsigned char* row = (unsigned char*)written.Scan0 + y * written.Stride;
    for (int x = 0; x < 3; ++x)
    {
      row[4 * x] = (unsigned char)(7 * (x + y) + 5);
      row[4 * x + 1] = (unsigned char)(100 * y + 20);
      row[4 * x + 2] = (unsigned char)(40 * x + 10);
      row[4 * x + 3] = (unsigned char)(255 - 60 * x);
    }
  }
  printf("write unlock %d\n", int(bitmap->UnlockBits(&written)));

  IStream* stream = SHCreateMemStream(nullptr, 0);
  printf("png save %d\n", int(bitmap->Save(stream, &png, nullptr)));
  rewindStream(stream);
  Bitmap* loaded = Bitmap::FromStream(stream, FALSE);
  printf("png load %d: %ux%u\n", int(loaded->GetLastStatus()), loaded->GetWidth(), loaded->GetHeight());
  printPixels(*loaded);
  delete loaded;
  stream->Release();

  ULONG quality = 90;
  EncoderParameters jpegParameters = {};
  jpegParameters.Count = 1;
  jpegParameters.Parameter[0].Guid = EncoderQuality;
  jpegParameters.Parameter[0].Type = EncoderParameterValueTypeLong;
  jpegParameters.Parameter[0].NumberOfValues = 1;
  jpegParameters.Parameter[0].Value = &quality;
  IStream* jpegStream = SHCreateMemStream(nullptr, 0);
  printf("jpeg save %d\n", int(bitmap->Save(jpegStream, &jpeg, &jpegParameters)));
  rewindStream(jpegStream);
  Bitmap* jpegLoaded = Bitmap::FromStream(jpegStream, TRUE);
  printf("jpeg load %d: %ux%u\n", int(jpegLoaded->GetLastStatus()), jpegLoaded->GetWidth(), jpegLoaded->GetHeight());
  delete jpegLoaded;
  jpegStream->Release();
  delete bitmap;

  const char* text = "not an image";
  IStream* garbage = SHCreateMemStream((const BYTE*)text, UINT(strlen(text)));
  Bitmap* none = Bitmap::FromStream(garbage, FALSE);
  Status loadStatus = none->GetLastStatus();
  UINT width = none->GetWidth();
  printf("garbage load %d width %u then %d\n", int(loadStatus), width, int(none->GetLastStatus()));
  delete none;
  garbage->Release();

  GdiplusShutdown(token);
  printf("done\n");
  return 0;
}
