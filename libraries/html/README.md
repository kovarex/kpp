# html

HTML for kpp: escaping, and templates whose holes are escaped by where they are, checked when the
program is compiled.

```cpp
constexpr HtmlTemplate libraryCard(R"(<li><a href="{{url}}">{{name}}</a> {{summary}}</li>)");
constexpr HtmlTemplate page(R"(<!DOCTYPE html>
<html><head><title>{{title}}</title></head><body><ul>{{items}}</ul></body></html>)");

SafeHtml items;
for (const Library& library : libraries)
  items << libraryCard.render({{"url", "/library/" + library.name}, {"name", library.name}, {"summary", library.summary}});
String html = page.render({{"title", "Libraries"}, {"items", items}}).html;
```

## Templates

A template is HTML with holes, `{{name}}` (letters, digits and `_`). It has no loops or
conditions: the program renders a part per item and passes the result to a hole as `SafeHtml`.

**Made as a `constexpr` variable, a template is read by the compiler** (its constructor runs in
the compile-time interpreter), so a malformed template is a compile error at the variable that
says why: `error: the constexpr variable broken is no constant: it throws
HtmlTemplateError("1:4: a '{{' without its '}}'")`. A template made at run time (from a file) is
checked the same way when it is made, its constructor throwing `HtmlTemplateError` with
`line:column: what`.

What a template must be:
- **Every element is closed explicitly**, in order, except the void ones (`br`, `img`,
  `input`, `meta`, `link`, `hr`...); `<x/>` closes itself. HTML's optional end tags (`</p>`,
  `</li>`) are not optional here.
- **`<` starts a tag, a comment or `<!DOCTYPE>`**: a lone `<` in text is an error (write `&lt;`).
- **Holes go in three places**: an element's text, quoted attribute values, and where an
  attribute goes in a tag, for a boolean attribute: `<option value="{{value}}" {{selected}}>`
  takes a bool named as the attribute (lowercase letters), true writes ` selected` and false
  nothing (the spaces before the hole are its own). A hole is an error in a tag name, an
  attribute name, an unquoted attribute value, a comment, `<!DOCTYPE>`, `<script>` and `<style>`,
  and in the `style`, `srcset`, `srcdoc` and `on*` attributes: escaping for JavaScript, CSS and a
  document in an attribute is not HTML text's, so the template passes data another way.

## How a hole's value is written

| Where the hole is | A String (or `int`, `long long`) | A `SafeHtml` |
|---|---|---|
| an element's text | escaped: `& < > " '` | inserted as it is |
| a quoted attribute value | escaped | error |
| where an attribute goes in a tag | error (a bool: ` name` or nothing) | error |
| the start of `href`, `src`, `action`, `formaction`, `cite` or `poster` | a URL: its scheme, if it has one, must be `http`, `https` or `mailto` (else an error, so `javascript:` never gets in); then escaped | error |
| later in such a URL, before `?` or `#` | percent-encoded, `/` kept: `/library/{{name}}` | error |
| in its query or fragment | percent-encoded, `/` too: `?q={{query}}` | error |

**`render` throws** `HtmlTemplateError` for a hole without a value, a value with no hole (a typo
in a name), a URL with another scheme, `SafeHtml` outside an element's text, and a bool anywhere
but a boolean attribute's hole (or anything else there). It returns `SafeHtml`, so a rendered
template goes into another one's hole unescaped.

## API

| | |
|---|---|
| `String htmlEscape(StringView)`, `htmlAppendEscaped(String&, StringView)` | `& < > " '` as `&amp; &lt; &gt; &quot; &#39;` |
| `SafeHtml` | HTML inserted as it is: `SafeHtml::trusted(html)`, `SafeHtml::escape(text)`, `<<` to append, `.str()` |
| `constexpr HtmlTemplate(const char* text)` | reads the template; throws `HtmlTemplateError` when it runs at run time. The text must outlive the template (a literal does) |
| `SafeHtml render({{"name", value}, ...}) const`, `renderTo(String&, ...)` | the template with its holes filled |
| `Vector<String> holes() const` | the holes' names in order |

Limits: 256 parts (text runs and holes), elements nested 64 deep; beyond them a template is an
error that says so.

## Tests

`kpp --test` in this directory, or `kppc --test libraries/html` anywhere: escaping, templates made
at compile time (a card, URL holes, a whole page with a comment, `<style>`, void and self-closed
elements, boolean attributes), each template error and each render error with its message.

## Not yet

- Character references in a template's own text are not checked (`&bogus;` passes).
- Holes in `<textarea>` and `<title>` are text, which is right; the obsolete raw-text elements
  (`<xmp>`, `<plaintext>`) are not recognized.
