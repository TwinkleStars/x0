/* <x0/flow/ir/xxx.h>
 *
 * This file is part of the x0 web server project and is released under AGPL-3.
 * http://www.xzero.io/
 *
 * (c) 2009-2014 Christian Parpart <trapni@gmail.com>
 */

#pragma once

#include <x0/Api.h>
#include <x0/flow/ir/Value.h>
#include <x0/flow/ir/InstructionVisitor.h>
#include <x0/flow/vm/Instruction.h>
#include <x0/flow/vm/MatchClass.h>
#include <x0/flow/vm/Signature.h>
#include <x0/IPAddress.h>
#include <x0/RegExp.h>
#include <x0/Cidr.h>

#include <string>
#include <vector>
#include <list>

namespace x0 {

class Instr;
class BasicBlock;
class IRHandler;
class IRProgram;
class IRBuilder;

class InstructionVisitor;

/**
 * Base class for native instructions.
 *
 * An instruction is derived from base class \c Value because its result can be used
 * as an operand for other instructions.
 *
 * @see IRBuilder
 * @see BasicBlock
 * @see IRHandler
 */
class X0_API Instr : public Value {
protected:
    Instr(const Instr& v);

public:
    Instr(FlowType ty, const std::vector<Value*>& ops = {}, const std::string& name = "");
    ~Instr();

    /**
     * Retrieves parent basic block this instruction is part of.
     */
    BasicBlock* parent() const { return parent_; }

    /**
     * Read-only access to operands.
     */
    const std::vector<Value*>& operands() const { return operands_; }

    /**
     * Retrieves n'th operand at given \p index.
     */
    Value* operand(size_t index) const { return operands_[index]; }

    /**
     * Adds given operand \p value to the end of the operand list.
     */
    void addOperand(Value* value);

    /**
     * Sets operand at index \p i to given \p value.
     *
     * This operation will potentially replace the value that has been at index \p i before,
     * properly unlinking it from any uses or successor/predecessor links.
     */
    Value* setOperand(size_t i, Value* value);

    /**
     * Replaces \p old operand with \p replacement.
     *
     * @param old value to replace
     * @param replacement new value to put at the offset of \p old.
     *
     * @returns number of actual performed replacements.
     */
    size_t replaceOperand(Value* old, Value* replacement);

    /**
     * Clones given instruction.
     *
     * This will not clone any of its operands but reference them.
     */
    virtual Instr* clone() = 0;

    /**
     * Generic extension interface.
     *
     * @param v extension to pass this instruction to.
     *
     * @see InstructionVisitor
     */
    virtual void accept(InstructionVisitor& v) = 0;

protected:
    void dumpOne(const char* mnemonic);

    void setParent(BasicBlock* bb) { parent_ = bb; }

    friend class BasicBlock;

private:
    BasicBlock* parent_;
    std::vector<Value*> operands_;
};

} // namespace x0
