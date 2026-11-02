# kpp's version (docs/build.md, "Versions"), worked out at every build by CMakeLists.txt's kpp_version
# target and written into OUTPUT (a header version.cpp includes) only when it changed:
#   cmake -DROOT=<the repository> -DOUTPUT=<version.hpp> -P version.cmake
# MAJOR.MINOR is the VERSION file at the repository's root, changed by kovarex's decision. PATCH is the
# number of commits on HEAD's first-parent history since the commit that last changed VERSION, so the
# commit that changes it is X.Y.0 and every later one adds 1, with no file to edit. "+dirty" when a
# tracked file differs from HEAD. Outside a git checkout (a source zip, no git) the version is
# "unknown", never a guess; a shallow clone that lacks VERSION's last change is an error.
cmake_minimum_required(VERSION 3.25)
file(READ "${ROOT}/VERSION" majorMinor)
string(STRIP "${majorMinor}" majorMinor)
if (NOT majorMinor MATCHES "^[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "${ROOT}/VERSION holds '${majorMinor}', not MAJOR.MINOR")
endif()

set(version "unknown")
set(commit "")
find_program(GIT git)
if (GIT)
  execute_process(COMMAND "${GIT}" -C "${ROOT}" rev-parse HEAD
                  OUTPUT_VARIABLE commit RESULT_VARIABLE noGit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if (GIT AND noGit EQUAL 0)
  execute_process(COMMAND "${GIT}" -C "${ROOT}" log -1 --first-parent --format=%H -- VERSION
                  OUTPUT_VARIABLE base OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${GIT}" -C "${ROOT}" rev-parse --is-shallow-repository
                  OUTPUT_VARIABLE shallow OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  # In a shallow clone, a boundary commit (its parents missing) seems to add every file, VERSION too:
  # VERSION's last change found there may be older, and the count wrong.
  set(boundary FALSE)
  if (shallow STREQUAL "true")
    execute_process(COMMAND "${GIT}" -C "${ROOT}" rev-parse --path-format=absolute --git-path shallow
                    OUTPUT_VARIABLE shallowFile OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    file(STRINGS "${shallowFile}" boundaries)
    if (base STREQUAL "" OR base IN_LIST boundaries)
      set(boundary TRUE)
    endif()
  endif()
  if (boundary)
    message(FATAL_ERROR "kpp's version counts the commits since VERSION last changed, and this shallow clone of "
                        "${ROOT} may lack that commit: git fetch --unshallow (a CI checkout: fetch-depth 0)")
  endif()
  if (base STREQUAL "")
    set(patch 0) # VERSION is not committed yet: X.Y.0, and dirty
  else()
    execute_process(COMMAND "${GIT}" -C "${ROOT}" rev-list --count --first-parent "${base}..HEAD"
                    OUTPUT_VARIABLE patch OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  endif()
  execute_process(COMMAND "${GIT}" -C "${ROOT}" status --porcelain --untracked-files=no
                  OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  set(version "${majorMinor}.${patch}")
  if (NOT changes STREQUAL "" OR base STREQUAL "")
    string(APPEND version "+dirty")
  endif()
else()
  set(commit "")
endif()

string(CONCAT text "// Written by compiler/version.cmake at every build, not kept in the repository.\n"
                   "#define KPP_VERSION \"${version}\"\n"
                   "#define KPP_COMMIT \"${commit}\"\n")
if (EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" old)
endif()
if (NOT old STREQUAL text)
  file(WRITE "${OUTPUT}" "${text}")
endif()
