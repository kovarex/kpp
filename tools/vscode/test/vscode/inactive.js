// run.ps1 -Name inactive: the installed kpp extension in VS Code on a copy of compiler/tests/lsp/inactive
// (target app): the code the cell does not build is drawn faded (textDocument/inactiveRegions), in
// debug and after a switch to release.
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
async function until(what, f, ms = 60000)
{
  const end = Date.now() + ms;
  while (Date.now() < end) { const v = await f(); if (v) return v; await sleep(200); }
  log(`     (timed out waiting for ${what})`);
}

// The faded ranges' first and last lines (1-based), as `8-10`.
async function faded(p)
{
  const ranges = await vscode.commands.executeCommand('kpp.inactiveRanges', uri(p).toString());
  return ranges.map((r) => `${r.start.line + 1}-${r.end.line + 1}`).join(' ');
}

async function run()
{
  const ext = vscode.extensions.getExtension('kovarex.kpp');
  await until('activation', () => ext.isActive);
  check('extension active', ext.isActive);
  const main = 'src/main.kpp';
  await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri(main)));
  // debug: release's pick, the else branch and the local when on !config::debug, release's println.
  const debug = '8-10 25-26 38-38';
  let start = Date.now();
  let now = await until('the debug regions', async () => { const f = await faded(main); return f === debug ? f : null; });
  check('debug: release code faded', now === debug, `${await faded(main)} (expected ${debug}); ${((Date.now() - start) / 1000).toFixed(1)} s`);

  // release: the when global and member, debug's pick, the debug branches.
  const release = '2-2 4-6 15-15 20-23 36-36';
  start = Date.now();
  await vscode.workspace.getConfiguration('kpp').update('configuration', 'release', vscode.ConfigurationTarget.Workspace);
  now = await until('the release regions', async () => { const f = await faded(main); return f === release ? f : null; });
  check('release: debug code faded', now === release, `${await faded(main)} (expected ${release}); ${((Date.now() - start) / 1000).toFixed(1)} s`);

  fs.writeFileSync(out + '.screenshot', 'now');
  await sleep(6000);
  log(`done, ${failures} failed`);
}

exports.run = run;
