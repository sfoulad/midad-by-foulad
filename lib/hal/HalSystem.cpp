#include "HalSystem.h"

#include <string>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"
#if !__riscv
#include <xtensa_context.h>  // XtExcFrame for the stack capture below
#endif

#define MAX_PANIC_STACK_DEPTH 32
#define PANIC_CAPTURE_MAGIC 0x50414E49u

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];
// Survives the reboot the panic causes, which is the whole point -- these are read
// back out on the next boot when the report is written.
RTC_NOINIT_ATTR uint32_t lastFreeHeap;
RTC_NOINIT_ATTR uint32_t lastLargestBlock;
RTC_NOINIT_ATTR uint32_t heapSampleValid;
constexpr uint32_t HEAP_SAMPLE_MAGIC = 0x48454150;  // "HEAP" -- RTC memory is garbage on a cold boot
// RTC_NOINIT is uninitialized on cold boot, so only this exact marker proves a
// panic diagnostic was captured before the reset.
RTC_NOINIT_ATTR volatile uint32_t panicCaptureMarker;

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";
void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_abort(message);
}

// Reason text for a CPU exception, which never reaches __wrap_panic_abort().
//
// panic_abort() is only called for abort()/assert()-style panics, so a fault --
// a null dereference, a misaligned access -- left panicMessage empty and the
// crash report read "(No reason was recorded)". That happened on a real X3 load
// fault and made the report worthless while the serial log said exactly what
// went wrong. Everything here is DRAM_ATTR and hand-rolled: this runs in panic
// context, where flash may be inaccessible and snprintf is not safe to call.
// mcause codes and the m-registers below are RISC-V; the C3 decodes them, the
// S3 has no equivalent here and falls through to the backtrace alone.
#if __riscv
static DRAM_ATTR const char CAUSE_INSN_MISALIGN[] = "Instruction address misaligned";
static DRAM_ATTR const char CAUSE_INSN_FAULT[] = "Instruction access fault";
static DRAM_ATTR const char CAUSE_ILLEGAL[] = "Illegal instruction";
static DRAM_ATTR const char CAUSE_BREAKPOINT[] = "Breakpoint";
static DRAM_ATTR const char CAUSE_LOAD_MISALIGN[] = "Load address misaligned";
static DRAM_ATTR const char CAUSE_LOAD_FAULT[] = "Load access fault";
static DRAM_ATTR const char CAUSE_STORE_MISALIGN[] = "Store address misaligned";
static DRAM_ATTR const char CAUSE_STORE_FAULT[] = "Store access fault";
static DRAM_ATTR const char CAUSE_OTHER[] = "CPU exception";
static DRAM_ATTR const char TXT_MCAUSE[] = " mcause=";
static DRAM_ATTR const char TXT_MEPC[] = " mepc=0x";
static DRAM_ATTR const char TXT_MTVAL[] = " mtval=0x";
#endif
static DRAM_ATTR const char HEX_DIGITS[] = "0123456789abcdef";

// Bounded append into panicMessage; returns the new length.
static int IRAM_ATTR panicAppend(int at, const char* text) {
  const int cap = (int)sizeof(panicMessage) - 1;
  while (at < cap && *text) panicMessage[at++] = *text++;
  panicMessage[at] = '\0';
  return at;
}

