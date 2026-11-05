# http

HTTP for kpp web programs: the request (method, path, query, headers, cookies, urlencoded and
multipart forms) and the response (status, headers, cookies, redirects, body), with two ways to
run the same handler:
- **CGI** (`httpCgi`): the web server starts the program for each request. NearlyFreeSpeech.NET's
  Apache runs any executable ending in `.cgi` this way, and does TLS and HTTP itself.
- **A small server** (`httpServe`): for trying a program on one's own machine. One connection at a
  time, closed after its response; not for the internet.

Written for gorating.com's move from PHP. It uses [net](../net/README.md) and
[url](../url/README.md); HTML comes from [html](../html/README.md).

```cpp
void handle(const http::HttpRequest& request, http::HttpResponse& response)
{
  if (request.path == "/login" && request.method == "POST")
  {
    Vector<url::UrlQueryParameter> form = request.form();
    ...
    response.setCookie("gorating_id", sessionId, options);
    response.redirect("/");
    return;
  }
  response.body = page(request).str();
}

int main()
{
  if (getenv("REQUEST_METHOD") != nullptr)
    return http::httpCgi(handle);          // under Apache
  http::HttpServerOptions options;
  options.port = 8080;
  http::httpServe(options, handle);        // on one's own machine
  return 0;
}
```

## API

| | |
|---|---|
| `HttpRequest` | `method`, `target` (as sent), `path` (decoded), `query` (as sent), `parameters` (the query's, decoded), `headers` (names in lower case), `body`, `remoteAddress` |
| `request.header(name)`, `request.parameter(name)` | the first one, or `nullptr` |
| `request.cookie(name)` | `Optional<String>` |
| `request.form()` | the fields of an urlencoded or multipart body (an empty body has none; another type throws 415) |
| `request.multipart()` | the parts of a multipart/form-data body: `name`, `filename` (`Optional`), `contentType`, `data` |
| `HttpResponse` | `status` (200), `headers` (`Content-Type: text/html; charset=utf-8` to start with), `body` |
| `response.setHeader`, `addHeader`, `header` | a header name must be a token and a value has no line break or zero byte: otherwise `HttpError`, so a value cannot write headers of its own |
| `response.setCookie(name, value, HttpCookieOptions)`, `deleteCookie(name)` | `maxAgeSeconds`, `path` ("/"), `httpOnly` (true), `secure`, `sameSite` ("Lax"); a value a cookie cannot carry throws (encode it first) |
| `response.redirect(location, status = 303)` | 303 See Other: after a POST the browser GETs the location |
| `httpCgi(handler)` | reads the request CGI gives (REQUEST_URI or SCRIPT_NAME + PATH_INFO + QUERY_STRING, the HTTP_ variables, stdin), writes the response to stdout; returns main's exit code |
| `httpServe(HttpServerOptions, handler)` | `address` (127.0.0.1), `port`, `maxRequests`, `maxBodyBytes`, `log` |
| `HttpError` | a request this library does not read, with the `status` to answer (400, 411, 413, 415, 431, 501), or a response it will not write (500) |

**Errors.** A request that cannot be read gets its 4xx answer. An exception from the handler is
a 500: under CGI the browser gets no detail and the message goes to stderr, which Apache writes to
its error log; the small server puts the message in the body, since it runs on one's own machine.

**Not supported:** chunked request bodies (501; browsers send Content-Length for forms),
keep-alive and pipelining in the small server, HTTP/2, TLS (the web server or a proxy does it).

## Tests

`kpp --test` in this directory: cookies both ways, the header and cookie checks (a `\r\n` in a
redirect's location, a `;` in a cookie value), responses as CGI and HTTP/1.1 write them,
urlencoded and multipart forms (a file, an empty file field, quoted names, a quoted boundary,
malformed bodies), and the small server on the loopback: a GET with a cookie and a query, a POST
form answered with a redirect, a handler that throws (500), a chunked body (501) and a request
that is not HTTP (400). CGI's reading of the environment is tested by gorating's own CGI program.
