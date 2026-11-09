# io

File streams and console output: InFileStream, OutFileStream, cerr.

There is no text mode, on any platform (kovarex, 2026-11-06): a file is its bytes. An OutFileStream
writes `"\n"` as the one byte 0a, also on Windows, where std::ofstream's text mode writes 0d 0a;
an InFileStream reads every byte as it is. `getline(stream, line)` with the newline as delimiter
drops a `\r` right before it, so the lines of a CRLF file come without it on every platform; a `\r`
anywhere else stays (spec library/file_streams_have_no_text_mode).

A stream's state is std's (spec library/file_stream_state_follows_std). `if (in)` asks whether anything
failed, so a stream that only reached the end is true while `good()` is false. `getline` of a stream
that cannot read leaves the string as it was. A stream closed or default-constructed stays good until
an operation fails, so `out.close(); if (!out)` is true only when a write failed. An InFileStream's
`fail()` says whether anything failed, and `clear()` forgets the state, so the next read asks the file
again (spec library/in_file_stream_fail_and_clear_follow_std). OutFileStream has no `fail()` or `clear()`
yet, and neither has `rdstate()`: a program that calls one does not compile.
