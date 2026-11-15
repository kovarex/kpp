// Test coverage's probes and region maps (--coverage, docs/design.md 28; Codegen/CoverageProbes.hpp).
#include "Emitter.hpp"
#include "../Ast/AstWalk.hpp"

#include <cinttypes>
#include <cstdio>

namespace
{
// Text as an LLVM c"..." constant's contents.
std::string llvmBytes(const std::string& text)
{
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size() + 16);
  for (unsigned char c : text)
    if (c >= 32 && c < 127 && c != '"' && c != '\\')
      out += char(c);
    else
    {
      out += '\\';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  return out;
}

bool isBracketOpen(const Token& t)
{
  return t.is(P::LParen) || t.is(P::LBracket) || t.is(P::LBrace);
}

bool isBracketClose(const Token& t)
{
  return t.is(P::RParen) || t.is(P::RBracket) || t.is(P::RBrace);
}
}

auto codegen::Emitter::coverArrayName() const -> std::string
{
  std::string name = currentFunctionName.substr(1);
  if (name.size() >= 2 && name.front() == '"')
    name = name.substr(1, name.size() - 2);
  return "@\"kpp.cov." + name + "\"";
}

auto codegen::Emitter::coverProbeAt(size_t probe) const -> std::string
{
  return probe == 0 ? coverArrayName() : "getelementptr inbounds (i8, ptr " + coverArrayName() + ", i64 " + std::to_string(probe) + ")";
}

auto codegen::Emitter::coverProbe(char kind, const std::string& place) -> size_t
{
  size_t probe = coverCount++;
  coverMap += kind;
  if (!place.empty())
    coverMap += " " + place;
  coverMap += "\n";
  coverSet(coverProbeAt(probe));
  return probe;
}

auto codegen::Emitter::coverSet(const std::string& address) -> void
{
  // Read first, written once: a store each time would have every thread that runs the code write the
  // byte's cache line again and again (the game's tests took three times as long: thread contention
  // on the probes of the code all its threads run).
  std::string old = newTemp(), unset = newTemp(), setL = newLabel("cov.set"), doneL = newLabel("cov.done");
  fn += "  " + old + " = load i8, ptr " + address + "\n";
  fn += "  " + unset + " = icmp eq i8 " + old + ", 0\n";
  fn += "  br i1 " + unset + ", label %" + setL + ", label %" + doneL + ", !prof !" + std::to_string(coverWeightsId) + "\n";
  int region = coverRegion; // (the blocks are the probe's own: the statements around it stay one region)
  startBlock(setL);
  fn += "  store i8 1, ptr " + address + "\n";
  emitBranch(doneL);
  startBlock(doneL);
  coverRegion = region;
}

auto codegen::Emitter::coverTokens(Node* n) -> std::pair<uint32_t, uint32_t>
{
  uint32_t first = n->token, last = n->token;
  forEachNode(n, [&](Node* inner)
  {
    if (!inner)
      return;
    first = std::min(first, inner->token);
    last = std::max(last, inner->token);
  });
  // What it opens and has not closed by its last node's token closes after it: a call's `)`.
  const std::vector<Token>& toks = *currentFile->tokens;
  if (last >= toks.size())
    return {std::min<uint32_t>(first, uint32_t(toks.size() - 1)), uint32_t(toks.size() - 1)};
  int depth = 0;
  for (uint32_t t = first; t <= last; ++t)
    if (isBracketOpen(toks[t]))
      ++depth;
    else if (isBracketClose(toks[t]))
      --depth;
  while (depth > 0 && last + 1 < toks.size() && isBracketClose(toks[last + 1]))
  {
    ++last;
    --depth;
  }
  return {first, last};
}

auto codegen::Emitter::coverStatementTokens(Stmt* s) -> std::pair<uint32_t, uint32_t>
{
  const std::vector<Token>& toks = *currentFile->tokens;
  auto before = [&](Node* n) { return n && n->token > s->token ? n->token - 1 : s->token; };
  switch (s->kind)
  {
    // A compound statement's own part: its head; its statements have places of their own.
    case NodeKind::IfStmt: return {s->token, before(static_cast<IfStmt*>(s)->then)};
    case NodeKind::WhileStmt: return {s->token, before(static_cast<WhileStmt*>(s)->body)};
    case NodeKind::ForStmt: return {s->token, before(static_cast<ForStmt*>(s)->body)};
    case NodeKind::RangeForStmt: return {s->token, before(static_cast<RangeForStmt*>(s)->body)};
    case NodeKind::SwitchStmt: return {s->token, before(static_cast<SwitchStmt*>(s)->body)};
    case NodeKind::DoStmt: return {s->token, s->token};
    case NodeKind::TryStmt: return {s->token, s->token};
    default: break;
  }
  // A simple statement ends at its `;` (a label at its `:`), outside the brackets it opens (a
  // lambda's body has statements of its own).
  bool label = s->kind == NodeKind::CaseStmt || s->kind == NodeKind::DefaultStmt || s->kind == NodeKind::LabelStmt;
  int depth = 0;
  for (uint32_t t = s->token; t < toks.size(); ++t)
  {
    if (isBracketOpen(toks[t]))
      ++depth;
    else if (isBracketClose(toks[t]))
    {
      if (--depth < 0)
        return {s->token, t > s->token ? t - 1 : s->token};
    }
    else if (depth == 0 && (label ? toks[t].is(P::Colon) : toks[t].is(P::Semi)))
      return {s->token, t};
  }
  return coverTokens(s);
}

