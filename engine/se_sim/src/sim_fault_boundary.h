// Copyright 2026 The simple-engine-2 Authors.
//
// The sim thread's tick, contained (ADR-083; the ADR-006 list of fault
// boundaries). sim_fault_boundary.cc is the only file of se_sim that
// catches: a standard-library, EnTT or glaze throw out of a tick ends that
// tick, and the SimThread tears the session down on its own thread and
// posts the fault to its FaultBox.

#ifndef ENGINE_SE_SIM_SRC_SIM_FAULT_BOUNDARY_H_
#define ENGINE_SE_SIM_SRC_SIM_FAULT_BOUNDARY_H_

#include <cstdint>
#include <exception>

#include "se_core/fault_box.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_sim/sim_driver.h"
#include "se_sim/sim_thread.h"

namespace se {

// Runs SimDriver::RunTick for |session|. False when it threw: |error|
// holds the exception.
[[nodiscard]] bool RunTickContained(SimSession& session,
                                    const InputFrame& frame,
                                    std::uint64_t latch_ns, TickResult& result,
                                    std::exception_ptr& error) noexcept
    SE_REQUIRES(g_sim_owner);

// Writes |error|'s what() (or "unknown exception") into |record|.
void DescribeFault(const std::exception_ptr& error,
                   FaultRecord& record) noexcept;

}  // namespace se

#endif  // ENGINE_SE_SIM_SRC_SIM_FAULT_BOUNDARY_H_
