// Prints what a demo holds that the trainer needs: players, portals, shots,
// timer events.  node --experimental-strip-types tools/probe.ts <demo>...
import {readFileSync} from 'node:fs';
import {basename} from 'node:path';
import {loadRun} from '../../sar-web/src/core/load.ts';
import {decodeEntities} from '../../sar-web/src/net/entities.ts';

const f = (v: number[]) => v.map(x => x.toFixed(1)).join(' ');

for (const path of process.argv.slice(2)) {
	const run = await loadRun(new Uint8Array(readFileSync(path)), basename(path));
	console.log(`\n== ${run.fileName}  game=${run.game} dir=${run.gameDir} player=${run.player}`);
	for (const [k, v] of run.info) console.log(`  ${k}: ${v}`);
	for (const w of run.warnings) console.log(`  warning: ${w}`);
	for (const seg of run.segments) {
		console.log(`segment ${seg.index} ${seg.map} ticks ${seg.firstTick}..${seg.lastTick} dt=${seg.intervalPerTick} views=${seg.views.length} cmds=${seg.userCmds.length}`);
		const tl = decodeEntities(seg, (seg.dataTables ?? run.dataTables)!);
		console.log(`  entities ${tl.tracks.length}, errors ${tl.errors.length}; names ${[...tl.playerNames].map(([i, n]) => `${i}=${n}`).join(', ')}`);
		console.log(`  recording player: #${tl.player?.index}`);
		for (const t of tl.tracks.filter(t => t.className === 'CPortal_Player')) {
			const k0 = t.keys[0], k1 = t.keys[t.keys.length - 1];
			console.log(`  player #${t.index} model=${t.model} keys=${t.keys.length} ticks ${k0.tick}..${k1.tick} from ${f(k0.origin)} to ${f(k1.origin)}`);
		}
		for (const p of tl.tracks.filter(t => t.className === 'CProp_Portal')) {
			let last = '';
			for (const k of p.keys) {
				const s = `${k.active ? 'on ' : 'off'} ${f(k.origin)} ang ${f(k.angles)}`;
				if (s !== last) console.log(`  portal #${p.index} ${p.orange ? 'orange' : 'blue  '} team=${k.extra?.m_iTeamNum} @${k.tick}: ${s}`);
				last = s;
			}
		}
		for (const e of tl.effects.filter(e => e.name === 'PortalBlast' || e.name === 'PortalFX_Surface'))
			console.log(`  fx @${e.tick} ${e.name} color=${e.color} ent=${e.entity} from ${f(e.origin)} to ${f(e.start)} type=${e.damageType} team=${e.hitbox}`);
		for (const s of seg.sar) {
			if (s.kind === 'frametime' || s.kind === 'fileChecksum' || s.kind === 'vscriptChecksum' || s.kind === 'cvar') continue;
			console.log(`  sar @${s.tick} ${JSON.stringify(s)}`);
		}
		for (const c of seg.consoleCmds.filter(c => !c.initialValue)) console.log(`  cmd @${c.tick} ${c.text.slice(0, 110)}`);
		for (const h of tl.hudMessages) console.log(`  hud @${h.tick} ch${h.channel} ${h.text}`);
		for (const fd of tl.fades) console.log(`  fade @${fd.tick} dur=${fd.duration} hold=${fd.hold} flags=${fd.flags}`);
		const classes = [...tl.classCounts].sort((a, b) => b[1] - a[1]).map(([c, n]) => `${c}:${n}`).join(' ');
		console.log(`  classes: ${classes}`);
	}
}
