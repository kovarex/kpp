#pragma once
// Part of codegen: CodegenShared.hpp includes it inside namespace codegen.

struct StructInfo
{
  std::string name;                        // %struct.N
  std::vector<const Ty*> fieldTypes;       // In layout order: bases first, then data members.
  std::unordered_map<Decl*, uint32_t> fieldIndex;
  std::vector<std::pair<Decl*, uint32_t>> bases; // base class definition -> field index
  std::vector<Decl*> fieldDecls;           // data members in layout order
  // An empty class (no data members, no vtable, only empty bases) is one byte as an object of
  // its own (`{ i8 }`) and none as a base: such a base's field is `[0 x i8]` (C++'s empty-base
  // optimisation, which the game's size static_asserts count on).
  bool empty = false;
  std::vector<bool> zeroSize;              // per field: an empty base laid out without storage
  bool packed = false;                     // [[packed]]: `<{ ... }>`, every member at alignment 1
  uint64_t packedTo = 0;                   // #pragma pack(n) changed this layout: `<{ ... }>` with its padding as fields
  uint64_t requiredAlign = 1;              // requiredAlignment's answer
  // Its last data member is a flexible array member (`T items[];`, C99's): a `[0 x T]` field, so
  // the size ends where the array starts (rounded up to the alignment); the elements are past it.
  bool flexible = false;
  std::optional<bool> needsDestruction;
  // Virtual functions: a polymorphic class has a vtable pointer at offset 0 (its own, or its
  // primary base's: the first polymorphic one), whose slots extend the primary base's in
  // declaration order, and one in each other polymorphic base's subobject (vtables[1] and on).
  bool polymorphic = false;
  std::vector<Vtable> vtables;              // the primary first
  std::string vtableName;                   // @"vtable.Class", the primary's: which modules have the vtables
  std::string vtableText;                   // the vtables and their thunks, emitted into every module that stores them
  std::string vptrValue;                    // what the primary vtable pointer holds
  std::string typeInfoName;                 // @"typeinfo.Class", the descriptor slot -1 points to (descriptorFor)
  std::string descriptorText;               // its definition, emitted into every module that uses it
  const Ty* primaryBase = nullptr;          // the polymorphic base at offset 0
  bool isUnion = false;                     // all members at offset 0, storage sized for the largest
  std::vector<std::pair<Decl*, uint32_t>> anonymous; // anonymous struct/union members -> field index
  // Bit-field member -> its place inside its storage field: this layout's own, never looked up by
  // the member alone (an instance's members are its template's declarations, and Bits<3> and
  // Bits<5> place `uint32_t a : N` differently).
  std::unordered_map<Decl*, BitfieldInfo> bitfields;
  bool complete = false;                    // every field is laid out (structFor returned it once)
  uint64_t size = 0, align = 0;             // set when the layout completes (align 0: not yet), or by alignPadding
  // The alignment LLVM gives the struct type (its fields' largest); alignas raises the class's own
  // (align) above it: then its padding is written out as fields, and its objects get `align`.
  uint64_t llvmAlign = 1;
  bool overAligned = false;                 // align > llvmAlign
  uint64_t tailPadding = 0;                 // the `[n x i8]` the struct's text ends with (after its byte of its own, when empty)
  // MSVC's zero-width bit fields (`unsigned : 0;` right after a bit field): the alignment the
  // next field starts at, by that field's index (none past the last); and the alignment the class
  // takes from its bit fields (those zero-width ones on Windows, its named bit fields' types off
  // it, SysV's rule, whose storage units are byte arrays); alignPadding pads for both.
  std::vector<std::pair<uint32_t, uint64_t>> zeroWidthAligned;
  uint64_t bitFieldAlign = 0;
  bool failed = false;                      // laying it out failed: every use is an error, never the partial layout
  std::string failedWhy;                    // what made it fail, which every use reports
  bool failedShown = true;                  // false: it follows from a checker error (fail's shown), at every use
};
