// Checks the C++ demo reader against the TypeScript one it was ported from:
// runs demodump and tools/tsdump.ts on every demo and compares their JSON.
//
//   node tools/compare.mjs [--demodump <exe>] [--tolerance 0.02] [--diffs 5] <demo.dem | folder>...
//
// One line per demo (MATCH or DIFF with the first differences), then a
// summary.  Exit code 1 when any demo differs.
//   - Decimal fields (coordinates, angles, speeds, times) may differ by the
//     tolerance (or a millionth of their size): the C++ keeps floats, the
//     TypeScript doubles, and its header time comes as text.  Everything
//     else must be equal.
//   - Warnings are not compared (the wording is each reader's own).
//   - A demo both refuse counts as a match.
import {spawnSync} from 'node:child_process';
import {existsSync, readdirSync, statSync} from 'node:fs';
import {basename, dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const DECIMAL_FIELDS = new Set([
	'origin', 'angles', 'pos', 'velocity', 'viewAngles', 'eyePitch', 'eyeYaw', 'viewOffsetZ', 'forwardMove', 'sideMove', 'upMove', 'timescale',
	'intervalPerTick', 'playbackTime',
]);
const NOT_COMPARED = new Set(['warnings']);

const options = {demodump: '', tolerance: 0.02, diffs: 5};
const inputs = [];
for (let i = 2; i < process.argv.length; i++) {
	const arg = process.argv[i];
	if (arg === '--demodump') options.demodump = process.argv[++i];
	else if (arg === '--tolerance') options.tolerance = Number(process.argv[++i]);
	else if (arg === '--diffs') options.diffs = Number(process.argv[++i]);
	else inputs.push(arg);
}
if (!options.demodump) {
	options.demodump = ['x86', 'x64'].map(arch => join(root, 'build', arch, 'tools', 'demodump', 'Release', 'demodump.exe')).find(existsSync) ?? '';
}
if (!inputs.length || !options.demodump) {
	console.error(options.demodump ? 'usage: node tools/compare.mjs [--demodump <exe>] [--tolerance 0.02] [--diffs 5] <demo.dem | folder>...' : 'compare: demodump.exe not found; build it or pass --demodump <exe>');
	process.exit(2);
}

const demos = inputs.flatMap(input =>
	statSync(input).isDirectory() ? readdirSync(input).filter(f => f.toLowerCase().endsWith('.dem')).sort().map(f => join(input, f)) : [input]);

// Runs a dumper: its parsed JSON, or the message it refused the file with
function run(command, args) {
	const result = spawnSync(command, args, {encoding: 'utf8', maxBuffer: 2 ** 31 - 1});
	if (result.error) return {crashed: `${basename(command)}: ${result.error.message}`};
	if (result.status === 1) return {rejected: result.stderr.trim().split('\n').pop()};
	if (result.status !== 0) return {crashed: `exit code ${result.status ?? result.signal}: ${result.stderr.trim().slice(-300)}`};
	try {
		return {demo: JSON.parse(result.stdout)};
	} catch (e) {
		return {crashed: `unreadable output (${e.message})`};
	}
}

// Walks both values; returns the differences as "path: C++ value / TS value"
function differences(cpp, ts, tolerance) {
	const found = [];
	let largest = 0;
	const show = v => {
		const text = JSON.stringify(v);
		return text === undefined ? 'missing' : text.length > 60 ? text.slice(0, 57) + '...' : text;
	};
	const walk = (a, b, path, decimal) => {
		if (b === null && a !== null) return;  // the TypeScript model doesn't have this
		if (Array.isArray(a) && Array.isArray(b)) {
			if (a.length !== b.length) found.push(`${path}: ${a.length} / ${b.length} entries`);
			for (let i = 0; i < Math.min(a.length, b.length); i++) walk(a[i], b[i], `${path}[${i}]`, decimal);
		} else if (a && b && typeof a === 'object' && typeof b === 'object') {
			for (const key of new Set([...Object.keys(a), ...Object.keys(b)])) {
				if (!NOT_COMPARED.has(key)) walk(a[key], b[key], path ? `${path}.${key}` : key, DECIMAL_FIELDS.has(key));
			}
		} else if (decimal && typeof a === 'number' && typeof b === 'number') {
			const d = Math.abs(a - b);
			if (d > Math.max(tolerance, 1e-6 * Math.max(Math.abs(a), Math.abs(b)))) found.push(`${path}: ${show(a)} / ${show(b)}`);
			// (the header time is text rounded to the millisecond on the TypeScript side: not a rounding of the port's)
			else if (d > largest && path !== 'header.playbackTime') largest = d;
		} else if (a !== b) {
			found.push(`${path}: ${show(a)} / ${show(b)}`);
		}
	};
	walk(cpp, ts, '', false);
	return {found, largest};
}

const keyCount = tracks => tracks.reduce((n, t) => n + t.keys.length, 0);
const kindOf = demo =>
	!demo.entitiesDecoded ? 'entities not decoded' : demo.players.length < 2 ? 'single player' : demo.header.server.startsWith('localhost') ? 'co-op host' : 'co-op client';

const totals = new Map();
const count = (kind, matched) => {
	const t = totals.get(kind) ?? totals.set(kind, {demos: 0, matched: 0}).get(kind);
	t.demos++;
	if (matched) t.matched++;
};
let differing = 0;

for (const path of demos) {
	const name = basename(path);
	const cpp = run(options.demodump, [path]);
	const ts = run(process.execPath, ['--experimental-strip-types', '--no-warnings', join(root, 'tools', 'tsdump.ts'), path]);
	if (cpp.crashed || ts.crashed) {
		console.log(`DIFF   ${name}: ${cpp.crashed ? `demodump failed (${cpp.crashed})` : `tsdump failed (${ts.crashed})`}`);
		count('failed to run', false);
		differing++;
		continue;
	}
	if (cpp.rejected || ts.rejected) {
		const both = cpp.rejected && ts.rejected;
		console.log(`${both ? 'MATCH ' : 'DIFF  '} ${name}: ${both ? 'both refuse it' : cpp.rejected ? 'only the C++ refuses it' : 'only the TypeScript refuses it'} (C++: ${cpp.rejected ?? 'reads it'}; TS: ${ts.rejected ?? 'reads it'})`);
		count('not a demo', both);
		if (!both) differing++;
		continue;
	}
	const {found, largest} = differences(cpp.demo, ts.demo, options.tolerance);
	const d = cpp.demo;
	const kind = kindOf(d);
	count(kind, !found.length);
	const cutShort = d.warnings.some(w => /cut short|no stop frame/.test(w)) ? ', cut short' : '';
	const contents = `${d.header.map}, ${kind}${cutShort}; ${d.views.length} views, ${d.userCmds.length} cmds, ${d.players.length} players/${keyCount(d.players)} keys, ` +
		`${d.portals.length} portals/${keyCount(d.portals)}, ${d.movers.length} movers/${keyCount(d.movers)}, ${d.portalShots.length} shots`;
	if (!found.length) {
		console.log(`MATCH  ${name} (${contents}; largest decimal difference ${largest.toExponential(1)})`);
	} else {
		differing++;
		console.log(`DIFF   ${name} (${contents}): ${found.length} differences, C++ / TypeScript`);
		for (const line of found.slice(0, options.diffs)) console.log(`         ${line}`);
	}
}

console.log(`\n${demos.length} demos: ${demos.length - differing} match, ${differing} differ`);
for (const [kind, t] of [...totals].sort()) console.log(`  ${kind}: ${t.matched} of ${t.demos} match`);
process.exit(differing ? 1 : 0);
