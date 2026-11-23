#include "cppwriter.hpp"

const char* cppRuntimeSource()
{
  // A kpp exception is the 64-byte header kpp_throw fills (Codegen/ItaniumExceptions.cpp, finishExceptionRuntime: the
  // object, its descriptor and its destructor at 32, 40 and 48), carried by a C++ exception of one type, which each
  // translation unit catches around a call that an `invoke` was (cppwriter.cpp).
  //
  // kpp_cpp_raise stands where the unwinder's _Unwind_RaiseException stood, and does the search kpp's personality
  // does in the unwinder's first phase (Driver/Runtime.cpp, __kpp_personality_v0), before anything unwinds: a call
  // whose landing pad has catch clauses or a noexcept function's filter is made inside a kpp_cpp_scope, this thread's
  // list of which the search walks, innermost first. A clause that takes the thrown type (kpp_eh_offset; null takes
  // everything) ends the search and the exception is thrown; a filter, or the end of the list, ends the program
  // through kpp_eh_terminate with the throw site still on the stack (why 2 and 1), as kpp's own unwinding does.
  return R"(/* kpp's C++ back end: the runtime (compiler/src/cppwriter.cpp, cppRuntimeSource). */
struct kpp_cpp_exception { char* h; };
struct kpp_cpp_scope { kpp_cpp_scope* outer; char* const* clauses; unsigned long long count; };
static void (*kpp_cpp_terminate)(char*, unsigned);
extern "C"
{
__thread kpp_cpp_scope* kpp_cpp_scopes;
[[noreturn]] void kpp_cpp_raise(char* h, char* terminate, char* offset)
{
  kpp_cpp_terminate = (void (*)(char*, unsigned))terminate;
  long long (*find)(char*, char*) = (long long (*)(char*, char*))offset;
  char* thrown = *(char**)(h + 40);
  bool found = false;
  for (kpp_cpp_scope* scope = kpp_cpp_scopes; scope && !found; scope = scope->outer)
    for (unsigned long long i = 0; i < scope->count && !found; ++i)
    {
      char* clause = scope->clauses[i];
      if (clause == (char*)1)
      {
        if (kpp_cpp_terminate)
          kpp_cpp_terminate(h, 2);
        __builtin_trap();
      }
      found = !clause || (find && find(thrown, clause) >= 0);
    }
  if (!found)
  {
    if (kpp_cpp_terminate)
      kpp_cpp_terminate(h, 1);
    __builtin_trap();
  }
  throw kpp_cpp_exception{h};
}
[[noreturn]] void kpp_cpp_resume(char* h) { throw kpp_cpp_exception{h}; }
[[noreturn]] void kpp_cpp_uncaught(char* h)
{
  if (kpp_cpp_terminate)
    kpp_cpp_terminate(h, 1);
  __builtin_trap();
}
[[noreturn]] void kpp_cpp_through_noexcept(char* h)
{
  if (kpp_cpp_terminate)
    kpp_cpp_terminate(h, 2);
  __builtin_trap();
}
}
)";
}

#ifndef KPP_WITH_LLVM

bool cppWriterAvailable() { return false; }

bool writeCppModule(std::string_view, const CppWriterOptions&, std::string&, std::string& error)
{
  error = "this kpp was built without LLVM (CMake's KPP_LLVM_DIR), which --via-cpp reads the modules with";
  return false;
}

#else

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

bool cppWriterAvailable() { return true; }

namespace
{
  using namespace llvm;

