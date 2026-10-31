#pragma once

// Project files for an IDE (docs/build.md, "Project files for an IDE"):
// `kppc --generate-visual-studio-files <build options> <sources> --exe OUT` writes a solution and a
// Makefile project into build\visual-studio\, `--generate-vs-code-files ...` writes .vscode\
// (tasks, launch, settings). Both list every source file and build with the same arguments,
// debug and release, through the compile server.

bool isGenerateOption(const char* arg);
int generateProjectFiles(int argc, char** argv);
