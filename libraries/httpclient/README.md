# httpclient

HTTP and HTTPS requests for kpp: through Windows' own client (WinHTTP, with its TLS, certificates
and proxy settings), and HTTP/1.1 with the base system's OpenSSL on FreeBSD. Written for
gorating.com's background jobs, which read the European Go Database's pages.

```cpp
httpclient::HttpClientResponse page = httpclient::fetch("https://www.europeangodatabase.eu/EGD/EGD_2_0/downloads/tlist.html");
if (page.status == 200)
  parse(page.body);

httpclient::HttpClientResponse found = httpclient::postForm("https://example.test/search", "country=CZ&year=2026");
```

## API

| | |
|---|---|
| `fetch(StringView url)` | a GET |
| `postForm(StringView url, StringView form)` | a POST of `application/x-www-form-urlencoded` (url's `writeUrlQuery` writes one) |
| `fetch(const HttpClientRequest&)` | `method`, `url`, `headers`, `body`, `timeoutMilliseconds` (30 s), `followRedirects` (true) |
| `HttpClientResponse` | `status`, `headers`, `body`; `header(name)` (any case) |
| `HttpClientError` | no response: no connection, a timeout ("timed out"), a certificate that is not valid, an address that is not http or https, a header with a line break |

A response with any status is a response, not an error. Redirects are followed except from https
to http. The URL's path and query are sent as written: encode them first (url's `urlEncode`).

## Platforms

- **Windows**: WinHTTP, looked up in winhttp.dll when first used (a kpp library cannot name a link
  library in its build.kpp yet).
- **FreeBSD**: HTTP/1.1 over net's TCP, TLS through the base system's OpenSSL 3 with the system's
  certificates and the host name checked (src/HttpClientPosix.kpp); one connection per request, the
  body by Content-Length, chunked or until the server closes; no proxy. A program using it links
  OpenSSL's libssl and libcrypto (build.kpp's `systemLibrary(..., Platform::FreeBSD)`). FreeBSD's
  own client, `fetch(3)`, cannot POST, send headers, give the response's headers or a response with
  an error status.
- **Linux**: not yet, and a program using the library is a compile error there that says so: the
  FreeBSD client needs OpenSSL's headers and libraries in the Linux sysroot.

## Tests

`kpp --test` here: a thread on the loopback answers with canned responses and keeps what it was
sent: a GET with an encoded path and query, response headers, a POST form, a redirect, a 404; the
errors: a refused connection, another scheme, a user name in the address, a bad port, a header
with a line break, a server that never answers (the timeout). With `HTTPCLIENT_TEST_HTTPS` set, a
GET of EGD's tournament list over HTTPS and the refusal of expired.badssl.com's expired
certificate (passed on Windows 10, 2026-11-02, and on FreeBSD 14.3 in CI's FreeBSD VM, 2026-11-04).
