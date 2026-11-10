// The driver: patching a COFF object's CodeView line tables for lines an edit moved (LinePatch.hpp).
#include "LinePatch.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace driver
{
  namespace
  {
    uint32_t read32(const std::string& b, size_t at)
    {
      uint32_t v;
      memcpy(&v, b.data() + at, 4);
      return v;
    }

    uint16_t read16(const std::string& b, size_t at)
    {
      uint16_t v;
      memcpy(&v, b.data() + at, 2);
      return v;
    }

    void write32(std::string& b, size_t at, uint32_t v) { memcpy(b.data() + at, &v, 4); }

    // A COFF section's checksum as LLVM's object writer computes it (JamCRC from 0: CRC-32's table, no
    // final complement), which its section symbol's definition record holds.
    uint32_t sectionChecksum(const char* data, size_t size)
    {
      static const auto table = []
      {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i)
        {
          uint32_t c = i;
          for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
          t[i] = c;
        }
        return t;
      }();
      uint32_t crc = 0;
      for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ (unsigned char)data[i]) & 0xff] ^ (crc >> 8);
      return crc;
    }

    // A path as Windows compares it: case and separators aside.
    std::string pathKey(std::string_view path)
    {
      std::string out(path);
      for (char& c : out)
        c = c == '/' ? '\\' : char(tolower((unsigned char)c));
      return out;
    }

    // A CodeView subsection of a .debug$S section: its kind (the ignore bit off) and its data's place.
    struct Subsection
    {
      uint32_t kind;
      size_t data, size;
      size_t section; // (its .debug$S section's index, from 0)
    };

    // The subsections of the .debug$S section at [start, start + size) of the object; false when it is
    // not what CodeView's C13 form writes.
    bool subsectionsOf(const std::string& b, size_t start, size_t size, size_t section, std::vector<Subsection>& out)
    {
      if (size < 4 || read32(b, start) != 4) // CV_SIGNATURE_C13
        return false;
      size_t at = start + 4, end = start + size;
      while (at + 8 <= end)
      {
        uint32_t kind = read32(b, at) & 0x7fffffff, length = read32(b, at + 4);
        if (at + 8 + length > end)
          return false;
        out.push_back({kind, at + 8, length, section});
        at += 8 + ((length + 3) & ~size_t(3));
      }
      return true;
    }
  }

  bool patchLineTables(const std::string& object, const std::vector<LineMap>& maps, std::string& why)
  {
    std::string b;
    {
      FILE* f = fopen(object.c_str(), "rb");
      if (!f)
        return why = "the object cannot be read", false;
      char buffer[1 << 16];
      for (size_t n; (n = fread(buffer, 1, sizeof buffer, f)) > 0;)
        b.append(buffer, n);
      fclose(f);
    }
    // The COFF header, or the big object's (/bigobj, which clang writes past 65,279 sections).
    if (b.size() < 20)
      return why = "no COFF header", false;
    size_t sections = 0, headers = 0, symbols = 0, symbolCount = 0, symbolSize = 18;
    bool big = read16(b, 0) == 0 && read16(b, 2) == 0xffff;
    if (big)
    {
      if (b.size() < 56)
        return why = "no big object header", false;
      sections = read32(b, 44);
      headers = 56;
      symbols = read32(b, 48);
      symbolCount = read32(b, 52);
      symbolSize = 20;
    }
    else
    {
      sections = read16(b, 2);
      headers = 20 + read16(b, 16);
      symbols = read32(b, 8);
      symbolCount = read32(b, 12);
    }
    if (symbols + symbolCount * symbolSize > b.size())
      return why = "the symbol table runs past the file", false;
    if (headers + sections * 40 > b.size())
      return why = "the section headers run past the file", false;
    // Every .debug$S section's subsections; the one string table and file checksum table they share.
    std::vector<Subsection> all;
    for (size_t s = 0; s < sections; ++s)
    {
      size_t h = headers + s * 40;
      if (memcmp(b.data() + h, ".debug$S", 8) != 0)
        continue;
      size_t size = read32(b, h + 16), at = read32(b, h + 20);
      if (at + size > b.size() || !subsectionsOf(b, at, size, s, all))
        return why = "a .debug$S section is not CodeView's", false;
    }
    const Subsection* strings = nullptr;
    const Subsection* checksums = nullptr;
    for (const Subsection& sub : all)
    {
      if (sub.kind != 0xf3 && sub.kind != 0xf4)
        continue;
      const Subsection*& slot = sub.kind == 0xf3 ? strings : checksums;
      if (slot)
        return why = "two string or file checksum tables", false;
      slot = &sub;
    }
    if (!strings || !checksums)
      return why = "no string or file checksum table", false;
    // The file checksum entries whose files the maps name, by their offset in the table (what a line
    // block names its file by).
    std::vector<std::pair<uint32_t, const LineMap*>> mapped;
    for (size_t at = 0; at + 6 <= checksums->size;)
    {
      uint32_t name = read32(b, checksums->data + at);
      uint8_t bytes = uint8_t(b[checksums->data + at + 4]);
      if (name >= strings->size)
        return why = "a file name past the string table", false;
      const char* text = b.data() + strings->data + name;
      std::string key = pathKey(std::string_view(text, strnlen(text, strings->size - name)));
      for (const LineMap& m : maps)
        if (pathKey(m.file) == key)
          mapped.push_back({uint32_t(at), &m});
      at += (6 + bytes + 3) & ~size_t(3);
    }
    if (mapped.empty())
      return true; // (nothing of the moved files here: the object stays as it is)
    auto mapOf = [&](uint32_t file) -> const LineMap*
    {
      for (auto& [offset, m] : mapped)
        if (offset == file)
          return m;
      return nullptr;
    };
    auto moved = [](const LineMap& m, uint32_t line, uint32_t& to)
    {
      auto it = std::lower_bound(m.lines.begin(), m.lines.end(), std::pair<uint32_t, uint32_t>{line, 0});
      if (it == m.lines.end() || it->first != line)
        return false;
      to = it->second;
      return true;
    };
    std::string patched = b;
    std::vector<bool> changed(sections, false); // (the .debug$S sections a line moved in: their checksums anew)
    for (const Subsection& sub : all)
    {
      // An inlinee's source line names its file too: none in kpp's objects (no inline sites at -O0);
      // one of a moved file is compiled instead.
      if (sub.kind == 0xf6)
      {
        if (sub.size < 4)
          return why = "an inlinee lines subsection too short", false;
        bool extended = read32(b, sub.data) == 1;
        for (size_t at = sub.data + 4; at + 12 <= sub.data + sub.size;)
        {
          if (mapOf(read32(b, at + 4)))
            return why = "an inline site of a moved file", false;
          uint32_t extra = extended && at + 16 <= sub.data + sub.size ? read32(b, at + 12) : 0;
          at += 12 + (extended ? 4 + 4 * size_t(extra) : 0);
        }
        continue;
      }
      if (sub.kind != 0xf2)
        continue;
      if (sub.size < 12)
        return why = "a lines subsection too short", false;
      bool columns = (read16(b, sub.data + 6) & 1) != 0;
      for (size_t at = sub.data + 12; at < sub.data + sub.size;)
      {
        if (at + 12 > sub.data + sub.size)
          return why = "a line block past its subsection", false;
        uint32_t file = read32(b, at), count = read32(b, at + 4), size = read32(b, at + 8);
        if (size < 12 + size_t(count) * (columns ? 12 : 8) || at + size > sub.data + sub.size)
          return why = "a line block of another size than its lines", false;
        if (const LineMap* m = mapOf(file))
          for (uint32_t i = 0; i < count; ++i)
          {
            size_t entry = at + 12 + size_t(i) * 8 + 4;
            uint32_t data = read32(b, entry), line = data & 0xffffff, delta = (data >> 24) & 0x7f;
            if (line == 0xfeefee || line == 0xf00f00 || line == 0)
              continue; // (code of no line)
            uint32_t to = 0, end = 0;
            if (!moved(*m, line, to) || (delta && (!moved(*m, line + delta, end) || end - to != delta)))
              return why = "line " + std::to_string(line) + " of " + m->file + " has no place after the edit", false;
            write32(patched, entry, (data & 0xff000000) | to);
            changed[sub.section] = changed[sub.section] || to != line;
          }
        at += size;
      }
    }
    // Each changed section's checksum, in its section symbol's definition record (the symbol of
    // storage class static, value 0, named as the section, numbering it; its first auxiliary record).
    for (size_t i = 0; i < symbolCount;)
    {
      size_t at = symbols + i * symbolSize;
      uint32_t value = read32(b, at + 8);
      int64_t number = big ? int64_t(int32_t(read32(b, at + 12))) : int64_t(int16_t(read16(b, at + 12)));
      uint8_t storage = uint8_t(b[at + (big ? 18 : 16)]), aux = uint8_t(b[at + (big ? 19 : 17)]);
      if (storage == 3 && aux >= 1 && value == 0 && number >= 1 && size_t(number) <= sections && changed[size_t(number) - 1] &&
          memcmp(b.data() + at, ".debug$S", 8) == 0)
      {
        size_t h = headers + size_t(number - 1) * 40;
        size_t size = read32(b, h + 16), data = read32(b, h + 20);
        write32(patched, at + symbolSize + 8, sectionChecksum(patched.data() + data, size));
        changed[size_t(number) - 1] = false;
      }
      i += 1 + aux;
    }
    for (size_t s = 0; s < sections; ++s)
      if (changed[s])
        return why = "a patched section's symbol is not found", false;
    // Written beside it and moved over it: an object is never half written.
    std::string temporary = object + ".patching";
    {
      FILE* f = fopen(temporary.c_str(), "wb");
      if (!f)
        return why = "the patched object cannot be written", false;
      bool written = fwrite(patched.data(), 1, patched.size(), f) == patched.size();
      written = fclose(f) == 0 && written;
      if (!written)
      {
        std::remove(temporary.c_str());
        return why = "the patched object cannot be written", false;
      }
    }
    // (An object written a moment ago can be held a while by a scanner, as clang's own rename of it
    // then fails too: tried again for up to 2 s.)
    std::error_code ec;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
      std::filesystem::rename(temporary, object, ec);
      if (!ec)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (ec)
    {
      std::remove(temporary.c_str());
      return why = "the patched object cannot replace the kept one", false;
    }
    return true;
  }
}
