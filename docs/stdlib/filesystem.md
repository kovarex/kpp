# Filesystem: paths, files and directories

What `std::filesystem` converts to: namespace `filesystem`, with std's names (`filesystem::path`,
`filesystem::exists`, ...), so that code which reaches it through a namespace alias or a
using-directive (Factorio's `namespace __fs = std::filesystem;` with `using namespace __fs;`)
reads the same. On Windows paths are UTF-16 text as Windows keeps them, and every operation is a
Win32 wide-character call. On Linux and FreeBSD paths are wide text too (UTF-32: wchar_t is 32 bits
there), and every operation is a POSIX call with the path in UTF-8 (stat, opendir, realpath, ...;
stdlib/platform/FilesPosix.kpp). *Difference from C++*: there std's `path::value_type` is char and
its native string the bytes; kpp keeps one wide type on every target. A name there is bytes, UTF-8
or not, and the path keeps every one: each byte of a sequence that is not UTF-8 (0x80 to 0xFF) is
the lone surrogate U+DC80 to U+DCFF in the wide text, which no UTF-8 decodes to, and is that byte
again at each system call and in `string()` (Python's surrogateescape, PEP 383). So a path that
`directory_iterator` gives names its file, and `filename().string()` is its bytes, as libstdc++'s. The error types it reports
with (ErrorCode, Errc, SystemError) are specified at the end.

## path

On Windows a path is text in Windows form: `\` and `/` are both separators, `preferred_separator`
is `\`. On Linux and FreeBSD `/` is the one separator and `preferred_separator`; a `\` is part of a
name, a path has no root name, `make_preferred()` changes nothing and the generic forms are the
text with each run of `/` as one, as libstdc++'s are there. The rest of this section is Windows' form; elsewhere read it
without root names, with `/` for `\`. It is stored as a WString (`native()`, `c_str()`, and an
implicit conversion to WString, the native string); `value_type` is wchar_t.

Construction (implicit, except the two-pointer forms): from a WString, a `const wchar_t*`, or a
`[first, last)` range of wchar_t; from a String, a `const char*`, a StringView or a range of char,
read as UTF-8. *Difference from C++*: on Windows C++ reads a narrow string in the ANSI code page;
kpp always reads UTF-8 (what Factorio's own Path does with its widen). Assignment from the same
types.

The parts of a path, by example (`C:\a\b.txt`):
- root name: a drive (`C:`) or a server (`\\server`, two separators and a name); else empty.
- root directory: the separator right after the root name (`\`), if there is one.
- root path: the two together (`C:\`). Relative path: the rest, after every separator there
  (`a\b.txt`).
- filename: the text after the last separator of the relative path (`b.txt`), empty when the
  path ends with a separator. `stem()`, `extension()`: the filename split at its last `.`
  (`b`, `.txt`), except that a filename starting with its only `.` (`.profile`) and the
  filenames `.` and `..` have no extension.
- `parent_path()`: the path without its last element, trailing separators before it removed
  (`a\b\` gives `a\b`, `a\b` gives `a`, `\a` gives `\`, `a` gives an empty path); a path without a
  relative path is its own parent.
- Each `has_x()` says whether x is non-empty. `is_absolute()`: a root name and a root directory
  (`C:\x`, `\\server\share`); `C:x` and `\x` are relative, as on Windows. Elsewhere a root
  directory (`/x`). `is_relative()`: not absolute. `empty()`.

Elements, for iteration (`begin()`, `end()`, bidirectional; each element a path): the root name,
the root directory, each filename of the relative path in order, and an empty element when the
path ends with a separator after a filename. `C:\a\b\` has `C:`, `\`, `a`, `b` and ``.

Changing:
- `p /= q`, `p / q` (also `append`): an absolute q, or one with a different root name, replaces
  p; a q with a root directory keeps only p's root name before it; otherwise q follows p after a
  separator, which is added unless p is empty, ends with a separator or is a root name alone
  (`C:` / `a` is `C:a`).
- `p += x` (also `concat`): x's text appended without a separator (x a path, WString,
  `const wchar_t*`, wchar_t, String or `const char*`).
- `remove_filename()`: the text up to the filename (`a\b` becomes `a\`). `replace_filename(q)`:
  that, then `/= q`. `replace_extension(e)`: the extension removed, then e appended with a `.`
  in front when it has none; with no argument only removed.
- `make_preferred()`: every `/` becomes `\`. `clear()`, `swap(other)`.

Text: `string()` and `u8string()` (UTF-8 String), `wstring()` (WString), `generic_string()`,
`generic_u8string()` and `generic_wstring()` (the same with `/` for every `\`). A lone surrogate has
no UTF-8 form: the narrow ones throw SystemError (Windows: ERROR_NO_UNICODE_TRANSLATION, as MSVC's;
elsewhere EILSEQ, and every operation on such a path, whose name the system takes in UTF-8, throws it
too: libstdc++ refuses the path itself), where U+FFFD would name another file. Off Windows U+DC80 to
U+DCFF are a name's bytes that are not UTF-8 (above) and give those bytes. `wstring()` keeps it,
and a filesystem_error's `what()` shows it as U+FFFD.

`lexically_normal()`: separators made `\`; every `.` element and every `name\..` pair (name not
`..`) removed; a `..` right after the root directory removed; no trailing separator after a final
`..`; an empty result becomes `.`. A removed element that ended the path leaves a trailing
separator (`a\b\..` is `a\`). `lexically_relative(base)`: empty when the root names differ, when
one is absolute and the other not, or when only base has a root directory; otherwise, after the
elements the two have in common, one `..` for each remaining element of base (a `..` there counts
minus one, `.` and empty elements nothing; a negative total gives an empty path), followed by
this path's remaining elements, or `.` when there are none. `lexically_proximate(base)`:
`lexically_relative(base)`, or this path when that is empty.

Comparison: `compare(other)` and `==`, `!=`, `<`, `<=`, `>`, `>=` (free functions, so a String
converts on either side) compare the root names as text, then a path without a root directory
before one with it, then the relative paths element by element as text (UTF-16 units, so case
matters). `a\b` equals `a/b`; `a/b` sorts before `a-b`.

## What a path names

`file_status`: a `file_type` (`type()`) and `perms` (`permissions()`), both settable.
`file_type`: none (unknown because of an error), not_found, regular, directory, symlink (a
reparse point: a symbolic link or a junction), block, character, fifo, socket, unknown,
junction; Windows gives only not_found, regular, directory and symlink.
`perms`: the POSIX bits (owner_read 0400 ... others_exec 01, owner_all, group_all, others_all,
all 0777, set_uid, set_gid, sticky_bit, mask, unknown 0xFFFF) with `|`, `&`, `^`, `~` and their
assignments. *Windows*: a file is read-only or not; status reports 0555 for a read-only one and
0777 otherwise.

`status(p)`: what p names, links followed: a regular file or a directory with its permissions,
or not_found when p does not resolve (a missing file or directory, a bad name, a missing network
path). `symlink_status(p)`: the same without following: a reparse point is symlink.
`status_known(s)`, `exists(s)`, `is_regular_file(s)`, `is_directory(s)`, `is_symlink(s)`,
`is_other(s)` (exists, and not a file, directory or link), `is_block_file(s)`,
`is_character_file(s)`, `is_fifo(s)`, `is_socket(s)`; and each of these also for a path.
`file_size(p)`: the size of the regular file p (links followed); an error for a directory
(errc::is_a_directory) or a missing file. `is_empty(p)`: a file of size 0, or a directory
without entries. `equivalent(a, b)`: whether a and b name the same file or directory (links
followed: the same volume and file index); an error when either does not resolve (2026-10-30).
`space(p)`: `space_info` {capacity, free, available} of the volume of p.
`last_write_time(p)`: when what p names (links followed) was last written, a `file_time_type`
(`TimePoint<FileClock>`, time.md); `last_write_time(p, t)` sets it, the access time left as it is
(on Linux and FreeBSD to 100 ns: FileClock's ticks). `hard_link_count(p)`: how many names (hard
links) what p names has. `resize_file(p, size)`: the regular file p cut to size bytes, or extended
with zero bytes. `hash_value(p)`: a hash that equal paths share (`a\b` and `a/b` alike).
`u8path(text)`: the path of UTF-8 text, what `path(text)` is (deprecated in C++20).

## Directories

`directory_iterator(p)`: the entries of directory p, without `.` and `..`, in the order the file
system lists them. An input iterator: copies share one position; the default-constructed one is
the end, which an iterator becomes after its last entry. `begin(it)` and `end(it)` make it a
range. `increment(ec)` advances reporting errors in ec; `operator++` throws. A missing p (or one
that is not a directory) is an error; an empty directory gives the end at once.

`recursive_directory_iterator(p, options)`: every entry under directory p, depth first: a
directory's entries come right after it, each directory's in the file system's order. A link to a
directory is entered only with `directory_options::follow_directory_symlink`; a directory that
cannot be opened is an error, or skipped with `skip_permission_denied` when access is denied.
`depth()` (0 for p's own entries), `options()`, `recursion_pending()` and
`disable_recursion_pending()` (the next increment does not enter the current directory),
`pop()` (the next entry one level up; the end from the top), `increment(ec)`; copies share the
position, as directory_iterator's, and `begin` and `end` make it a range.

`directory_entry`: `path()` (and an implicit conversion to `const path&`), and the status the
listing gave without asking the file system again: `status()`, `symlink_status()`, `exists()`,
`is_directory()`, `is_regular_file()`, `is_symlink()`, `file_size()`, `last_write_time()` (a link
is asked anew, followed); `hard_link_count()` asks the file system. `refresh()` forgets the listing's data; `assign(p)` makes it another path's entry.
Compared by path.

## Changing the file system

- `create_directory(p)`: creates the directory; true if it did; false, and no error, when a
  directory is there already.
- `create_directories(p)`: creates p and every missing directory above it; true if it created
  any; an existing non-directory on the way is an error (errc::not_a_directory).
- `remove(p)`: removes the file or the empty directory p; true if it did; false, and no error,
  when there is none. A read-only file is not removed: the error is access denied (as with
  MSVC); make it writable with permissions first.
- `remove_all(p)`: p and, when p is a directory (not a link to one), everything in it, depth
  first; the number removed (0 when p does not exist).
- `rename(from, to)`: moves a file or directory, replacing an existing file `to`. Not across
  volumes (errc::cross_device_link, the system's ERROR_NOT_SAME_DEVICE), as with MSVC.
- `copy_file(from, to, options)`: copies a regular file. When `to` exists: an error
  (errc::file_exists) with `copy_options::none`; nothing with skip_existing; replaced with
  overwrite_existing; replaced with update_existing only if from was written later. True when it
  copied.
- `copy(from, to, options)`: a regular file as copy_file does, into `to` when that is a
  directory (nothing with directories_only); a directory, with `recursive` or with no options at
  all, to a directory created at `to` if needed, entry by entry, its subdirectories copied (and
  recursed into) only with recursive. Copying a file or directory onto itself is an error.
  Links are followed (copy_symlinks, skip_symlinks, create_symlinks and create_hard_links are
  accepted and do nothing more).
- `permissions(p, prms, opts)`: with `perm_options::replace` (also the default), `add` or
  `remove`, computes the new bits from p's current ones; p becomes read-only when they have no
  write bit, writable otherwise (Windows keeps nothing else). nofollow is accepted.
- `create_hard_link(target, link)`: another name for the existing file target. `create_symlink(target,
  link)` and `create_directory_symlink(target, link)`: a symbolic link whose text is target (which
  need not exist; a relative one is read from the link's directory); on Windows a directory's link
  is made with the second, and making either needs Developer Mode or the privilege to create
  symbolic links (otherwise the error is the system's ERROR_PRIVILEGE_NOT_HELD). `read_symlink(p)`:
  the link's text as written (on Windows a reparse point's print name, also a junction's); anything
  else is an error (errc::invalid_argument on Windows, the system's EINVAL elsewhere).
  `copy_symlink(from, to)`: a link at to with from's text, a directory link when from leads to a
  directory.
- `current_path()`, `current_path(p)`: the current directory, read or set.
  `temp_directory_path()`: the system's directory for temporary files.
- `absolute(p)`: p against the current directory (GetFullPathName, which also removes `.` and
  `..`); empty for an empty p. `canonical(p)`: the absolute path of what p names with every link
  resolved; p must exist; the system's `\\?\` prefix is removed (`\\?\UNC\` becomes `\\`).
  `weakly_canonical(p)`: canonical of the longest leading part of p that exists, followed by the
  rest, lexically normal. `relative(p, base)`: `weakly_canonical(p).lexically_relative(
  weakly_canonical(base))`, base the current directory unless given; `proximate(p, base)`: the
  same with lexically_proximate.

## Errors

Every operation that can fail comes in two forms: one that throws `filesystem_error`, and one
with a last parameter `ErrorCode& ec` that sets ec instead (and clears it on success) and returns
what the other would (an empty path, false, -1 for counts and sizes). status and symlink_status
do not throw for a path that does not resolve: they return not_found (and the ec form sets ec to
the reason, as std's does); exists clears ec when the status is known.

`filesystem_error` is a SystemError with `path1()` and `path2()`; what() is the message given,
`: `, the error code's message, then the paths in brackets: `remove: Access is denied.
[C:\x.txt]`. *Difference from C++*: the format of what() is kpp's own.

Paths' stream operators (`<<` and `>>` with C++'s quoting) are not provided.

# ErrorCode, Errc, SystemError

What `std::error_code`, `std::errc` and `std::system_error` convert to (global names).

`ErrorCategory`: what an error value means: `systemCategory()` (the system's error: on Windows what
GetLastError returns, elsewhere an errno value) or `genericCategory()` (an errno value); `name()` is
"system" or "generic".

`ErrorCode`: a value and a category. `ErrorCode()` is 0 in the system category, no error.
`value()`, `category()`, `explicit operator bool` (value is not 0), `clear()`, `assign(value,
category)`, `message()`: the system's text for a system error (on Windows FormatMessage's, without
its trailing line break, as UTF-8; elsewhere strerror's), strerror's for a generic one. `==`, `!=` with another ErrorCode
compare value and category.

`Errc`: the portable conditions with their errno values (operation_not_permitted 1,
no_such_file_or_directory 2, no_such_process 3, interrupted 4, io_error 5,
resource_unavailable_try_again 11, permission_denied 13, illegal_byte_sequence 42, device_or_resource_busy 16,
file_exists 17, cross_device_link 18, no_such_device 19, not_a_directory 20, is_a_directory 21,
invalid_argument 22, too_many_files_open 24, file_too_large 27, no_space_on_device 28,
read_only_file_system 30, resource_deadlock_would_occur 36, filename_too_long 38,
function_not_supported 40, directory_not_empty 41, not_supported 129,
operation_not_supported 130, timed_out 138: the UCRT's; glibc's and FreeBSD's for the nine that differ there,
resource_unavailable_try_again, resource_deadlock_would_occur, filename_too_long,
function_not_supported, directory_not_empty, not_supported, operation_not_supported and timed_out,
stdlib/platform). `make_error_code(e)`: the ErrorCode of e in the
generic category. `ec == e` (and `!=`) for an ErrorCode and an Errc: a generic code equals the
condition of its value; a system code equals, elsewhere than Windows, the condition of its value,
and on Windows the condition its Windows error means: file, path
or network path not found, a bad name: no_such_file_or_directory; access denied, write
protected, sharing violation: permission_denied; already exists: file_exists; not same device:
cross_device_link; directory not empty: directory_not_empty; a directory name expected:
not_a_directory; and the others listed in Error.kpp's errcOfWindowsError. A zero code equals no
condition.

`SystemError`: a RuntimeError with an ErrorCode (`code()`); `SystemError(code)` has the code's
message as what(), `SystemError(code, what)` has `what: message`.
