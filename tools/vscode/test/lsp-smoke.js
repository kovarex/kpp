// What every editor's client relies on, checked against a language server: the stub
// (`node lsp-smoke.js node stub-server.js`) or the compiler's (`node lsp-smoke.js kppc.exe --lsp`).
// In a new project (build.kpp, src\main.kpp) it initializes with the agreed initializationOptions,
// expects the agreed capabilities, opens main.kpp with an undeclared name and expects an error on
// its line, fixes the line and expects the error gone, asks a hover, and shuts the server down.
// Prints `ok   lsp ...` or `FAIL lsp: ...`; the exit code is the number of failures.
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { pathToFileURL } = require('url');
const { writer, reader } = require('./jsonrpc');

// Arguments that are existing files are made absolute: the server runs in the new project.
const [command, ...args] = process.argv.slice(2).map((a) => (fs.existsSync(a) ? path.resolve(a) : a));
if (!command)
{
  console.log('usage: node lsp-smoke.js SERVER [ARGS...]');
  process.exit(2);
}

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kpp-lsp-'));
fs.mkdirSync(path.join(root, 'src'));
fs.writeFileSync(path.join(root, 'build.kpp'), 'void build(Project& project)\n{\n}\n');
const mainPath = path.join(root, 'src', 'main.kpp');
const broken = 'int answer() { return 42; }\nint main()\n{\n  return answer() + error_here;\n}\n';
const fixed = 'int answer() { return 42; }\nint main()\n{\n  return answer() + 0;\n}\n';
fs.writeFileSync(mainPath, broken);
const mainUri = pathToFileURL(mainPath).href;

const server = spawn(command, args, { cwd: root, stdio: ['pipe', 'pipe', 'inherit'] });
// Rejected when the server ends or cannot start: every wait fails at once then.
const serverExit = new Promise((_, reject) =>
{
  server.on('exit', (code) => reject(new Error(`the server exited (code ${code})`)));
  server.on('error', (error) => reject(new Error(`the server did not start: ${error.message}`)));
});
serverExit.catch(() => {});
const send = writer(server.stdin);
let nextId = 1;
const pending = new Map();      // id -> resolve
const diagnosticWaiters = [];   // functions given each publishDiagnostics for main.kpp
let failures = 0;

function fail(what)
{
  console.log(`FAIL lsp: ${what}`);
  ++failures;
}

reader(server.stdout, (message) =>
{
  if (message.id !== undefined && pending.has(message.id))
  {
    pending.get(message.id)(message);
    pending.delete(message.id);
  }
  else if (message.method === 'textDocument/publishDiagnostics' && message.params.uri.toLowerCase() === mainUri.toLowerCase())
    diagnosticWaiters.splice(0).forEach((waiter) => waiter(message.params.diagnostics));
  else if (message.id !== undefined && message.method)
    send({ jsonrpc: '2.0', id: message.id, result: null }); // a request of the server's (workspace/configuration ...)
});

function request(method, params)
{
  const id = nextId++;
  send({ jsonrpc: '2.0', id, method, params });
  return new Promise((resolve) => pending.set(id, resolve));
}

function notify(method, params)
{
  send({ jsonrpc: '2.0', method, params });
}

function nextDiagnostics()
{
  return new Promise((resolve) => diagnosticWaiters.push(resolve));
}

function within(ms, promise, what)
{
  return Promise.race([promise, serverExit, new Promise((_, reject) => setTimeout(() => reject(new Error(`no ${what} within ${ms} ms`)), ms))]);
}

async function main()
{
  const initialized = await within(30000, request('initialize', {
    processId: process.pid,
    rootUri: pathToFileURL(root).href,
    workspaceFolders: [{ uri: pathToFileURL(root).href, name: path.basename(root) }],
    capabilities: { general: { positionEncodings: ['utf-16'] }, textDocument: { publishDiagnostics: {} } },
    initializationOptions: { configuration: 'debug', executable: null, settings: {} }
  }), 'initialize result');
  const capabilities = (initialized.result || {}).capabilities || {};
  const sync = typeof capabilities.textDocumentSync === 'number' ? capabilities.textDocumentSync : (capabilities.textDocumentSync || {}).change;
  if (sync !== 1 && sync !== 2)
    fail(`textDocumentSync change is ${JSON.stringify(sync)}, not full or incremental`);
  for (const provider of ['hoverProvider', 'definitionProvider', 'referencesProvider', 'documentSymbolProvider',
                          'workspaceSymbolProvider', 'completionProvider', 'renameProvider'])
    if (!capabilities[provider])
      fail(`no ${provider}`);
  notify('initialized', {});

  const first = nextDiagnostics();
  notify('textDocument/didOpen', { textDocument: { uri: mainUri, languageId: 'kpp', version: 1, text: broken } });
  const errors = await within(30000, first, 'diagnostics for main.kpp');
  if (!errors.some((d) => d.range.start.line === 3 && (d.severity === undefined || d.severity === 1)))
    fail(`no error on line 4 of main.kpp: ${JSON.stringify(errors)}`);

  const second = nextDiagnostics();
  notify('textDocument/didChange', {
    textDocument: { uri: mainUri, version: 2 },
    contentChanges: [{ range: { start: { line: 3, character: 20 }, end: { line: 3, character: 30 } }, text: '0' }]
  });
  const after = await within(30000, second, 'diagnostics after the fix');
  if (after.length !== 0)
    fail(`errors left after the fix: ${JSON.stringify(after)}`);
  fs.writeFileSync(mainPath, fixed);

  const hover = await within(30000, request('textDocument/hover', { textDocument: { uri: mainUri }, position: { line: 3, character: 10 } }), 'hover');
  if (!hover.result || !hover.result.contents)
    fail(`no hover on answer(): ${JSON.stringify(hover)}`);

  await within(10000, request('shutdown', null), 'shutdown result');
  serverExit.catch(() => {});
  notify('exit', null);
}

main()
  .catch((error) => fail(error.message))
  .finally(() =>
  {
    setTimeout(() =>
    {
      server.kill();
      try { fs.rmSync(root, { recursive: true, force: true }); } catch (error) { console.log(`note: ${root} is left: ${error.message}`); }
      if (failures === 0)
        console.log(`ok   lsp (${[command, ...args].map((a) => path.basename(a)).join(' ')})`);
      process.exit(failures);
    }, 200);
  });
