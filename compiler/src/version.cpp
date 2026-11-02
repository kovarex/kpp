// kpp's version and commit (docs/build.md, "Versions"), from the header compiler/version.cmake writes
// at every build: only this file is compiled again when they change.
#include "source.hpp"
#include "version.hpp"

const char* kppVersion() { return KPP_VERSION; }
const char* kppCommit() { return KPP_COMMIT; }
