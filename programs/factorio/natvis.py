"""The game's own debugger rules (utils/tools/vs15/Visualizers/factorio.natvis) for the converted game.

    natvis.py FACTORIO CONVERTED OUT

Reads FACTORIO's factorio.natvis and writes OUT (the converted tree's tools/factorio.natvis, which its
build.kpp gives the game's executables): the rules as they are, with what names C++ alone changed.
- A scoped enumerator in an expression (`type == PropertyTree::Type::Dictionary`) is its value
  (`type == 5`), read from the enum's definition in CONVERTED: a member declared with the enum's
  underlying integer type (PropertyTree's `u8 type : 5`) compares with a number in kpp's debug
  information, and the enumerator's name needs the enum in it.
- MSVC's standard library members are kpp's stdlib members (STD below): a std::vector's elements
  are a Vector's items, a std::unique_ptr<T[]>'s pointer a UniqueArray's pointer.
Anything else of MSVC's library (a `_My` member, a std:: type) left in a rule is an error, as is an
enumerator not found, so that a new rule of the game's is not shipped wrong. Comments are left as
they are.
"""
import os
import re
import sys

# MSVC's members on the paths the game's rules take, and kpp's stdlib's (stdlib/core/Vector.kpp,
# UniqueArray.kpp).
STD = [
    ("bigData._Mypair._Myval2._Myfirst", "bigData.items"),  # SmallVector's std::vector<T>
    ("data._Mypair._Myval2", "data.pointer"),  # Inventory's std::unique_ptr<ItemStack[]>
]

# Rules left out (design.md 26, kovarex 2026-11-11: a type's debugValue is what the debugger shows): the
# types that have a debugValue in the converted game (overrides/source-edits.txt), and those whose
# members already show what the rule did (MapPosition's x and y, FixedPointNumbers; a Vector's x and y).
DEBUG_VALUE = r"FixedPointNumberTemplate&lt;[^\"]*&gt;|RealOrientation|SmallVector&lt;[^\"]*&gt;|MapPosition|Vector|Vector2f"


def enumBodies(converted):
    """(outer class, enum name) -> {enumerator: value} for each enum nested in a class of CONVERTED."""
    found = {}
    for root, dirs, files in os.walk(converted):
        dirs[:] = [d for d in dirs if not d.startswith(".") and d not in ("build", "bin", "data")]
        for name in files:
            if not name.endswith(".kpp"):
                continue
            with open(os.path.join(root, name), encoding="utf-8", errors="replace") as f:
                text = re.sub(r"//[^\n]*", "", f.read())
            for outer in re.finditer(r"^(?:class|struct) (?:\[\[\w+\]\] )*(\w+)\b[^;{]*\{", text, re.M):
                # The class's body: up to its closing brace at the start of a line.
                end = text.find("\n};", outer.end())
                body = text[outer.end():end if end >= 0 else len(text)]
                for e in re.finditer(r"\benum (?:class )?(\w+)\s*(?::\s*\w+)?\s*\{([^}]*)\}", body):
                    values = {}
                    next = 0
                    for item in e.group(2).split(","):
                        item = item.strip()
                        if not item:
                            continue
                        m = re.fullmatch(r"(\w+)(?:\s*=\s*(-?(?:0x[0-9a-fA-F]+|\d+)))?", item)
                        if not m:
                            values = None  # a value the script does not compute
                            break
                        if m.group(2) is not None:
                            next = int(m.group(2), 0)
                        values[m.group(1)] = next
                        next += 1
                    if values is not None:
                        found.setdefault((outer.group(1), e.group(1)), values)
    return found


def main():
    factorio, converted, out = sys.argv[1:4]
    with open(os.path.join(factorio, "utils", "tools", "vs15", "Visualizers", "factorio.natvis"), encoding="utf-8-sig") as f:
        text = f.read()
    enums = enumBodies(converted)
    errors = []
    dropped = []

    def drop(m):
        dropped.append(m.group(1).replace("&lt;", "<").replace("&gt;", ">"))
        return ""

    text = re.sub(r'[ \t]*<Type Name="(' + DEBUG_VALUE + r')">.*?</Type>[ \t]*\r?\n', drop, text, flags=re.S)
    pieces = re.split(r"(<!--.*?-->)", text, flags=re.S)
    for i in range(0, len(pieces), 2):  # the even pieces are outside comments
        piece = pieces[i]
        for old, new in STD:
            piece = piece.replace(old, new)

        def enumerator(m):
            values = enums.get((m.group(1), m.group(2)))
            if values is None or m.group(3) not in values:
                errors.append("no enumerator " + m.group(0) + " in the converted tree")
                return m.group(0)
            return str(values[m.group(3)])

        piece = re.sub(r"\b(\w+)::(\w+)::(\w+)\b", enumerator, piece)
        for m in re.finditer(r"\b_My\w*|\bstd::\w+", piece):
            errors.append("MSVC's standard library in a rule: " + m.group(0) + " (natvis.py's STD has no kpp name for it)")
        pieces[i] = piece
    if errors:
        for e in errors:
            print("natvis.py: " + e)
        return 1
    header = ("<!-- Factorio's utils/tools/vs15/Visualizers/factorio.natvis, written by kpp's programs/factorio/natvis.py:\n"
              "     scoped enumerators as their values, MSVC's library members as kpp's stdlib's; without the rules of\n"
              "     " + ", ".join(dropped) + ", which their debugValue or their members show. -->\n")
    result = "".join(pieces)
    decl = result.find("?>")
    result = result[:decl + 2] + "\n" + header + result[decl + 2:].lstrip("\r\n") if decl >= 0 else header + result
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(result.replace("\r\n", "\n"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
