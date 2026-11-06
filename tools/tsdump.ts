// Prints what the TypeScript parser (sar-web) reads from a demo, in the JSON
// layout of demodump, so that tools/compare.mjs can check the C++ port
// against it:
//   node --experimental-strip-types --no-warnings tools/tsdump.ts <file.dem>
// Exit code 1 and a message on stderr when the file can't be read as a demo.
//
// The TypeScript model keeps more than the C++ Demo in some places and less
// in others, so three things are worked out here rather than taken from it:
//   - which console commands are the cvar values written when recording
//     starts, and every console command's slot: the model keeps neither, so
//     a few lines here walk the frames for them (frameFacts);
//   - one player key per snapshot: the TypeScript tracks only hold changes,
//     so the snapshots' ticks come from a second walk over the packets;
//   - the view offset and velocity of players: read through EXTRA_PROPS,
//     and picked per player as the C++ does (see playerKeys).
import {readFileSync} from 'node:fs';
import {basename} from 'node:path';
import {isDem, readDem} from '../../sar-web/src/core/dem.ts';
import type {Segment} from '../../sar-web/src/core/model.ts';
import {decodeEntities, EXTRA_PROPS} from '../../sar-web/src/net/entities.ts';
import type {EntityKey, EntityTimeline, EntityTrack} from '../../sar-web/src/net/entities.ts';
import {parseDataTables} from '../../sar-web/src/net/datatables.ts';
import type {DataTables} from '../../sar-web/src/net/datatables.ts';
import {walkPacket} from '../../sar-web/src/net/messages.ts';
import {readStringTableSnapshot} from '../../sar-web/src/net/stringtables.ts';
import type {StringTable} from '../../sar-web/src/net/stringtables.ts';

type Vec3 = [number, number, number];

const VELOCITY = ['localdata.m_vecVelocity[0]', 'localdata.m_vecVelocity[1]', 'localdata.m_vecVelocity[2]'];
const VIEW_OFFSET_OWNER = ['portallocaldata.m_vecViewOffset', 'localdata.m_vecViewOffset'];
const VIEW_OFFSET_OTHERS = ['portalnonlocaldata.m_vecViewOffset[2]', 'm_vecViewOffset[2]'];

// The props the C++ reads by name that the viewer doesn't keep
EXTRA_PROPS.CPortal_Player.push('m_iTeamNum', ...VELOCITY, ...VIEW_OFFSET_OTHERS);
EXTRA_PROPS.CProp_Portal.push('m_iLinkageGroupID', 'm_hFiredByPlayer');

const num = (v: unknown, fallback = 0) => (typeof v === 'number' ? v : fallback);
// an EHANDLE's entity index, -1 for none
const handleIndex = (v: unknown) => {
	const h = num(v, -1);
	return h >= 0 && (h & 2047) !== 2047 ? h & 2047 : -1;
};

// ---- the frames: what the model drops ----

interface FrameFacts {
	consoleCmds: {slot: number; initialValue: boolean}[];  // one per console command frame
	cutShort?: {kind: string; tick: number};               // the file ends inside this frame's data
}

const looksLikeCvarValue = (text: string) => {
	const space = text.indexOf(' ');
	return space > 0 && text.length >= space + 3 && text[space + 1] === '"' && text.endsWith('"');
};

function frameFacts(bytes: Uint8Array): FrameFacts {
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const facts: FrameFacts = {consoleCmds: []};
	let pos = 1072;
	// the cvar values follow the sync tick, the callback table and the string tables
	let cvarValuesMayFollow = false;
	try {
		while (bytes.length - pos >= 6) {
			const type = bytes[pos], tick = view.getInt32(pos + 1, true), slot = bytes[pos + 5];
			pos += 6;
			const mayFollow: boolean = cvarValuesMayFollow;
			cvarValuesMayFollow = false;
			const payload = (kind: string) => {
				const size = view.getInt32(pos, true);
				if (size < 0) throw new RangeError('negative frame size');
				pos += 4;
				const data = bytes.subarray(pos, pos + size);
				if (size > bytes.length - pos) facts.cutShort = {kind, tick: Math.max(0, tick)};
				pos += size;
				return data;
			};
			if (type === 1 || type === 2) {
				pos += 160;
				payload('packet');
			} else if (type === 3) {
				cvarValuesMayFollow = true;
			} else if (type === 4) {
				const text = new TextDecoder().decode(payload('console')).replace(/\0+$/, '');
				const initialValue: boolean = mayFollow && looksLikeCvarValue(text);
				cvarValuesMayFollow = initialValue;
				facts.consoleCmds.push({slot, initialValue});
			} else if (type === 5) {
				pos += 4;
				payload('usercmd');
			} else if (type === 6) {
				payload('datatables');
			} else if (type === 9) {
				payload('snapshot');
				cvarValuesMayFollow = mayFollow;
			} else if (type === 8) {
				const callback = view.getInt32(pos, true);
				pos += 4;
				payload('custom');
				if (callback === -1) cvarValuesMayFollow = mayFollow;
			} else {
				break;
			}
		}
	} catch {
		// cut short inside a frame's fixed part
	}
	return facts;
}

