// run.ps1 -Name cells: the installed kpp extension in VS Code on a copy of compiler/tests/lsp/cells
// (target game, configuration debug): errors, completion, definition, hover, and the cells.
const vscode = require('vscode');
const fs = require('fs');
const path = require('path');

const out = process.env.KPP_HARNESS_LOG;
function log(line) { fs.appendFileSync(out, line + '\n'); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
function check(name, ok, detail) { log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`); if (!ok) failures++; }

const root = vscode.workspace.workspaceFolders[0].uri.fsPath;
const uri = (p) => vscode.Uri.file(path.join(root, p));

async function until(what, f, ms = 30000)
{
  const end = Date.now() + ms;
  while (Date.now() < end)
  {
    const v = await f();
    if (v) return v;
    await sleep(200);
  }
  log(`     (timed out waiting for ${what})`);
  return undefined;
}
const diags = (p) => vscode.languages.getDiagnostics(uri(p)).map((d) => `${d.range.start.line + 1}:${d.range.start.character + 1} ${d.message}`);
const all = () => vscode.languages.getDiagnostics().filter(([u, d]) => d.length).map(([u, d]) => `${path.relative(root, u.fsPath)}: ${d.map((x) => `${x.range.start.line + 1}:${x.range.start.character + 1} ${x.message}`).join('; ')}`);
const hoverText = (h) => (h || []).map((x) => x.contents.map((c) => c.value || c).join('\n')).join('\n---\n');
async function hover(p, line, ch) { return hoverText(await vscode.commands.executeCommand('vscode.executeHoverProvider', uri(p), new vscode.Position(line, ch))); }

async function pickCell(index)
{
  vscode.commands.executeCommand('kpp.selectCell');
  await sleep(1500);
  for (let i = 0; i < index; i++) { await vscode.commands.executeCommand('workbench.action.quickOpenSelectNext'); await sleep(100); }
  await vscode.commands.executeCommand('workbench.action.acceptSelectedQuickOpenItem');
  await sleep(500);
  const c = vscode.workspace.getConfiguration('kpp');
  return `${c.get('target')} | ${c.get('configuration')}`;
}

async function run()
{
  const ext = vscode.extensions.getExtension('kovarex.kpp');
  check('extension installed', !!ext, ext && ext.extensionPath);
  await until('activation', () => ext.isActive);
  check('extension active', ext.isActive);

  const game = 'src/game/main.kpp';
  const editor = await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri(game)));
  await sleep(4000);
  check('game, debug: no errors in game/main.kpp', diags(game).length === 0, JSON.stringify(diags(game)));

  // An error typed into the buffer, then taken back.
  await editor.edit((e) => e.insert(new vscode.Position(5, 0), '  undefinedName();\n'));
  const typed = await until('the typed error', () => diags(game).length ? diags(game) : null);
  check('typed error reported', !!typed, JSON.stringify(typed));
  await vscode.commands.executeCommand('undo');
  const cleared = await until('the error to clear', () => diags(game).length === 0);
  check('error cleared after undo', !!cleared, JSON.stringify(diags(game)));

  // Completion of `sha` -> shared.
  await editor.edit((e) => e.insert(new vscode.Position(5, 0), '  sha\n'));
  await sleep(1500);
  const list = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', uri(game), new vscode.Position(5, 5));
  const labels = list.items.map((i) => (typeof i.label === 'string' ? i.label : i.label.label));
  check('completion offers shared', labels.includes('shared'), `${labels.length} items, first: ${labels.slice(0, 8).join(', ')}`);
  await vscode.commands.executeCommand('undo');
  await until('the buffer to be saved-equal', () => !editor.document.isDirty, 5000);
  await sleep(1500);

  // Go to definition of shared() on `println(shared());` (line 6).
  const defs = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', uri(game), new vscode.Position(5, 12));
  const def = (defs || []).map((d) => `${path.relative(root, (d.uri || d.targetUri).fsPath)}:${(d.range || d.targetRange).start.line + 1}`);
  check('definition of shared', def.length === 1 && def[0].replace(/\\/g, '/') === 'src/common/shared.kpp:1', JSON.stringify(def));
  const h1 = await hover(game, 5, 12);
  check('hover on shared', /shared/.test(h1), JSON.stringify(h1));

  // Hover on config::sound::volume in the sound library.
  const sound = 'libraries/sound/src/sound.kpp';
  await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri(sound)));
  await sleep(1500);
  const h2 = await hover(sound, 0, 40);
  check('hover config::sound::volume in game|debug', /`7`/.test(h2) && /set by/.test(h2) && /default `5`/.test(h2), JSON.stringify(h2));

  // Switch: kpp.configuration = release (workspace setting) -> the static_assert fires.
  await vscode.window.showTextDocument(editor.document);
  await vscode.workspace.getConfiguration('kpp').update('configuration', 'release', vscode.ConfigurationTarget.Workspace);
  const rel = await until('the release error', () => diags(game).length ? diags(game) : null);
  check('settings switch to game|release shows the static_assert', !!rel && /debug only/.test(rel.join()), JSON.stringify(rel));

  // The status-bar picker: each index in turn, the picked cell's settings and game/main.kpp's errors.
  // kpp/cells' order, then every target per configuration: the game's release cells have its
  // static_assert, every target has two mains.
  const expected = [['game | debug', 0], ['game | release', 1], ['tool | debug', 0], ['tool | release', 0], ['null | debug', 1], ['null | release', 2]];
  for (const i of [0, 1, 2, 3, 4, 5, 0])
  {
    const picked = await pickCell(i);
    const errors = await until(`the errors of ${picked}`, () => (all().length === expected[i][1] ? all() : null), 30000);
    check(`picker item ${i}`, picked === expected[i][0] && !!errors, `${picked}: ${JSON.stringify(all())}`);
  }
  const h3 = await hover(sound, 0, 40);
  check('hover config::sound::volume back in game|debug', /cell \*game, debug\*/.test(h3), JSON.stringify(h3));

  fs.writeFileSync(out + '.screenshot', 'now');
  await sleep(6000);
  log(`done, ${failures} failed`);
}

exports.run = run;
