'use strict';

const Homey = require('homey');

class ShutterDevice extends Homey.Device {

  async onInit() {
    this.registerCapabilityListener(
      'windowcoverings_state',
      this.onWindowCoveringsState.bind(this),
    );
    this.log(`Shutter on channel ${this.getChannel()} initialized`);
  }

  getChannel() {
    return this.getSetting('channel') || 1;
  }

  /**
   * up / idle / down -> Becker UP / HALT / DOWN.
   * Centronic is unidirectional: no position feedback exists, the state we
   * set is the last command sent.
   */
  async onWindowCoveringsState(value) {
    const map = { up: 'UP', idle: 'HALT', down: 'DOWN' };
    const command = map[value];
    if (!command) throw new Error(`Unsupported state: ${value}`);
    await this.homey.app.sendCommand(this.getChannel(), command);
  }

  /** Flow action: TRAIN — register this channel on a motor in pairing mode. */
  async trainShutter() {
    this.log(`Training shutter on channel ${this.getChannel()}`);
    await this.homey.app.sendTrain(this.getChannel());
  }

  /** Flow action: force the receiver to re-sync to our rolling counter. */
  async resyncCounter() {
    this.log(`Re-syncing counter on channel ${this.getChannel()}`);
    await this.homey.app.sendResync(this.getChannel());
  }

}

module.exports = ShutterDevice;
