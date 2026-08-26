'use strict';

/**
 * Becker Centronic protocol — JavaScript port of ole1986/centronic-py.
 *
 * Frame layout (40 hex chars + 2-char checksum = 42 chars / 21 bytes):
 *
 *   0000000002010B  IIII  000000  UUUUU  021  RR  CC  00  MM  KK
 *   |-- prefix --|  incr  suffix  unit        rem  ch      cmd  checksum
 *
 *   incr : 16-bit rolling counter (hex4). Receiver accepts next counter
 *          within +1..+49 of the last seen value. Sending the SAME command
 *          3x in a row force-resyncs the receiver to that counter.
 *   unit : 20-bit sender identity (hex5) — our virtual "remote".
 *   rem  : '01' for CentralControl-style sender, '00' when channel==0.
 *   ch   : channel 0x0-0x7, 0xF = all channels of the unit.
 *   cmd  : see COMMANDS.
 *   checksum : (0x03 - sum(bytes)) & 0xFF
 */

const CODE_PREFIX = '0000000002010B';
const CODE_SUFFIX = '000000';
const CODE_21 = '021';
const CODE_REMOTE = '01';

const COMMANDS = {
  RELEASE: 0x00,
  HALT: 0x10,
  UP: 0x20,
  UP_INTERMEDIATE: 0x24, // intermediate "up" position
  DOWN: 0x40,
  DOWN_INTERMEDIATE: 0x44, // intermediate "down" / sun-protection position
  PAIR: 0x80, // pair button press
  PAIR_3S: 0x81, // pair button held 3s
  PAIR_6S: 0x82, // pair button held 6s
  PAIR_9S: 0x83, // pair button held 9s
  CLEARPOS: 0x90,
  CLEARPOS2: 0x91,
  CLEARPOS3: 0x92,
  CLEARPOS4: 0x93,
};

const COUNTER_MAX = 0xFFFF;

function hex2(n) {
  return (n & 0xFF).toString(16).toUpperCase().padStart(2, '0');
}

function hex4(n) {
  return (n & 0xFFFF).toString(16).toUpperCase().padStart(4, '0');
}

/**
 * Append the Becker checksum to a 40-char hex code.
 * checksum = (0x03 - sum(all 20 bytes)) & 0xFF
 */
function checksum(code) {
  if (code.length !== 40) {
    throw new Error(`Code must be 40 hex chars, got ${code.length}`);
  }
  let sum = 0;
  for (let i = 0; i < 40; i += 2) {
    sum += parseInt(code.substr(i, 2), 16);
  }
  return code.toUpperCase() + hex2(0x03 - sum);
}

/**
 * Build one complete 42-char frame.
 * @param {string} unitId - 5 hex chars identifying this virtual remote
 * @param {number} increment - current rolling counter (0..0xFFFF)
 * @param {number} channel - 0-7, or 15 (0xF) for all
 * @param {number} command - one of COMMANDS
 */
function generateCode(unitId, increment, channel, command) {
  if (!/^[0-9a-fA-F]{5}$/.test(unitId)) {
    throw new Error(`unitId must be 5 hex chars, got "${unitId}"`);
  }
  if (channel < 0 || (channel > 7 && channel !== 15)) {
    throw new Error(`channel must be 0-7 or 15, got ${channel}`);
  }
  const remote = channel === 0 ? '00' : CODE_REMOTE;
  const code = CODE_PREFIX + hex4(increment) + CODE_SUFFIX
    + unitId.toLowerCase() + CODE_21 + remote + hex2(channel) + '00' + hex2(command);
  return checksum(code);
}

/**
 * Command sequences. Each entry consumes one counter increment.
 * Returns { frames: string[], nextIncrement: number }.
 */
function buildSequence(unitId, increment, channel, commandNames) {
  const frames = [];
  let inc = increment;
  for (const name of commandNames) {
    if (!(name in COMMANDS)) throw new Error(`Unknown command "${name}"`);
    frames.push(generateCode(unitId, inc, channel, COMMANDS[name]));
    inc = (inc + 1) & COUNTER_MAX;
  }
  return { frames, nextIncrement: inc };
}

/** Single movement command (UP / DOWN / HALT / intermediates). */
function move(unitId, increment, channel, commandName) {
  return buildSequence(unitId, increment, channel, [commandName]);
}

/** TRAIN: registers this unit+channel on a receiver that is in pairing mode. */
function train(unitId, increment, channel) {
  return buildSequence(unitId, increment, channel, ['PAIR_3S', 'RELEASE', 'PAIR_3S']);
}

/** REMOVE: unregisters this unit+channel from the receiver. */
function remove(unitId, increment, channel) {
  return buildSequence(unitId, increment, channel, ['PAIR_3S', 'RELEASE', 'PAIR_3S', 'PAIR_6S', 'PAIR_9S']);
}

/** Convert a 42-char hex frame to a flat bit array (MSB first) for RF tx. */
function frameToBits(frame) {
  const bits = [];
  for (let i = 0; i < frame.length; i += 2) {
    const byte = parseInt(frame.substr(i, 2), 16);
    for (let b = 7; b >= 0; b--) bits.push((byte >> b) & 1);
  }
  return bits;
}

/**
 * Manchester-encode a bit array into Homey signal WORD INDICES.
 *
 * Becker Centronic is Manchester-coded (half-bit ~417 us). Each data bit is
 * two half-bit levels: 0 -> (high, low), 1 -> (low, high). Adjacent equal
 * half-bits merge into one double-length pulse, so the pulse stream contains
 * only 1-unit and 2-unit marks/spaces. Homey words must start with a mark,
 * therefore the word table has 4 entries:
 *
 *   index 0: [417, 417]   short mark, short space
 *   index 1: [417, 834]   short mark, long space
 *   index 2: [834, 417]   long mark, short space
 *   index 3: [834, 834]   long mark, long space
 *
 * If the stream would start with a space, one idle mark half-bit is
 * prepended (receivers tolerate a leading carrier pulse).
 */
function manchesterEncode(bits) {
  // 1. expand bits into half-bit levels
  const half = [];
  for (const b of bits) {
    if (b) half.push(0, 1);
    else half.push(1, 0);
  }
  if (half[0] === 0) half.unshift(1); // words must start with a mark
  // 2. run-length encode (runs are 1 or 2 half-bits by Manchester property)
  const pulses = [];
  let level = half[0];
  let units = 1;
  for (let i = 1; i < half.length; i++) {
    if (half[i] === level) {
      units++;
    } else {
      pulses.push([level, units]);
      level = half[i];
      units = 1;
    }
  }
  pulses.push([level, units]);
  // 3. pair mark+space into word indices
  const words = [];
  for (let i = 0; i < pulses.length; i += 2) {
    const mark = pulses[i];
    const space = pulses[i + 1] || [0, 1]; // trailing mark: close with short space
    words.push(((mark[1] - 1) << 1) | (space[1] - 1));
  }
  return words;
}

/** Generate a random 5-hex-char unit id for a fresh virtual remote. */
function randomUnitId() {
  return Math.floor(Math.random() * 0x100000).toString(16).padStart(5, '0');
}

module.exports = {
  COMMANDS,
  COUNTER_MAX,
  checksum,
  generateCode,
  buildSequence,
  move,
  train,
  remove,
  frameToBits,
  manchesterEncode,
  randomUnitId,
  hex2,
  hex4,
};