// ---- entities ----

interface Snapshot {
	tick: number;
	isDelta: boolean;
}

// Every svc_PacketEntities the decoder is handed, in order (the same walk
// decodeEntities does, without decoding the entities).  Of a packet the file
// ends inside, only the bits that are there count: the C++ stops at the end
// of the data, where the TypeScript decoder reads zeros up to the packet's
// stated size (and can find a snapshot's header in them).
function snapshotsOf(seg: Segment, dt: DataTables): Snapshot[] {
	const snapshots: Snapshot[] = [];
	const tables: StringTable[] = [];
	for (const r of seg.net) {
		try {
			if (r.kind === 'packet') {
				const bits = Math.min(r.bits, r.data.length * 8);
				walkPacket(r.data, bits, {tables, classBits: dt.classBits, onPacketEntities: h => snapshots.push({tick: r.tick, isDelta: h.isDelta})});
			} else if (r.kind === 'snapshot') {
				const map = new Map(tables.map(t => [t.name, t]));
				readStringTableSnapshot(r.data, map);
				for (const t of map.values()) if (!tables.includes(t)) tables.push(t);
			}
		} catch {
			// decodeEntities reports it
		}
	}
	return snapshots;
}

// A full snapshot makes every entity anew, and the TypeScript decoder starts
// a new track for each.  The C++ carries on in the same track when it is the
// same entity (same index, class and serial): join those.
function joinTracks(tracks: EntityTrack[], snapshots: Snapshot[]): EntityTrack[] {
	const fullTicks = new Set(snapshots.filter(s => !s.isDelta).map(s => s.tick));
	const out: EntityTrack[] = [];
	const lastAt = new Map<number, EntityTrack>();
	for (const t of tracks) {
		const before = lastAt.get(t.index);
		const beforeEnd = before?.keys[before.keys.length - 1];
		if (before && beforeEnd && !beforeEnd.removed && t.keys.length && fullTicks.has(t.keys[0].tick) && before.className === t.className && before.serial === t.serial) {
			before.keys.push(...t.keys);
			before.model ||= t.model;
			before.orange = t.orange;
			continue;
		}
		const copy = {...t, keys: [...t.keys]};
		out.push(copy);
		lastAt.set(t.index, copy);
	}
	return out;
}

const sameVec = (a: Vec3, b: Vec3) => a[0] === b[0] && a[1] === b[1] && a[2] === b[2];

interface MotionKey {
	tick: number;
	origin: Vec3;
	angles: Vec3;
	[field: string]: unknown;
}

// The C++ rule for portals and movers: a key only when one of the listed
// fields changed; of two on one tick the later stays.
function changeKeys<K extends MotionKey>(keys: K[]): K[] {
	const same = (a: K, b: K) => Object.keys(a).every(f => f === 'tick' || (f === 'origin' || f === 'angles' ? sameVec(a[f], b[f]) : a[f] === b[f]));
	const out: K[] = [];
	for (const k of keys) {
		if (out.length && same(out[out.length - 1], k)) continue;
		if (out.length && out[out.length - 1].tick === k.tick) {
			out.pop();
			if (out.length && same(out[out.length - 1], k)) continue;
		}
		out.push(k);
	}
	return out;
}

