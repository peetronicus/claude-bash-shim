// End-to-end proof through Claude Code itself: a headless run executes one Bash
// tool call and this reports what the model sent and whether bash received it
// intact (exit 0) or with its backslash pairs collapsed (exit 1).
//
//   node test/verify.js                        whatever settings.json selects
//   node test/verify.js --bash <bash.exe>      that bash for this run only
//   node test/verify.js --claude <claude.exe>  a particular Claude Code binary
//
// The run uses your Claude Code login and spends one short model turn.
const { spawnSync } = require('child_process');
const path = require('path');

const args = process.argv.slice(2);
const option = (name) => (args.includes(name) ? args[args.indexOf(name) + 1] : null);

function findClaude() {
  const listed = spawnSync('where.exe', ['claude'], { encoding: 'utf8' }).stdout || '';
  return listed.split(/\r?\n/).find((line) => line.toLowerCase().endsWith('.exe')) || null;
}

const claude = option('--claude') || findClaude();
if (!claude) {
  console.log('No claude.exe on PATH; pass --claude <path to claude.exe>.');
  process.exit(2);
}
const bash = option('--bash');

// The child must not inherit this session's Claude Code state.
const env = {};
for (const [name, value] of Object.entries(process.env)) if (!/^CLAUDE/i.test(name)) env[name] = value;
const extra = bash ? ['--settings', JSON.stringify({ env: { CLAUDE_CODE_GIT_BASH_PATH: path.resolve(bash) } })] : [];

const command = "x='a\\\\b'; echo \"length ${#x}\"; cat <<'EOF'\nheredoc c\\\\d\nEOF";
const prompt = 'Call the Bash tool exactly once with exactly this command, character for character, then reply with the single word done.\n\n' + command;
const run = spawnSync(claude, ['-p', prompt, '--allowedTools', 'Bash', '--model', 'haiku', '--output-format', 'stream-json', '--verbose', ...extra],
  { encoding: 'utf8', env, cwd: __dirname, maxBuffer: 1 << 26, timeout: 180000 });
if (run.error) console.log('launch error:', run.error.message);

let sent = null;
let result = null;
for (const line of (run.stdout || '').split('\n')) {
  let entry;
  try { entry = JSON.parse(line); } catch { continue; }
  if (entry.type === 'system' && entry.subtype === 'init') console.log('claude code', entry.claude_code_version || entry.version || '(version not reported)');
  const content = entry.message && entry.message.content;
  if (!Array.isArray(content)) continue;
  for (const part of content) {
    if (part.type === 'tool_use' && part.name === 'Bash') sent = part.input.command;
    if (part.type === 'tool_result') result = typeof part.content === 'string' ? part.content : part.content.map((c) => c.text || '').join('');
  }
}
console.log('model sent the probe verbatim:', sent === command);
if (sent !== command) console.log('  sent:', JSON.stringify(sent));
console.log('bash printed:', JSON.stringify(result));
const intact = result !== null && result.includes('length 4') && result.includes('heredoc c\\\\d');
console.log(intact ? 'INTACT: backslash pairs reached bash' : 'COLLAPSED or not run');
if (!run.stdout) console.log('stderr:', (run.stderr || '').slice(0, 800));
process.exit(intact ? 0 : 1);
