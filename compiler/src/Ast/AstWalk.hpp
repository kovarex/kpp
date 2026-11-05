#pragma once
// Every node a declaration list reaches (each once): its declarations, their types, statements and
// expressions, the names' template arguments, lambdas' closures. For changing what every node of a
// file holds in place: a compile server's kept file points its nodes at a new version's tokens
// (docs/compile-speed.md, step 4). It follows what InstanceCopier copies (instance.cpp), plus the
// declarations only a file's top level has (namespaces, imports).
#include "Ast.hpp"
#include <functional>

void forEachNode(Span<Decl*> decls, const std::function<void(Node*)>& each);
void forEachNode(Node* root, const std::function<void(Node*)>& each);

// The token indexes a node holds (its first token, a declaration's end, an assert's text, ...),
// each passed to remap and replaced by what it returns.
void remapNodeTokens(Node* n, const std::function<uint32_t(uint32_t)>& remap);