// One key per snapshot that holds the player, and one (inSnapshot false)
// where it leaves the snapshot or is deleted.
function playerKeys(track: EntityTrack, snapshots: Snapshot[], isRecorder: boolean, cls: {index: Map<string, number>}) {
	const hasVelocity = isRecorder && VELOCITY.every(n => cls.index.has(n));
	const ownerOffset = VIEW_OFFSET_OWNER.some(n => cls.index.has(n));
	const othersOffset = VIEW_OFFSET_OTHERS.find(n => cls.index.has(n));
	const keyAt = (k: EntityKey, tick: number) => ({
		tick,
		origin: k.origin,
		eyePitch: k.angles[0],
		eyeYaw: k.angles[1],
		// (the viewer always takes the owner's copy, which only the recording
		// player's entity is kept up to date in; the C++ takes the other
		// copy for the other player)
		viewOffsetZ: ownerOffset && (isRecorder || !othersOffset) ? k.eyeZ ?? 0 : othersOffset ? num(k.extra?.[othersOffset]) : 0,
		flags: num(k.extra?.m_fFlags) | 0,
		hasVelocity,
		velocity: hasVelocity ? VELOCITY.map(n => num(k.extra?.[n])) : [0, 0, 0],
		team: num(k.extra?.m_iTeamNum),
		heldEntity: handleIndex(k.extra?.m_hAttachedObject),
		inSnapshot: !(k.dormant || k.removed),
	});
	const out: ReturnType<typeof keyAt>[] = [];
	const put = (k: EntityKey, tick: number) => {
		if (out.length && out[out.length - 1].tick === tick) out.pop();
		out.push(keyAt(k, tick));
	};
	let current = -1;
	for (const s of snapshots) {
		let changed = false;
		while (current + 1 < track.keys.length && track.keys[current + 1].tick === s.tick) {
			current++;
			changed = true;
		}
		if (current < 0) continue;
		const k = track.keys[current];
		if (!(k.dormant || k.removed) || changed) put(k, s.tick);
	}
	return out;
}

function entities(seg: Segment, tl: EntityTimeline, dt: DataTables) {
	const snapshots = snapshotsOf(seg, dt);
	const tracks = joinTracks(tl.tracks, snapshots).filter(t => t.keys.length);
	const classNamed = (name: string) => dt.classes.find(c => c.name === name);
	const recorder = tl.player?.index ?? 0;

	const players = tracks.filter(t => t.className === 'CPortal_Player').map(t => ({
		entIndex: t.index,
		name: tl.playerNames.get(t.index) ?? '',
		model: t.model,
		isRecorder: t.index === recorder,
		keys: playerKeys(t, snapshots, t.index === recorder, classNamed('CPortal_Player')!),
	})).filter(p => p.keys.length);

	const portals = tracks.filter(t => t.className === 'CProp_Portal').map(t => ({
		entIndex: t.index,
		isPortal2: !!t.orange,
		keys: changeKeys(t.keys.map(k => ({
			tick: k.tick,
			active: !!k.active,
			origin: k.origin,
			angles: k.angles,
			linkageGroup: num(k.extra?.m_iLinkageGroupID),
			team: num(k.extra?.m_iTeamNum),
			firedBy: handleIndex(k.extra?.m_hFiredByPlayer),
			linkedPortal: handleIndex(k.extra?.m_hLinkedPortal),
			inSnapshot: !(k.dormant || k.removed),
			removed: !!k.removed,
		}))),
	}));

	// everything else with a model that moved or turned at least once
	const hasModelIndex = new Set(dt.classes.filter(c => c.index.has('m_nModelIndex')).map(c => c.name));
	const movers = tracks.filter(t => t.className !== 'CPortal_Player' && t.className !== 'CProp_Portal' && t.className !== 'CRopeKeyframe' && hasModelIndex.has(t.className) && t.model).map(t => ({
		entIndex: t.index,
		className: t.className,
		model: t.model,
		keys: changeKeys(t.keys.map(k => ({
			tick: k.tick,
			origin: k.origin,
			angles: k.angles,
			parent: k.parent ?? -1,
			inSnapshot: !(k.dormant || k.removed),
			removed: !!k.removed,
			noDraw: !!k.nodraw,
		}))),
	})).filter(m => m.keys.some(k => !sameVec(k.origin, m.keys[0].origin) || !sameVec(k.angles, m.keys[0].angles)));

	const portalShots = tl.effects.filter(e => e.name === 'PortalFX_Surface').map(e => ({
		tick: e.tick, shooter: e.entity, team: e.hitbox, portalNum: e.color, result: e.damageType, origin: e.origin, angles: e.angles,
	}));

	return {recorderEntIndex: players.length ? recorder : 0, players, portals, movers, portalShots};
}

// ---- the whole demo ----

