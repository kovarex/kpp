# net

TCP for kpp: connections (`TcpStream`) and listeners (`TcpListener`), over Winsock on Windows and
BSD sockets on FreeBSD and Linux. The first user is a MariaDB client for gorating.com's move to
Kpp; an HTTP server comes later. No TLS: a reverse proxy (NFS's front end) or a C library does that.

```cpp
TcpStream connection = TcpStream::connect("127.0.0.1", 3307);
connection.setTimeout(10000);           // milliseconds; a read or write waiting longer throws
connection.write(request);
String reply = connection.readExactly(4);

TcpListener listener = TcpListener::listen("127.0.0.1", 0);   // port 0: any free one
println(listener.port());
TcpStream client = listener.accept();
```

## API

| | |
|---|---|
| `TcpStream::connect(StringView host, int port)` | a name or an address; each address the name has is tried |
| `stream.write(StringView)` | all the bytes, or throws |
| `stream.read(char* buffer, int64_t capacity)` | what has arrived, at least one byte; 0 when the other side closed |
| `stream.readExactly(int64_t n)`, `stream.readAll()` | n bytes (closing early throws), or everything until the other side closes |
| `stream.setTimeout(int milliseconds)` | for reads and writes; 0 waits forever (the default) |
| `stream.shutdownWrite()`, `stream.close()`, `stream.isOpen()` | the destructor closes too; streams move, they do not copy |
| `TcpListener::listen(StringView address, int port)`, `listener.port()`, `listener.accept()` | a numeric address ("127.0.0.1", "0.0.0.0", "::1") |
| `NetError` | a `RuntimeError` naming the call, the host and port, and the system's message ("timed out" for a timeout) |

## Platforms

- **Windows**: tested (`kpp --test`). Winsock starts on first use.
- **Linux**: written (src/NetPosix.kpp) but **not working yet**: glibc spells `sa_family_t` and
  `in_port_t` `unsigned short int`, which kpp reads as 4 bytes, so `sockaddr_in` is laid out wrong
  (pending spec language/short_int_spellings).
- **FreeBSD**: the same file; not compiled yet (no FreeBSD sysroot for C imports on the build
  machine). Its headers are believed to spell those types `unsigned short` and `uint8_t`, which kpp
  reads correctly (unverified until it compiles there).
  Exceptions do not unwind on either POSIX target yet (docs/design.md, "Targets and hosts").

## Tests

`kpp --test` here, or `kppc --test libraries/net`: a loopback connection both ways (100,000 bytes
back, `shutdownWrite`, `readAll`), a read timeout, and the errors: a refused connection, a name
that does not resolve, a closed stream, a connection closing before `readExactly` has its bytes.
