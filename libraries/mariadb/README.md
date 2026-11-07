# mariadb

A client for MariaDB (and MySQL) in kpp: the client/server protocol's text queries over TCP
([net](../net/README.md)), with `mysql_native_password`. Written for gorating.com's move from PHP,
whose database is MariaDB 11.7 with triggers and procedures; what a query returns is what PHP's
mysqli returns, every value as the server's text and NULL apart.

```cpp
MariaDbOptions options;
options.host = "127.0.0.1";
options.port = 3306;
options.user = "gorating";
options.password = password;          // from the configuration, never from the source
options.database = "gorating";
MariaDbConnection db = MariaDbConnection::connect(options);

MariaDbResult players = db.query("SELECT id, name, rating FROM user WHERE username = ", mariaDbQuote(username));
for (ContainerSize i = 0; i < players.size(); ++i)
  println(players.text(i, "name"));

db.begin();
MariaDbResult insert = db.query("INSERT INTO game (...) VALUES (...)");
println(insert.insertId);
db.commit();
```

Another library or a program names these `mariadb::MariaDbConnection` and so on (scoped
libraries, docs/build.md).

## API

| | |
|---|---|
| `MariaDbConnection::connect(const MariaDbOptions&)` | host, port, user, password, database, `timeoutMilliseconds` (30 s; 0 waits forever) |
| `connection.query(Text sql)` | one statement, given as pieces (`db.query("SELECT name FROM user WHERE id=", id)`, design.md 14); a `CALL`'s results are read, the last one returned |
| `connection.scalar(Text sql)` | the first row's first value (`Optional<String>`); no row throws |
| `connection.begin()`, `commit()`, `rollback()` | `START TRANSACTION`, `COMMIT`, `ROLLBACK` |
| `connection.serverVersion` | as the server says it ("11.7.2-MariaDB") |
| `MariaDbResult` | `columns`, `rows` (`Vector<Optional<String>>` each), `affectedRows`, `insertId`; `size()`, `column(name)`, `at(row, name)`, `text(row, name)` (NULL throws) |
| `mariaDbQuote(StringView)` | the text in single quotes, escaped as PHP's `real_escape_string` does (`\0 \n \r \\ ' "` and Ctrl-Z) |
| `mariaDbQuoteOrNull(const Optional<String>&)` | `NULL` for an empty one |
| `MariaDbError` | the server's `code` and `sqlState`, its message and the statement; also for a protocol the client does not follow |

A connection closes (`COM_QUIT`) when destroyed; it moves, it does not copy. One connection is
for one thread at a time. Connecting can also throw `net::NetError` (no server, a timeout).

**Not supported, as errors:** authentication plugins other than `mysql_native_password`
(`caching_sha2_password`, `ed25519`, `parsec`: an error naming the plugin), TLS, prepared
statements (the binary protocol), `LOAD DATA LOCAL INFILE`, several statements in one `query`.
Quoting assumes the server's default `sql_mode` without `NO_BACKSLASH_ESCAPES`, as PHP's does.

## Tests

`kpp --test` in this directory (not `kpp --test libraries/mariadb`, which leaves out the build.kpp
that brings in net):
- SHA-1 against FIPS 180's examples (the million `a` too), `mysql_native_password` against
  Python's `hashlib`;
- quoting, length-encoded integers (each width, and a bad first byte);
- the protocol against a fake server on a thread: a MariaDB 11 greeting, the handshake response
  (user, scramble, database), a result set with a NULL, an OK with its counts, an ERR, `COM_QUIT`;
- against a real server when `MARIADB_TEST_PORT` (with `MARIADB_TEST_USER`, `MARIADB_TEST_PASSWORD`,
  `MARIADB_TEST_DATABASE`) names one on 127.0.0.1, which it says it skipped otherwise: quoting of
  quotes, backslashes, zero bytes and UTF-8 round-trips, binary data, NULL, a procedure's result,
  a rolled-back transaction, a 20 MB result, an error and then a query on the same connection,
  and a wrong password (1045). Passed against MariaDB 11.7.2 on Windows.
