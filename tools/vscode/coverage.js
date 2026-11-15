// kpp's test coverage in VS Code (docs/design.md 28, docs/editors.md "Coverage"): a coverage file that
// `kpp --coverage` wrote (one JSON line per source file) shown through VS Code's own test coverage:
// the gutter's marks, the statements' and conditions' parts of a line, the Explorer's percentages,
// "Test: Toggle Inline Coverage" to hide and show them, "Test: Clear Coverage" to close them.
// - "Run Tests with Coverage" in the Testing view runs kpp.coverageCommand in the workspace folder and
//   shows the file it writes (kpp.coverageFile);
// - "kpp: Show a Coverage File" shows a file written elsewhere (a game's tests.cmd, a CI run).
// A source changed since the run (its hash is not the file's) is left out, and the message says so.
const vscode = require('vscode');
const fs = require('fs');
const path = require('path');
const cp = require('child_process');

let controller;
let profile;
const details = new Map(); // document URI -> its statements and declarations, for loadDetailedCoverage

// The hash of the file's bytes kpp writes (16 hex digits): 64-bit FNV-1a, with the offset kpp's
// hashes all use, 1469598103934665603 (FNV's own is 14695981039346656037).
function sourceHash(bytes)
{
  let h = 1469598103934665603n;
  const prime = 0x100000001b3n;
  const mask = 0xffffffffffffffffn;
  for (const b of bytes)
    h = ((h ^ BigInt(b)) * prime) & mask;
  return h.toString(16).padStart(16, '0');
}

function range(p)
{
  return new vscode.Range(p[0] - 1, p[1] - 1, p[2] - 1, p[3] - 1);
}

// A file's line of the coverage file as VS Code's statements, each condition's true and false a
// branch of the innermost statement holding it (or a statement of its own), each missing default a
// branch of its switch, and the functions.
function fileDetails(file)
{
  const statements = file.statements.map((s) => ({ place: s, coverage: new vscode.StatementCoverage(s[4], range(s), []) }));
  const holder = (r) =>
  {
    let best;
    for (const s of statements)
      if (s.coverage.location.contains(r) && (!best || best.coverage.location.contains(s.coverage.location)))
        best = s;
    return best;
  };
  const out = statements.map((s) => s.coverage);
  for (const c of file.conditions)
  {
    const where = range(c);
    const branches = [new vscode.BranchCoverage(c[4], where, 'true'), new vscode.BranchCoverage(c[5], where, 'false')];
    const s = holder(where);
    if (s)
      s.coverage.branches.push(...branches);
    else
      out.push(new vscode.StatementCoverage(c[4] || c[5] ? 1 : 0, where, branches));
  }
  for (const d of file.defaults)
  {
    const where = new vscode.Range(d[0] - 1, d[1] - 1, d[0] - 1, d[1] - 1 + 'switch'.length);
    const s = holder(where);
    const branch = new vscode.BranchCoverage(d[2], where, 'default (no case)');
    if (s)
      s.coverage.branches.push(branch);
    else
      out.push(new vscode.StatementCoverage(d[2], where, [branch]));
  }
  for (const f of file.functions)
    out.push(new vscode.DeclarationCoverage(f[0], f[2], new vscode.Position(f[1] - 1, 0)));
  return out;
}

// Where a coverage file's path is: absolute as it is, else from the coverage file's folder, else
// from each workspace folder.
function sourcePath(name, coverageFile)
{
  if (path.isAbsolute(name))
    return name;
  const candidates = [path.resolve(path.dirname(coverageFile), name)];
  for (const folder of vscode.workspace.workspaceFolders || [])
    candidates.push(path.resolve(folder.uri.fsPath, name));
  return candidates.find((c) => fs.existsSync(c)) || candidates[0];
}