auto codegen::Emitter::coverPlace(uint32_t first, uint32_t last) -> std::string
{
  const std::vector<Token>& toks = *currentFile->tokens;
  const LineStarts& starts = lineStarts.try_emplace(currentFile, currentFile->text).first->second;
  const Token& a = toks[std::min<size_t>(first, toks.size() - 1)];
  const Token& b = toks[std::min<size_t>(last, toks.size() - 1)];
  size_t lastChar = b.offset + (b.length > 0 ? b.length - 1 : 0);
  return std::to_string(starts.line(a.offset)) + " " + std::to_string(starts.column(a.offset)) + " " + std::to_string(starts.line(lastChar)) + " " +
         std::to_string(starts.column(lastChar) + 1);
}

auto codegen::Emitter::coverageIsLogical(Node* n) -> bool
{
  while (n && n->kind == NodeKind::ParenExpr)
    n = static_cast<ParenExpr*>(n)->inner;
  if (!n || n->kind != NodeKind::BinaryExpr)
    return false;
  auto* b = static_cast<BinaryExpr*>(n);
  if (b->isKeyword)
    return (Kw(b->op) == Kw::and_ || Kw(b->op) == Kw::or_) && !b->callee;
  return (b->op == uint32_t(P::AndAnd) || b->op == uint32_t(P::OrOr)) && !b->callee;
}

auto codegen::Emitter::coverBegin(Node* at, const std::string& name) -> void
{
  coverActive = sh.coverage && at && currentFile && currentFile->tokens && !currentFile->tokens->empty() && !currentFile->isImported &&
                !currentFile->isLibrary && !currentFile->path.empty() && currentFile->path[0] != '<' && inlining.empty() &&
                !(currentFunction && currentFunction->testCode); // (a test's code is not what the tests cover)
  if (!coverActive)
    return;
  coverCount = 0;
  coverRegion = -1;
  auto hash = coverHashes.find(currentFile);
  if (hash == coverHashes.end())
  {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : currentFile->text)
      h = (h ^ c) * 1099511628211ull;
    char text[17];
    snprintf(text, sizeof text, "%016" PRIx64, h);
    hash = coverHashes.emplace(currentFile, text).first;
  }
  std::string shown = !name.empty() ? name : currentFunction ? qualifiedName(currentFunction) : currentFunctionName;
  coverMap = programPathOf(currentFile) + "\n" + hash->second + "\n" + shown + "\n" + std::to_string(lineOf(at, currentFile)) + "\n";
  coverProbe('F', coverPlace(at->token, at->token));
  coverRegion = 0; // (the body's first statements run whenever the entry does: one probe less per call)
}

auto codegen::Emitter::coverStatement(Stmt* s) -> void
{
  if (!coverActive || !s || s->kind == NodeKind::BlockStmt || s->kind == NodeKind::EmptyStmt)
    return;
  if (coverRegion < 0)
    coverRegion = int(coverProbe('R', ""));
  auto [first, last] = coverStatementTokens(s);
  coverMap += "S " + std::to_string(coverRegion) + " " + coverPlace(first, last) + "\n";
}

auto codegen::Emitter::coverAfter(size_t from) -> void
{
  if (!coverActive || coverRegion < 0)
    return;
  std::string_view code(fn);
  code = code.substr(std::min(from, code.size()));
  if (code.find("call ") != std::string_view::npos || code.find("invoke ") != std::string_view::npos)
    coverRegion = -1; // (what comes after a call that may throw is not reached whenever the call is)
}

auto codegen::Emitter::coverCondition(Node* e, const std::string& value) -> void
{
  if (!coverActive || !e)
    return;
  auto [first, last] = coverTokens(e);
  size_t probe = coverCount;
  coverCount += 2;
  coverMap += "C " + coverPlace(first, last) + "\n";
  std::string chosen = newTemp();
  fn += "  " + chosen + " = select i1 " + value + ", ptr " + coverProbeAt(probe) + ", ptr " + coverProbeAt(probe + 1) + "\n";
  coverSet(chosen);
}

auto codegen::Emitter::coverMissingDefault(Node* sw) -> void
{
  if (!coverActive)
    return;
  coverProbe('D', coverPlace(sw->token, sw->token));
}

auto codegen::Emitter::coverFinish() -> void
{
  if (!coverActive)
    return;
  coverActive = false;
  std::string array = coverArrayName();
  std::string map = "@\"kpp.covmap." + array.substr(10);
  std::string text = "$" + array.substr(1) + " = comdat any\n" + array + " = linkonce_odr global [" + std::to_string(coverCount) +
                     " x i8] zeroinitializer, comdat\n" + "$" + map.substr(1) + " = comdat any\n" + map + " = linkonce_odr constant [" +
                     std::to_string(coverMap.size() + 1) + " x i8] c\"" + llvmBytes(coverMap) + "\\00\", comdat\n";
  auto lock = sharedLock();
  if (declaredNames.claim(array))
  {
    decls += text;
    sh.coverageEntries[array] = Shared::CoverageEntry{coverCount, map};
  }
}

auto codegen::Emitter::coverageTableText() -> std::string
{
  if (!sh.coverage)
    return "";
  std::string entries;
  for (const auto& [array, entry] : sh.coverageEntries)
    entries += std::string(entries.empty() ? "" : ", ") + "{ ptr, i64, ptr } { ptr " + array + ", i64 " + std::to_string(entry.count) + ", ptr " + entry.map + " }";
  std::string count = std::to_string(sh.coverageEntries.size());
  return "@\"kpp.coverage.entries\" = internal constant [" + count + " x { ptr, i64, ptr }] [" + entries + "]\n" +
         "define ptr @kpp_coverage_entries(ptr %count) {\nentry:\n  store i64 " + count + ", ptr %count\n  ret ptr @\"kpp.coverage.entries\"\n}\n";
}
