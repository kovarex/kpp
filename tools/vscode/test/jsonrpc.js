// LSP's framing over a pair of streams: `Content-Length: N\r\n\r\n` and N bytes of JSON. Used by
// the stub server and the smoke client, which need no packages.
function writer(stream)
{
  return (message) =>
  {
    const body = Buffer.from(JSON.stringify(message), 'utf8');
    stream.write(`Content-Length: ${body.length}\r\n\r\n`);
    stream.write(body);
  };
}

// Calls onMessage with each message read from stream.
function reader(stream, onMessage)
{
  let buffer = Buffer.alloc(0);
  stream.on('data', (chunk) =>
  {
    buffer = Buffer.concat([buffer, chunk]);
    for (;;)
    {
      const end = buffer.indexOf('\r\n\r\n');
      if (end < 0)
        return;
      const header = buffer.subarray(0, end).toString('ascii');
      const match = /Content-Length:\s*(\d+)/i.exec(header);
      if (!match)
        throw new Error(`LSP header without Content-Length: ${header}`);
      const length = Number(match[1]);
      if (buffer.length < end + 4 + length)
        return;
      const body = buffer.subarray(end + 4, end + 4 + length).toString('utf8');
      buffer = buffer.subarray(end + 4 + length);
      onMessage(JSON.parse(body));
    }
  });
}

module.exports = { writer, reader };
