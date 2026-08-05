#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);

// Records the current heap figures for the next crash report. Sampled from normal
// task context, never read from the panic handler itself: esp_get_free_heap_size()
// walks the allocator under a lock, and taking a lock inside a panic -- interrupts
// off, scheduler gone -- hangs the device instead of reporting on it.
//
// So the report carries "heap a moment before the fault" rather than "heap at the
// fault". For the abort() family that dominates our reports (a failed allocation
// reaching __cxxabiv1::__terminate) that distinction does not matter: an allocator
// that cannot satisfy a request was already low a moment earlier. Without it the
// only figure in a report is whatever a LOG line happened to print, which is why
// heap has had to be inferred from an [HTTP] line forty lines up.
void noteHeap(uint32_t freeBytes, uint32_t largestBlock);
bool isRebootFromPanic();
}  // namespace HalSystem
