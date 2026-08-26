'use strict';

const Homey = require('homey');
const P = require('./lib/BeckerProtocol');

/**
 * Becker Centronic app.
 *
 * The app owns ONE virtual remote "unit" (5-hex id) shared by all shutter
 * devices, each on its own channel (1-7) — mirroring how a physical
 * multi-channel Becker remote works. The rolling counter is per-unit and
 * persisted in app settings so it survives restarts (a lost counter would
 * desync every paired shutter).
 */
class BeckerCentronicApp extends Homey.App {

  async onInit() {
    // Stable virtual remote identity, created once per install
    let unitId = this.homey.settings.get('unitId');
    if (!unitId) {
      unitId = P.randomUnitId();
      this.homey.settings.set('unitId', unitId);
      this.homey.settings.set('increment', 0);
    }
    this.unitId = unitId;

    this._txQueue = Promise.resolve();
    this._signal = this.homey.rf.getSignal868('becker');

    // Flow: train (pair with a shutter that is in programming mode)
    this.homey.flow.getActionCard('train')
      .registerRunListener(async ({ device }) => device.trainShutter());

    // Flow: force counter re-sync (same command 3x resets receiver counter)
    this.homey.flow.getActionCard('resync_counter')
      .registerRunListener(async ({ device }) => device.resyncCounter());

    this.log(`Becker Centronic initialized — unit ${this.unitId}`);
  }

  /** Current rolling counter (persisted). */
  getIncrement() {
    return this.homey.settings.get('increment') || 0;
  }

  setIncrement(value) {
    this.homey.settings.set('increment', value & P.COUNTER_MAX);
  }

  /**
   * Serialize all RF transmissions: frames of one sequence must go out in
   * order, and two devices must never interleave counter updates.
   */
  async transmitSequence(builderFn) {
    this._txQueue = this._txQueue.then(async () => {
      const { frames, nextIncrement } = builderFn(this.unitId, this.getIncrement());
      for (const frame of frames) {
        const words = P.manchesterEncode(P.frameToBits(frame));
        this.log(`TX ${frame} (${words.length} words)`);
        await this._signal.tx(words);
        await new Promise(resolve => setTimeout(resolve, 100));
      }
      this.setIncrement(nextIncrement);
    });
    return this._txQueue;
  }

  /** Send a single movement command on a channel. */
  async sendCommand(channel, commandName) {
    return this.transmitSequence(
      (unitId, inc) => P.move(unitId, inc, channel, commandName),
    );
  }

  /** Send the TRAIN pairing sequence on a channel. */
  async sendTrain(channel) {
    return this.transmitSequence(
      (unitId, inc) => P.train(unitId, inc, channel),
    );
  }

  /** Send HALT 3x with the SAME counter to force receiver counter re-sync. */
  async sendResync(channel) {
    return this.transmitSequence((unitId, inc) => {
      const { frames } = P.move(unitId, inc, channel, 'HALT');
      return {
        frames: [frames[0], frames[0], frames[0]],
        nextIncrement: (inc + 1) & P.COUNTER_MAX,
      };
    });
  }

}

module.exports = BeckerCentronicApp;
