// struct Emitter's members: errors, failures and their places; their declarations in
// Diagnostics.hpp, part of the struct's body in Codegen/Emitter.hpp. A translation unit of its own
// since 2026-11-05 (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::error(Node* at, const std::string& message, bool shown) -> void
{
  if (shown)
    errors.push_back(locationOf(at, currentFile, true) + ": error: " + message); // every codegen error is labelled, the gaps too
  else
    ++hiddenErrors;
  // A program function's own emission (not an instance's, a lambda's operator on demand, a
  // global's constructor or a shared text it happens to emit first): the function emitted again
  // gives the error again, at its place then.
  if (!owner.empty() && owner == emittingName && emittingNow.args.empty() && !emittingNow.classTy && emittingNow.depth == 0 &&
      emittingNow.name.empty() && emittingNow.requestedBy.empty() && onDemandNow.name.empty() && !emittingShared)
    erroring.emplace(owner, emittingNow.f);
  else
    unattributedErrors = true;
}

auto codegen::Emitter::locationOf(Node* at, ParsedFile* file, bool column) -> std::string
{
  size_t line = lineOf(at, file);
  if (!line)
    return "?";
  if (!column)
    return sourceLocation(file->path, line);
  // A member access's at its member's name (its node's token is the `.` or `->`).
  const std::vector<Token>& toks = *file->tokens;
  size_t token = std::min<size_t>(at->token, toks.size() - 1);
  if (at->kind == NodeKind::MemberExpr && token + 1 < toks.size() && toks[token + 1].getKind() == TokenKind::Identifier)
    ++token;
  const LineStarts& starts = lineStarts.try_emplace(file, file->text).first->second;
  return sourceLocation(file->path, starts.line(toks[token].offset), starts.column(toks[token].offset));
}

auto codegen::Emitter::programLocationOf(Node* at, ParsedFile* file) -> std::string
{
  size_t line = lineOf(at, file);
  // (the path as the program's text gives it: --file-prefix-map's, no path of this machine in a release)
  return line ? programPathOf(file) + ":" + std::to_string(line) : std::string("?");
}

auto codegen::Emitter::lineOf(Node* at, ParsedFile* file) -> size_t
{
  if (!file || !file->tokens || !at || file->tokens->empty())
    return 0;
  const std::vector<Token>& toks = *file->tokens;
  const Token& t = toks[std::min<size_t>(at->token, toks.size() - 1)];
  return lineStarts.try_emplace(file, file->text).first->second.line(t.offset);
}

auto codegen::Emitter::fail(Node* at, const std::string& message, bool shown) -> void
{
  std::string where;
  if (!currentContext.empty())
    where = " (in " + currentContext + (currentRequestedByContext.empty() ? "" : ", requested by " + currentRequestedByContext) + ")";
  else if (!currentFunctionName.empty())
    where = " (in " + currentFunctionName + (currentRequestedBy.empty() ? "" : ", requested by " + currentRequestedBy) + ")";
  if (where.rfind(" (in __kpp_program_main(", 0) == 0)
    where.replace(5, 18, "main"); // (a --tests build renames the program's main, parser.cpp: to the reader it is main)
  // A value of unknown type in a function the checker reported an error in (`auto v = p.size()`
  // on an undeclared type's p) follows from that error too; and so does any failure on an
  // expression that holds an error the checker reported (`undefinedName(1)`, a call given
  // `NOT_A_NAME`): the build fails with that error already, and this one only repeats it (an
  // editor showed both). A failure elsewhere in the same function is shown.
  if (shown && ((message.find("unknown type") != std::string::npos && global.checkerErrorIn(checkerChecked)) || holdsCheckerError(at)))
    shown = false;
  lastFailure = message;
  lastFailureShown = shown;
  error(at, message + where, shown);
  throw Unsupported{};
}

auto codegen::Emitter::holdsCheckerError(Node* at, uint32_t tokensAfter) -> bool
{
  if (!at || at->kind < NodeKind::NameExpr || at->kind > NodeKind::ParenExpr || !currentFile || !global.checkerErrorIn(checkerChecked))
    return false;
  uint32_t first = at->token, last = at->token;
  forEachNode(at, [&](Node* n)
  {
    first = std::min(first, n->token);
    last = std::max(last, n->token);
  });
  return global.checkerErrorWithin(currentFile, first, last + tokensAfter);
}

auto codegen::Emitter::followsUndeclared(const Ty* t) -> bool
{
  while (t && (t->isReference() || t->kind == TyKind::Pointer))
    t = t->inner;
  return t && t->decl && (t->decl->attributes & ATTR_Draft) && global.undeclaredReported(global.draftName(t->decl));
}
