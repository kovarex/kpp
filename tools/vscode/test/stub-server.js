// A stand-in for `kpp --lsp` (docs/editors.md, "The language server"), so that the extension and the
// smoke client can be tried before the compiler's server answers: it offers the agreed
// capabilities, reports a diagnostic on every line containing `error_here`, answers a hover with the
// word under the cursor, and echoes the initializationOptions it got in a window/logMessage.
const { writer, reader } = require('./jsonrpc');

const send = writer(process.stdout);
const documents = new Map(); // uri -> text

function publish(uri)
{
  const lines = (documents.get(uri) || '').split(/\r?\n/);
  const diagnostics = [];
  lines.forEach((line, i) =>
  {
    const at = line.indexOf('error_here');
    if (at >= 0)
      diagnostics.push({
        range: { start: { line: i, character: at }, end: { line: i, character: at + 'error_here'.length } },
        severity: 1,
        source: 'kpp',
        message: "'error_here' is not declared (the stub server)"
      });
  });
  send({ jsonrpc: '2.0', method: 'textDocument/publishDiagnostics', params: { uri, diagnostics } });
}

// Applies LSP content changes: whole texts, or ranges in UTF-16 positions.
function apply(text, changes)
{
  for (const change of changes)
  {
    if (!change.range)
    {
      text = change.text;
      continue;
    }
    const offset = (position) =>
    {
      let line = 0, i = 0;
      while (line < position.line && i < text.length)
        if (text[i++] === '\n')
          ++line;
      return i + position.character;
    };
    text = text.slice(0, offset(change.range.start)) + change.text + text.slice(offset(change.range.end));
  }
  return text;
}

function wordAt(text, position)
{
  const line = text.split(/\r?\n/)[position.line] || '';
  let start = position.character, end = position.character;
  while (start > 0 && /\w/.test(line[start - 1]))
    --start;
  while (end < line.length && /\w/.test(line[end]))
    ++end;
  return line.slice(start, end);
}

reader(process.stdin, (message) =>
{
  const { id, method, params } = message;
  const reply = (result) => send({ jsonrpc: '2.0', id, result });
  switch (method)
  {
    case 'initialize':
      send({ jsonrpc: '2.0', method: 'window/logMessage',
             params: { type: 3, message: `stub: initializationOptions ${JSON.stringify(params.initializationOptions || null)}` } });
      reply({
        capabilities: {
          textDocumentSync: { openClose: true, change: 2, save: { includeText: false } },
          hoverProvider: true,
          definitionProvider: true,
          referencesProvider: true,
          documentSymbolProvider: true,
          workspaceSymbolProvider: true,
          completionProvider: { triggerCharacters: ['.', '>', ':'] },
          renameProvider: { prepareProvider: true }
        },
        serverInfo: { name: 'kpp-stub', version: '0' }
      });
      break;
    case 'textDocument/didOpen':
      documents.set(params.textDocument.uri, params.textDocument.text);
      publish(params.textDocument.uri);
      break;
    case 'textDocument/didChange':
      documents.set(params.textDocument.uri, apply(documents.get(params.textDocument.uri) || '', params.contentChanges));
      publish(params.textDocument.uri);
      break;
    case 'textDocument/didClose':
      documents.delete(params.textDocument.uri);
      break;
    case 'workspace/didChangeConfiguration':
      send({ jsonrpc: '2.0', method: 'window/logMessage',
             params: { type: 3, message: `stub: configuration ${JSON.stringify(params.settings)}` } });
      break;
    case 'textDocument/hover':
    {
      const word = wordAt(documents.get(params.textDocument.uri) || '', params.position);
      reply(word ? { contents: { kind: 'markdown', value: `\`${word}\` (the stub server)` } } : null);
      break;
    }
    case 'textDocument/definition':
    case 'textDocument/references':
    case 'textDocument/documentSymbol':
    case 'workspace/symbol':
    case 'textDocument/prepareRename':
    case 'textDocument/rename':
      reply(null);
      break;
    case 'textDocument/completion':
      reply([]);
      break;
    case 'kpp/cells':
      reply({ cells: [{ target: 'main', configuration: 'debug', visualStudio: 'Debug' }], active: { target: null, configuration: 'debug' } });
      break;
    case 'shutdown':
      reply(null);
      break;
    case 'exit':
      process.exit(0);
      break;
    default:
      if (id !== undefined)
        send({ jsonrpc: '2.0', id, error: { code: -32601, message: `stub: ${method} is not handled` } });
  }
});
