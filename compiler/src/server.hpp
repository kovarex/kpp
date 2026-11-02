#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The compile server (docs/compile-speed.md, "The compile server: protocol"). `kppc --server ARGS`
// is a client: it hands ARGS to a server process that builds them and streams back what the build
// printed and its exit code, starting the server first if there is none. `kppc --serve KEY` is the
// server; it is started by a client, never by hand.

// The build itself, with main's arguments (argv[0] is the kppc the client ran).
using CompileFunction = int (*)(int argc, char** argv);

// The client: argv without --server. Builds in this process if no server can be reached or the
// server ends without an answer.
int runClient(int argc, char** argv, CompileFunction compile);

// The server: serves builds for its key until it has been idle for an hour or a client asks it to
// stop (--stop-server).
int runServer(const char* key, CompileFunction compile);

// `kppc --stop-server ARGS`: stops the server for ARGS if one runs.
int stopServer(int argc, char** argv);
// The same without a word: whether one ran.
bool stopServerQuietly(int argc, char** argv);
// Stops every running server, of any arguments, whose arguments whose accepts (the client that
// starts a server keeps them in %TEMP%\kppc-server\KEY.args), and returns the arguments of each one
// it stopped. A record whose server has ended is removed.
std::vector<std::vector<std::string>> stopServers(const std::function<bool(const std::vector<std::string>& args)>& whose);

// Called by the build when it is done and only freeing what it built is left: a process exits on
// the spot (freeing costs seconds), a server answers its client first and frees afterwards.
void buildDone(int exitCode);

// Whether this build runs in a compile server (for its client).
bool servingBuild();

// A build in this process as a compile server runs one, for the language server (`kppc --lsp`,
// lsp.hpp): kept for the next, which is incremental from it where it can be, and what it printed
// captured into out and err instead of reaching this process's streams (they point at NUL after
// it); notes: what a server would log about it (why it was not incremental). One at a time.
int buildInThisProcess(CompileFunction compile, std::vector<std::string> args, std::string& out, std::string& err,
                       std::vector<std::string>* notes = nullptr);

// Step 3a: a server's build whose inputs are as the previous build's. fingerprint hashes every
// input (the sources, the configuration, the environment); outputs are the files the build
// writes, which --verify-incremental compares; left lists every file the build leaves (its modules'
// objects too), listed again when the build is kept. When the previous build had the same
// fingerprint, every file it left is as it left it (none removed, none changed) and the C headers it
// imported are unchanged (headersUnchanged), its stdout and stderr are sent again and its exit code
// returned: true. Otherwise false, and the build goes on and is kept when done.
bool replayBuild(uint64_t fingerprint, const std::vector<std::string>& outputs, const std::function<std::vector<std::string>()>& left,
                 bool headersUnchanged, int& exitCode);

// --verify-incremental: after a replayed build, the server builds again from scratch (through
// compile, with replay off) and compares what it printed and wrote: any difference is an error.
bool replayDisabled();

// The process's environment, as GetEnvironmentStringsW gives it (UTF-16 bytes).
std::string environmentBlock();

// A line in the compile server's log about the build running (nothing outside a server).
void logBuildNote(const std::string& note);

// The build running is the kept one with changed bodies checked again (step 3b): --verify-incremental checks it.
void noteIncrementalBuild();
