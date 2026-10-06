// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "common/types.h"

class Error;

#ifdef __SWITCH__
struct ExceptionFrameA64;
#endif

namespace PageFaultHandler {
enum class HandlerResult
{
  ContinueExecution,
  ExecuteNextHandler,
};

HandlerResult HandlePageFault(void* exception_pc, void* fault_address, bool is_write);
bool Install(Error* error = nullptr);

#ifdef __SWITCH__
/// Offered every CPU exception by the platform's exception entry; true when it was a fastmem access that was handled.
bool HandleSwitchException(ExceptionFrameA64* frame);
#endif
} // namespace PageFaultHandler