function dump(path: string) {
	const bytes = new Uint8Array(readFileSync(path));
	if (!isDem(bytes)) throw new Error('Not a Source demo (.dem).');
	const run = readDem(bytes, basename(path));
	const seg = run.segments[0];
	const facts = frameFacts(bytes);
	const info = new Map(run.info);
	const warnings = [...run.warnings];

	// console commands: without the initial cvar values, with their slots
	const framesMatch = facts.consoleCmds.length === seg.consoleCmds.length;
	if (!framesMatch) warnings.push('tsdump: the frame walk and the parser disagree on the console commands');
	const consoleCmds = seg.consoleCmds
		.map((c, i) => ({tick: c.tick, slot: framesMatch ? facts.consoleCmds[i].slot : null, text: c.text, initialValue: framesMatch && facts.consoleCmds[i].initialValue}))
		.filter(c => !c.initialValue)
		.map(({tick, slot, text}) => ({tick, slot, text}));

	const sar = seg.sar;
	const wrap32 = (n: number) => n | 0;
	const sarSpeedruns = sar.flatMap(e => {
		if (e.kind !== 'speedrunTime') return [];
		const splits = e.splits.map(s => ({name: s.name, ticks: wrap32(s.segments.reduce((n, g) => n + g.ticks, 0))}));
		const total = e.splits.reduce((n, s) => n + s.segments.reduce((m, g) => m + g.ticks, 0), 0);
		return [{tick: e.tick, complete: e.complete, totalTicks: wrap32(total), splits}];
	});

	// entities: decoded when every complete frame reads through.  (Class
	// baselines that don't decode and temp entities are not what the C++
	// reads; the frame a cut-short demo ends in is expected to fail.)
	let decoded: ReturnType<typeof entities> | undefined;
	const dataTables = seg.dataTables ?? run.dataTables;
	if (!dataTables) {
		warnings.push('no data tables');
	} else {
		try {
			const tl = decodeEntities(seg, dataTables);
			warnings.push(...tl.errors);
			const cut = facts.cutShort;
			const expected = (e: string) => e.startsWith('baseline for ') || e.startsWith('temp entities at tick ') || (!!cut && e.startsWith(`${cut.kind} at tick ${cut.tick}:`));
			if (tl.errors.every(expected)) decoded = entities(seg, tl, parseDataTables(dataTables));
		} catch (e) {
			warnings.push(`entities: ${(e as Error).message}`);
		}
	}

	return {
		header: {
			server: info.get('Server'),
			client: info.get('Client'),
			map: info.get('Map'),
			gameDir: info.get('Game'),
			demoProtocol: Number(info.get('Demo protocol')),
			networkProtocol: Number(info.get('Network protocol')),
			playbackTime: parseFloat(info.get('Header time')!),  // (to the millisecond: the model keeps it as text)
			playbackTicks: Number(info.get('Header ticks')),
			playbackFrames: Number(info.get('Frames')),
		},
		intervalPerTick: seg.intervalPerTick,
		firstTick: seg.firstTick,
		lastTick: seg.lastTick,
		recorderEntIndex: decoded?.recorderEntIndex ?? 0,
		views: seg.views.map(v => ({tick: v.tick, origin: v.origin, angles: v.angles})),
		userCmds: seg.userCmds,
		consoleCmds,
		hasSarData: sar.length > 0,
		// (a record that ends before its slot byte is no record)
		sarPortals: sar.flatMap(e => (e.kind === 'portal' && e.slot !== undefined ? [{tick: e.tick, slot: e.slot, orange: e.orange, pos: e.pos}] : [])),
		sarEntityInputs: sar.flatMap(e => (e.kind === 'entityInput' ? [{tick: e.tick, slot: e.slot ?? -1, target: e.target, className: e.className, input: e.input, param: e.param}] : [])),
		sarFlags: sar.flatMap(e => (e.kind === 'cmFlags' && e.slot !== undefined ? [{tick: e.tick, slot: e.slot}] : [])),
		sarSpeedruns,
		sarTimescales: sar.flatMap(e => (e.kind === 'timescale' ? [{tick: e.tick, timescale: e.timescale}] : [])),
		entitiesDecoded: !!decoded,
		players: decoded?.players ?? [],
		portals: decoded?.portals ?? [],
		movers: decoded?.movers ?? [],
		portalShots: decoded?.portalShots ?? [],
		warnings,
	};
}

const path = process.argv[2];
if (!path) {
	console.error('usage: node --experimental-strip-types tools/tsdump.ts <file.dem>');
	process.exitCode = 2;
} else {
	try {
		process.stdout.write(JSON.stringify(dump(path)) + '\n');
	} catch (e) {
		console.error(`tsdump: ${(e as Error).message}`);
		process.exitCode = 1;
	}
}
