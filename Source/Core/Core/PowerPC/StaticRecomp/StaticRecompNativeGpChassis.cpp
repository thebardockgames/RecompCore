// RecompCore: StaticRecomp CPU core - chassis adapter of the native GP front (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGpChassis.h"

#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGp.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"

namespace StaticRecompNativeGpChassis
{
using StaticRecompNativeGp::NativeGp;
namespace CpReg = StaticRecompNativeGp::CpReg;
namespace CpCtrl = StaticRecompNativeGp::CpCtrl;

static_assert(CommandProcessor::STATUS_REGISTER == CpReg::kStatus &&
              CommandProcessor::CTRL_REGISTER == CpReg::kCtrl &&
              CommandProcessor::CLEAR_REGISTER == CpReg::kClear &&
              CommandProcessor::PERF_SELECT == CpReg::kPerfSelect &&
              CommandProcessor::FIFO_BASE_LO == CpReg::kFifoBaseLo &&
              CommandProcessor::FIFO_BASE_HI == CpReg::kFifoBaseHi &&
              CommandProcessor::FIFO_END_LO == CpReg::kFifoEndLo &&
              CommandProcessor::FIFO_END_HI == CpReg::kFifoEndHi &&
              CommandProcessor::FIFO_HI_WATERMARK_LO == CpReg::kFifoHiWatermarkLo &&
              CommandProcessor::FIFO_HI_WATERMARK_HI == CpReg::kFifoHiWatermarkHi &&
              CommandProcessor::FIFO_LO_WATERMARK_LO == CpReg::kFifoLoWatermarkLo &&
              CommandProcessor::FIFO_LO_WATERMARK_HI == CpReg::kFifoLoWatermarkHi &&
              CommandProcessor::FIFO_RW_DISTANCE_LO == CpReg::kFifoRwDistanceLo &&
              CommandProcessor::FIFO_RW_DISTANCE_HI == CpReg::kFifoRwDistanceHi &&
              CommandProcessor::FIFO_WRITE_POINTER_LO == CpReg::kFifoWritePointerLo &&
              CommandProcessor::FIFO_WRITE_POINTER_HI == CpReg::kFifoWritePointerHi &&
              CommandProcessor::FIFO_READ_POINTER_LO == CpReg::kFifoReadPointerLo &&
              CommandProcessor::FIFO_READ_POINTER_HI == CpReg::kFifoReadPointerHi &&
              CommandProcessor::FIFO_BP_LO == CpReg::kFifoBpLo &&
              CommandProcessor::FIFO_BP_HI == CpReg::kFifoBpHi);

void Install(Core::System& system)
{
  NativeGp::ChassisHooks hooks;
  hooks.seed_and_park = [&system](NativeGp::FifoState& s) {
    // The state the chassis's CP/PI hold: zero after a boot, or what a
    // savestate written by the previous path carried. Once ours, the
    // chassis's FIFO loop must stay idle: reads disabled, nothing pending.
    auto& fifo = system.GetCommandProcessor().GetFifo();
    auto& pi = system.GetProcessorInterface();
    s.base = fifo.CPBase.load();
    s.end = fifo.CPEnd.load();
    s.hi_watermark = fifo.CPHiWatermark;
    s.lo_watermark = fifo.CPLoWatermark;
    s.distance = fifo.CPReadWriteDistance.load();
    s.write_pointer = fifo.CPWritePointer.load();
    s.read_pointer = fifo.CPReadPointer.load();
    s.breakpoint = fifo.CPBreakpoint.load();
    s.breakpoint_hit = fifo.bFF_Breakpoint.load() != 0;
    u16 ctrl = 0;
    if (fifo.bFF_GPReadEnable.load())
      ctrl |= CpCtrl::kGpReadEnable;
    if (fifo.bFF_BPEnable.load())
      ctrl |= CpCtrl::kBpEnable;
    if (fifo.bFF_HiWatermarkInt.load())
      ctrl |= CpCtrl::kOverflowInt;
    if (fifo.bFF_LoWatermarkInt.load())
      ctrl |= CpCtrl::kUnderflowInt;
    if (fifo.bFF_GPLinkEnable.load())
      ctrl |= CpCtrl::kGpLinkEnable;
    if (fifo.bFF_BPInt.load())
      ctrl |= CpCtrl::kBpInt;
    s.ctrl = ctrl;
    s.pi_base = pi.m_fifo_cpu_base;
    s.pi_end = pi.m_fifo_cpu_end;
    s.pi_write_pointer = pi.m_fifo_cpu_write_pointer;
    fifo.bFF_GPReadEnable.store(0);
    fifo.bFF_GPLinkEnable.store(0);
    fifo.CPReadWriteDistance.store(0);
  };
  hooks.copy = [&system](const NativeGp::FifoState& s) {
    auto& fifo = system.GetCommandProcessor().GetFifo();
    auto& pi = system.GetProcessorInterface();
    fifo.CPBase.store(s.base);
    fifo.CPEnd.store(s.end);
    fifo.CPHiWatermark = s.hi_watermark;
    fifo.CPLoWatermark = s.lo_watermark;
    fifo.CPReadWriteDistance.store(s.distance);
    fifo.CPWritePointer.store(s.write_pointer);
    fifo.CPReadPointer.store(s.read_pointer);
    fifo.SafeCPReadPointer.store(s.read_pointer);
    fifo.CPBreakpoint.store(s.breakpoint);
    fifo.bFF_Breakpoint.store(s.breakpoint_hit ? 1 : 0);
    fifo.bFF_GPReadEnable.store((s.ctrl & CpCtrl::kGpReadEnable) ? 1 : 0);
    fifo.bFF_BPEnable.store((s.ctrl & CpCtrl::kBpEnable) ? 1 : 0);
    fifo.bFF_HiWatermarkInt.store((s.ctrl & CpCtrl::kOverflowInt) ? 1 : 0);
    fifo.bFF_LoWatermarkInt.store((s.ctrl & CpCtrl::kUnderflowInt) ? 1 : 0);
    fifo.bFF_GPLinkEnable.store((s.ctrl & CpCtrl::kGpLinkEnable) ? 1 : 0);
    fifo.bFF_BPInt.store((s.ctrl & CpCtrl::kBpInt) ? 1 : 0);
    pi.m_fifo_cpu_base = s.pi_base;
    pi.m_fifo_cpu_end = s.pi_end;
    pi.m_fifo_cpu_write_pointer = s.pi_write_pointer;
  };
  hooks.dual_core = [&system] { return system.IsDualCoreMode(); };
  hooks.frame_update = [] { Core::FrameUpdateOnCPUThread(); };
  NativeGp::SetChassisHooks(std::move(hooks));
}
}  // namespace StaticRecompNativeGpChassis
