// The Kpp extension (tools/vscode/README.md): the language, its grammar, and a language client
// that starts `kpp --lsp` (docs/editors.md, "The language server") for the workspace.
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;
let output;
let status; // the cell checked (docs/editors.md, "Cells"); a click picks another

// What the server is told at its start: the build.kpp cell (target and configuration) and settings
// it checks (docs/editors.md, "initializationOptions"). Changes of the kpp.* settings reach it as
// workspace/didChangeConfiguration with the whole kpp section.
function options()
{
  const c = vscode.workspace.getConfiguration('kpp');
  return {
    configuration: c.get('configuration', 'debug'),
    target: c.get('target', null) || c.get('executable', null),
    settings: c.get('settings', {})
  };
}

function showCell(cell)
{
  status.text = `$(gear) ${cell.target || 'every target'} | ${cell.configuration}`;
  status.tooltip = 'Kpp: the target and configuration IntelliSense checks the code in. Click to change.';
  status.show();
}

// kpp.selectCell: build.kpp's cells (kpp/cells) in a quick pick; the one picked becomes the
// workspace's kpp.target and kpp.configuration, which the server switches to.
async function selectCell()
{
  if (!client)
  {
    vscode.window.showWarningMessage('Kpp: the language server is not running.');
    return;
  }
  const answer = await client.sendRequest('kpp/cells', {});
  if (!answer || !answer.cells || answer.cells.length === 0)
  {
    vscode.window.showInformationMessage('Kpp: there is no build.kpp, so there are no targets and configurations to pick.');
    return;
  }
  const items = [];
  const configurations = [];
  for (const cell of answer.cells)
  {
    if (!configurations.includes(cell.configuration))
      configurations.push(cell.configuration);
    items.push({ label: cell.target, description: cell.configuration, cell });
  }
  for (const configuration of configurations)
    items.push({ label: 'every target', description: configuration, cell: { target: null, configuration } });
  const active = answer.active || {};
  for (const item of items)
    item.picked = item.cell.target === (active.target || null) && item.cell.configuration === active.configuration;
  const picked = await vscode.window.showQuickPick(items, { placeHolder: 'The target and configuration to check the code in' });
  if (!picked)
    return;
  const c = vscode.workspace.getConfiguration('kpp');
  if (vscode.workspace.workspaceFolders)
  {
    if (c.inspect('executable').workspaceValue !== undefined)
      await c.update('executable', undefined, vscode.ConfigurationTarget.Workspace);
    await c.update('target', picked.cell.target, vscode.ConfigurationTarget.Workspace);
    await c.update('configuration', picked.cell.configuration, vscode.ConfigurationTarget.Workspace);
  }
  else
    await client.sendRequest('kpp/selectCell', picked.cell);
}

function serverCommand()
{
  const path = vscode.workspace.getConfiguration('kpp').get('path', '');
  return path || 'kpp';
}

async function start(context)
{
  const command = serverCommand();
  const server = { command, args: ['--lsp'], transport: TransportKind.stdio };
  client = new LanguageClient('kpp', 'Kpp', { run: server, debug: server }, {
    documentSelector: [{ scheme: 'file', language: 'kpp' }],
    initializationOptions: options(),
    outputChannel: output,
    synchronize: {
      configurationSection: 'kpp',
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.kpp')
    }
  });
  try
  {
    await client.start();
    client.onNotification('kpp/activeCell', showCell);
  }
  catch (error)
  {
    const open = 'Settings';
    const choice = await vscode.window.showErrorMessage(
      `Kpp: the language server did not start (${command} --lsp): ${error.message || error}. ` +
      'Install Kpp, or set kpp.path to kpp.exe.', open);
    if (choice === open)
      vscode.commands.executeCommand('workbench.action.openSettings', 'kpp.path');
    client = undefined;
  }
}

async function stop()
{
  if (client)
  {
    const running = client;
    client = undefined;
    await running.stop();
  }
}

async function activate(context)
{
  output = vscode.window.createOutputChannel('Kpp');
  context.subscriptions.push(output);
  context.subscriptions.push(vscode.commands.registerCommand('kpp.restartServer', async () =>
  {
    await stop();
    await start(context);
  }));
  context.subscriptions.push(vscode.commands.registerCommand('kpp.showOutput', () => output.show()));
  context.subscriptions.push(vscode.commands.registerCommand('kpp.selectCell', selectCell));
  status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left);
  status.command = 'kpp.selectCell';
  context.subscriptions.push(status);
  // kpp.path names another server: start that one. The other kpp.* settings reach the running
  // server as workspace/didChangeConfiguration (synchronize.configurationSection).
  context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(async (event) =>
  {
    if (event.affectsConfiguration('kpp.path'))
    {
      await stop();
      await start(context);
    }
  }));
  await start(context);
}

function deactivate()
{
  return stop();
}

module.exports = { activate, deactivate };
