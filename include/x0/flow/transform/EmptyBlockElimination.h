#pragma once

#include <x0/Api.h>
#include <x0/flow/ir/HandlerPass.h>

namespace x0 {

/**
 * Eliminates empty blocks, that are just jumping to the next block.
 */
class X0_API EmptyBlockElimination : public HandlerPass {
public:
    bool run(IRHandler* handler) override;
};

} // namespace x0
