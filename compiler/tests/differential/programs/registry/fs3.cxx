#include <stdio.h>
#include <filesystem>
#include <chrono>
namespace filesystem = std::filesystem;
using ErrorCode = std::error_code;
using Seconds = std::chrono::seconds;
// battery fs3: the filesystem functions added for E's list, against MSVC's std::filesystem: relative
// and proximate, hash_value of equal paths, u8path, last_write_time set and read back (and through a
// directory entry), resize_file, hard links and their count, symbolic links (made, read, copied,
// statused; skipped where the system refuses them), recursive_directory_iterator with depth, pop and
// disable_recursion_pending.
int main()
{
  using namespace filesystem;
  path root = temp_directory_path() / "kpp-fs3-battery";
  remove_all(root);
  create_directories(root / "a" / "b" / "c");
  create_directories(root / "d");
  { FILE* f = fopen((root / "a" / "one.txt").string().c_str(), "wb"); fputs("hello", f); fclose(f); }
  { FILE* f = fopen((root / "a" / "b" / "two.txt").string().c_str(), "wb"); fclose(f); }
  { FILE* f = fopen((root / "d" / "three.txt").string().c_str(), "wb"); fclose(f); }
  path here = current_path();
  current_path(root);
  printf("relative [%s] [%s] [%s]\n", relative(root / "a" / "b", root / "d").generic_string().c_str(), relative("a/b/c").generic_string().c_str(),
         relative(root / "a", root / "a").generic_string().c_str());
  printf("proximate [%s] [%s]\n", proximate("a/./b/../one.txt", root / "d").generic_string().c_str(), proximate(root / "a", root / "a" / "b").generic_string().c_str());
  current_path(here);
  printf("hash %d %d %d\n", (int)(hash_value(path("x/y/z")) == hash_value(path("x\\y\\z"))), (int)(hash_value(path("x/y")) == hash_value(path("x/z"))),
         (int)(hash_value(path("C:/q")) == hash_value(path("C:\\q"))));
  printf("u8path [%s] %d\n", u8path("caf\xC3\xA9/x").filename().string().c_str(), (int)u8path("caf\xC3\xA9").native().size());
  path one = root / "a" / "one.txt";
  file_time_type t0 = last_write_time(one);
  file_time_type set = t0 - Seconds(3600 * 24 * 365);
  last_write_time(one, set);
  file_time_type t1 = last_write_time(one);
  printf("time %d %d\n", (int)(t1 == set), (int)((t0 - t1) == Seconds(3600 * 24 * 365)));
  for (const directory_entry& e : directory_iterator(root / "a"))
    if (e.path().filename() == path("one.txt"))
      printf("entry time %d\n", (int)(e.last_write_time() == set));
  resize_file(one, 2);
  printf("resize %d", (int)file_size(one));
  resize_file(one, 10);
  printf(" %d\n", (int)file_size(one));
  create_hard_link(one, root / "d" / "same.txt");
  printf("hardlink %d %d %d\n", (int)hard_link_count(one), (int)hard_link_count(root / "d" / "same.txt"), (int)equivalent(one, root / "d" / "same.txt"));
  ErrorCode ec;
  create_symlink("a/one.txt", root / "link.txt", ec);
  if (!ec)
  {
    create_directory_symlink("a", root / "dirlink", ec);
    printf("symlink [%s] %d %d %d", read_symlink(root / "link.txt").generic_string().c_str(), (int)is_symlink(root / "link.txt"), (int)is_regular_file(root / "link.txt"),
           (int)file_size(root / "link.txt"));
    copy_symlink(root / "link.txt", root / "link2.txt");
    printf(" copy [%s] %d", read_symlink(root / "link2.txt").generic_string().c_str(), (int)is_symlink(root / "link2.txt"));
    printf(" dir [%s] %d\n", read_symlink(root / "dirlink").generic_string().c_str(), (int)is_directory(root / "dirlink"));
  }
  else
    printf("symlink refused\n");
  int count = 0, deepest = 0;
  for (recursive_directory_iterator it(root / "a"), end; it != end; it.increment(ec))
  {
    ++count;
    deepest = it.depth() > deepest ? it.depth() : deepest;
  }
  printf("recursive %d %d\n", count, deepest);
  count = 0;
  for (recursive_directory_iterator it(root), end; it != end; it.increment(ec))
  {
    if (it->path().filename() == path("a"))
      it.disable_recursion_pending();
    ++count;
  }
  printf("disabled %d\n", count);
  recursive_directory_iterator it(root / "a");
  while (it != recursive_directory_iterator() && it.depth() == 0)
    it.increment(ec);
  int depthBeforePop = it == recursive_directory_iterator() ? -1 : it.depth();
  it.pop();
  printf("pop %d %d\n", depthBeforePop, (int)(it == recursive_directory_iterator() ? -1 : it.depth()));
  printf("removed %d\n", (int)remove_all(root));
  return 0;
}