static int IRAM_ATTR panicAppendHex(int at, uint32_t value) {
  const int cap = (int)sizeof(panicMessage) - 1;
  bool started = false;
  for (int shift = 28; shift >= 0; shift -= 4) {
    const uint32_t nibble = (value >> shift) & 0xF;
    if (!started && nibble == 0 && shift != 0) continue;
    started = true;
    if (at < cap) panicMessage[at++] = HEX_DIGITS[nibble];
  }
  panicMessage[at] = '\0';
  return at;
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Stack window dump, mirroring components/esp_system/port/arch/*/panic_arch.c.
  // Hardware exceptions never reach __wrap_panic_abort, so on both
  // architectures this dump is the only diagnostic a crash leaves on-device.
#if __riscv
  const uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
#else
  const uint32_t sp = (uint32_t)((XtExcFrame*)frame)->a1;
#endif
  constexpr uint32_t captureBytes = 1024;
  if (!esp_stack_ptr_is_sane(sp) || sp > UINT32_MAX - captureBytes ||
      !esp_ptr_in_dram(reinterpret_cast<const void*>(sp + captureBytes - 1))) {
    __real_panic_print_backtrace(frame, core);
    return;
  }
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < captureBytes; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }
  // Only when panic_abort() did not run -- an abort/assert message is always
  // more specific than a register decode, so never overwrite one.
#if __riscv
  if (panicMessage[0] == '\0') {
    const RvExcFrame* f = (const RvExcFrame*)frame;
    const uint32_t cause = (uint32_t)f->mcause;
    const char* name = CAUSE_OTHER;
    switch (cause) {
      case 0:
        name = CAUSE_INSN_MISALIGN;
        break;
      case 1:
        name = CAUSE_INSN_FAULT;
        break;
      case 2:
        name = CAUSE_ILLEGAL;
        break;
      case 3:
        name = CAUSE_BREAKPOINT;
        break;
      case 4:
        name = CAUSE_LOAD_MISALIGN;
        break;
      case 5:
        name = CAUSE_LOAD_FAULT;
        break;
      case 6:
        name = CAUSE_STORE_MISALIGN;
        break;
      case 7:
        name = CAUSE_STORE_FAULT;
        break;
      default:
        break;
    }
    int at = panicAppend(0, name);
    at = panicAppend(at, TXT_MCAUSE);
    at = panicAppendHex(at, cause);
    at = panicAppend(at, TXT_MEPC);
    at = panicAppendHex(at, (uint32_t)f->mepc);
    at = panicAppend(at, TXT_MTVAL);
    panicAppendHex(at, (uint32_t)f->mtval);
  }
#endif

  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_print_backtrace(frame, core);
}
}

namespace HalSystem {

void begin() {
  // On a panic reboot, preserve diagnostics until checkPanic() has tried to write them to the SD card.
  // Ordinary boots clear any stale retained diagnostics.
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      const size_t written = file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      if (written == panicInfo.size()) {
        LOG_INF("SYS", "Dumped panic info to SD card");
      } else {
        LOG_ERR("SYS", "Failed to write complete crash report (%zu of %zu bytes)", written, panicInfo.size());
      }
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
    // Keep panicMessage/panicStack for CrashActivity, but mark the panic
    // consumed after this boot's write attempt regardless of outcome -- a
    // marker left set by a failed SD write would otherwise make a later,
    // unrelated watchdog reset misreport as this same stale panic.
    panicCaptureMarker = 0;
  }
}

void clearPanic() {
  panicCaptureMarker = 0;
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  clearLastLogs();
}

void noteHeap(const uint32_t freeBytes, const uint32_t largestBlock) {
  lastFreeHeap = freeBytes;
  lastLargestBlock = largestBlock;
  heapSampleValid = HEAP_SAMPLE_MAGIC;
}

static const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_PANIC:
      return "PANIC (exception/abort)";
    case ESP_RST_CPU_LOCKUP:
      return "CPU_LOCKUP";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT (other)";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    default:
      return "OTHER";
  }
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossPoint version: " CROSSPOINT_VERSION;
    // A lockup or hardware watchdog resets without running any panic hook, so
    // the reason and stack come back empty; the reset cause is then the only
    // way to tell those apart from a true panic.
    info += "\n\nReset reason: " + std::string(resetReasonName(esp_reset_reason()));
    info += "\n\nPanic reason: " + std::string(panicMessage);
    // Named as a sample, not as the value at the fault -- see noteHeap().
    if (heapSampleValid == HEAP_SAMPLE_MAGIC) {
      info += "\n\nHeap before panic: free " + std::to_string(lastFreeHeap) + " bytes, largest block " +
              std::to_string(lastLargestBlock) + " bytes";
    } else {
      info += "\n\nHeap before panic: not sampled";
    }
    info += "\n\nLast logs:\n" + getLastLogs();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP) {
    return true;
  }

  // Watchdog resets (task/interrupt/RTC WDT) fire when code hangs badly enough that
  // it never reaches ESP_RST_PANIC -- functionally the same "something crashed"
  // event and just as worth a crash report, not a silent boot. But panicMessage/
  // panicStack are RTC_NOINIT and persist across many ordinary reboots until
  // clearPanic() runs, so an UNGATED watchdog reset would attribute whatever stale
  // panic text is sitting in RTC memory (possibly from days ago) to today's
  // unrelated hang. panicCaptureMarker is only set inside __wrap_panic_abort/
  // __wrap_panic_print_backtrace, so it proves THIS boot's watchdog reset fired
  // while a panic was actively being handled (e.g. the panic handler itself hung
  // mid-backtrace) rather than from an unrelated hang that never touched panic code.
  const bool watchdogReset =
      resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
  return watchdogReset && panicCaptureMarker == PANIC_CAPTURE_MAGIC;
}

}  // namespace HalSystem