  // What every translation unit starts with: the scalar types by their sizes, the types memory is read and
  // written through (`_a`: may alias anything, as LLVM's loads and stores do; `_u`: at any alignment too), and
  // the runtime's three functions.
  const char* const prelude = R"(typedef unsigned char u8; typedef signed char i8; typedef unsigned short u16; typedef short i16;
typedef unsigned int u32; typedef int i32; typedef unsigned long long u64; typedef long long i64;
typedef unsigned __int128 u128; typedef __int128 i128; typedef float f32; typedef double f64; typedef char* ptr;
#define KPP_A __attribute__((may_alias))
#define KPP_U __attribute__((may_alias, aligned(1)))
typedef u8 KPP_A u8_a; typedef u16 KPP_A u16_a; typedef u32 KPP_A u32_a; typedef u64 KPP_A u64_a; typedef u128 KPP_A u128_a;
typedef f32 KPP_A f32_a; typedef f64 KPP_A f64_a; typedef ptr KPP_A ptr_a;
typedef u8 KPP_U u8_u; typedef u16 KPP_U u16_u; typedef u32 KPP_U u32_u; typedef u64 KPP_U u64_u; typedef u128 KPP_U u128_u;
typedef f32 KPP_U f32_u; typedef f64 KPP_U f64_u; typedef ptr KPP_U ptr_u;
struct kpp_cpp_exception { ptr h; };
struct kpp_cpp_scope { kpp_cpp_scope* outer; const ptr* clauses; u64 count; };
extern "C" { extern __thread kpp_cpp_scope* kpp_cpp_scopes; [[noreturn]] void kpp_cpp_raise(ptr h, ptr terminate, ptr offset); [[noreturn]] void kpp_cpp_resume(ptr h); [[noreturn]] void kpp_cpp_uncaught(ptr h); [[noreturn]] void kpp_cpp_through_noexcept(ptr h); }
template<class To, class From> static inline To kpp_bits(From v) { To r; __builtin_memcpy(&r, &v, sizeof r); return r; }
)";

  std::string number(uint64_t n) { return std::to_string(n); }

  // A symbol's or a section's name as a C++ string literal's contents.
  std::string quoted(StringRef text)
  {
    std::string out;
    for (unsigned char c : text)
      if (c == '"' || c == '\\')
      {
        out += '\\';
        out += char(c);
      }
      else if (c < 32 || c >= 127)
      {
        char octal[8];
        snprintf(octal, sizeof octal, "\\%03o", unsigned(c));
        out += octal;
      }
      else
        out += char(c);
    return out;
  }

  struct Writer
  {
    explicit Writer(const Module& module) : m(module), dl(module.getDataLayout()) {}

    // --- what the writer does not translate ---

    // The first such construct is the module's error: the translation unit is not written.
    void fail(const std::string& what)
    {
      if (error.empty())
        error = what + (current ? " (in " + current->getName().str() + ")" : std::string());
    }

    // --- types ---

    std::string intType(unsigned bits, bool isSigned = false)
    {
      switch (bits)
      {
      case 1: return "bool";
      case 8: return isSigned ? "i8" : "u8";
      case 16: return isSigned ? "i16" : "u16";
      case 32: return isSigned ? "i32" : "u32";
      case 64: return isSigned ? "i64" : "u64";
      case 128: return isSigned ? "i128" : "u128";
      }
      fail("an integer of " + number(bits) + " bits");
      return "u64";
    }

    // The C++ type of a value of type t.
    std::string ty(Type* t)
    {
      switch (t->getTypeID())
      {
      case Type::VoidTyID: return "void";
      case Type::IntegerTyID: return intType(t->getIntegerBitWidth());
      case Type::FloatTyID: return "f32";
      case Type::DoubleTyID: return "f64";
      case Type::PointerTyID: return "ptr";
      case Type::StructTyID:
      case Type::ArrayTyID: return aggregate(t);
      case Type::FixedVectorTyID: return vectorType(t, false);
      default: break;
      }
      std::string text;
      raw_string_ostream(text) << *t;
      fail("a value of type " + text);
      return "u64";
    }

    std::string signedTy(Type* t) { return t->isVectorTy() ? vectorType(t, true) : intType(t->getIntegerBitWidth(), true); }

    // A vector of count lanes of the C++ type lane (u32, i16, f32): the compiler's own vector type, which may
    // alias anything as LLVM's does; NAME_u is the same read or written at any alignment.
    std::string vectorNamed(const std::string& lane, unsigned count)
    {
      std::string name = "V" + number(count) + lane;
      if (vectors.insert(name).second)
      {
        uint64_t bytes = count * (std::stoull(lane.substr(1)) / 8);
        types += "typedef " + lane + " " + name + " __attribute__((vector_size(" + number(bytes) + "), may_alias));\n";
        types += "typedef " + lane + " " + name + "_u __attribute__((vector_size(" + number(bytes) + "), may_alias, aligned(1)));\n";
      }
      return name;
    }

    // The C++ type of a vector of type t: its integer lanes unsigned, or signed for the operations that read
    // them so. A vector of i1 has no type of its own: it is a comparison's result (maskType).
    std::string vectorType(Type* t, bool isSigned)
    {
      auto* v = dyn_cast<FixedVectorType>(t);
      Type* lane = v ? v->getElementType() : nullptr;
      if (lane && lane->isIntegerTy() && !lane->isIntegerTy(1))
      {
        unsigned bits = lane->getIntegerBitWidth();
        if (bits == 8 || bits == 16 || bits == 32 || bits == 64)
          return vectorNamed(intType(bits, isSigned), v->getNumElements());
      }
      if (lane && lane->isFloatTy())
        return vectorNamed("f32", v->getNumElements());
      if (lane && lane->isDoubleTy())
        return vectorNamed("f64", v->getNumElements());
      std::string text;
      raw_string_ostream(text) << *t;
      fail("a vector of type " + text);
      return vectorNamed("u32", 4);
    }

    static bool isMask(Type* t) { return t->isVectorTy() && cast<VectorType>(t)->getElementType()->isIntegerTy(1); }

    // The C++ type of v, a vector of i1: what the compiler's vector comparison gives, signed lanes as wide as
    // the lanes compared, all ones or zero. An operation on such vectors has its operands' type.
    std::string maskType(const Value* v, unsigned depth = 0)
    {
      auto* t = cast<FixedVectorType>(v->getType());
      if (auto* c = dyn_cast<CmpInst>(v))
      {
        Type* lane = cast<FixedVectorType>(c->getOperand(0)->getType())->getElementType();
        unsigned bits = lane->isPointerTy() ? 64 : unsigned(lane->getPrimitiveSizeInBits());
        return vectorNamed(intType(bits, true), t->getNumElements());
      }
      if (depth < 8)
      {
        if (auto* b = dyn_cast<BinaryOperator>(v))
          return maskType(b->getOperand(0), depth + 1);
        if (auto* s = dyn_cast<SelectInst>(v))
          return maskType(s->getTrueValue(), depth + 1);
        if (auto* p = dyn_cast<PHINode>(v); p && p->getNumIncomingValues())
          return maskType(p->getIncomingValue(0), depth + 1);
      }
      fail("a vector of i1 that is no comparison's result");
      return vectorNamed("i32", t->getNumElements());
    }

    // The type of the local that holds v.
    std::string localType(const Value* v) { return isMask(v->getType()) ? maskType(v) : ty(v->getType()); }

    // A struct or an array as a value (a landing pad's { ptr, i32 }, a C function's { i64, i64 }): a C++ struct of the
    // same layout, which the translation unit asserts. An array's elements are its member f.
    std::string aggregate(Type* t)
    {
      auto found = aggregates.find(t);
      if (found != aggregates.end())
        return found->second;
      std::string members;
      std::string name = "T" + number(aggregates.size());
      aggregates[t] = name; // (before its members: a type never holds itself by value)
      std::string checks;
      if (auto* s = dyn_cast<StructType>(t))
      {
        if (s->isOpaque())
        {
          fail("a value of an opaque struct type");
          return name;
        }
        const StructLayout* layout = dl.getStructLayout(s);
        for (unsigned i = 0; i < s->getNumElements(); ++i)
        {
          if (dl.getTypeAllocSize(s->getElementType(i)) == 0)
            continue; // (no storage: no member)
          members += " " + ty(s->getElementType(i)) + " f" + number(i) + ";";
          checks += "static_assert(__builtin_offsetof(" + name + ", f" + number(i) + ") == " + number(layout->getElementOffset(i)) + ", \"\");\n";
        }
        std::string packed = s->isPacked() ? "__attribute__((packed)) " : "";
        types += "struct KPP_A " + packed + name + " {" + members + " };\n";
      }
      else
      {
        auto* a = cast<ArrayType>(t);
        types += "struct KPP_A " + name + " { " + ty(a->getElementType()) + " f[" + number(std::max<uint64_t>(a->getNumElements(), 1)) + "]; };\n";
      }
      if (uint64_t size = dl.getTypeAllocSize(t))
        checks += "static_assert(sizeof(" + name + ") == " + number(size) + ", \"\");\n";
      types += checks;
      return name;
    }

    // Storage of a size and an alignment with no members: what a C function takes or returns by value in memory
    // (LLVM's byval and sret), and an alloca.
    std::string blob(uint64_t size, uint64_t align)
    {
      std::string name = "B" + number(size) + "_" + number(align);
      if (blobs.insert(name).second)
        types += "struct KPP_A " + name + " { alignas(" + number(align) + ") u8 b[" + number(std::max<uint64_t>(size, 1)) + "]; };\n";
      return name;
    }

    // The type memory is read or written through for a scalar of type t at alignment align.
    std::string memoryType(Type* t, Align align)
    {
      if (t->isVectorTy())
        return ty(t) + (align < dl.getABITypeAlign(t) ? "_u" : "");
      std::string scalar = t->isIntegerTy(1) ? "u8" : ty(t);
      return scalar + (align < dl.getABITypeAlign(t) ? "_u" : "_a");
    }

    // --- names ---

    std::string name(const GlobalValue* g)
    {
      if (isa<GlobalIFunc>(g))
        fail("an indirect function (" + g->getName().str() + ")");
      referenced.insert(g);
      auto found = globalIds.find(g);
      if (found == globalIds.end())
        found = globalIds.insert({g, unsigned(globalIds.size())}).first;
      return "g" + number(found->second);
    }

    // --- constants ---

    std::string intConstant(const APInt& value)
    {
      unsigned bits = value.getBitWidth();
      if (bits == 1)
        return value.isZero() ? "false" : "true";
      if (bits == 128)
        return "(((u128)" + number(value.getHiBits(64).getZExtValue()) + "ull << 64) | (u128)" + number(value.getLoBits(64).getZExtValue()) + "ull)";
      if (bits > 64)
      {
        fail("an integer constant of " + number(bits) + " bits");
        return "0";
      }
      uint64_t n = value.getZExtValue();
      if (bits == 64)
        return number(n) + "ull";
      if (bits == 32)
        return number(n) + "u";
      return "(" + intType(bits) + ")" + number(n);
    }

    std::string fpConstant(const ConstantFP* f)
    {
      const APFloat& value = f->getValueAPF();
      bool single = f->getType()->isFloatTy();
      if (!single && !f->getType()->isDoubleTy())
      {
        fail("a floating-point constant that is no float or double");
        return "0";
      }
      // A finite value as a hexadecimal literal, which is exact; the others by their bits.
      if (value.isFinite())
      {
        double d = single ? double(value.convertToFloat()) : value.convertToDouble();
        char text[64];
        snprintf(text, sizeof text, "%a", d);
        return single ? "(" + std::string(text) + "f)" : "(" + std::string(text) + ")";
      }
      uint64_t bits = value.bitcastToAPInt().getZExtValue();
      return single ? "kpp_bits<f32, u32>(" + number(bits) + "u)" : "kpp_bits<f64, u64>(" + number(bits) + "ull)";
    }

    std::string zero(Type* t)
    {
      if (t->isIntegerTy(1))
        return "false";
      if (t->isIntegerTy() || t->isFloatingPointTy())
        return "(" + ty(t) + ")0";
      if (t->isPointerTy())
        return "((ptr)0)";
      return ty(t) + "{}";
    }

    std::string constant(const Constant* c)
    {
      if (auto* i = dyn_cast<ConstantInt>(c))
        return intConstant(i->getValue());
      if (auto* f = dyn_cast<ConstantFP>(c))
        return fpConstant(f);
      if (auto* g = dyn_cast<GlobalValue>(c))
        return "((ptr)&" + name(g) + ")";
      if (isa<ConstantPointerNull>(c) || isa<UndefValue>(c) || isa<ConstantAggregateZero>(c))
        return zero(c->getType());
      if (auto* e = dyn_cast<ConstantExpr>(c))
        return expression(e);
      if (isa<ConstantVector>(c) || isa<ConstantDataVector>(c))
      {
        // A vector's lanes in order.
        std::string text = ty(c->getType()) + "{";
        unsigned count = cast<FixedVectorType>(c->getType())->getNumElements();
        for (unsigned i = 0; i < count; ++i)
          text += (i ? ", " : "") + (c->getAggregateElement(i) ? constant(c->getAggregateElement(i)) : std::string("0"));
        return text + "}";
      }
      if (isa<ConstantStruct>(c) || isa<ConstantArray>(c) || isa<ConstantDataArray>(c))
      {
        // An aggregate as a value: its members in order (an array's inside its member f).
        bool array = c->getType()->isArrayTy();
        std::string text = ty(c->getType()) + (array ? "{{" : "{");
        unsigned count = array ? unsigned(c->getType()->getArrayNumElements()) : c->getType()->getStructNumElements();
        bool first = true;
        for (unsigned i = 0; i < count; ++i)
        {
          Constant* element = c->getAggregateElement(i);
          if (!element || dl.getTypeAllocSize(element->getType()) == 0)
            continue;
          text += (first ? "" : ", ") + constant(element);
          first = false;
        }
        return text + (array ? "}}" : "}");
      }
      std::string text;
      raw_string_ostream(text) << *c;
      fail("the constant " + text);
      return "0";
    }

    // --- expressions: an instruction's or a constant expression's value, in parentheses ---

    std::string value(const Value* v)
    {
      if (auto* c = dyn_cast<Constant>(v))
        return constant(c);
      if (auto* i = dyn_cast<Instruction>(v); i && inlined.count(i))
        return expression(i);
      auto found = names.find(v);
      if (found != names.end())
        return found->second;
      fail("a value without a name");
      return "0";
    }

    // x op y for integers of type t, the result of type t: narrow ones computed as u32 (C++ promotes them to int,
    // whose overflow is undefined), a bool's as bits.
    std::string arithmetic(unsigned opcode, Type* t, const std::string& x, const std::string& y)
    {
      if (t->isVectorTy())
      {
        // Lane by lane, as the compiler's vector types compute: no promotion, an integer lane wraps.
        const char* op = nullptr;
        bool isSigned = false;
        switch (opcode)
        {
        case Instruction::Add:
        case Instruction::FAdd: op = "+"; break;
        case Instruction::Sub:
        case Instruction::FSub: op = "-"; break;
        case Instruction::Mul:
        case Instruction::FMul: op = "*"; break;
        case Instruction::UDiv:
        case Instruction::FDiv: op = "/"; break;
        case Instruction::URem: op = "%"; break;
        case Instruction::And: op = "&"; break;
        case Instruction::Or: op = "|"; break;
        case Instruction::Xor: op = "^"; break;
        case Instruction::Shl: op = "<<"; break;
        case Instruction::LShr: op = ">>"; break;
        case Instruction::AShr: op = ">>", isSigned = true; break;
        case Instruction::SDiv: op = "/", isSigned = true; break;
        case Instruction::SRem: op = "%", isSigned = true; break;
        default:
          fail("an operation on vectors");
          return x;
        }
        if (isMask(t))
        {
          if (opcode != Instruction::And && opcode != Instruction::Or && opcode != Instruction::Xor)
            fail("an operation on vectors of i1 that is no and, or or xor");
          return "(" + x + " " + op + " " + y + ")";
        }
        if (!isSigned)
          return "(" + x + " " + op + " " + y + ")";
        std::string sign = vectorType(t, true);
        return "((" + ty(t) + ")((" + sign + ")" + x + " " + op + " (" + sign + ")" + y + "))";
      }
      if (t->isFloatingPointTy())
      {
        switch (opcode)
        {
        case Instruction::FAdd: return "(" + x + " + " + y + ")";
        case Instruction::FSub: return "(" + x + " - " + y + ")";
        case Instruction::FMul: return "(" + x + " * " + y + ")";
        case Instruction::FDiv: return "(" + x + " / " + y + ")";
        case Instruction::FRem: return std::string(t->isFloatTy() ? "__builtin_fmodf(" : "__builtin_fmod(") + x + ", " + y + ")";
        }
        fail("a floating-point operation");
        return x;
      }
      if (!t->isIntegerTy())
      {
        fail("arithmetic on a value that is no number");
        return x;
      }
      unsigned bits = t->getIntegerBitWidth();
      std::string type = ty(t), sign = intType(bits == 1 ? 8 : bits, true);
      if (bits == 1)
      {
        switch (opcode)
        {
        case Instruction::Add:
        case Instruction::Sub:
        case Instruction::Xor: return "(bool)(" + x + " ^ " + y + ")";
        case Instruction::Mul:
        case Instruction::And: return "(bool)(" + x + " & " + y + ")";
        case Instruction::Or: return "(bool)(" + x + " | " + y + ")";
        }
        fail("an operation on an i1");
        return x;
      }
      bool narrow = bits < 32;
      std::string a = narrow ? "(u32)" + x : x, b = narrow ? "(u32)" + y : y;
      auto result = [&](const std::string& text) { return narrow ? "(" + type + ")(" + text + ")" : "(" + text + ")"; };
      switch (opcode)
      {
      case Instruction::Add: return result(a + " + " + b);
      case Instruction::Sub: return result(a + " - " + b);
      case Instruction::Mul: return result(a + " * " + b);
      case Instruction::And: return result(a + " & " + b);
      case Instruction::Or: return result(a + " | " + b);
      case Instruction::Xor: return result(a + " ^ " + b);
      case Instruction::Shl: return result(a + " << " + b);
      case Instruction::LShr: return result(a + " >> " + b);
      case Instruction::UDiv: return result(a + " / " + b);
      case Instruction::URem: return result(a + " % " + b);
      case Instruction::AShr: return "(" + type + ")((" + sign + ")" + x + " >> " + y + ")";
      case Instruction::SDiv: return "(" + type + ")((" + sign + ")" + x + " / (" + sign + ")" + y + ")";
      case Instruction::SRem: return "(" + type + ")((" + sign + ")" + x + " % (" + sign + ")" + y + ")";
      }
      fail("an integer operation");
      return x;
    }

    std::string compare(const CmpInst* c)
    {
      std::string x = value(c->getOperand(0)), y = value(c->getOperand(1));
      Type* t = c->getOperand(0)->getType();
      auto is = [&](const char* op) { return "(" + x + " " + op + " " + y + ")"; };
      auto isNot = [&](const char* op) { return "(!(" + x + " " + op + " " + y + "))"; };
      if (t->isVectorTy())
      {
        // Lane by lane: each lane of the result all ones or zero (maskType).
        auto lanes = [&](const char* op) { return "(" + x + " " + op + " " + y + ")"; };
        auto notLanes = [&](const char* op) { return "(~(" + x + " " + op + " " + y + "))"; };
        std::string sign = cast<VectorType>(t)->getElementType()->isIntegerTy() ? "(" + vectorType(t, true) + ")" : std::string();
        auto signedLanes = [&](const char* op) { return "(" + sign + x + " " + op + " " + sign + y + ")"; };
        switch (c->getPredicate())
        {
        case CmpInst::ICMP_EQ:
        case CmpInst::FCMP_OEQ: return lanes("==");
        case CmpInst::ICMP_NE:
        case CmpInst::FCMP_UNE: return lanes("!=");
        case CmpInst::ICMP_UGT:
        case CmpInst::FCMP_OGT: return lanes(">");
        case CmpInst::ICMP_UGE:
        case CmpInst::FCMP_OGE: return lanes(">=");
        case CmpInst::ICMP_ULT:
        case CmpInst::FCMP_OLT: return lanes("<");
        case CmpInst::ICMP_ULE:
        case CmpInst::FCMP_OLE: return lanes("<=");
        case CmpInst::ICMP_SGT: return signedLanes(">");
        case CmpInst::ICMP_SGE: return signedLanes(">=");
        case CmpInst::ICMP_SLT: return signedLanes("<");
        case CmpInst::ICMP_SLE: return signedLanes("<=");
        case CmpInst::FCMP_ONE: return "((" + x + " < " + y + ") | (" + x + " > " + y + "))";
        case CmpInst::FCMP_UEQ: return "(~((" + x + " < " + y + ") | (" + x + " > " + y + ")))";
        case CmpInst::FCMP_ORD: return "((" + x + " == " + x + ") & (" + y + " == " + y + "))";
        case CmpInst::FCMP_UNO: return "((" + x + " != " + x + ") | (" + y + " != " + y + "))";
        case CmpInst::FCMP_UGT: return notLanes("<=");
        case CmpInst::FCMP_UGE: return notLanes("<");
        case CmpInst::FCMP_ULT: return notLanes(">=");
        case CmpInst::FCMP_ULE: return notLanes(">");
        default: break;
        }
        fail("a comparison of vectors");
        return x;
      }
      if (t->isFloatingPointTy())
      {
        switch (c->getPredicate())
        {
        case CmpInst::FCMP_FALSE: return "false";
        case CmpInst::FCMP_TRUE: return "true";
        case CmpInst::FCMP_OEQ: return is("==");
        case CmpInst::FCMP_OGT: return is(">");
        case CmpInst::FCMP_OGE: return is(">=");
        case CmpInst::FCMP_OLT: return is("<");
        case CmpInst::FCMP_OLE: return is("<=");
        case CmpInst::FCMP_ONE: return "(" + x + " < " + y + " || " + x + " > " + y + ")";
        case CmpInst::FCMP_ORD: return "(" + x + " == " + x + " && " + y + " == " + y + ")";
        case CmpInst::FCMP_UNO: return "(" + x + " != " + x + " || " + y + " != " + y + ")";
        case CmpInst::FCMP_UEQ: return "(!(" + x + " < " + y + " || " + x + " > " + y + "))";
        case CmpInst::FCMP_UGT: return isNot("<=");
        case CmpInst::FCMP_UGE: return isNot("<");
        case CmpInst::FCMP_ULT: return isNot(">=");
        case CmpInst::FCMP_ULE: return isNot(">");
        case CmpInst::FCMP_UNE: return is("!=");
        default: break;
        }
        fail("a floating-point comparison");
        return "false";
      }
      if (!t->isIntegerTy() && !t->isPointerTy())
      {
        fail("a comparison of values that are no numbers or pointers");
        return "false";
      }
      const char* op = "";
      bool isSigned = false;
      switch (c->getPredicate())
      {
      case CmpInst::ICMP_EQ: return is("==");
      case CmpInst::ICMP_NE: return is("!=");
      case CmpInst::ICMP_UGT: op = ">"; break;
      case CmpInst::ICMP_UGE: op = ">="; break;
      case CmpInst::ICMP_ULT: op = "<"; break;
      case CmpInst::ICMP_ULE: op = "<="; break;
      case CmpInst::ICMP_SGT: op = ">", isSigned = true; break;
      case CmpInst::ICMP_SGE: op = ">=", isSigned = true; break;
      case CmpInst::ICMP_SLT: op = "<", isSigned = true; break;
      case CmpInst::ICMP_SLE: op = "<=", isSigned = true; break;
      default:
        fail("an integer comparison");
        return "false";
      }
      if (t->isPointerTy())
        return isSigned ? "((i64)(u64)" + x + " " + op + " (i64)(u64)" + y + ")" : "((u64)" + x + " " + op + " (u64)" + y + ")";
      if (!isSigned)
        return is(op);
      if (t->isIntegerTy(1)) // (an i1 as a signed number is 0 or -1)
        return "(-(i32)" + x + " " + op + " -(i32)" + y + ")";
      std::string sign = signedTy(t);
      return "((" + sign + ")" + x + " " + op + " (" + sign + ")" + y + ")";
    }

    std::string cast(unsigned opcode, Type* from, Type* to, const std::string& x, const Value* source)
    {
      if (from->isVectorTy() || to->isVectorTy())
      {
        // Bits kept (a bitcast), or each lane converted.
        if (opcode == Instruction::BitCast && !isMask(from) && !isMask(to))
          return from->isVectorTy() && to->isVectorTy() ? "(" + ty(to) + ")" + x : "kpp_bits<" + ty(to) + ", " + ty(from) + ">(" + x + ")";
        if (!from->isVectorTy() || !to->isVectorTy() || isMask(to))
        {
          fail("a conversion of a vector");
          return x;
        }
        std::string type = ty(to);
        auto convert = [&](const std::string& what, const std::string& into) { return "__builtin_convertvector(" + what + ", " + into + ")"; };
        if (isMask(from))
        {
          // A comparison's result: its lanes are all ones or zero already.
          bool integers = cast<VectorType>(to)->getElementType()->isIntegerTy();
          std::string lanes = integers ? "(" + type + ")" + convert(x, vectorType(to, true)) : std::string();
          switch (opcode)
          {
          case Instruction::SExt: return lanes;
          case Instruction::ZExt: return "(" + lanes + " & 1)";
          }
          (void)source;
          fail("a conversion of a vector of i1");
          return x;
        }
        switch (opcode)
        {
        case Instruction::Trunc:
        case Instruction::ZExt:
        case Instruction::UIToFP:
        case Instruction::FPToUI:
        case Instruction::FPTrunc:
        case Instruction::FPExt: return convert(x, type);
        case Instruction::SExt: return "(" + type + ")" + convert("(" + vectorType(from, true) + ")" + x, vectorType(to, true));
        case Instruction::SIToFP: return convert("(" + vectorType(from, true) + ")" + x, type);
        case Instruction::FPToSI: return "(" + type + ")" + convert(x, vectorType(to, true));
        }
        fail("a conversion of a vector");
        return x;
      }
      std::string type = ty(to);
      bool fromBool = from->isIntegerTy(1), toBool = to->isIntegerTy(1);
      switch (opcode)
      {
      case Instruction::Trunc: return toBool ? "(bool)(" + x + " & 1)" : "(" + type + ")" + x;
      case Instruction::ZExt: return "(" + type + ")" + x;
      case Instruction::SExt: return fromBool ? "(" + type + ")-(" + signedTy(to) + ")" + x : "(" + type + ")(" + signedTy(to) + ")(" + signedTy(from) + ")" + x;
      case Instruction::FPToUI: return toBool ? "(bool)((u32)" + x + " & 1)" : "(" + type + ")" + x;
      case Instruction::FPToSI: return toBool ? "(bool)((i32)" + x + " & 1)" : "(" + type + ")(" + signedTy(to) + ")" + x;
      case Instruction::UIToFP: return "(" + type + ")" + x;
      case Instruction::SIToFP: return fromBool ? "(" + type + ")-(i32)" + x : "(" + type + ")(" + signedTy(from) + ")" + x;
      case Instruction::FPTrunc:
      case Instruction::FPExt: return "(" + type + ")" + x;
      case Instruction::PtrToInt: return "(" + type + ")(u64)" + x;
      case Instruction::IntToPtr: return "(ptr)(u64)" + x;
      case Instruction::BitCast:
        if (from == to || (from->isPointerTy() && to->isPointerTy()))
          return x;
        if ((from->isIntegerTy() || from->isFloatingPointTy()) && (to->isIntegerTy() || to->isFloatingPointTy()) && !fromBool && !toBool)
          return "kpp_bits<" + type + ", " + ty(from) + ">(" + x + ")";
        break;
      }
      fail("a conversion");
      return x;
    }

    // base + offsets: the constant ones folded into bytes by the module's data layout, each index by its
    // element's size.
    std::string elementAddress(const GEPOperator* gep)
    {
      std::string text = value(gep->getPointerOperand());
      int64_t constant = 0;
      for (gep_type_iterator it = gep_type_begin(gep), end = gep_type_end(gep); it != end; ++it)
      {
        Value* index = it.getOperand();
        if (StructType* s = it.getStructTypeOrNull())
        {
          constant += int64_t(dl.getStructLayout(s)->getElementOffset(unsigned(cast<ConstantInt>(index)->getZExtValue())));
          continue;
        }
        if (it.getSequentialElementStride(dl).isScalable())
        {
          fail("an element of a scalable type");
          continue;
        }
        int64_t size = int64_t(it.getSequentialElementStride(dl).getFixedValue());
        if (auto* known = dyn_cast<ConstantInt>(index))
        {
          constant += known->getSExtValue() * size;
          continue;
        }
        unsigned bits = index->getType()->getIntegerBitWidth();
        std::string i = bits == 64 ? "(i64)" + value(index) : "(i64)(" + intType(bits, true) + ")" + value(index);
        text += size == 1 ? " + " + i : " + " + i + " * " + number(uint64_t(size)) + "ll";
      }
      if (constant)
        text += constant < 0 ? " - " + number(uint64_t(-constant)) : " + " + number(uint64_t(constant));
      return "(" + text + ")";
    }

    // Where an aggregate's member is: .f1 of a struct, .f[2] of an array, one after the other.
    std::string memberPath(Type* t, ArrayRef<unsigned> indices)
    {
      std::string path;
      for (unsigned index : indices)
        if (auto* s = dyn_cast<StructType>(t))
        {
          path += ".f" + number(index);
          t = s->getElementType(index);
        }
        else
        {
          path += ".f[" + number(index) + "]";
          t = t->getArrayElementType();
        }
      return path;
    }

    std::string expression(const User* u)
    {
      unsigned opcode = Operator::getOpcode(u);
      Type* type = u->getType();
      auto operand = [&](unsigned i) { return value(u->getOperand(i)); };
      switch (opcode)
      {
      case Instruction::Add:
      case Instruction::Sub:
      case Instruction::Mul:
      case Instruction::UDiv:
      case Instruction::SDiv:
      case Instruction::URem:
      case Instruction::SRem:
      case Instruction::Shl:
      case Instruction::LShr:
      case Instruction::AShr:
      case Instruction::And:
      case Instruction::Or:
      case Instruction::Xor:
      case Instruction::FAdd:
      case Instruction::FSub:
      case Instruction::FMul:
      case Instruction::FDiv:
      case Instruction::FRem: return arithmetic(opcode, type, operand(0), operand(1));
      case Instruction::FNeg: return "(-" + operand(0) + ")";
      case Instruction::ICmp:
      case Instruction::FCmp:
        if (auto* c = dyn_cast<CmpInst>(u))
          return compare(c);
        break;
      case Instruction::Trunc:
      case Instruction::ZExt:
      case Instruction::SExt:
      case Instruction::FPToUI:
      case Instruction::FPToSI:
      case Instruction::UIToFP:
      case Instruction::SIToFP:
      case Instruction::FPTrunc:
      case Instruction::FPExt:
      case Instruction::PtrToInt:
      case Instruction::IntToPtr:
      case Instruction::BitCast: return "(" + cast(opcode, u->getOperand(0)->getType(), type, operand(0), u->getOperand(0)) + ")";
      case Instruction::ExtractElement:
        if (isMask(u->getOperand(0)->getType()))
          break;
        return "(" + operand(0) + "[" + (isa<ConstantInt>(u->getOperand(1)) ? number(cast<ConstantInt>(u->getOperand(1))->getZExtValue()) : operand(1)) + "])";
      case Instruction::ShuffleVector:
        if (auto* s = dyn_cast<ShuffleVectorInst>(u); s && !isMask(type))
        {
          // The lanes of the two operands the mask names; -1 is a lane nobody reads.
          std::string text = "__builtin_shufflevector(" + operand(0) + ", " + operand(1);
          for (int lane : s->getShuffleMask())
            text += ", " + std::to_string(lane);
          return text + ")";
        }
        break;
      case Instruction::GetElementPtr: return elementAddress(cast<GEPOperator>(u));
      case Instruction::Select: return "(" + operand(0) + " ? " + operand(1) + " : " + operand(2) + ")";
      case Instruction::Freeze: return operand(0);
      case Instruction::ExtractValue:
        if (auto* e = dyn_cast<ExtractValueInst>(u))
          return "(" + operand(0) + memberPath(e->getAggregateOperand()->getType(), e->getIndices()) + ")";
        break;
      }
      std::string text;
      raw_string_ostream(text) << *u;
      fail("the operation " + text);
      return "0";
    }

    // --- memory ---

    // (a volatile access is one through a volatile type: the C++ compiler keeps it as LLVM does)
    std::string load(Type* t, const std::string& address, Align align, bool isVolatile = false)
    {
      std::string qualifier = isVolatile ? "volatile " : "";
      if (t->isStructTy() || t->isArrayTy())
        return "(*(" + qualifier + ty(t) + "*)" + address + ")";
      std::string read = "(*(" + qualifier + memoryType(t, align) + "*)" + address + ")";
      return t->isIntegerTy(1) ? "(bool)(" + read + " & 1)" : read;
    }

    std::string store(Type* t, const std::string& address, Align align, const std::string& stored, bool isVolatile = false)
    {
      std::string qualifier = isVolatile ? "volatile " : "";
      if (t->isStructTy() || t->isArrayTy())
        return "*(" + qualifier + ty(t) + "*)" + address + " = " + stored + ";";
      return "*(" + qualifier + memoryType(t, align) + "*)" + address + " = " + (t->isIntegerTy(1) ? "(u8)" + stored : stored) + ";";
    }

    static const char* ordering(AtomicOrdering order)
    {
      switch (order)
      {
      case AtomicOrdering::Acquire: return "2";
      case AtomicOrdering::Release: return "3";
      case AtomicOrdering::AcquireRelease: return "4";
      case AtomicOrdering::SequentiallyConsistent: return "5";
      default: return "0"; // (relaxed: LLVM's monotonic and unordered)
      }
    }

    // --- functions' types ---

    // The C++ type of a parameter of type t: signed where the C function's is (LLVM's signext: the caller extends
    // it), the storage itself where it is passed by value in memory (byval).
    std::string parameterType(Type* t, bool signExtended, Type* byValue, MaybeAlign align)
    {
      if (byValue)
        return blob(dl.getTypeAllocSize(byValue), std::max<uint64_t>(align ? align->value() : 1, dl.getABITypeAlign(byValue).value()));
      if (signExtended && t->isIntegerTy() && !t->isIntegerTy(1))
        return signedTy(t);
      return ty(t);
    }

    // A function's result: the storage of its sret parameter where it has one (a C function returning a struct in
    // memory: the C++ compiler passes the address as the target's C does), else its type.
    std::string resultType(Type* t, bool signExtended, Type* structResult)
    {
      if (structResult)
        return blob(dl.getTypeAllocSize(structResult), dl.getABITypeAlign(structResult).value());
      if (signExtended && t->isIntegerTy() && !t->isIntegerTy(1))
        return signedTy(t);
      return ty(t);
    }

    static Type* structResultOf(const Function& f)
    {
      for (unsigned i = 0; i < f.arg_size(); ++i)
        if (Type* t = f.getParamStructRetType(i))
          return t;
      return nullptr;
    }

    // f's declaration without its storage class: `u32 g7(ptr p0, u32 p1)`, the parameters named in a definition.
    std::string signature(const Function& f, bool definition)
    {
      Type* structResult = structResultOf(f);
      std::string text = resultType(f.getReturnType(), f.hasRetAttribute(Attribute::SExt), structResult) + " " + name(&f) + "(";
      bool first = true;
      for (const Argument& a : f.args())
      {
        unsigned i = a.getArgNo();
        if (f.getParamStructRetType(i))
          continue;
        text += (first ? "" : ", ") + parameterType(a.getType(), f.hasParamAttribute(i, Attribute::SExt), f.getParamByValType(i), f.getParamAlign(i));
        if (definition)
          text += " p" + number(i);
        first = false;
      }
      if (f.isVarArg())
        text += first ? "..." : ", ...";
      return text + ")";
    }

    std::string symbol(const GlobalValue& g)
    {
      if (!g.hasName())
        return "";
      return " __asm__(\"" + quoted(dl.getGlobalPrefix() ? std::string(1, dl.getGlobalPrefix()) + g.getName().str() : g.getName().str()) + "\")";
    }

    // --- function bodies ---

    // Whether i's value is written where its one use is, not kept in a local: an operation without a side
    // effect, used once, in its own block, by something that is no phi. Its operands are values that do not
    // change, so where it is computed does not matter.
    bool writtenInPlace(const Instruction& i)
    {
      if (!i.hasOneUse())
        return false;
      if (!(isa<BinaryOperator>(i) || isa<CastInst>(i) || isa<GetElementPtrInst>(i) || isa<CmpInst>(i) || isa<SelectInst>(i) || isa<UnaryOperator>(i) ||
            isa<ExtractValueInst>(i) || isa<FreezeInst>(i) || isa<ExtractElementInst>(i) || isa<ShuffleVectorInst>(i)))
        return false;
      auto* user = dyn_cast<Instruction>(*i.user_begin());
      if (!user || user->getParent() != i.getParent() || isa<PHINode>(user))
        return false;
      unsigned depth = 1;
      for (const Use& operand : i.operands())
        if (auto* inner = dyn_cast<Instruction>(operand))
          if (auto found = depths.find(inner); found != depths.end())
            depth = std::max(depth, found->second + 1);
      if (depth > 12)
        return false;
      depths[&i] = depth;
      return true;
    }

    // The assignments a branch from `from` to `to` makes to to's phis. Where one of them reads another of to's
    // phis, all are read before any is written.
    std::string edge(const BasicBlock* from, const BasicBlock* to)
    {
      std::vector<const PHINode*> phis;
      bool reads = false;
      for (const PHINode& phi : to->phis())
      {
        phis.push_back(&phi);
        auto* source = dyn_cast<PHINode>(phi.getIncomingValueForBlock(from));
        reads = reads || (source && source->getParent() == to);
      }
      std::string text;
      if (!reads)
      {
        for (const PHINode* phi : phis)
          text += " " + names[phi] + " = " + value(phi->getIncomingValueForBlock(from)) + ";";
        return text;
      }
      for (const PHINode* phi : phis)
      {
        if (phiCopies.insert(phi).second)
          locals += "  " + localType(phi) + " " + names[phi] + "_x;\n";
        text += " " + names[phi] + "_x = " + value(phi->getIncomingValueForBlock(from)) + ";";
      }
      for (const PHINode* phi : phis)
        text += " " + names[phi] + " = " + names[phi] + "_x;";
      return text;
    }

    std::string go(const BasicBlock* from, const BasicBlock* to) { return edge(from, to) + " goto L" + number(labels[to]) + ";"; }

    // An intrinsic's call as statements; false for one the writer does not know.
    bool intrinsic(const CallBase& c, std::string& text)
    {
      auto argument = [&](unsigned i) { return value(c.getArgOperand(i)); };
      std::string result = c.getType()->isVoidTy() || c.use_empty() ? std::string() : names[&c] + " = ";
      Type* type = c.getType();
      unsigned bits = type->isIntegerTy() ? type->getIntegerBitWidth() : 0;
      auto math = [&](const char* function, unsigned count)
      {
        text = result + "__builtin_" + function + (type->isFloatTy() ? "f(" : "(");
        for (unsigned i = 0; i < count; ++i)
          text += (i ? ", " : "") + argument(i);
        text += ");";
        return true;
      };
      auto overflow = [&](const char* builtin, bool isSigned)
      {
        Type* operand = c.getArgOperand(0)->getType();
        std::string t = intType(operand->getIntegerBitWidth(), isSigned);
        text = "{ " + t + " r; " + names[&c] + ".f1 = __builtin_" + builtin + "_overflow((" + t + ")" + argument(0) + ", (" + t + ")" + argument(1) + ", &r); " +
               names[&c] + ".f0 = (" + ty(operand) + ")r; }";
        return true;
      };
      auto select = [&](const char* op, bool isSigned)
      {
        std::string t = isSigned ? "(" + signedTy(type) + ")" : std::string();
        text = result + "(" + t + argument(0) + " " + op + " " + t + argument(1) + " ? " + argument(0) + " : " + argument(1) + ");";
        return true;
      };
      StringRef intrinsicName = c.getCalledFunction()->getName();
      if (intrinsicName.starts_with("llvm.x86."))
      {
        // An x86 instruction's intrinsic is the compiler's builtin of that instruction: its vectors passed with
        // signed lanes, as the builtins declare them, an integer vector it gives read back with unsigned ones.
        static const std::map<std::string, const char*> builtins = {
          {"sse2.pslli.w", "psllwi128"}, {"sse2.pslli.d", "pslldi128"}, {"sse2.pslli.q", "psllqi128"}, {"sse2.psrli.w", "psrlwi128"},
          {"sse2.psrli.d", "psrldi128"}, {"sse2.psrli.q", "psrlqi128"}, {"sse2.psrai.w", "psrawi128"}, {"sse2.psrai.d", "psradi128"},
          {"sse2.psll.w", "psllw128"}, {"sse2.psll.d", "pslld128"}, {"sse2.psll.q", "psllq128"}, {"sse2.psrl.w", "psrlw128"},
          {"sse2.psrl.d", "psrld128"}, {"sse2.psrl.q", "psrlq128"}, {"sse2.psra.w", "psraw128"}, {"sse2.psra.d", "psrad128"},
          {"sse2.psad.bw", "psadbw128"}, {"sse2.pmovmskb.128", "pmovmskb128"}, {"sse2.pmadd.wd", "pmaddwd128"}, {"sse2.packuswb.128", "packuswb128"},
          {"sse2.packssdw.128", "packssdw128"}, {"sse2.packsswb.128", "packsswb128"}, {"sse41.packusdw", "packusdw128"}, {"sse2.pmulh.w", "pmulhw128"},
          {"sse2.pmulhu.w", "pmulhuw128"}, {"sse2.pmulu.dq", "pmuludq128"}, {"sse2.cvttps2dq", "cvttps2dq"}, {"sse2.cvtps2dq", "cvtps2dq"},
          {"sse2.cvtpd2dq", "cvtpd2dq"}, {"sse2.cvttpd2dq", "cvttpd2dq"}, {"sse2.cvttsd2si64", "cvttsd2si64"}, {"sse2.cvttsd2si", "cvttsd2si"},
          {"sse2.cvtsd2si64", "cvtsd2si64"}, {"sse2.cvtsd2si", "cvtsd2si"}, {"sse.cvttss2si64", "cvttss2si64"}, {"sse.cvttss2si", "cvttss2si"},
          {"sse.cvtss2si64", "cvtss2si64"}, {"sse.cvtss2si", "cvtss2si"}, {"sse.movmsk.ps", "movmskps"}, {"sse2.movmsk.pd", "movmskpd"},
          {"sse.max.ss", "maxss"}, {"sse.min.ss", "minss"}, {"sse.max.ps", "maxps"}, {"sse.min.ps", "minps"},
          {"sse2.max.sd", "maxsd"}, {"sse2.min.sd", "minsd"}, {"sse2.max.pd", "maxpd"}, {"sse2.min.pd", "minpd"},
          {"sse.rcp.ps", "rcpps"}, {"sse.rsqrt.ps", "rsqrtps"}, {"sse.rcp.ss", "rcpss"}, {"sse.rsqrt.ss", "rsqrtss"},
        };
        std::string instruction = intrinsicName.substr(9).str();
        if (instruction == "sse.stmxcsr")
        {
          text = "*(u32_a*)" + argument(0) + " = __builtin_ia32_stmxcsr();";
          return true;
        }
        if (instruction == "sse.ldmxcsr")
        {
          text = "__builtin_ia32_ldmxcsr(*(u32_a*)" + argument(0) + ");";
          return true;
        }
        auto found = builtins.find(instruction);
        if (found == builtins.end())
          return false;
        std::string made = std::string("__builtin_ia32_") + found->second + "(";
        for (unsigned i = 0; i < c.arg_size(); ++i)
        {
          Type* t = c.getArgOperand(i)->getType();
          bool lanes = t->isVectorTy() && cast<VectorType>(t)->getElementType()->isIntegerTy();
          made += (i ? ", " : "") + (lanes ? "(" + vectorType(t, true) + ")" + argument(i) : t->isIntegerTy() ? "(" + signedTy(t) + ")" + argument(i) : argument(i));
        }
        made += ")";
        bool lanes = type->isVectorTy() && cast<VectorType>(type)->getElementType()->isIntegerTy();
        text = result + (lanes || type->isIntegerTy() ? "(" + ty(type) + ")" + made : made) + ";";
        return true;
      }
      if (type->isVectorTy())
      {
        // A generic operation on vectors, lane by lane.
        bool integers = cast<VectorType>(type)->getElementType()->isIntegerTy();
        std::string sign = integers ? "(" + vectorType(type, true) + ")" : std::string(), back = integers ? "(" + ty(type) + ")" : std::string();
        auto elementwise = [&](const char* function, unsigned count, bool isSigned)
        {
          text = result + (isSigned ? back : std::string()) + "__builtin_elementwise_" + function + "(";
          for (unsigned i = 0; i < count; ++i)
            text += (i ? ", " : "") + (isSigned ? sign : std::string()) + argument(i);
          text += ");";
          return true;
        };
        switch (c.getIntrinsicID())
        {
        case Intrinsic::umin: return elementwise("min", 2, false);
        case Intrinsic::umax: return elementwise("max", 2, false);
        case Intrinsic::smin: return elementwise("min", 2, true);
        case Intrinsic::smax: return elementwise("max", 2, true);
        case Intrinsic::uadd_sat: return elementwise("add_sat", 2, false);
        case Intrinsic::usub_sat: return elementwise("sub_sat", 2, false);
        case Intrinsic::sadd_sat: return elementwise("add_sat", 2, true);
        case Intrinsic::ssub_sat: return elementwise("sub_sat", 2, true);
        case Intrinsic::sqrt: return elementwise("sqrt", 1, false);
        case Intrinsic::fabs: return elementwise("abs", 1, false);
        case Intrinsic::abs: return elementwise("abs", 1, true);
        default: return false;
        }
      }
      switch (c.getIntrinsicID())
      {
      case Intrinsic::returnaddress: text = result + "(ptr)__builtin_return_address(" + number(cast<ConstantInt>(c.getArgOperand(0))->getZExtValue()) + ");"; return true;
      case Intrinsic::frameaddress: text = result + "(ptr)__builtin_frame_address(" + number(cast<ConstantInt>(c.getArgOperand(0))->getZExtValue()) + ");"; return true;
      case Intrinsic::memcpy:
      case Intrinsic::memcpy_inline: text = "__builtin_memcpy(" + argument(0) + ", " + argument(1) + ", (u64)" + argument(2) + ");"; return true;
      case Intrinsic::memmove: text = "__builtin_memmove(" + argument(0) + ", " + argument(1) + ", (u64)" + argument(2) + ");"; return true;
      case Intrinsic::memset:
      case Intrinsic::memset_inline: text = "__builtin_memset(" + argument(0) + ", (i32)" + argument(1) + ", (u64)" + argument(2) + ");"; return true;
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
      case Intrinsic::dbg_declare:
      case Intrinsic::dbg_value:
      case Intrinsic::dbg_label:
      case Intrinsic::donothing:
      case Intrinsic::assume:
      case Intrinsic::prefetch:
      case Intrinsic::stackrestore: text = ""; return true;
      case Intrinsic::stacksave: text = result.empty() ? "" : result + "(ptr)0;"; return true;
      case Intrinsic::threadlocal_address:
      case Intrinsic::expect: text = result.empty() ? "" : result + argument(0) + ";"; return true;
      case Intrinsic::eh_typeid_for: text = result.empty() ? "" : result + number(typeId(cast<Constant>(c.getArgOperand(0)))) + "u;"; return true;
      case Intrinsic::vastart:
      {
        const Function* f = c.getFunction();
        if (f->arg_empty())
        {
          fail("va_start in a function without a named parameter");
          return true;
        }
        text = "__builtin_va_start(*(__builtin_va_list*)" + argument(0) + ", p" + number(f->arg_size() - 1) + ");";
        return true;
      }
      case Intrinsic::vaend: text = "__builtin_va_end(*(__builtin_va_list*)" + argument(0) + ");"; return true;
      case Intrinsic::vacopy: text = "__builtin_va_copy(*(__builtin_va_list*)" + argument(0) + ", *(__builtin_va_list*)" + argument(1) + ");"; return true;
      case Intrinsic::trap: text = "__builtin_trap();"; return true;
      case Intrinsic::debugtrap: text = "__builtin_debugtrap();"; return true;
      case Intrinsic::ctpop:
        text = result + (bits == 64 ? "(u64)__builtin_popcountll(" + argument(0) + ");" : "(" + ty(type) + ")__builtin_popcount((u32)" + argument(0) + ");");
        return bits <= 64;
      case Intrinsic::ctlz:
        text = result + "(" + argument(0) + " ? (" + ty(type) + ")" +
               (bits == 64 ? "__builtin_clzll(" + argument(0) + ")" : "(__builtin_clz((u32)" + argument(0) + ") - " + number(32 - bits) + ")") + " : (" + ty(type) + ")" +
               number(bits) + ");";
        return bits <= 64;
      case Intrinsic::cttz:
        text = result + "(" + argument(0) + " ? (" + ty(type) + ")" + (bits == 64 ? "__builtin_ctzll(" : "__builtin_ctz((u32)") + argument(0) + ") : (" + ty(type) + ")" +
               number(bits) + ");";
        return bits <= 64;
      case Intrinsic::bswap:
        text = result + "__builtin_bswap" + number(bits) + "(" + argument(0) + ");";
        return bits == 16 || bits == 32 || bits == 64;
      case Intrinsic::fabs: return math("fabs", 1);
      case Intrinsic::sqrt: return math("sqrt", 1);
      case Intrinsic::floor: return math("floor", 1);
      case Intrinsic::ceil: return math("ceil", 1);
      case Intrinsic::trunc: return math("trunc", 1);
      case Intrinsic::rint: return math("rint", 1);
      case Intrinsic::nearbyint: return math("nearbyint", 1);
      case Intrinsic::round: return math("round", 1);
      case Intrinsic::copysign: return math("copysign", 2);
      case Intrinsic::minnum: return math("fmin", 2);
      case Intrinsic::maxnum: return math("fmax", 2);
      case Intrinsic::fma: return math("fma", 3);
      case Intrinsic::pow: return math("pow", 2);
      case Intrinsic::sin: return math("sin", 1);
      case Intrinsic::cos: return math("cos", 1);
      case Intrinsic::exp: return math("exp", 1);
      case Intrinsic::log: return math("log", 1);
      case Intrinsic::sadd_with_overflow: return overflow("add", true);
      case Intrinsic::uadd_with_overflow: return overflow("add", false);
      case Intrinsic::ssub_with_overflow: return overflow("sub", true);
      case Intrinsic::usub_with_overflow: return overflow("sub", false);
      case Intrinsic::smul_with_overflow: return overflow("mul", true);
      case Intrinsic::umul_with_overflow: return overflow("mul", false);
      case Intrinsic::smax: return select(">", true);
      case Intrinsic::smin: return select("<", true);
      case Intrinsic::umax: return select(">", false);
      case Intrinsic::umin: return select("<", false);
      case Intrinsic::fshl:
      case Intrinsic::fshr:
      {
        // The high (fshl) or low (fshr) half of a:b shifted by c modulo the width: a rotate when a is b.
        if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
          return false;
        std::string wide = bits == 64 ? "(u64)" : "(u32)", count = "(" + wide + argument(2) + " & " + number(bits - 1) + ")", rest = "(" + number(bits) + " - " + count + ")";
        bool left = c.getIntrinsicID() == Intrinsic::fshl;
        text = result + "(" + count + " ? (" + ty(type) + ")(" + (left ? "(" + wide + argument(0) + " << " + count + ") | (" + wide + argument(1) + " >> " + rest + ")"
                                                                       : "(" + wide + argument(0) + " << " + rest + ") | (" + wide + argument(1) + " >> " + count + ")") +
               ") : " + argument(left ? 0 : 1) + ");";
        return true;
      }
      case Intrinsic::abs:
        text = result + "((" + signedTy(type) + ")" + argument(0) + " < 0 ? (" + ty(type) + ")(0 - " + argument(0) + ") : " + argument(0) + ");";
        return true;
      default: break;
      }
      return false;
    }

    // The number a catch clause's descriptor has in this function: what a landing pad's selector and
    // llvm.eh.typeid.for agree on.
    unsigned typeId(const Constant* descriptor)
    {
      descriptor = descriptor->stripPointerCasts();
      auto found = typeIds.find(descriptor);
      if (found == typeIds.end())
        found = typeIds.insert({descriptor, unsigned(typeIds.size()) + 1}).first;
      return found->second;
    }

    // The program's function `symbol` (the exception runtime's kpp_eh_offset and kpp_eh_terminate, which every
    // program that throws defines once), declared by the module or by the writer.
    std::string runtimeFunction(const char* symbol, const char* declaration)
    {
      if (const Function* f = m.getFunction(symbol))
        return "((" + std::string(declaration) + ")&" + name(f) + ")";
      std::string local = std::string("kpp_x_") + symbol;
      if (runtimeDeclared.insert(local).second)
        extras += "extern \"C\" { void " + local + "() __asm__(\"" + quoted(dl.getGlobalPrefix() ? std::string(1, dl.getGlobalPrefix()) + symbol : std::string(symbol)) +
                  "\"); }\n";
      return "((" + std::string(declaration) + ")&" + local + ")";
    }

    // A call's statement, its result assigned to the call's local. An sret argument is where the result goes, a
    // byval one the storage read at its address.
    std::string call(const CallBase& c)
    {
      if (c.isInlineAsm())
      {
        // The nop the code generator writes after an invoke (Emitter::emitCall) has no meaning here.
        const std::string& text = cast<InlineAsm>(c.getCalledOperand())->getAsmString();
        if (text == "nop")
          return "";
        // A spin loop's hint to the processor (Statements.cpp: x86's pause, AArch64's yield).
        if (text == "pause")
          return "__builtin_ia32_pause();";
        if (text == "yield")
          return "__asm__ volatile(\"yield\");";
        // x86's cpuid (SseBuiltins.cpp): eax and ecx in, the four registers out, as the result's four members.
        if (text == "cpuid" && c.arg_size() == 2)
        {
          std::string local = names.lookup(&c);
          return "__asm__ volatile(\"cpuid\" : \"=a\"(" + local + ".f0), \"=b\"(" + local + ".f1), \"=c\"(" + local + ".f2), \"=d\"(" + local + ".f3) : \"a\"(" +
                 value(c.getArgOperand(0)) + "), \"c\"(" + value(c.getArgOperand(1)) + "));";
        }
        fail("inline assembly: " + cast<InlineAsm>(c.getCalledOperand())->getAsmString());
        return "";
      }
      if (c.getIntrinsicID() != Intrinsic::not_intrinsic)
      {
        std::string text;
        if (!intrinsic(c, text))
          fail("the intrinsic " + c.getCalledFunction()->getName().str());
        return text;
      }
      if (c.getCallingConv() != CallingConv::C)
        fail("a call by another calling convention");
      const Function* direct = dyn_cast<Function>(c.getCalledOperand());
      if (direct && direct->getName() == "_Unwind_RaiseException" && c.arg_size() == 1)
      {
        // (with the program's two functions the search needs, which a program that throws defines: this is kpp_throw)
        return "kpp_cpp_raise(" + value(c.getArgOperand(0)) + ", " + runtimeFunction("kpp_eh_terminate", "ptr") + ", " + runtimeFunction("kpp_eh_offset", "ptr") + ");";
      }
      FunctionType* type = c.getFunctionType();
      Type* structResult = nullptr;
      std::string destination, arguments, parameters;
      bool first = true;
      for (unsigned i = 0; i < c.arg_size(); ++i)
      {
        Type* t = c.getArgOperand(i)->getType();
        if (Type* result = c.getParamStructRetType(i))
        {
          structResult = result;
          destination = value(c.getArgOperand(i));
          continue;
        }
        Type* byValue = c.getParamByValType(i);
        std::string parameter = parameterType(t, c.paramHasAttr(i, Attribute::SExt), byValue, c.getParamAlign(i));
        arguments += (first ? "" : ", ") + (byValue ? "(*(" + parameter + "*)" + value(c.getArgOperand(i)) + ")" : value(c.getArgOperand(i)));
        if (i < type->getNumParams())
          parameters += (parameters.empty() ? "" : ", ") + parameter;
        first = false;
      }
      std::string result = resultType(type->getReturnType(), c.hasRetAttr(Attribute::SExt), structResult);
      // The function itself where the call's type is its own; through a pointer of the call's type otherwise
      // (a call through a value, or of a function declared with another type).
      std::string callee;
      if (direct && direct->getFunctionType() == type && structResultOf(*direct) == structResult)
        callee = name(direct);
      else
      {
        if (type->isVarArg())
          parameters += parameters.empty() ? "..." : ", ...";
        callee = "((" + result + " (*)(" + parameters + "))" + value(c.getCalledOperand()) + ")";
      }
      std::string text = callee + "(" + arguments + ");";
      if (structResult)
        return "*(" + result + "*)" + destination + " = " + text;
      if (!type->getReturnType()->isVoidTy() && !c.use_empty())
        return names[&c] + " = " + text;
      return text;
    }

    // A landing pad's clauses as the search reads them (the runtime's kpp_cpp_raise): `TABLE, COUNT` of a table of the
    // function's, each entry a catch clause's descriptor, null for one that takes everything, 1 for a noexcept
    // function's filter. "" for a pad of cleanups alone, which the search passes.
    std::string clauseTable(const LandingPadInst* pad)
    {
      if (!pad || pad->getNumClauses() == 0)
        return "";
      auto found = clauseTables.find(pad);
      if (found != clauseTables.end())
        return found->second;
      std::string table = "kpp_c" + number(clauseTables.size()), entries;
      for (unsigned i = 0; i < pad->getNumClauses(); ++i)
        entries += (i ? ", " : "") + (pad->isFilter(i) ? std::string("(ptr)1") : constant(pad->getClause(i)->stripPointerCasts()));
      locals += "  static const ptr " + table + "[] = { " + entries + " };\n";
      return clauseTables[pad] = table + ", " + number(pad->getNumClauses());
    }

    void landingPad(const LandingPadInst& pad)
    {
      std::string local = names.lookup(&pad);
      body += "  " + local + ".f0 = kpp_lp; " + local + ".f1 = 0;\n";
      // The first clause that takes the exception, in the pad's order, as kpp's personality searches them
      // (Driver/Runtime.cpp, __kpp_personality_v0): a descriptor the thrown type or one of its bases
      // (kpp_eh_offset), null everything; the empty filter is a noexcept function's, which no exception leaves.
      std::string chain;
      for (unsigned i = 0; i < pad.getNumClauses(); ++i)
      {
        Constant* clause = pad.getClause(i);
        std::string lead = chain.empty() ? "  " : "  else ";
        if (pad.isFilter(i))
        {
          if (!clause->isNullValue() && clause->getType()->getArrayNumElements() != 0)
            fail("an exception specification that is not empty");
          chain += lead + "{ kpp_cpp_through_noexcept(kpp_lp); }\n";
          break;
        }
        if (clause->isNullValue())
        {
          chain += lead + "{ " + local + ".f1 = " + number(typeId(clause)) + "u; }\n";
          break;
        }
        chain += lead + "if ((i64)" + runtimeFunction("kpp_eh_offset", "u64 (*)(ptr, ptr)") + "(*(ptr_a*)(kpp_lp + 40), " + constant(clause->stripPointerCasts()) + ") >= 0) { " +
                 local + ".f1 = " + number(typeId(clause)) + "u; }\n";
      }
      body += chain;
    }

    void instruction(const Instruction& i)
    {
      std::string local = names.lookup(&i);
      auto assign = [&](const std::string& text) { body += "  " + local + " = " + text + ";\n"; };
      auto statement = [&](const std::string& text)
      {
        if (!text.empty())
          body += "  " + text + "\n";
      };
      const BasicBlock* block = i.getParent();
      switch (i.getOpcode())
      {
      case Instruction::Alloca:
      {
        auto& a = cast<AllocaInst>(i);
        if (isa<ConstantInt>(a.getArraySize()))
          return; // (a local of the function: declared)
        assign("(ptr)__builtin_alloca_with_align((u64)" + value(a.getArraySize()) + " * " + number(dl.getTypeAllocSize(a.getAllocatedType())) + "ull, " +
               number(a.getAlign().value() * 8) + ")");
        return;
      }
      case Instruction::Load:
      {
        auto& l = cast<LoadInst>(i);
        if (l.isVolatile() && l.isAtomic())
          fail("a volatile atomic load");
        if (l.isAtomic())
          assign("__atomic_load_n((" + ty(l.getType()) + "*)" + value(l.getPointerOperand()) + ", " + ordering(l.getOrdering()) + ")");
        else
          assign(load(l.getType(), value(l.getPointerOperand()), l.getAlign(), l.isVolatile()));
        return;
      }
      case Instruction::Store:
      {
        auto& s = cast<StoreInst>(i);
        Type* t = s.getValueOperand()->getType();
        if (s.isVolatile() && s.isAtomic())
          fail("a volatile atomic store");
        if (s.isAtomic())
          statement("__atomic_store_n((" + ty(t) + "*)" + value(s.getPointerOperand()) + ", " + value(s.getValueOperand()) + ", " + ordering(s.getOrdering()) + ");");
        else
          statement(store(t, value(s.getPointerOperand()), s.getAlign(), value(s.getValueOperand()), s.isVolatile()));
        return;
      }
      case Instruction::AtomicRMW:
      {
        auto& a = cast<AtomicRMWInst>(i);
        const char* function = nullptr;
        switch (a.getOperation())
        {
        case AtomicRMWInst::Xchg: function = "__atomic_exchange_n"; break;
        case AtomicRMWInst::Add: function = "__atomic_fetch_add"; break;
        case AtomicRMWInst::Sub: function = "__atomic_fetch_sub"; break;
        case AtomicRMWInst::And: function = "__atomic_fetch_and"; break;
        case AtomicRMWInst::Or: function = "__atomic_fetch_or"; break;
        case AtomicRMWInst::Xor: function = "__atomic_fetch_xor"; break;
        case AtomicRMWInst::Nand: function = "__atomic_fetch_nand"; break;
        default: fail("an atomic read-modify-write that is no exchange, add, sub, and, or, xor or nand"); return;
        }
        std::string text = std::string(function) + "((" + ty(a.getType()) + "*)" + value(a.getPointerOperand()) + ", " + value(a.getValOperand()) + ", " +
                           ordering(a.getOrdering()) + ")";
        if (a.use_empty())
          statement(text + ";");
        else
          assign(text);
        return;
      }
      case Instruction::AtomicCmpXchg:
      {
        auto& x = cast<AtomicCmpXchgInst>(i);
        std::string t = ty(x.getCompareOperand()->getType());
        statement("{ " + t + " expected = " + value(x.getCompareOperand()) + "; " + local + ".f1 = __atomic_compare_exchange_n((" + t + "*)" + value(x.getPointerOperand()) +
                  ", &expected, " + value(x.getNewValOperand()) + ", " + (x.isWeak() ? "1" : "0") + ", " + ordering(x.getSuccessOrdering()) + ", " +
                  ordering(x.getFailureOrdering()) + "); " + local + ".f0 = expected; }");
        return;
      }
      case Instruction::Fence: statement(std::string("__atomic_thread_fence(") + ordering(cast<FenceInst>(i).getOrdering()) + ");"); return;
      case Instruction::VAArg:
        assign("__builtin_va_arg(*(__builtin_va_list*)" + value(i.getOperand(0)) + ", " + ty(i.getType()) + ")");
        return;
      case Instruction::PHI: return; // (assigned on the branches into its block)
      case Instruction::InsertValue:
      {
        auto& v = cast<InsertValueInst>(i);
        assign(value(v.getAggregateOperand()));
        statement(local + memberPath(v.getType(), v.getIndices()) + " = " + value(v.getInsertedValueOperand()) + ";");
        return;
      }
      case Instruction::InsertElement:
        assign(value(i.getOperand(0)));
        statement(local + "[" + value(i.getOperand(2)) + "] = " + value(i.getOperand(1)) + ";");
        return;
      case Instruction::LandingPad: landingPad(cast<LandingPadInst>(i)); return;
      case Instruction::Call: statement(call(cast<CallInst>(i))); return;
      case Instruction::Invoke:
      {
        // A call that may throw: a kpp exception arrives as the runtime's C++ exception, and the landing pad
        // reads it from kpp_lp.
        auto& v = cast<InvokeInst>(i);
        std::string clauses = clauseTable(v.getUnwindDest()->getLandingPadInst());
        if (clauses.empty())
          body += "  try { " + call(v) + " } catch (kpp_cpp_exception& e) { kpp_lp = e.h;" + go(block, v.getUnwindDest()) + " }\n";
        else
          // (its clauses listed for the search while the call runs: kpp_cpp_raise, the runtime)
          body += "  { kpp_cpp_scope s = { kpp_cpp_scopes, " + clauses + " }; kpp_cpp_scopes = &s; try { " + call(v) +
                  " } catch (kpp_cpp_exception& e) { kpp_cpp_scopes = s.outer; kpp_lp = e.h;" + go(block, v.getUnwindDest()) + " } kpp_cpp_scopes = s.outer; }\n";
        body += " " + go(block, v.getNormalDest()) + "\n";
        return;
      }
      case Instruction::Ret:
        if (!sretName.empty())
          statement("return " + sretName + ";");
        else if (i.getNumOperands())
          statement("return " + value(i.getOperand(0)) + ";");
        else
          statement("return;");
        return;
      case Instruction::Br:
      {
        auto& b = cast<BranchInst>(i);
        if (b.isConditional())
          body += "  if (" + value(b.getCondition()) + ") {" + go(block, b.getSuccessor(0)) + " }\n";
        body += " " + go(block, b.getSuccessor(b.isConditional() ? 1 : 0)) + "\n";
        return;
      }
      case Instruction::Switch:
      {
        auto& s = cast<SwitchInst>(i);
        unsigned bits = s.getCondition()->getType()->getIntegerBitWidth();
        if (bits > 64)
        {
          fail("a switch on an integer wider than 64 bits");
          return;
        }
        body += std::string("  switch (") + (bits == 64 ? "(u64)" : "(u32)") + value(s.getCondition()) + ")\n  {\n";
        for (const auto& c : s.cases())
          body += "  case " + number(c.getCaseValue()->getZExtValue()) + (bits == 64 ? "ull" : "u") + ": {" + go(block, c.getCaseSuccessor()) + " }\n";
        body += "  default: {" + go(block, s.getDefaultDest()) + " }\n  }\n";
        return;
      }
      case Instruction::Unreachable: statement("__builtin_unreachable();"); return;
      case Instruction::Resume: statement("kpp_cpp_resume(" + value(i.getOperand(0)) + ".f0);"); return;
      default: break;
      }
      assign(expression(&i));
    }

    // With debug information (--debug-info): the statements that follow are i's line of kpp source, so that the C++
    // compiler's line tables, a debugger and a stack trace name kpp's files, not the translation unit.
    void sourceLine(const Instruction& i)
    {
      const DebugLoc& at = i.getDebugLoc();
      if (!at || !at.getLine())
        return;
      auto* scope = dyn_cast_or_null<DIScope>(at.getScope());
      const DIFile* file = scope ? scope->getFile() : nullptr;
      if (!file || (file == lineFile && at.getLine() == line))
        return;
      auto found = sourceFiles.find(file);
      if (found == sourceFiles.end())
      {
        std::string path = file->getFilename().str();
        bool absolute = path.starts_with("/") || (path.size() > 1 && path[1] == ':');
        if (!absolute && !file->getDirectory().empty())
          path = file->getDirectory().str() + "/" + path;
        for (char& c : path)
          if (c == '\\')
            c = '/';
        found = sourceFiles.insert({file, quoted(path)}).first;
      }
      lineFile = file;
      line = at.getLine();
      body += "#line " + number(line) + " \"" + found->second + "\"\n";
    }

    void function(const Function& f)
    {
      current = &f;
      lineFile = nullptr;
      line = 0;
      names.clear();
      labels.clear();
      inlined.clear();
      depths.clear();
      typeIds.clear();
      clauseTables.clear();
      phiCopies.clear();
      locals.clear();
      body.clear();
      sretName.clear();
      if (f.getCallingConv() != CallingConv::C)
        fail("a function of another calling convention");
      if (f.hasPersonalityFn() && f.getPersonalityFn()->stripPointerCasts()->getName() != "__kpp_personality_v0")
        fail("a function whose exceptions are not the Itanium model's (its personality is " + f.getPersonalityFn()->stripPointerCasts()->getName().str() + ")");
      // Each parameter as the body reads it: a number passed sign-extended as its unsigned value, a byval one's
      // and the sret one's address.
      for (const Argument& a : f.args())
      {
        unsigned i = a.getArgNo();
        std::string parameter = "p" + number(i);
        if (Type* result = f.getParamStructRetType(i))
        {
          sretName = "kpp_sret";
          locals += "  " + blob(dl.getTypeAllocSize(result), dl.getABITypeAlign(result).value()) + " kpp_sret;\n";
          names[&a] = "((ptr)&kpp_sret)";
        }
        else if (f.getParamByValType(i))
          names[&a] = "((ptr)&" + parameter + ")";
        else if (f.hasParamAttribute(i, Attribute::SExt) && a.getType()->isIntegerTy() && !a.getType()->isIntegerTy(1))
          names[&a] = "((" + ty(a.getType()) + ")" + parameter + ")";
        else
          names[&a] = parameter;
      }
      unsigned count = 0;
      bool pads = false;
      for (const BasicBlock& block : f)
      {
        labels[&block] = unsigned(labels.size());
        for (const Instruction& i : block)
        {
          pads = pads || isa<InvokeInst>(i) || isa<LandingPadInst>(i);
          if (auto* a = dyn_cast<AllocaInst>(&i); a && isa<ConstantInt>(a->getArraySize()))
          {
            // A local: its storage, and its address wherever the alloca is named.
            std::string storage = "a" + number(count++);
            uint64_t size = dl.getTypeAllocSize(a->getAllocatedType()) * cast<ConstantInt>(a->getArraySize())->getZExtValue();
            locals += "  alignas(" + number(a->getAlign().value()) + ") u8 " + storage + "[" + number(std::max<uint64_t>(size, 1)) + "];\n";
            names[&i] = "((ptr)" + storage + ")";
            continue;
          }
          if (i.getType()->isVoidTy())
            continue;
          if (writtenInPlace(i))
          {
            inlined.insert(&i);
            continue;
          }
          std::string local = "t" + number(count++);
          names[&i] = local;
          locals += "  " + localType(&i) + " " + local + ";\n";
        }
      }
      if (pads)
        locals += "  ptr kpp_lp;\n";
      for (const BasicBlock& block : f)
      {
        body += "L" + number(labels[&block]) + ":\n";
        for (const Instruction& i : block)
          if (!inlined.count(&i))
          {
            sourceLine(i);
            instruction(i);
          }
      }
      std::string storage = f.hasLocalLinkage() ? "static " : f.isWeakForLinker() ? "inline " : "";
      // The program's entry reports an exception nobody caught (kpp_eh_terminate's report, through the runtime).
      bool entry = f.getName() == "main" || f.getName() == "wmain";
      bodies += storage + attributes(f, 0, true) + " " + signature(f, true) + "\n{\n" + (entry ? "  try {\n" : "") + locals + body + (entry ? "  } catch (kpp_cpp_exception& e) { kpp_cpp_uncaught(e.h); }\n" : "") +
                "}\n";
      current = nullptr;
    }

    // --- globals ---

    struct Field
    {
      uint64_t offset;
      uint64_t size;
      std::string type;
      std::string initializer;
      uint64_t count = 0; // an array's elements; 0: one value
    };

    // The bytes of text as an array's initializer: a string literal where one fits (the bytes after it are
    // zeros), a list of numbers otherwise.
    std::string bytes(StringRef text)
    {
      size_t used = text.size();
      while (used && text[used - 1] == 0)
        --used;
      if (used < text.size())
        return "\"" + quoted(text.substr(0, used)) + "\"";
      std::string list = "{";
      for (size_t i = 0; i < text.size(); ++i)
        list += (i ? "," : "") + number(uint8_t(text[i]));
      return list + "}";
    }

    // c's parts that are not zero, each at its offset in the global, in the order of their offsets.
    void flatten(const Constant* c, uint64_t offset, std::vector<Field>& fields)
    {
      Type* t = c->getType();
      if (c->isNullValue() || isa<UndefValue>(c))
        return;
      uint64_t size = dl.getTypeStoreSize(t);
      if (auto* i = dyn_cast<ConstantInt>(c))
      {
        if (size == 16 && dl.isLittleEndian())
        {
          fields.push_back({offset, 8, "u64", number(i->getValue().getLoBits(64).getZExtValue()) + "ull"});
          fields.push_back({offset + 8, 8, "u64", number(i->getValue().getHiBits(64).getZExtValue()) + "ull"});
          return;
        }
        if (size != 1 && size != 2 && size != 4 && size != 8)
        {
          fail("a global's integer of " + number(size) + " bytes");
          return;
        }
        fields.push_back({offset, size, intType(unsigned(size) * 8), number(i->getZExtValue()) + (size == 8 ? "ull" : "u")});
        return;
      }
      if (auto* f = dyn_cast<ConstantFP>(c))
      {
        if (!t->isFloatTy() && !t->isDoubleTy())
        {
          fail("a global's floating-point number that is no float or double");
          return;
        }
        fields.push_back({offset, size, t->isFloatTy() ? "u32" : "u64", number(f->getValueAPF().bitcastToAPInt().getZExtValue()) + (t->isFloatTy() ? "u" : "ull")});
        return;
      }
      if (auto* d = dyn_cast<ConstantDataSequential>(c))
      {
        if (d->getElementType()->isIntegerTy(8))
        {
          StringRef raw = d->getRawDataValues();
          fields.push_back({offset, raw.size(), "u8", bytes(raw), raw.size()});
          return;
        }
        uint64_t step = dl.getTypeAllocSize(d->getElementType());
        for (unsigned i = 0; i < d->getNumElements(); ++i)
          flatten(d->getElementAsConstant(i), offset + i * step, fields);
        return;
      }
      if (isa<ConstantVector>(c))
      {
        uint64_t step = dl.getTypeStoreSize(cast<VectorType>(t)->getElementType());
        for (unsigned i = 0; i < c->getNumOperands(); ++i)
          flatten(cast<Constant>(c->getOperand(i)), offset + i * step, fields);
        return;
      }
      if (isa<ConstantArray>(c))
      {
        uint64_t step = dl.getTypeAllocSize(t->getArrayElementType());
        for (unsigned i = 0; i < c->getNumOperands(); ++i)
          flatten(cast<Constant>(c->getOperand(i)), offset + i * step, fields);
        return;
      }
      if (auto* s = dyn_cast<ConstantStruct>(c))
      {
        const StructLayout* layout = dl.getStructLayout(s->getType());
        for (unsigned i = 0; i < s->getNumOperands(); ++i)
          flatten(s->getOperand(i), offset + layout->getElementOffset(i), fields);
        return;
      }
      if ((isa<GlobalValue>(c) || isa<ConstantExpr>(c)) && (t->isPointerTy() || (t->isIntegerTy() && (size == 4 || size == 8))))
      {
        fields.push_back({offset, size, t->isPointerTy() ? "ptr" : intType(unsigned(size) * 8), constant(c)});
        return;
      }
      std::string text;
      raw_string_ostream(text) << *c;
      fail("a global's initializer " + text.substr(0, 200));
    }

    std::string attributes(const GlobalObject& g, uint64_t align, bool definition)
    {
      std::string list;
      auto add = [&](const std::string& attribute) { list += (list.empty() ? "" : ", ") + attribute; };
      if (align)
        add("aligned(" + number(align) + ")");
      if (g.hasSection())
        add("section(\"" + quoted(g.getSection()) + "\")");
      if (g.getVisibility() == GlobalValue::HiddenVisibility)
        add("visibility(\"hidden\")");
      if (g.hasExternalWeakLinkage())
        add("weak");
      // A definition the linker shares (linkonce, weak) is written in every translation unit that has it, used
      // there or not: the code generator defines a shared constant in one module, which the others only name.
      // (The attribute means something on a definition alone.)
      if (definition && (used.count(&g) || g.isWeakForLinker()))
        add("used");
      if (auto* f = dyn_cast<Function>(&g))
      {
        if (f->hasFnAttribute(Attribute::NoReturn))
          add("noreturn");
        if (f->hasFnAttribute(Attribute::NoInline))
          add("noinline");
        if (f->hasFnAttribute(Attribute::AlwaysInline))
          add("always_inline");
        if (f->hasFnAttribute(Attribute::Cold))
          add("cold");
        if (f->hasFnAttribute(Attribute::NoUnwind))
          add("nothrow");
      }
      return list.empty() ? "" : " __attribute__((" + list + "))";
    }

    // A global's storage type, G<n>: its parts that are not zero as members at their offsets, the bytes between
    // them members too; and its initializer.
    void global(const GlobalVariable& g, std::string& declarations, std::string& definitions)
    {
      current = nullptr;
      std::string variable = name(&g), type = "G" + variable.substr(1);
      uint64_t align = std::max<uint64_t>(g.getAlign() ? g.getAlign()->value() : 1, dl.getABITypeAlign(g.getValueType()).value());
      bool defined = g.hasInitializer() && !g.hasAvailableExternallyLinkage();
      bool local = g.hasLocalLinkage(), shared = defined && g.isWeakForLinker();
      if (g.hasCommonLinkage())
        fail("a common global (" + g.getName().str() + ")");
      std::string qualifiers = std::string(g.isThreadLocal() ? "__thread " : "") + (g.isConstant() ? "const " : "");
      std::string import = g.hasDLLImportStorageClass() ? "__declspec(dllimport) " : "";
      std::string declaration = import + "extern " + (shared ? "inline " : "") + qualifiers + type + " " + variable + symbol(g) + attributes(g, align, false) + ";";
      declarations += "struct " + type + ";\n" + (local ? "namespace { " + declaration + " }\n" : declaration + "\n");
      if (!defined)
        return;
      std::vector<Field> fields;
      flatten(g.getInitializer(), 0, fields);
      uint64_t size = dl.getTypeAllocSize(g.getValueType());
      std::string members, initializer;
      uint64_t at = 0;
      unsigned index = 0;
      auto gap = [&](uint64_t to)
      {
        if (to > at)
        {
          members += " u8 z" + number(index++) + "[" + number(to - at) + "];";
          initializer += (initializer.empty() ? "" : ", ") + std::string("{}");
        }
      };
      for (const Field& field : fields)
      {
        if (field.offset < at)
        {
          fail("a global whose parts overlap (" + g.getName().str() + ")");
          return;
        }
        gap(field.offset);
        members += " " + field.type + " f" + number(index++) + (field.count ? "[" + number(field.count) + "]" : std::string()) + ";";
        initializer += (initializer.empty() ? "" : ", ") + field.initializer;
        at = field.offset + field.size;
      }
      gap(std::max<uint64_t>(size, at ? at : 1));
      definitions += "#pragma pack(push, 1)\nstruct " + type + " {" + members + " };\n#pragma pack(pop)\n";
      std::string definition = std::string(shared ? "inline " : "") + qualifiers + type + " " + variable + attributes(g, align, true) + " = {" + (fields.empty() ? std::string() : " " + initializer + " ") + "};";
      definitions += local ? "namespace { " + definition + " }\n" : definition + "\n";
    }

    // llvm.global_ctors' functions, run at startup in its order.
    void constructors(const GlobalVariable& list)
    {
      auto* entries = dyn_cast<ConstantArray>(list.getInitializer());
      if (!entries)
        return; // (an empty list)
      for (unsigned i = 0; i < entries->getNumOperands(); ++i)
      {
        auto* entry = cast<ConstantStruct>(entries->getOperand(i));
        uint64_t priority = cast<ConstantInt>(entry->getOperand(0))->getZExtValue();
        auto* f = dyn_cast<Function>(entry->getOperand(1)->stripPointerCasts());
        if (!f || !entry->getOperand(2)->isNullValue())
        {
          fail("a constructor that is no function, or one with associated data");
          continue;
        }
        ctors += "__attribute__((constructor" + (priority == 65535 ? std::string() : "(" + number(priority) + ")") + ")) static void kpp_ctor" + number(i) + "() { ((void (*)())&" +
                 name(f) + ")(); }\n";
      }
    }

    std::string write()
    {
      if (!m.getModuleInlineAsm().empty())
        fail("module-level assembly");
      for (const char* list : {"llvm.used", "llvm.compiler.used"})
        if (const GlobalVariable* g = m.getGlobalVariable(list, true); g && g->hasInitializer())
          if (auto* entries = dyn_cast<ConstantArray>(g->getInitializer()))
            for (const Use& entry : entries->operands())
              if (auto* named = dyn_cast<GlobalObject>(entry->stripPointerCasts()))
                used.insert(named);
      // An alias is a symbol of the assembler's: its global at an offset (a debug build's name for a vtable's first
      // slot, which Visual Studio reads a dynamic type from). C++ has no word for one at an offset.
      for (const GlobalAlias& alias : m.aliases())
      {
        APInt offset(dl.getIndexTypeSizeInBits(alias.getType()), 0);
        auto* target = dyn_cast<GlobalObject>(alias.getAliasee()->stripAndAccumulateConstantOffsets(dl, offset, true));
        if (!target || !target->hasName() || !alias.hasName() || target->isDeclaration())
        {
          fail("an alias that is no named global of this module at an offset (" + alias.getName().str() + ")");
          continue;
        }
        std::string prefix = dl.getGlobalPrefix() ? std::string(1, dl.getGlobalPrefix()) : std::string();
        auto assembled = [&](const GlobalValue& g) { return "\\\"" + quoted(quoted(prefix + g.getName().str())) + "\\\""; };
        ctors += "__asm__(\"" + (alias.hasLocalLinkage() ? std::string() : ".globl " + assembled(alias) + "\\n") + ".set " + assembled(alias) + ", " + assembled(*target) + " + " +
                 std::to_string(offset.getSExtValue()) + "\\n\");\n";
        used.insert(target); // (the assembler names it: it stays in the object)
        // (and to the C++ a byte declared elsewhere, for the code that takes its address: a constructor's vtable pointer)
        extras += "extern const u8 " + name(&alias) + symbol(alias) + ";\n";
      }
      // The bodies and the initializers first: they name what has to be declared.
      for (const Function& f : m)
        if (!f.isDeclaration() && !f.hasAvailableExternallyLinkage())
          function(f);
      std::string declarations, definitions;
      std::vector<const GlobalVariable*> written;
      for (const GlobalVariable& g : m.globals())
      {
        if (g.getName() == "llvm.global_ctors")
        {
          constructors(g);
          continue;
        }
        if (g.getName().starts_with("llvm."))
        {
          if (g.getName() != "llvm.used" && g.getName() != "llvm.compiler.used")
            fail("the global " + g.getName().str());
          continue;
        }
        if (g.hasInitializer() && !g.hasAvailableExternallyLinkage())
        {
          global(g, declarations, definitions);
          written.push_back(&g);
        }
      }
      // (a declared global an initializer or a body names; the definitions' initializers may name more)
      DenseSet<const GlobalVariable*> done(written.begin(), written.end());
      for (bool more = true; more;)
      {
        more = false;
        for (const GlobalVariable& g : m.globals())
          if (referenced.count(&g) && done.insert(&g).second)
          {
            global(g, declarations, definitions);
            more = true;
          }
      }
      std::string prototypes;
      for (const Function& f : m)
      {
        bool defined = !f.isDeclaration() && !f.hasAvailableExternallyLinkage();
        if (!defined && !referenced.count(&f))
          continue;
        if (f.isIntrinsic())
        {
          fail("the address of the intrinsic " + f.getName().str());
          continue;
        }
        current = nullptr;
        std::string storage = f.hasLocalLinkage() ? "static " : defined && f.isWeakForLinker() ? "inline " : "";
        if (f.hasDLLImportStorageClass())
          storage = "__declspec(dllimport) " + storage;
        prototypes += storage + signature(f, false) + symbol(f) + attributes(f, 0, false) + ";\n";
      }
      return std::string("// Generated by kpp (--via-cpp): an intermediate build step, not source to read.\n") + prelude + types + extras + declarations + prototypes + definitions +
             bodies + ctors;
    }

    const Module& m;
    const DataLayout& dl;
    std::string error;
    const Function* current = nullptr; // the function being written, for the messages

    // The translation unit's parts, in the order they are written out.
    std::string types, extras, bodies, ctors;
    DenseMap<Type*, std::string> aggregates;
    std::set<std::string> blobs, vectors, runtimeDeclared;
    DenseMap<const GlobalValue*, unsigned> globalIds;
    DenseSet<const GlobalValue*> referenced;
    DenseSet<const GlobalObject*> used;

    // The function being written.
    DenseMap<const Value*, std::string> names;
    DenseMap<const BasicBlock*, unsigned> labels;
    DenseSet<const Instruction*> inlined;
    DenseMap<const Instruction*, unsigned> depths;
    DenseMap<const Constant*, unsigned> typeIds;
    DenseMap<const LandingPadInst*, std::string> clauseTables;
    DenseSet<const PHINode*> phiCopies;
    std::string locals, body, sretName;
    const DIFile* lineFile = nullptr; // the last #line written in it
    unsigned line = 0;
    DenseMap<const DIFile*, std::string> sourceFiles;
  };
}

bool writeCppModule(std::string_view ir, const CppWriterOptions& options, std::string& cpp, std::string& error)
{
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  std::unique_ptr<llvm::Module> module = llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(ir.data(), ir.size()), options.path), diagnostic, context);
  if (!module)
  {
    std::string text;
    llvm::raw_string_ostream out(text);
    diagnostic.print("kpp", out);
    error = out.str();
    return false;
  }
  Writer writer(*module);
  cpp = writer.write();
  if (!writer.error.empty())
  {
    error = "kpp's C++ back end does not write " + writer.error;
    return false;
  }
  return true;
}

#endif
