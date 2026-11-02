// open-vcdiff's decoding, which the game's Updater::processDiff applies a patch with, as a C function
// for kpp: the library (libraries/open-vcdiff, open-vcdiff.lib in the game's Release builds) is C++.
// overrides/added/src/OpenVcdiff.kpp declares the game's open_vcdiff::VCDiffDecoder on this one.
#include <open-vcdiff/google/vcdecoder.h>
#include <stdlib.h>
#include <string.h>
#include <string>

// Decodes the VCDIFF encoding against the dictionary: 1 and, in *output, a malloc'ed block of
// *outputSize bytes the caller frees; 0 for an encoding that is not well formed.
extern "C" int kppVcdiffDecode(const char* dictionary, size_t dictionarySize, const char* encoding, size_t encodingSize, char** output,
                               size_t* outputSize)
{
  open_vcdiff::VCDiffDecoder decoder;
  std::string target;
  if (!decoder.Decode(dictionary, dictionarySize, std::string(encoding, encodingSize), &target))
    return 0;
  *output = (char*)malloc(target.size() + 1);
  memcpy(*output, target.data(), target.size());
  *outputSize = target.size();
  return 1;
}
