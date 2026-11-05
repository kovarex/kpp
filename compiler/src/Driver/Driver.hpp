// The driver (main.cpp's namespace, split into Driver/ on 2026-11-05): what its translation units
// share, in namespace driver: the types, the globals, every function's declaration, piece by piece
// in main.cpp's order. Each Driver/X.cpp includes this.
#pragma once
#include "lexer.hpp"
#include "codegen.hpp"
#include "convert.hpp"
#include "parser.hpp"
#include "config.hpp"
#include "resolver.hpp"
#include "ast_walk.hpp"
#include "generate.hpp"
#include "project.hpp"
#include "run.hpp"
#include "tests.hpp"
#include "server.hpp"
#include "lsp.hpp"
#include "natvis.hpp"
#include "source.hpp"
#include "backend.hpp"
#include "packages.hpp"
#include "sha256.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <cctype>
#include <cstring>
#include <exception>
#include <functional>
#include <set>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <sstream>
#include <vector>
#ifdef _WIN32
#include <process.h> // _getpid
#endif

namespace driver
{
#include "Options.hpp"
#include "BackEndPool.hpp"
#include "Build.hpp"
#include "Runtime.hpp"
#include "Link.hpp"
#include "ConfigConstants.hpp"
#include "CImport.hpp"
#include "ModuleCompile.hpp"
#include "FrontEnd.hpp"
#include "BodyGraft.hpp"
}
