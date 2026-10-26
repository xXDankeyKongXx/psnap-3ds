#pragma once

#include <cstdint>

#include "librecomp/rsp.hpp"

// Runs one audio task's command list natively (see audio_hle.cpp). DMEM must already hold the
// microcode's data segment, as recomp::rsp::run_task arranges.
RspExitReason audio_hle_run(uint8_t* rdram, uint32_t alist_addr, uint32_t alist_size);

// Development: runs the task with both the recompiled microcode and the HLE from the same starting
// state, logs any difference in what they write to RDRAM, and keeps the HLE's result.
RspExitReason audio_hle_compare(uint8_t* rdram, uint32_t alist_addr, uint32_t alist_size, RspUcodeFunc* microcode, uint32_t ucode_addr);
