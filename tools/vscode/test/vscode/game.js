// run.ps1 -Name game: the installed kpp extension in VS Code on a copy of factorio-in-kpp (target
// factorio): the first check, definition, hover, completion, and switching configurations, timed.
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
async function hover(p, line, ch) { return hoverText(await vscode.commands.executeCommand('vscode.executeHoverProvider', uri(p), new vscode.Position(line, ch))); }
const short = (s) => JSON.stringify(s).slice(0, 400);
const counts = () => { let files = 0, n = 0; for (const [, d] of vscode.languages.getDiagnostics()) if (d.length) { files++; n += d.length; } return `${n} diagnostics in ${files} files`; };

async function run()
{
  const ext = vscode.extensions.getExtension('kovarex.kpp');
  await until('activation', () => ext && ext.isActive, 30000);
  check('extension active', ext && ext.isActive);
  const main = 'src/Main.kpp';
  const editor = await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri(main)));
  let start = Date.now();
  const h = await until('the first check (hover on preventSleep)', async () => { const t = await hover(main, 27, 16); return t || null; });
  check('first check: hover on SystemUtil::preventSleep', !!h, `${((Date.now() - start) / 1000).toFixed(1)} s, ${short(h)}`);
  await sleep(2000);
  log(`     after the first check: ${counts()}`);

  start = Date.now();
  const defs = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', uri(main), new vscode.Position(27, 16));
  const def = (defs || []).map((d) => `${path.relative(root, (d.uri || d.targetUri).fsPath)}:${(d.range || d.targetRange).start.line + 1}`);
  check('definition of preventSleep', def.length > 0, `${Date.now() - start} ms ${JSON.stringify(def)}`);
  // `Entity` in `Entity::count`: the class, with no type (the type is count's).
  const he = await hover(main, 52, 10);
  check('hover on the qualifier Entity in Entity::count', /class `Entity`/.test(he) && !/here of type/.test(he), short(he));

  // Completion after `SystemUtil::` typed on a new line.
  await editor.edit((e) => e.insert(new vscode.Position(28, 0), '  SystemUtil::\n'));
  start = Date.now();
  let labels = [];
  await until('completion of SystemUtil::', async () =>
  {
    const list = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', uri(main), new vscode.Position(28, 14));
    labels = list.items.map((i) => (typeof i.label === 'string' ? i.label : i.label.label));
    return labels.includes('preventSleep');
  }, 60000);
  check('completion SystemUtil:: offers preventSleep', labels.includes('preventSleep'), `${Date.now() - start} ms, ${labels.length} items: ${labels.slice(0, 10).join(', ')}`);
  await vscode.commands.executeCommand('undo');
  await sleep(500);

  // config::debug's hover, then a switch to release and back.
  const dx = 'src/Graphics/DX11/VideoBitmapDX11.kpp';
  await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri(dx)));
  const h1 = await hover(dx, 68, 31);
  check('hover config::debug (factorio | debug)', /cell \*factorio, debug\*/.test(h1), short(h1));
  for (const [configuration, value] of [['release', 'false'], ['debug', 'true'], ['release', 'false']])
  {
    start = Date.now();
    await vscode.workspace.getConfiguration('kpp').update('configuration', configuration, vscode.ConfigurationTarget.Workspace);
    const t = await until(`the ${configuration} cell`, async () => { const x = await hover(dx, 68, 31); return new RegExp(`cell \\*factorio, ${configuration}\\*`).test(x) ? x : null; });
    check(`switch to factorio | ${configuration}`, !!t, `${((Date.now() - start) / 1000).toFixed(1)} s, ${short(t)}`);
    await sleep(1500);
    log(`     ${counts()}`);
  }
  await vscode.workspace.getConfiguration('kpp').update('configuration', 'debug', vscode.ConfigurationTarget.Workspace);
  await sleep(3000);
  fs.writeFileSync(out + '.screenshot', 'now');
  await sleep(6000);
  log(`done, ${failures} failed`);
}
exports.run = run;
