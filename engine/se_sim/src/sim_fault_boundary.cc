// Copyright 2026 The simple-engine-2 Authors.
// se-fault-boundary: the dedicated sim thread's tick (ADR-083, ADR-006).

#include "src/sim_fault_boundary.h"

#include <cstdint>
#include <exception>

#include "se_core/fault_box.h"
#include "se_core/input_frame.h"
#include "se_sim/sim_driver.h"
#include "se_sim/sim_thread.h"

namespace se {

bool RunTickContained(SimSession& session, const InputFrame& frame,
                      std::uint64_t latch_ns, TickResult& result,
                      std::exception_ptr& error) noexcept {
  try {
    result =
        session.Driver().RunTick(session.States(), frame, session.Log(),
                                 session.Recorder(), session.Audio(), latch_ns);
    return true;
  } catch (...) {
    error = std::current_exception();
    return false;
  }
}

void DescribeFault(const std::exception_ptr& error,
                   FaultRecord& record) noexcept {
  if (!error) {
    record.SetMessage("no exception");
    return;
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& caught) {
    record.SetMessage(caught.what());
  } catch (...) {
    record.SetMessage("unknown exception");
  }
}

}  // namespace se
