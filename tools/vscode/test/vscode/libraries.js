// run.ps1 -Name libraries: the installed kpp extension in VS Code on a copy of a project that uses
// kpp libraries (gorating: json, http, html, ...; docs/editors.md, "VS Code"): go to definition from
// the program into a library's file, hover and semantic tokens in that file, completion after a
// scoped library's name (`json::`).
const vscode = require('vscode');
const fs = require('fs');
const path = require('path');

const out = process.env.KPP_HARNESS_LOG;
const t0 = Date.now();
function log(line) { fs.appendFileSync(out, `[${((Date.now() - t0) / 1000).toFixed(1)}s] ${line}\n`); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
function check(name, ok, detail) { log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`); if (!ok) failures++; }
const root = vscode.workspace.workspaceFolders[0].uri.fsPath;
const uri = (p) => (path.isAbsolute(p) ? vscode.Uri.file(p) : vscode.Uri.file(path.join(root, p)));
async function until(what, f, ms = 120000)
{
  const end = Date.now() + ms;
  while (Date.now() < end) { const v = await f(); if (v) return v; await sleep(250); }
  log(`     (timed out waiting for ${what})`);
}
const hoverText = (h) => (h || []).map((x) => x.contents.map((c) => c.value || c).join('\n')).join('\n---\n');
const short = (s) => JSON.stringify(s).slice(0, 300);

// The first line of P containing TEXT, at WORD's second character within it.
async function find(p, text, word)
{
  const document = await vscode.workspace.openTextDocument(uri(p));
  for (let line = 0; line < document.lineCount; ++line)
  {
    const at = document.lineAt(line).text.indexOf(text);
    if (at >= 0)
      return new vscode.Position(line, at + text.indexOf(word) + 1);
  }
  throw new Error(`libraries.js: ${p} has no line with ${text}`);
}

async function run()
{
  const ext = vscode.extensions.getExtension('kovarex.kpp');
  await until('activation', () => ext && ext.isActive, 30000);
  check('extension active', ext && ext.isActive);
  const file = 'src/Config.kpp';
  const editor = await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri(file)));
  const call = await find(file, 'json::parseJson(', 'parseJson');

  // Definition: the library's own file, beside kpp (or the project's copy of the library).
  const defs = await until('the definition of json::parseJson', async () =>
  {
    const d = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', uri(file), call);
    return d && d.length ? d : null;
  });
  const target = defs && defs[0] ? (defs[0].targetUri || defs[0].uri).fsPath : '';
  const line = defs && defs[0] ? (defs[0].targetRange || defs[0].range).start.line : -1;
  check('definition of json::parseJson in the library', /[\\/]json[\\/]src[\\/]JsonParse\.kpp$/i.test(target), `${target}:${line + 1}`);

  // In the library's file: hover and semantic tokens.
  if (target)
  {
    await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(vscode.Uri.file(target)));
    const inside = await find(target, '= parseJson(', 'parseJson');
    const h = await until('hover in the library', async () => hoverText(await vscode.commands.executeCommand('vscode.executeHoverProvider', vscode.Uri.file(target), inside)) || null, 30000);
    check('hover in the library file', /json::parseJson/.test(h || ''), short(h));
    const tokens = await until('the library file\'s semantic tokens', async () =>
    {
      const t = await vscode.commands.executeCommand('vscode.provideDocumentSemanticTokens', vscode.Uri.file(target));
      return t && t.data && t.data.length ? t : null;
    }, 30000);
    check('semantic tokens of the library file', !!tokens, `${tokens ? tokens.data.length / 5 : 0} tokens`);
  }

  // Completion after `json::` typed on a line of its own.
  const back = await vscode.window.showTextDocument(editor.document); // (the earlier editor was replaced)
  await back.edit((e) => e.insert(new vscode.Position(call.line + 1, 0), '  json::\n'));
  let labels = [];
  await until('completion after json::', async () =>
  {
    const list = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', uri(file), new vscode.Position(call.line + 1, 8), ':');
    labels = list.items.map((i) => (typeof i.label === 'string' ? i.label : i.label.label));
    return labels.includes('parseJson');
  }, 60000);
  check('completion after json:: offers parseJson', labels.includes('parseJson'), `${labels.length} items: ${labels.slice(0, 8).join(', ')}`);
  await vscode.commands.executeCommand('undo');
  await sleep(1000);
  fs.writeFileSync(out + '.screenshot', 'now');
  await sleep(4000);
  log(`done, ${failures} failed`);
}
exports.run = run;
