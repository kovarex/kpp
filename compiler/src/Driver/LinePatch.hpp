#pragma once
// The driver: a compile server's build whose edit only moved lines (--debug-info) patches the line
// tables of the kept objects instead of compiling their modules again (kovarex, 2026-11-10: "Line
// numbers should be just patched"). The declarations, in namespace driver; the definitions in
// LinePatch.cpp.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace driver
{
  // A file an edit moved lines of: its absolute path, as the debug info names it (codegen's
  // debugFileOf), and its lines (from 1) before and after, sorted by the line before, for every line a
  // token the edit kept is on. A line whose tokens are on two lines now is left out: an entry of it
  // cannot be patched.
  struct LineMap
  {
    std::string file;
    std::vector<std::pair<uint32_t, uint32_t>> lines;
  };

  // The CodeView line tables of a COFF object (its .debug$S sections' line subsections), each entry of a
  // file the maps name given its line after, and so each of its asserts' lines (the file's assert line
  // section, lineSlotSection in codegen.hpp): true when every one was, the object rewritten in place.
  // False, why said and the object as it was, when an entry's line is in no map's lines (the module is
  // then compiled), or the object is no COFF this reads.
  bool patchLineTables(const std::string& object, const std::vector<LineMap>& maps, std::string& why);
}
