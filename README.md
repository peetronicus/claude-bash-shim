# claude-bash-shim

A small `bash.exe` relay that stops Claude Code on Windows from corrupting Bash
tool commands on their way to Git Bash.

Unofficial; not affiliated with Anthropic or Git for Windows.

## The problem

On Windows, Claude Code runs Bash tool commands through Git Bash. Two things go
wrong silently, whatever quoting the command uses:

- Every backslash pair arrives as one backslash. `'a\\b'` reaches bash as
  `a\b`, a quoted heredoc containing `"C:\\projects\\tools"` writes
  `C:\projects\tools`, and a Python heredoc then reads `\t` as a tab.
- A command longer than about 8,000 characters is cut. Bash reports something
  like `unexpected EOF while looking for matching '` for a script that is
  correct.

Agents respond by retrying, writing helper scripts or falling back to file
edits, and sometimes a file is written with the wrong content first.

## The cause

Claude Code is a native Windows process, and it starts Git Bash as
`bash.exe -c "<script>"`. Windows passes a child one command-line string, so the
script is encoded into it by the Microsoft C runtime rules. Git Bash is an MSYS2
program, and when its parent is not an MSYS2 process the MSYS2 runtime decodes
the string by different rules: inside a quoted argument it reads every backslash
pair as one backslash, and it cuts a quoted argument near 8K characters. The
loss happens before bash parses anything, which is why single quotes and quoted
heredocs do not help.

The same happens to any native program that starts `bash.exe -c` with an
argument vector, such as Node's `child_process.spawn` or Python's `subprocess`.

## The fix

`bash.exe` in this project is launched in place of Git Bash. It decodes its own
command line by the Microsoft rules, hands the `-c` script to the real bash in
an environment variable, which nothing re-decodes, and passes everything else
through: standard handles, working directory, environment, other arguments and
the exit code. The variable is removed before the script runs.

Claude Code is pointed at the relay with its documented
`CLAUDE_CODE_GIT_BASH_PATH` setting. Nothing in Git for Windows is modified.

## Install

Requires Git for Windows, and LLVM or Visual Studio build tools to compile (or
take `bash.exe` from a release and place it in `dist\`).

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
```

This builds `dist\bash.exe` if needed, copies it to
`%USERPROFILE%\.claude\tools\bash-shim\bash.exe`, and edits
`%USERPROFILE%\.claude\settings.json`:

- `env.CLAUDE_CODE_GIT_BASH_PATH` is set to the relay.
- If that setting already named a custom bash, it moves to
  `env.CLAUDE_BASH_SHIM_TARGET` so the relay keeps using it.
- A `SessionStart` hook is added. It is silent while backslash pairs arrive
  intact and otherwise tells the agent, in that session, to keep such content
  out of Bash commands and to tell you.

The settings file as it was before the first install is kept as
`settings.json.before-bash-shim`. Start a new Claude Code session afterwards;
sessions already running keep the bash they started with.

To do it by hand instead, copy `bash.exe` anywhere and add to `settings.json`:

```json
{
  "env": {
    "CLAUDE_CODE_GIT_BASH_PATH": "C:\\path\\to\\bash.exe"
  }
}
```

## Check that it works

In a new session, ask the agent to run this with its Bash tool:

```bash
x='a\\b'; echo "length ${#x}"
```

`length 4` means commands arrive intact; `length 3` means the relay is not in
effect.

Two scripts prove more (both need Node):

- `node test/test.js` launches the relay the way Claude Code does and requires
  backslashes, quoting, non-ASCII text, heredocs up to 26K characters, exit
  codes, standard streams and arguments to arrive unchanged. Give it another
  `bash.exe` as an argument to see the same checks fail without the relay.
- `node test/verify.js` runs one Bash tool call through a headless Claude Code
  and reports whether it arrived intact. It spends one short model turn.

## Remove

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
```

## Details and limits

- The real bash is found in this order: `CLAUDE_BASH_SHIM_TARGET`; the Git for
  Windows installation named in the registry (per user, then per machine); the
  default installation folders; the installation that owns the `git.exe` on
  `PATH`.
- One Windows command line holds 32,767 characters, so a single command is
  still limited to roughly 32K. That limit is in the parent's launch, before
  the relay runs.
- The script runs through `eval`, so bash prefixes its own error messages with
  `eval:` and `$LINENO` counts from the start of the script.
- Arguments other than the `-c` script are re-encoded for MSYS2 and keep their
  backslashes, but one such argument is still cut near 8K characters.
- The relay adds one process start, around 10 ms, to each launch.
- If a later Claude Code release changes how it passes commands to bash, the
  session-start check reports it. Run `node test/verify.js --bash` with your
  Git Bash path; if that reports `INTACT`, the relay is no longer needed and
  can be uninstalled.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Uses `clang` when present, otherwise `cl` from a Visual Studio developer
prompt. The output is one statically linked `dist\bash.exe`.

## License

MIT. See [LICENSE](LICENSE).
