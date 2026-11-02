#pragma once

#include <cstdint>

#include "CrossPointPosition.h"

enum class ProgressComparison : uint8_t { LocalAhead, Synchronized, RemoteAhead, Unknown };

// Which of two positions is further into the book. Trusted evidence first (resolved
// spine, equal mapped page, visible-text offset, mapped page), percentage last:
// CrossPoint and KOReader paginate differently, so their percentages can disagree
// with the real reading order.
ProgressComparison compareProgress(const CrossPointPosition& local, float localPercentage,
                                   const CrossPointPosition& remote, float remotePercentage);
