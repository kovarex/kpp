# url

URLs for kpp, as [RFC 3986](https://www.rfc-editor.org/rfc/rfc3986) defines URI references:
parsing into components, resolving a link against the page it is on, percent-encoding, and query
strings as browsers send them.

```cpp
Url url = parseUrl("https://kpp.net/registry/json?format=json#readme");
println(url.host);                                         // kpp.net
String next = resolveUrl("https://kpp.net/docs/a/b.html", "../c.html");  // https://kpp.net/docs/c.html
String link = "/library/" + urlEncode(name);               // a path segment
Vector<UrlQueryParameter> query = parseUrlQuery(url.query);
const String* format = findUrlQueryParameter(query, "format");
```

## API

| | |
|---|---|
| `Url parseUrl(StringView)` | the components; text that is not a URI reference throws `UrlError` |
| `Url` | `scheme`, `userInfo`, `host`, `port`, `path`, `query`, `fragment`, as written (percent-encoded), with `hasAuthority`, `hasUserInfo`, `hasPort`, `hasQuery`, `hasFragment` telling an empty component from a missing one; `str()` writes it back (RFC 3986 5.3), `portNumber()`, `isRelative()` |
| `Url resolveUrl(const Url& base, const Url& reference)`, `String resolveUrl(StringView, StringView)` | the reference resolved against the base (RFC 3986 5.2, strict) |
| `String urlRemoveDotSegments(StringView path)` | `.` and `..` segments removed (5.2.4) |
| `String urlEncode(StringView)` | every byte but the unreserved characters (letters, digits, `- . _ ~`) as `%XX`: safe in any component |
| `String urlEncodePath(StringView)` | the same, `/` kept |
| `String urlDecode(StringView)` | `%XX` as the byte; a malformed `%` throws `UrlError`; `+` stays |
| `Vector<UrlQueryParameter> parseUrlQuery(StringView)` | `a=1&b=x+y`: `{name, value}` in order, `+` as a space, decoded |
| `String writeUrlQuery(const Vector<UrlQueryParameter>&)` | the reverse, a space as `+` |
| `const String* findUrlQueryParameter(parameters, name)` | the first value, or `nullptr` |

`UrlError` has `offset`, the byte where the problem is, and a message that starts with it.

## What parseUrl rejects

Characters that may not stand in their component (a space, `<`, `"`, bytes above ASCII: encode
them first), a `%` without two hex digits, a port that is not digits, an IP literal without its
`]`, and a `:` in the first segment of a relative path (`a:b` is a scheme; write `./a:b`). It does
not normalize: the case of the scheme and host, and `%XX` that need not be encoded, stay as
written.

## Tests

`kpp --test` in this directory, or `kpp --test libraries/url` anywhere:
- every example of RFC 3986 section 5.4, normal and abnormal (42), resolved against
  `http://a/b/c/d;p?q`. Python's `urljoin` gives the same results except for `http:g`, where it
  takes the RFC's backward-compatible reading (`http://a/b/c/g`) and this library the strict one
  (`http:g`);
- percent-encoding of all 256 byte values and query strings, against Python's
  `urllib.parse.quote` and `urlencode`;
- components, empty against missing ones, IP literals, and each rejection with its message.

## Not yet

- Internationalized domain names (punycode) and IRIs: non-ASCII text must be percent-encoded
  first.
- Normalization for comparison (RFC 3986 6): case, `%XX` of unreserved characters, default
  ports.
