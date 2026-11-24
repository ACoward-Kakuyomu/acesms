const assert = require('node:assert/strict');
const fs = require('node:fs');
const crypto = require('node:crypto');
const { DatabaseSync } = require('node:sqlite');

const [name, phase, dbPath, baselinePath, romPath, jsonPath] = process.argv.slice(2);
const fmCase = /^fm-(emu2413|ymfm)-(sinc|zoh-low-pass|linear)$/.exec(name);
const machineId = fmCase ? 0 : ['sms', 'gg', 'sg1000'].indexOf(name);
assert.ok(machineId >= 0);
const ramBytes = !fmCase && machineId < 2 ? 32768 : 0;
const mapper = ramBytes ? 1 : 0;
const method = fmCase ? ['sinc', 'zoh-low-pass', 'linear'].indexOf(fmCase[2]) : 0;
const plugin = fmCase?.[1] === 'ymfm' ? 'acesms-fm-ymfm.dll' : '';
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const rom = fs.readFileSync(romPath);
const romHash = hash(rom);
const db = new DatabaseSync(dbPath, { readOnly: true });
const baseline = new DatabaseSync(baselinePath, { readOnly: true });
const one = (db, query) => Object.values(db.prepare(query).get());
const rows = (db, query) => db.prepare(query).all().map(Object.values);

assert.deepEqual(one(baseline, 'PRAGMA user_version'), [1]);
assert.deepEqual(one(db, 'PRAGMA user_version'), [2]);
assert.deepEqual(one(db, 'PRAGMA quick_check'), ['ok']);
assert.deepEqual(rows(db, 'PRAGMA foreign_key_check'), []);
assert.deepEqual(rows(db, 'SELECT lower(hex(sha256)), rom_bytes FROM roms'), [[romHash, 8192]]);
assert.deepEqual(rows(db,
  'SELECT machine, video, mapper, ram_bytes, fm_enabled, fm_plugin, interpolation FROM rom_settings'),
  [[machineId, 0, mapper, ramBytes, Number(!!fmCase), plugin, method]]);

function slot(database, number) {
  const row = database.prepare('SELECT * FROM states WHERE slot=?').get(number);
  assert.ok(row, `Missing slot ${number}`);
  const data = Buffer.from(row.data);
  assert.equal(data.subarray(0, 8).toString('ascii'), 'ACESMSST');
  assert.equal(data.readUInt16LE(8), 1);
  assert.deepEqual([...data.subarray(26, 30)], [machineId, 0, mapper, Number(!!fmCase)]);
  assert.equal(data.readUInt32LE(30), ramBytes);
  return { ...row, data, ticks: data.readBigUInt64LE(42) };
}

function confirmFm({ data, ticks }) {
  const [backendId, compatibilityId] = plugin
    ? ['616365736d732d796d666d0000000001', '796d666d2d6f706c6c2d763100000001']
    : ['616365736d732d656d75323431330001', '656d75323431332d6e37322d76310001'];
  assert.equal(data[85 + 0x123], 0x6d, 'ROM executed the FM key-on');
  const rate = data.readUInt32LE(34);
  const length = data.readUInt32LE(38);
  assert.ok(rate > 0 && length > 67);
  const fm = data.subarray(data.length - length);
  assert.equal(fm.subarray(0, 16).toString('hex'), backendId);
  assert.equal(fm.readUInt32LE(16), 1);
  assert.equal(fm.subarray(20, 36).toString('hex'), compatibilityId);
  assert.equal(fm.readUInt32LE(36), 3579545);
  assert.equal(fm.readUInt32LE(40), 72);
  assert.equal(fm.readBigUInt64LE(44), ticks);
  assert.equal(fm.readUInt32LE(52), Number(ticks % 72n));
  assert.equal(fm[56], method);
  assert.equal(fm.readUInt16LE(57), 1);
  const payloadSize = fm.readUInt32LE(59);
  assert.equal(payloadSize, plugin ? 327 : 3193);
  const samples = [33, 4, 3][method]; // ZOH includes the two filter delays.
  const resampler = fm.subarray(67 + payloadSize);
  assert.equal(resampler.length, 32 + samples * 8);
  assert.equal(fm.readUInt32LE(63), resampler.length);
  assert.equal(resampler.readBigUInt64LE(0), ticks / 72n);
  assert.equal(resampler.readBigUInt64LE(8), ticks * BigInt(rate) / 3579545n);
  const history = Array.from({ length: samples }, (_, i) => resampler.readDoubleLE(32 + i * 8));
  assert.ok(history.every(Number.isFinite));
  const rawCount = method === 1 ? 2 : samples;
  assert.ok(history.slice(0, rawCount).some(value => value !== 0), 'FM sample history');
  if (method === 1) assert.ok(history.slice(rawCount).some(value => value !== 0), 'low-pass filter delays');
  return rate;
}

function confirmCheats(enabled) {
  const cheat = { name: 'Compatibility fixture', address: 0xC123, width: 1, value: 90, mode: 'once' };
  assert.deepEqual(JSON.parse(fs.readFileSync(jsonPath, 'utf8')),
    { version: 1, machine: name, rom: { size: 8192, sha256: romHash }, cheats: [cheat] });
  const query = 'SELECT machine, name, address, width, value, enabled, always FROM cheats ORDER BY position';
  assert.deepEqual(rows(db, query),
    enabled === null ? [] : [[machineId, cheat.name, cheat.address, cheat.width, cheat.value, enabled, 0]]);
}

if (phase === 'reuse') {
  const original = slot(baseline, 5);
  assert.ok(original.ticks > 0n);
  assert.deepEqual(slot(db, 5), original);
  assert.deepEqual(slot(db, 7).data, original.data, 'immediate resave');
  if (fmCase) {
    const continued = slot(db, 8);
    assert.ok(continued.ticks > original.ticks);
    const rate = confirmFm(original);
    assert.equal(confirmFm(continued), rate);
    console.log(`${name}: slot 5 and slot 7 match, FM resumed at ${continued.ticks} T-state (${rate} Hz)`);
  } else {
    assert.deepEqual(one(db, 'SELECT aspect, machine, ram FROM app_settings'), [1, -1, 16384]);
    confirmCheats(1);
    assert.equal(original.data[85 + 0x123], 90);
    const boot = slot(db, 6);
    assert.equal(boot.ticks, 0n);
    const originalSram = rows(baseline, 'SELECT data FROM sram');
    assert.deepEqual(rows(db, 'SELECT data FROM sram'), originalSram);
    assert.equal(originalSram.length, ramBytes ? 1 : 0);
    if (ramBytes) {
      const sram = Buffer.from(originalSram[0][0]);
      assert.equal(sram.length, ramBytes);
      const start = 85 + 8192 + 3;
      assert.deepEqual(original.data.subarray(start, start + ramBytes), sram);
      assert.deepEqual(boot.data.subarray(start, start + ramBytes), sram);
    }
    console.log(`${name}: settings, enabled cheat, slot 5 and slot 7 match, SRAM bytes=${ramBytes}`);
  }
} else {
  assert.ok(!fmCase && ['before-save', 'imported'].includes(phase));
  confirmCheats(phase === 'before-save' ? null : 0);
  console.log(`${name}: JSON ${phase} verified`);
}
baseline.close();
db.close();