function show(coverageFile, output)
{
  let text;
  try
  {
    text = fs.readFileSync(coverageFile, 'utf8');
  }
  catch (error)
  {
    vscode.window.showErrorMessage(`kpp: ${coverageFile} cannot be read: ${error.message}`);
    return;
  }
  const run = controller.createTestRun(new vscode.TestRunRequest(undefined, undefined, profile), `kpp coverage: ${path.basename(coverageFile)}`, false);
  details.clear();
  let shown = 0;
  const stale = [];
  for (const line of text.split(/\r?\n/))
  {
    if (!line.trim())
      continue;
    const file = JSON.parse(line);
    if (!file.file)
      continue;
    const where = sourcePath(file.file, coverageFile);
    let bytes;
    try
    {
      bytes = fs.readFileSync(where);
    }
    catch
    {
      stale.push(file.file);
      continue;
    }
    if (sourceHash(bytes) !== file.hash)
    {
      stale.push(file.file); // (the lines would be another version's)
      continue;
    }
    const uri = vscode.Uri.file(where);
    const items = fileDetails(file);
    details.set(uri.toString(), items);
    run.addCoverage(vscode.FileCoverage.fromDetails(uri, items));
    ++shown;
  }
  run.end();
  output.appendLine(`coverage: ${coverageFile}, ${shown} file(s) shown` + (stale.length ? `, left out as changed since the run: ${stale.join(', ')}` : ''));
  if (stale.length)
    vscode.window.showWarningMessage(`kpp: ${stale.length} file(s) changed since the coverage run, their coverage is not shown: ${stale.slice(0, 5).join(', ')}` +
                                     (stale.length > 5 ? ', ...' : ''));
}

function coverageFile(folder)
{
  const name = vscode.workspace.getConfiguration('kpp').get('coverageFile', 'kpp-coverage.json');
  return path.isAbsolute(name) ? name : path.join(folder, name);
}

function register(context, output)
{
  if (!vscode.tests || !vscode.FileCoverage)
    return; // (a VS Code older than 1.88 has no test coverage)
  controller = vscode.tests.createTestController('kpp', 'kpp');
  context.subscriptions.push(controller);
  // "Run Tests with Coverage": kpp.coverageCommand in the first workspace folder, then its file.
  profile = controller.createRunProfile('kpp tests with coverage', vscode.TestRunProfileKind.Coverage, async (request, token) =>
  {
    const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (!folder)
      return;
    const command = vscode.workspace.getConfiguration('kpp').get('coverageCommand', 'kpp --coverage --test . -- --coverage kpp-coverage.json');
    output.appendLine(`coverage: ${command}`);
    const run = controller.createTestRun(request, 'kpp tests', false);
    await new Promise((resolve) =>
    {
      const child = cp.exec(command, { cwd: folder.uri.fsPath, maxBuffer: 256 * 1024 * 1024 }, (error, stdout, stderr) =>
      {
        run.appendOutput((stdout + stderr).replace(/\r?\n/g, '\r\n'));
        resolve();
      });
      token.onCancellationRequested(() => child.kill());
    });
    run.end();
    show(coverageFile(folder.uri.fsPath), output);
  }, true);
  profile.loadDetailedCoverage = async (run, fileCoverage) => details.get(fileCoverage.uri.toString()) || [];
  context.subscriptions.push(vscode.commands.registerCommand('kpp.showCoverage', async (file) =>
  {
    let chosen = typeof file === 'string' ? file : undefined;
    if (!chosen)
    {
      const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
      const picked = await vscode.window.showOpenDialog({
        canSelectMany: false,
        defaultUri: folder ? vscode.Uri.file(coverageFile(folder.uri.fsPath)) : undefined,
        filters: { 'kpp coverage': ['json'] },
        openLabel: 'Show its coverage'
      });
      if (!picked || picked.length === 0)
        return;
      chosen = picked[0].fsPath;
    }
    show(chosen, output);
  }));
  // (for tests: what a document's coverage is, as shown)
  context.subscriptions.push(vscode.commands.registerCommand('kpp.coverageOf', (uri) => details.get(vscode.Uri.parse(String(uri)).toString()) || []));
}

module.exports = { register, sourceHash, fileDetails };
