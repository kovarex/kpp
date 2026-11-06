// kpp new, and kpp alone: the menu of what it can do.
// (Moved out of project.cpp on 2026-11-05 without a change.)
#include "Project/Internal.hpp"

namespace projects
{
  int newProject(const fs::path& directory)
  {
    std::error_code ec;
    if (fs::exists(directory / "build.kpp", ec))
    {
      fprintf(stderr, "error: %s already has a build.kpp\n", directory.string().c_str());
      return 1;
    }
    fs::create_directories(directory / "src", ec);
    auto write = [&](const fs::path& path, const std::string& text)
    {
      if (fs::exists(path, ec))
        return;
      FILE* file = host::openFile(path, "wb");
      if (file)
        fwrite(text.data(), 1, text.size(), file), fclose(file);
      printf("wrote %s\n", path.string().c_str());
    };
    write(directory / "build.kpp",
          "// How this project is built (https://github.com/kovarex/kpp/blob/master/docs/build.md, \"build.kpp\").\n"
          "// As it is, src\\ is one executable named after this directory, in debug and release.\n"
          "void build(Project& project)\n"
          "{\n"
          "}\n");
    write(directory / "src" / "main.kpp",
          "int main()\n"
          "{\n"
          "  println(\"Hello from kpp\");\n"
          "  return 0;\n"
          "}\n");
    write(directory / ".gitignore", "build/\n.vscode/\n");
    return 0;
  }

  std::string ask(const char* prompt)
  {
    printf("%s", prompt);
    fflush(stdout);
    char line[256] = {};
    if (!fgets(line, sizeof(line), stdin))
      return {};
    std::string answer = line;
    while (!answer.empty() && (answer.back() == '\n' || answer.back() == '\r' || answer.back() == ' '))
      answer.pop_back();
    return answer;
  }

  // `kpp` alone in a console: the actions, each with the command that does it.
  // Whether a person answers on stdin: host::interactiveInput (a console, or Git Bash's mintty pipe:
  // kovarex, 2026-11-04, `kpp` alone in Git Bash showed no menu).
  bool interactiveInput() { return host::interactiveInput(); }

  // `kpp` alone in a project: the menu, its answer read from stdin. Without anyone to answer (input
  // from a file or a pipe), the commands it offers, and a word that it asked nothing: never silence.
  int menu(const fs::path& root, CompileFunction compile, bool asking)
  {
    if (root.empty())
    {
      fs::path here = fs::current_path();
      printf("kpp: no build.kpp in %s or above it.\n"
             "  1  make a new project here (build.kpp, src\\main.kpp)   kpp new\n"
             "  0  quit\n", here.string().c_str());
      return ask("> ") == "1" ? newProject(here) : 0;
    }
    Request request;
    Description d;
    if (!describe(root, request, d))
      return 1;
    std::vector<std::string> targets = targetNames(d);
    std::string first = targets.empty() ? "" : targets[0];
    printf("kpp: project %s (%s)\n  targets:", root.filename().string().c_str(), root.string().c_str());
    for (const std::string& t : targets)
      printf(" %s", t.c_str());
    printf("\n  configurations:");
    for (const Configuration& c : d.configurations)
      printf(" %s", c.name.c_str());
    printf("\n"
           "  1  build, debug                     kpp debug\n"
           "  2  build, release                   kpp release\n"
           "  3  build and run %-18s kpp %s --run\n"
           "  4  write Visual Studio files        kpp --generate-visual-studio-files\n"
           "  5  write VS Code files              kpp --generate-vs-code-files\n"
           "  0  quit\n", first.c_str(), first.c_str());
    if (!asking)
    {
      printf("kpp: input is no terminal, so nothing was asked and nothing done: run one of the commands above\n");
      return 2;
    }
    std::string answer = ask("> ");
    if (answer == "1" || answer == "2")
      request.configurations = {answer == "1" ? "debug" : "release"};
    else if (answer == "3")
      request.run = true, request.names = {first};
    else if (answer == "4")
      request.visualStudio = true;
    else if (answer == "5")
      request.vsCode = true;
    else
      return 0;
    return act(root, request, compile);
  }
}
