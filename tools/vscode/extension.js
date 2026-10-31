// The Kpp extension (tools/vscode/README.md): the language, its grammar, and a language client
// that starts `kpp --lsp` (docs/editors.md, "The language server") for the workspace.
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;
let output;

// What the server is told at its start and on every change of the kpp.* settings: the build.kpp
// configuration, executable and settings it checks (docs/editors.md, "initializationOptions").
function options()
{
  const c = vscode.workspace.getConfiguration('kpp');
  return {
    configuration: c.get('configuration', 'debug'),
    executable: c.get('executable', null),
    settings: c.get('settings', {})
  };
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
