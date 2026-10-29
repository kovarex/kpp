#pragma once

#include <cstdint>
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

// Called by the build when it is done and only freeing what it built is left: a process exits on
// the spot (freeing costs seconds), a server answers its client first and frees afterwards.
void buildDone(int exitCode);

// Whether this build runs in a compile server (for its client).
bool servingBuild();

// Step 3a: a server's build whose inputs are as the previous build's. fingerprint hashes every
// input (the sources, the configuration, the environment); outputs are the files the build
// writes. When the previous build had the same fingerprint, its outputs are as it left them and the
// C headers it imported are unchanged (headersUnchanged), its stdout and stderr are sent again and
// its exit code returned: true. Otherwise false, and the build goes on and is kept when done.
bool replayBuild(uint64_t fingerprint, const std::vector<std::string>& outputs, bool headersUnchanged, int& exitCode);

// --verify-incremental: after a replayed build, the server builds again from scratch (through
// compile, with replay off) and compares what it printed and wrote: any difference is an error.
bool replayDisabled();

// The process's environment, as GetEnvironmentStringsW gives it (UTF-16 bytes).
std::string environmentBlock();
