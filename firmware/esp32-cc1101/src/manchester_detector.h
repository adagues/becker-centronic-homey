#pragma once

#include <stddef.h>
#include <stdint.h>

namespace becker {

enum class ManchesterEvent : uint8_t {
  None,
  Rejected,
  Detected,
};

struct ManchesterResult {
  static constexpr size_t kMaxPulses = 256;
  static constexpr size_t kMaxBits = 256;

  uint16_t pulseCount = 0;
  uint16_t clockUs = 0;
  uint16_t halfBitCount = 0;
  uint16_t bitCount = 0;
  uint8_t phase = 0;
  uint8_t bits[kMaxBits] = {};
  uint16_t durations[kMaxPulses] = {};
  uint8_t levels[kMaxPulses] = {};
};

class ManchesterDetector {
 public:
  static constexpr uint16_t kShortMinUs = 240;
  static constexpr uint16_t kShortMaxUs = 620;
  static constexpr uint16_t kLongMinUs = 650;
  static constexpr uint16_t kLongMaxUs = 1150;
  static constexpr size_t kMinTimingPulses = 40;

  ManchesterEvent push(uint16_t duration, uint8_t level,
                       ManchesterResult& result) {
    uint8_t units = 0;
    uint16_t clockSample = 0;
    if (duration >= kShortMinUs && duration <= kShortMaxUs) {
      units = 1;
      clockSample = duration;
    } else if (duration >= kLongMinUs && duration <= kLongMaxUs) {
      units = 2;
      clockSample = duration / 2;
    } else {
      return finish(result);
    }

    if (pulseCount_ == ManchesterResult::kMaxPulses) {
      const ManchesterEvent event = finish(result);
      if (event != ManchesterEvent::None) return event;
    }

    durations_[pulseCount_] = duration;
    levels_[pulseCount_] = level ? 1 : 0;
    units_[pulseCount_] = units;
    pulseCount_++;
    clockSum_ += clockSample;
    return ManchesterEvent::None;
  }

  ManchesterEvent flush(ManchesterResult& result) { return finish(result); }

  void reset() {
    pulseCount_ = 0;
    clockSum_ = 0;
  }

 private:
  ManchesterEvent finish(ManchesterResult& result) {
    if (pulseCount_ < kMinTimingPulses) {
      reset();
      return ManchesterEvent::None;
    }

    const uint16_t clockUs = static_cast<uint16_t>(clockSum_ / pulseCount_);
    uint8_t halfLevels[ManchesterResult::kMaxPulses * 2] = {};
    size_t halfCount = 0;
    for (size_t i = 0; i < pulseCount_; i++) {
      for (uint8_t unit = 0; unit < units_[i]; unit++) {
        halfLevels[halfCount++] = levels_[i];
      }
    }

    size_t bestStart = 0;
    size_t bestBits = 0;
    uint8_t bestPhase = 0;
    for (uint8_t phase = 0; phase < 2; phase++) {
      size_t currentStart = 0;
      size_t currentBits = 0;
      size_t pairIndex = 0;
      for (size_t i = phase; i + 1 < halfCount; i += 2, pairIndex++) {
        if (halfLevels[i] != halfLevels[i + 1]) {
          if (currentBits == 0) currentStart = pairIndex;
          currentBits++;
          if (currentBits > bestBits) {
            bestBits = currentBits;
            bestStart = currentStart;
            bestPhase = phase;
          }
        } else {
          currentBits = 0;
        }
      }
    }

    result = ManchesterResult{};
    result.pulseCount = static_cast<uint16_t>(pulseCount_);
    result.clockUs = clockUs;
    result.halfBitCount = static_cast<uint16_t>(halfCount);
    result.bitCount = static_cast<uint16_t>(bestBits);
    result.phase = bestPhase;
    for (size_t i = 0; i < pulseCount_; i++) {
      result.durations[i] = durations_[i];
      result.levels[i] = levels_[i];
    }

    if (bestBits >= 40 && bestBits <= ManchesterResult::kMaxBits) {
      const size_t firstHalf = bestPhase + bestStart * 2;
      for (size_t bit = 0; bit < bestBits; bit++) {
        const size_t half = firstHalf + bit * 2;
        // Polarity is intentionally explicit; callers also print its inverse.
        result.bits[bit] =
            (halfLevels[half] == 1 && halfLevels[half + 1] == 0) ? 1 : 0;
      }
      reset();
      return ManchesterEvent::Detected;
    }

    reset();
    return ManchesterEvent::Rejected;
  }

  uint16_t durations_[ManchesterResult::kMaxPulses] = {};
  uint8_t levels_[ManchesterResult::kMaxPulses] = {};
  uint8_t units_[ManchesterResult::kMaxPulses] = {};
  size_t pulseCount_ = 0;
  uint32_t clockSum_ = 0;
};

}  // namespace becker
