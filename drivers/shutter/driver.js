'use strict';

const Homey = require('homey');

class ShutterDriver extends Homey.Driver {

  async onInit() {
    this.log('Centronic shutter driver initialized');
  }

  /**
   * Offer one pairable device per free channel (1-7).
   * The app's single virtual remote unit has 7 channels, like a
   * physical Becker multi-channel remote.
   */
  async onPairListDevices() {
    const usedChannels = new Set(
      this.getDevices().map(d => d.getSetting('channel')),
    );
    const devices = [];
    for (let ch = 1; ch <= 7; ch++) {
      if (usedChannels.has(ch)) continue;
      devices.push({
        name: `Roller Shutter (channel ${ch})`,
        data: { id: `becker-shutter-ch${ch}` },
        settings: { channel: ch },
      });
    }
    return devices;
  }

}

module.exports = ShutterDriver;
