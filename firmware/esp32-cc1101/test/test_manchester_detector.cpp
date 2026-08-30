#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/manchester_detector.h"

using becker::ManchesterDetector;
using becker::ManchesterEvent;
using becker::ManchesterResult;

static ManchesterEvent feedFrame(const uint8_t* bits, size_t bitCount,
                                  bool invert, bool injectGlitches,
                                  ManchesterDetector& detector,
                                  ManchesterResult& result) {
  uint8_t halfLevels[ManchesterResult::kMaxBits * 2] = {};
  size_t halfCount = 0;
  for (size_t i = 0; i < bitCount; i++) {
    uint8_t first = bits[i] ? 1 : 0;
    uint8_t second = bits[i] ? 0 : 1;
    if (invert) {
      first ^= 1;
      second ^= 1;
    }
    halfLevels[halfCount++] = first;
    halfLevels[halfCount++] = second;
  }

  size_t pulseIndex = 0;
  size_t i = 0;
  while (i < halfCount) {
    const uint8_t level = halfLevels[i];
    size_t units = 1;
    while (i + units < halfCount && halfLevels[i + units] == level) units++;
    assert(units <= 2);
    const int jitter = static_cast<int>((pulseIndex % 7) * 6) - 18;
    const uint16_t duration = static_cast<uint16_t>(417 * units + jitter);

    if (injectGlitches && units == 2 && pulseIndex % 5 == 2) {
      const uint16_t firstPart = 300;
      const uint16_t glitch = 90;
      const uint16_t secondPart = duration - firstPart - glitch;
      assert(secondPart > ManchesterDetector::kGlitchMaxUs);
      assert(detector.push(firstPart, level, result) == ManchesterEvent::None);
      assert(detector.push(glitch, level ^ 1, result) == ManchesterEvent::None);
      assert(detector.push(secondPart, level, result) == ManchesterEvent::None);
    } else {
      assert(detector.push(duration, level, result) == ManchesterEvent::None);
    }

    pulseIndex++;
    i += units;
  }
  assert(detector.push(1400, 0, result) == ManchesterEvent::None);
  return detector.flush(result);
}

static void testDetects65BitsWithNoiseBoundaries() {
  uint8_t bits[65] = {};
  for (size_t i = 0; i < 65; i++) bits[i] = ((i * 7 + 3) & 1) != 0;

  ManchesterDetector detector;
  ManchesterResult result;
  for (size_t i = 0; i < 20; i++) {
    detector.push(i % 2 == 0 ? 120 : 730, 0, result);
  }
  const ManchesterEvent event =
      feedFrame(bits, 65, false, false, detector, result);
  assert(event == ManchesterEvent::Detected);
  assert(result.bitCount == 65);
  assert(result.clockUs >= 400 && result.clockUs <= 435);
  assert(result.glitchCount == 0);
  for (size_t i = 0; i < 65; i++) assert(result.bits[i] == bits[i]);
}

static void testDetects65BitsWithNarrowGlitches() {
  uint8_t bits[65] = {};
  // This pattern creates both one- and two-half-bit pulse widths.
  for (size_t i = 0; i < 65; i++) bits[i] = ((i * 5 + i / 3) & 1) != 0;

  ManchesterDetector detector;
  ManchesterResult result;
  const ManchesterEvent event =
      feedFrame(bits, 65, false, true, detector, result);
  assert(event == ManchesterEvent::Detected);
  assert(result.bitCount == 65);
  assert(result.glitchCount > 0);
  assert(detector.mergedGlitchCount() == result.glitchCount);
  for (size_t i = 0; i < 65; i++) assert(result.bits[i] == bits[i]);
}

static void testDetectsInvertedPolarity() {
  uint8_t bits[66] = {};
  for (size_t i = 0; i < 66; i++) bits[i] = (i % 3) == 0;

  ManchesterDetector detector;
  ManchesterResult result;
  const ManchesterEvent event =
      feedFrame(bits, 66, true, true, detector, result);
  assert(event == ManchesterEvent::Detected);
  assert(result.bitCount == 66);
  assert(result.glitchCount > 0);
  for (size_t i = 0; i < 66; i++) assert(result.bits[i] == (bits[i] ^ 1));
}

static void testRejectsTimingCompatibleNoise() {
  ManchesterDetector detector;
  ManchesterResult result;
  for (size_t i = 0; i < 100; i++) {
    const ManchesterEvent event = detector.push(i % 2 == 0 ? 410 : 820, 1, result);
    assert(event == ManchesterEvent::None);
  }
  assert(detector.push(1500, 0, result) == ManchesterEvent::None);
  const ManchesterEvent event = detector.flush(result);
  assert(event == ManchesterEvent::Rejected);
  assert(result.bitCount < 40);
}

int main() {
  testDetects65BitsWithNoiseBoundaries();
  testDetects65BitsWithNarrowGlitches();
  testDetectsInvertedPolarity();
  testRejectsTimingCompatibleNoise();
  puts("Manchester detector tests passed");
  return 0;
}
