// ParamNamePlace.inl: where a member function declaration's parameter name is written, for
// collectInlineBodies and markInlineBody.

// Where a declaration's parameter gets its definition's name, when the body moves into the class
// (markInlineBody renames it, as the body uses it): the token of its name, or, unnamed, the offset its
// name is written at (before a default argument's `=`, or at its end). None when its tokens are not
// known or, unnamed, its declarator is not a plain type's (`void (*)(int)`, `int[4]` place the name
// inside).
struct ParamNamePlace
{
  bool found = false;
  uint32_t nameToken = UINT32_MAX; // named
  uint32_t insertBefore = UINT32_MAX; // unnamed: the token the name goes before (or endToken)
};

// One past a parameter's last token: the `,` or `)` that ends it (the parser keeps no end for one).
static uint32_t paramEndToken(const ParamDecl* a)
{
  if (!a->file || !a->file->tokens)
    return 0;
  const std::vector<Token>& toks = *a->file->tokens;
  int depth = 0, angles = 0; // (angle brackets in the type alone: a default argument's `<` compares)
  bool inDefault = false;
  for (uint32_t k = a->token; k < toks.size(); ++k)
  {
    if (!inDefault && depth == 0 && toks[k].is(P::Less))
      ++angles;
    else if (!inDefault && depth == 0 && angles > 0 && (toks[k].is(P::Greater) || toks[k].is(P::Shr)))
      angles = std::max(0, angles - (toks[k].is(P::Shr) ? 2 : 1));
    else if (depth == 0 && angles == 0 && toks[k].is(P::Assign))
      inDefault = true;
    else if (angles > 0 && depth == 0 && toks[k].is(P::Comma))
      continue;
    else if (toks[k].is(P::LParen) || toks[k].is(P::LBracket) || toks[k].is(P::LBrace))
      ++depth;
    else if (toks[k].is(P::RParen) || toks[k].is(P::RBracket) || toks[k].is(P::RBrace))
    {
      if (depth-- == 0)
        return k;
    }
    else if (depth == 0 && (toks[k].is(P::Comma) || toks[k].is(P::Semi)))
      return toks[k].is(P::Comma) ? k : 0;
  }
  return 0;
}

static ParamNamePlace paramNamePlace(const ParamDecl* a, const SymbolTable& symbols)
{
  ParamNamePlace place;
  uint32_t paramEnd = paramEndToken(a);
  if (paramEnd == 0 || paramEnd <= a->token)
    return place;
  const std::vector<Token>& toks = *a->file->tokens;
  int depth = 0;
  uint32_t end = paramEnd;
  for (uint32_t k = a->token; k < paramEnd; ++k)
  {
    if (toks[k].is(P::LParen) || toks[k].is(P::LBracket) || toks[k].is(P::LBrace))
      ++depth;
    else if (toks[k].is(P::RParen) || toks[k].is(P::RBracket) || toks[k].is(P::RBrace))
      --depth;
    else if (depth == 0 && toks[k].is(P::Assign))
    {
      end = k;
      break;
    }
  }
  if (a->name != UINT32_MAX)
  {
    // (the name is the declarator's last token before a default argument)
    if (end > a->token && toks[end - 1].getKind() == TokenKind::Identifier &&
        a->file->text.substr(toks[end - 1].offset, toks[end - 1].length) == symbols.name(a->name))
      place.found = true, place.nameToken = end - 1;
    return place;
  }
  for (uint32_t k = a->token; k < end; ++k)
    if (toks[k].is(P::LParen) || toks[k].is(P::LBracket) || toks[k].is(P::Ellipsis))
      return place;
  place.found = true;
  place.insertBefore = end;
  return place;
}

static bool paramRenameable(const ParamDecl* a, const SymbolTable& symbols) { return paramNamePlace(a, symbols).found; }
