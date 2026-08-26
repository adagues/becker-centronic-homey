'use strict';

/* Plain-node tests for BeckerProtocol — run: node test/protocol.test.js */

const assert = require('assert');
const P = require('../lib/BeckerProtocol');

// --- checksum ---
// checksum = (0x03 - sum(bytes)) & 0xFF; verify round trip
{
  const code = '0'.repeat(40);
  const out = P.checksum(code);
  assert.strictEqual(out.length, 42);
  assert.strictEqual(out.slice(40), '03'); // sum=0 -> 0x03
}
{
  // sum of one byte 0x03 -> checksum 0x00
  const code = '03' + '0'.repeat(38);
  assert.strictEqual(P.checksum(code).slice(40), '00');
}
{
  // wraps below zero: byte 0x04 -> 0x03-0x04 = 0xFF
  const code = '04' + '0'.repeat(38);
  assert.strictEqual(P.checksum(code).slice(40), 'FF');
}

// --- frame layout ---
{
  const frame = P.generateCode('1737c', 0, 2, P.COMMANDS.UP);
  assert.strictEqual(frame.length, 42);
  assert.strictEqual(frame.slice(0, 14), '0000000002010B'); // prefix
  assert.strictEqual(frame.slice(14, 18), '0000'); // increment
  assert.strictEqual(frame.slice(18, 24), '000000'); // suffix
  assert.strictEqual(frame.slice(24, 29), '1737C'); // unit
  assert.strictEqual(frame.slice(29, 32), '021');
  assert.strictEqual(frame.slice(32, 34), '01'); // remote (ch>0)
  assert.strictEqual(frame.slice(34, 36), '02'); // channel 2
  assert.strictEqual(frame.slice(36, 38), '00');
  assert.strictEqual(frame.slice(38, 40), '20'); // UP
  // verify checksum recomputes
  assert.strictEqual(P.checksum(frame.slice(0, 40)), frame);
}

// channel 0 uses remote code '00'
{
  const frame = P.generateCode('1737c', 5, 0, P.COMMANDS.HALT);
  assert.strictEqual(frame.slice(32, 34), '00');
  assert.strictEqual(frame.slice(14, 18), '0005');
}

// --- sequences & counter ---
{
  const { frames, nextIncrement } = P.train('abcde', 100, 1);
  assert.strictEqual(frames.length, 3);
  assert.strictEqual(nextIncrement, 103); // 3 commands consume 3 increments
}
{
  // counter wraps at 0xFFFF
  const { nextIncrement } = P.move('abcde', 0xFFFF, 1, 'UP');
  assert.strictEqual(nextIncrement, 0);
}
{
  const { frames } = P.remove('abcde', 0, 3);
  assert.strictEqual(frames.length, 5);
}

// --- bits ---
{
  const bits = P.frameToBits('FF00' + '0'.repeat(38));
  assert.strictEqual(bits.length, 21 * 8);
  assert.deepStrictEqual(bits.slice(0, 8), [1, 1, 1, 1, 1, 1, 1, 1]);
  assert.deepStrictEqual(bits.slice(8, 16), [0, 0, 0, 0, 0, 0, 0, 0]);
}

// --- unit id ---
{
  for (let i = 0; i < 100; i++) {
    assert.match(P.randomUnitId(), /^[0-9a-f]{5}$/);
  }
}

// --- manchester encoding ---
{
  // bit 0 -> half-bits (1,0): one word [short mark, short space] -> index 0
  assert.deepStrictEqual(P.manchesterEncode([0]), [0]);
}
{
  // bit 1 -> half-bits (0,1): prepend idle mark -> 1,0,1 ->
  // pulses (mark 1u)(space 1u)(mark 1u) -> words: [0, then trailing mark 0]
  assert.deepStrictEqual(P.manchesterEncode([1]), [0, 0]);
}
{
  // bits [0,0] -> 1,0,1,0 -> (m1)(s1)(m1)(s1) -> [0,0]
  assert.deepStrictEqual(P.manchesterEncode([0, 0]), [0, 0]);
}
{
  // bits [0,1] -> 1,0,0,1 -> (m1)(s2)(m1) -> word0: (0<<1|1)=1, word1: trailing mark -> 0
  assert.deepStrictEqual(P.manchesterEncode([0, 1]), [1, 0]);
}
{
  // bits [1,0] -> prepend -> 1,0,1,1,0 -> (m1)(s1)(m2)(s1) -> [0, 2]
  assert.deepStrictEqual(P.manchesterEncode([1, 0]), [0, 2]);
}
{
  // total half-bit units are conserved (+1 possible idle mark, +1 closing space)
  const bits = P.frameToBits('0000000002010B00000000001737C02101020020C1'.slice(0, 42));
  const words = P.manchesterEncode(bits);
  let units = 0;
  for (const w of words) units += ((w >> 1) & 1) + 1 + (w & 1) + 1;
  assert.ok(units === bits.length * 2 || units === bits.length * 2 + 1 || units === bits.length * 2 + 2);
  // all indices valid for the 4-word table
  assert.ok(words.every(w => w >= 0 && w <= 3));
}

// --- validation ---
assert.throws(() => P.generateCode('123', 0, 1, 0x20)); // bad unit
assert.throws(() => P.generateCode('1737c', 0, 9, 0x20)); // bad channel
assert.throws(() => P.checksum('1234')); // bad length
assert.throws(() => P.move('1737c', 0, 1, 'NOPE')); // bad command

console.log('All BeckerProtocol tests passed ✅');
