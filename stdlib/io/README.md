# io

File streams and console output: InFileStream, OutFileStream, cerr.

There is no text mode, on any platform (kovarex, 2026-11-06): a file is its bytes. An OutFileStream
writes `"\n"` as the one byte 0a, also on Windows, where std::ofstream's text mode writes 0d 0a;
an InFileStream reads every byte as it is. `getline(stream, line)` with the newline as delimiter
drops a `\r` right before it, so the lines of a CRLF file come without it on every platform; a `\r`
anywhere else stays (spec library/file_streams_have_no_text_mode).
