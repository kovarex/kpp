// run.ps1 -Name game: the installed kpp extension in VS Code on a copy of factorio-in-kpp (target
// factorio): the first check, definition, hover, completion, a typed error, and switching
// configurations, timed. The positions are found by their text, so the converted tree may move them.
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
const uri = (p) => vscode.Uri.file(path.join(root, p));
async function until(what, f, ms = 120000)
{
  const end = Date.now() + ms;
  while (Date.now() < end) { const v = await f(); if (v) return v; await sleep(250); }
  log(`     (timed out waiting for ${what})`);
}
const hoverText = (h) => (h || []).map((x) => x.contents.map((c) => c.value || c).join('\n')).join('\n---\n');
async function hover(p, at) { return hoverText(await vscode.commands.executeCommand('vscode.executeHoverProvider', uri(p), at)); }
const short = (s) => JSON.stringify(s).slice(0, 400);
const counts = () => { let files = 0, n = 0; for (const [, d] of vscode.languages.getDiagnostics()) if (d.length) { files++; n += d.length; } return `${n} diagnostics in ${files} files`; };

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
  throw new Error(`game.js: ${p} has no line with ${text}`);
}

async function run()
{
  const ext = vscode.extensions.getExtension('kovarex.kpp');
  await until('activation', () => ext && ext.isActive, 30000);
  check('extension active', ext && ext.isActive);
  const main = 'src/Main.kpp';
  const editor = await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri(main)));
  const prevent = await find(main, 'SystemUtil::preventSleep', 'preventSleep');
  log(`     SystemUtil::preventSleep() at ${main}:${prevent.line + 1}`);
  let start = Date.now();
  const h = await until('the first check (hover on preventSleep)', async () => { const t = await hover(main, prevent); return t || null; });
  check('first check: hover on SystemUtil::preventSleep', /preventSleep/.test(h), `${((Date.now() - start) / 1000).toFixed(1)} s, ${short(h)}`);
  await sleep(2000);
  log(`     after the first check: ${counts()}`);

  start = Date.now();
  const defs = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', uri(main), prevent);
  const def = (defs || []).map((d) => `${path.relative(root, (d.uri || d.targetUri).fsPath)}:${(d.range || d.targetRange).start.line + 1}`);
  check('definition of preventSleep', def.length === 1 && /SystemUtil\.kpp/.test(def[0]), `${Date.now() - start} ms ${JSON.stringify(def)}`);
  // `Entity` in `Entity::count`: the class, with no type (the type is count's).
  const he = await hover(main, await find(main, 'Entity::count', 'Entity'));
  check('hover on the qualifier Entity in Entity::count', /class `Entity`/.test(he) && !/here of type/.test(he), short(he));

  // Completion after `SystemUtil::` typed on a line of its own after preventSleep's.
  await editor.edit((e) => e.insert(new vscode.Position(prevent.line + 1, 0), '  SystemUtil::\n'));
  start = Date.now();
  let labels = [];
  await until('completion of SystemUtil::', async () =>
  {
    const list = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', uri(main), new vscode.Position(prevent.line + 1, 14));
    labels = list.items.map((i) => (typeof i.label === 'string' ? i.label : i.label.label));
    return labels.includes('preventSleep');
  }, 60000);
  check('completion SystemUtil:: offers preventSleep', labels.includes('preventSleep'), `${Date.now() - start} ms, ${labels.length} items: ${labels.slice(0, 10).join(', ')}`);
  await vscode.commands.executeCommand('undo');
  await sleep(500);

  // A typed error before preventSleep's line (as tools/vs/vsix/test.ps1 types it) and its undo, timed.
  const diagnostics = () => vscode.languages.getDiagnostics(uri(main)).map((d) => d.message);
  await editor.edit((e) => e.insert(new vscode.Position(prevent.line, 0), '  kppUndeclaredName();\n'));
  start = Date.now();
  const typed = await until('the typed error', () => diagnostics().some((m) => /kppUndeclaredName/.test(m)), 120000);
  check('a typed error reported', !!typed, `${((Date.now() - start) / 1000).toFixed(1)} s, ${short(diagnostics())}`);
  await vscode.commands.executeCommand('undo');
  start = Date.now();
  const gone = await until('the error to clear', () => !diagnostics().some((m) => /kppUndeclaredName/.test(m)), 120000);
  check('the error gone after undo', !!gone, `${((Date.now() - start) / 1000).toFixed(1)} s`);

  // config::debug's hover, then a switch to release and back.
  const dx = 'src/Graphics/DX11/VideoBitmapDX11.kpp';
  await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri(dx)));
  // (In an `if constexpr`, there in every configuration: a `when (config::debug)` member is gone in release.)
  const debug = await find(dx, 'if constexpr (config::debug)', 'debug');
  const h1 = await hover(dx, debug);
  check('hover config::debug (factorio | debug)', /cell \*factorio, debug\*/.test(h1), short(h1));
  for (const configuration of ['release', 'debug', 'release'])
  {
    start = Date.now();
    await vscode.workspace.getConfiguration('kpp').update('configuration', configuration, vscode.ConfigurationTarget.Workspace);
    const t = await until(`the ${configuration} cell`, async () => { const x = await hover(dx, debug); return new RegExp(`cell \\*factorio, ${configuration}\\*`).test(x) ? x : null; });
    check(`switch to factorio | ${configuration}`, !!t, `${((Date.now() - start) / 1000).toFixed(1)} s, ${short(t)}`);
    await sleep(1500);
    log(`     ${counts()}`);
  }
  await vscode.workspace.getConfiguration('kpp').update('configuration', 'debug', vscode.ConfigurationTarget.Workspace);
  await sleep(3000);
  // The colors (docs/editors.md, "Colors"): the semantic tokens VS Code has for the file.
  const legend = await vscode.commands.executeCommand('vscode.provideDocumentSemanticTokensLegend', uri(dx));
  const tokens = await vscode.commands.executeCommand('vscode.provideDocumentSemanticTokens', uri(dx));
  const n = tokens && tokens.data ? tokens.data.length / 5 : 0;
  check('semantic tokens of ' + dx, n > 100, `${n} tokens, legend ${legend ? legend.tokenTypes.length : 0} types, editor.semanticHighlighting.enabled ${vscode.workspace.getConfiguration('editor').get('semanticHighlighting.enabled')}, theme ${vscode.workspace.getConfiguration('workbench').get('colorTheme')}`);
  fs.writeFileSync(out + '.screenshot', 'now');
  await sleep(6000);
  log(`done, ${failures} failed`);
}
exports.run = run;
