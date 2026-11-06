"""The language server's smoke test on any host (Linux's CI job; Windows has tests/lsp.ps1's sessions):
`kppc --lsp --stdio --check-each-change` on a program with an error, opened, then fixed: the server
answers initialize, publishes the error as a diagnostic at its place, then none once the file is
fixed, and ends on shutdown and exit.
    python3 compiler/tests/lsp_smoke.py [path/to/kppc]
"""
import json, os, subprocess, sys, tempfile, threading, queue

HERE = os.path.dirname(os.path.abspath(__file__))
KPPC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "kppc.exe" if os.name == "nt" else "kppc")


def main():
    work = tempfile.mkdtemp(prefix="kpp-lsp-")
    source = os.path.join(work, "main.kpp")
    broken = "int main()\n{\n  return undeclaredName;\n}\n"
    fixed = "int main()\n{\n  return 0;\n}\n"
    with open(source, "w", newline="\n") as f:
        f.write(broken)
    uri = "file://" + ("/" if os.name == "nt" else "") + source.replace("\\", "/")
    server = subprocess.Popen([KPPC, "--lsp", "--stdio", "--check-each-change"], cwd=work, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
    messages = queue.Queue()

    def reader():
        out = server.stdout
        while True:
            length = None
            while True:
                line = out.readline()
                if not line:
                    messages.put(None)
                    return
                line = line.strip()
                if not line:
                    break
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            messages.put(json.loads(out.read(length)))

    threading.Thread(target=reader, daemon=True).start()

    def send(message):
        body = json.dumps(message).encode()
        server.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        server.stdin.flush()

    def wait_for(test, what, seconds=120):
        while True:
            try:
                m = messages.get(timeout=seconds)
            except queue.Empty:
                fail(f"no {what} in {seconds} s")
            if m is None:
                fail(f"the server ended before {what}: {server.stderr.read().decode(errors='replace')[-2000:]}")
            if test(m):
                return m

    def fail(why):
        print("FAIL lsp_smoke:", why)
        server.kill()
        sys.exit(1)

    send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"processId": None, "rootUri": uri.rsplit("/", 1)[0], "capabilities": {}}})
    wait_for(lambda m: m.get("id") == 1 and "result" in m, "the answer to initialize")
    send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "kpp", "version": 1, "text": broken}}})
    published = lambda want: lambda m: m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"].lower() == uri.lower() and want(m["params"]["diagnostics"])
    errors = wait_for(published(lambda d: len(d) > 0), "the error's diagnostic")["params"]["diagnostics"]
    if not any("undeclaredName" in d["message"] and d["range"]["start"]["line"] == 2 for d in errors):
        fail(f"the diagnostics name no undeclaredName on line 3: {errors}")
    send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": fixed}]}})
    wait_for(published(lambda d: len(d) == 0), "no diagnostics once fixed")
    send({"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None})
    wait_for(lambda m: m.get("id") == 2, "the answer to shutdown")
    send({"jsonrpc": "2.0", "method": "exit", "params": None})
    try:
        code = server.wait(timeout=30)
    except subprocess.TimeoutExpired:
        fail("the server did not end after exit")
    print(f"ok   lsp_smoke: an error published at its place, none once fixed, ended with exit code {code}")


main()
