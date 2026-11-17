// Proof for the bash relay: launch it the way Claude Code launches bash (an
// argument vector from a native parent) and require every script to arrive intact.
//   node test/test.js       the relay built in dist
//   node test/test.js <bash.exe>   any other bash, to show what an unrelayed launch does
const { spawnSync, spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const bash = process.argv[2] || path.join(__dirname, '..', 'dist', 'bash.exe');
let failures = 0;

function check(label, actual, expected) {
  if (actual === expected) return;
  failures++;
  const at = [...actual].findIndex((c, i) => c !== expected[i]);
  console.log(`FAIL ${label}: lengths ${actual.length}/${expected.length}, first difference at ${at}`);
  console.log(`  got      ${JSON.stringify(actual.slice(Math.max(0, at - 20), at + 40))}`);
  console.log(`  expected ${JSON.stringify(expected.slice(Math.max(0, at - 20), at + 40))}`);
}

function sh(args, options = {}) {
  return spawnSync(bash, args, { encoding: 'utf8', maxBuffer: 1 << 26, ...options });
}

// The wrapper Claude Code puts around a command, as read from /proc/$$/cmdline.
function harness(command) {
  return "shopt -u extglob 2>/dev/null || true && eval '" + command.replace(/'/g, `'"'"'`) + "' && pwd -P >| /dev/null";
}

function heredoc(label, payload) {
  const r = sh(['-c', harness("cat <<'CLAUDE_EOF'\n" + payload + "\nCLAUDE_EOF")]);
  check(label, r.stdout + (r.stderr || ''), payload + '\n');
}

// 1. Backslashes in every position the reports name.
heredoc('backslash pairs', 'a\\\\b \\\\\\\\ c\\td \\$X \\" \\` \\n e\\\\"f" g\\\\\\"h\' tail\\\\');
heredoc('windows path literal', 'r"C:\\\\projects\\\\playground-engine\\\\tools\\\\bake\\\\bake.py"');
heredoc('regex', "re.compile(r'\\\\d+\\\\.\\\\d+|\\\\\\\\server\\\\share')");
check('single-quoted argument', sh(['-c', harness("printf '%s\\n' 'a\\\\b' \"c\\\\\\\\d\" e\\\\\\\\f")]).stdout, 'a\\\\b\nc\\\\d\ne\\\\f\n');

// 2. Every printable ASCII character next to every other, and non-ASCII text.
let pairs = '';
for (let a = 32; a < 127; a++) for (const b of ['\\', '"', "'", '$', '`', ' ']) pairs += String.fromCharCode(a) + b;
heredoc('ascii pairs', pairs);
heredoc('non-ascii', 'naïve — 日本語 ✓ \\\\ é');

// 3. Length: quoted heredocs from 1K up to what one Windows command line holds
// once the parent has escaped its quotes (32767 characters in all).
for (const size of [1000, 8000, 8200, 9000, 16000, 26000]) {
  let body = '';
  for (let i = 0; body.length < size; i++) body += `line ${i}: it's "quoted" \`ticked\` \\\\ $HOME ${'x'.repeat(i % 60)}\n`;
  heredoc(`length ${size}`, body.trimEnd());
}

// 4. A recorded command that failed in a session, when its text is supplied.
const recorded = process.env.RECORDED_COMMAND;
if (recorded && fs.existsSync(recorded)) {
  const lines = fs.readFileSync(recorded, 'utf8').split('\n');
  const body = lines.slice(1, lines.indexOf('EOF')).join('\n');
  const r = sh(['-c', harness("cat <<'EOF'\n" + body + "\nEOF\necho \"reached\\|end\"")]);
  check('recorded command', r.stdout + r.stderr, body + '\nreached\\|end\n');
}

// 5. What a relay must not change.
check('exit code', String(sh(['-c', 'exit 37']).status), '37');
check('stdin', sh(['-c', 'cat'], { input: 'from stdin\\\\\n' }).stdout, 'from stdin\\\\\n');
check('stderr', sh(['-c', 'echo oops >&2']).stderr, 'oops\n');
check('positional arguments', sh(['-c', 'printf "%s|" "$0" "$@"', 'name', 'a\\\\b c', '*', '', 'q"q', "it's", '~', '@x']).stdout, 'name|a\\\\b c|*||q"q|it\'s|~|@x|');
check('login and command flags', sh(['-l', '-c', 'printf %s "a\\\\b"']).stdout, 'a\\b');
check('clustered flags', sh(['-lc', "printf %s 'a\\\\b'"]).stdout, 'a\\\\b');
check('option with a value', sh(['-o', 'pipefail', '-c', "printf %s 'a\\\\b'; shopt -qo pipefail && echo ' on'"]).stdout, 'a\\\\b on\n');
check('relay variable is gone', sh(['-c', 'echo "[${CLAUDE_BASH_SHIM_SCRIPT-unset}]"; env | grep -c SHIM_SCRIPT']).stdout, '[unset]\n0\n');
check('working directory', sh(['-c', 'pwd -W'], { cwd: __dirname }).stdout.trim().toLowerCase(), __dirname.replace(/\\/g, '/').toLowerCase());
check('environment', sh(['-c', 'printf %s "$PROBE"'], { env: { ...process.env, PROBE: 'p\\\\q "r"' } }).stdout, 'p\\\\q "r"');
check('no command string', /GNU bash/.test(sh(['--version']).stdout) ? 'ok' : 'missing', 'ok');
const scriptFile = path.join(__dirname, 'probe-script.sh');
fs.writeFileSync(scriptFile, 'printf "%s|" file "$@"\n');
check('script file', sh([scriptFile, 'a\\\\b', 'c d']).stdout, 'file|a\\\\b|c d|');
fs.unlinkSync(scriptFile);
check('syntax error status', String(sh(['-c', "echo 'unterminated"]).status), '2');

// 6. Timing, and a tree kill reaching the real bash.
const started = process.hrtime.bigint();
for (let i = 0; i < 10; i++) sh(['-c', 'true']);
console.log(`mean launch ${(Number(process.hrtime.bigint() - started) / 1e7).toFixed(0)} ms`);

// The launched process, Git's bash.exe wrapper and the MSYS2 bash below it all
// end; a direct launch of Git's wrapper ends two of those.
const child = spawn(bash, ['-c', 'sleep 20; true'], { stdio: 'ignore' });
setTimeout(() => {
  const kill = spawnSync('taskkill', ['/pid', String(child.pid), '/T', '/F'], { encoding: 'utf8' });
  const ended = (kill.stdout.match(/SUCCESS/g) || []).length;
  check('tree kill reaches the real bash', String(ended >= 2), 'true');
  console.log(failures ? `${failures} FAILED` : 'all passed');
  process.exit(failures ? 1 : 0);
}, 1500);
