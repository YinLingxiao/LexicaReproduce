#include "parity/contracts/cancel.h"
namespace lexica::parity {
namespace {
thread_local const Cancellation *current = nullptr;
}
void setCurrentCancellation(const Cancellation *value) { current = value; }
const Cancellation *currentCancellation() { return current; }
} // namespace lexica::parity
