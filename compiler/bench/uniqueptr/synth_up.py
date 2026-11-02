# Generates a kpp program with N owner classes that use UniquePtr<Item_i> the way Factorio does
# (member, Vector of owners, makeUnique, ->, *, bool, get, reset, move in and out), or the same
# program with plain pointers and the deletes written out (what a built-in owning pointer would
# lower to, minus its unwinding cleanups): the difference is what the UniquePtr class costs.
# Usage: python -X utf8 -I synth_up.py N unique|raw OUTDIR (64 files plus main.kpp)
import sys

import os
n = int(sys.argv[1]); mode = sys.argv[2]; outdir = sys.argv[3]
os.makedirs(outdir, exist_ok=True)
parts = 64
files = [[] for _ in range(parts)]
lines = None
for i in range(n):
    lines = files[i % parts]
    I = f'Item{i}'
    O = f'Owner{i}'
    lines.append(f'struct {I}\n{{\n  int value = {i};\n  virtual ~{I}() {{}}\n  int get() const {{ return this->value; }}\n}};')
    if mode == 'unique':
        lines.append(f'''struct {O}
{{
  UniquePtr<{I}> item;
  Vector<UniquePtr<{I}>> items;
  void create() {{ this->item = makeUnique<{I}>(); }}
  void add() {{ this->items.push_back(makeUnique<{I}>()); }}
  int read() const {{ return this->item ? this->item->get() + (*this->item).value : 0; }}
  int sum() const
  {{
    int s = 0;
    for (const UniquePtr<{I}>& p : this->items)
      s += p->value;
    return s;
  }}
  UniquePtr<{I}> take() {{ return move(this->item); }}
  void give(UniquePtr<{I}> p) {{ this->item = move(p); }}
  void clear()
  {{
    this->item.reset();
    this->items.clear();
  }}
  {I}* peek() const {{ return this->item.get(); }}
  bool has() const {{ return this->item != nullptr; }}
}};
int use{i}()
{{
  {O} o;
  o.create();
  o.add();
  o.add();
  UniquePtr<{I}> t = o.take();
  o.give(move(t));
  int r = o.read() + o.sum() + (o.has() ? 1 : 0) + (o.peek() ? 1 : 0);
  o.clear();
  return r;
}}''')
    else:
        lines.append(f'''struct {O}
{{
  {I}* item = nullptr;
  Vector<{I}*> items;
  {O}() {{}}
  {O}(const {O}& other) = delete;
  ~{O}()
  {{
    if (this->item)
      delete this->item;
    for ({I}* p : this->items)
      if (p)
        delete p;
  }}
  void create()
  {{
    if (this->item)
      delete this->item;
    this->item = new {I}();
  }}
  void add() {{ this->items.push_back(new {I}()); }}
  int read() const {{ return this->item ? this->item->get() + (*this->item).value : 0; }}
  int sum() const
  {{
    int s = 0;
    for ({I}* p : this->items)
      s += p->value;
    return s;
  }}
  {I}* take()
  {{
    {I}* r = this->item;
    this->item = nullptr;
    return r;
  }}
  void give({I}* p)
  {{
    if (this->item)
      delete this->item;
    this->item = p;
  }}
  void clear()
  {{
    if (this->item)
      delete this->item;
    this->item = nullptr;
    for ({I}* p : this->items)
      if (p)
        delete p;
    this->items.clear();
  }}
  {I}* peek() const {{ return this->item; }}
  bool has() const {{ return this->item != nullptr; }}
}};
int use{i}()
{{
  {O} o;
  o.create();
  o.add();
  o.add();
  {I}* t = o.take();
  o.give(t);
  int r = o.read() + o.sum() + (o.has() ? 1 : 0) + (o.peek() ? 1 : 0);
  o.clear();
  return r;
}}''')
for k in range(parts):
    open(os.path.join(outdir, f'part{k}.kpp'), 'w', encoding='utf-8').write('\n'.join(files[k]) + '\n')
main = ['extern "C" int printf(const char* format, ...);', 'int main()', '{', '  int total = 0;']
for i in range(n):
    main.append(f'  total += use{i}();')
main.append('  printf("%d\\n", total);')
main.append('  return 0;')
main.append('}')
open(os.path.join(outdir, 'main.kpp'), 'w', encoding='utf-8').write('\n'.join(main) + '\n')
