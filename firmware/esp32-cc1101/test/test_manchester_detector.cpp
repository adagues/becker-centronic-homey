#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/manchester_detector.h"

using becker::ManchesterDetector;
using becker::ManchesterEvent;
using becker::ManchesterResult;

static ManchesterEvent feedFrame(const uint8_t* bits, size_t bitCount,
                                  bool invert, ManchesterDetector& detector,
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
    const ManchesterEvent event = detector.push(duration, level, result);
    assert(event == ManchesterEvent::None);
    pulseIndex++;
    i += units;
  }
  return detector.push(1400, 0, result);
}

static void testDetects65BitsWithNoiseBoundaries() {
  uint8_t bits[65] = {};
  for (size_t i = 0; i < 65; i++) bits[i] = ((i * 7 + 3) & 1) != 0;

  ManchesterDetector detector;
  ManchesterResult result;
  for (size_t i = 0; i < 20; i++) {
    detector.push(i % 2 == 0 ? 120 : 730, 0, result);
  }
  const ManchesterEvent event = feedFrame(bits, 65, false, detector, result);
  assert(event == ManchesterEvent::Detected);
  assert(result.bitCount == 65);
  assert(result.clockUs >= 400 && result.clockUs <= 435);
  for (size_t i = 0; i < 65; i++) assert(result.bits[i] == bits[i]);
}

static void testDetectsInvertedPolarity() {
  uint8_t bits[66] = {};
  for (size_t i = 0; i < 66; i++) bits[i] = (i % 3) == 0;

  ManchesterDetector detector;
  ManchesterResult result;
  const ManchesterEvent event = feedFrame(bits, 66, true, detector, result);
  assert(event == ManchesterEvent::Detected);
  assert(result.bitCount == 66);
  for (size_t i = 0; i < 66; i++) assert(result.bits[i] == (bits[i] ^ 1));
}

static void testRejectsTimingCompatibleNoise() {
  ManchesterDetector detector;
  ManchesterResult result;
  for (size_t i = 0; i < 100; i++) {
    const ManchesterEvent event = detector.push(i % 2 == 0 ? 410 : 820, 1, result);
    assert(event == ManchesterEvent::None);
  }
  const ManchesterEvent event = detector.push(1500, 0, result);
  assert(event == ManchesterEvent::Rejected);
  assert(result.bitCount < 40);
}

int main() {
  testDetects65BitsWithNoiseBoundaries();
  testDetectsInvertedPolarity();
  testRejectsTimingCompatibleNoise();
  puts("Manchester detector tests passed");
  return 0;
}
