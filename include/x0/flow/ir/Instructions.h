/* <x0/flow/ir/Instructions.h>
 *
 * This file is part of the x0 web server project and is released under AGPL-3.
 * http://www.xzero.io/
 *
 * (c) 2009-2014 Christian Parpart <trapni@gmail.com>
 */

#pragma once

#include <x0/Api.h>
#include <x0/flow/ir/Instr.h>
#include <x0/flow/ir/ConstantValue.h>
#include <x0/flow/vm/MatchClass.h>

#include <string>
#include <vector>
#include <list>

namespace x0 {

class Instr;
class BasicBlock;
class IRProgram;
class IRBuilder;
class IRBuiltinHandler;
class IRBuiltinFunction;

class X0_API NopInstr : public Instr {
public:
    NopInstr() :
        Instr(FlowType::Void, {}, "nop")
    {
    }

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

/**
 * Allocates an array of given type and elements.
 */
class X0_API AllocaInstr : public Instr {
private:
    static FlowType computeType(FlowType elementType, Value* size) { // {{{
        if (auto n = dynamic_cast<ConstantInt*>(size)) {
            if (n->get() == 1)
                return elementType;
        }

        switch (elementType) {
            case FlowType::Number:
                return FlowType::IntArray;
            case FlowType::String:
                return FlowType::StringArray;
            default:
                return FlowType::Void;
        }
    } // }}}

public:
    AllocaInstr(FlowType ty, Value* n, const std::string& name) :
        Instr(ty, {n}, name)
    {
    }

    FlowType elementType() const {
        switch (type()) {
            case FlowType::StringArray:
                return FlowType::String;
            case FlowType::IntArray:
                return FlowType::Number;
            default:
                return FlowType::Void;
        }
    }

    Value* arraySize() const { return operands()[0]; }

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

class X0_API StoreInstr : public Instr {
public:
    StoreInstr(Value* variable, ConstantInt* index, Value* expression, const std::string& name) :
        Instr(FlowType::Void, {variable, index, expression}, name)
        {}

    Value* variable() const { return operand(0); }
    ConstantInt* index() const { return static_cast<ConstantInt*>(operand(1)); }
    Value* expression() const { return operand(2); }

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

class X0_API LoadInstr : public Instr {
public:
    LoadInstr(Value* variable, const std::string& name) :
        Instr(variable->type(), {variable}, name)
        {}

    Value* variable() const { return operand(0); }

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

class X0_API CallInstr : public Instr {
private:
    CallInstr(const std::vector<Value*>& args, const std::string& name);

public:
    CallInstr(IRBuiltinFunction* callee, const std::vector<Value*>& args, const std::string& name);

    IRBuiltinFunction* callee() const { return (IRBuiltinFunction*) operand(0); }

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

class X0_API HandlerCallInstr : public Instr {
private:
    explicit HandlerCallInstr(const std::vector<Value*>& args);

public:
    HandlerCallInstr(IRBuiltinHandler* callee, const std::vector<Value*>& args);

    IRBuiltinHandler* callee() const { return (IRBuiltinHandler*) operand(0); }

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

class X0_API CastInstr : public Instr {
public:
    CastInstr(FlowType resultType, Value* op, const std::string& name) :
        Instr(resultType, {op}, name)
        {}

    Value* source() const { return operand(0); }

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

template<const UnaryOperator Operator, const FlowType ResultType>
class X0_API UnaryInstr : public Instr {
public:
    UnaryInstr(Value* op, const std::string& name) :
        Instr(ResultType, {op}, name),
        operator_(Operator)
        {}

    UnaryOperator op() const {
        return operator_;
    }

    void dump() override {
        dumpOne(cstr(operator_));
    }

    Instr* clone() override {
        return new UnaryInstr<Operator, ResultType>(operand(0), name());
    }

    void accept(InstructionVisitor& v) override {
        v.visit(*this);
    }

private:
    UnaryOperator operator_;
};

template<const BinaryOperator Operator, const FlowType ResultType>
class X0_API BinaryInstr : public Instr {
public:
    BinaryInstr(Value* lhs, Value* rhs, const std::string& name) :
        Instr(ResultType, {lhs, rhs}, name),
        operator_(Operator)
        {}

    BinaryOperator op() const {
        return operator_;
    }

    void dump() override {
        dumpOne(cstr(operator_));
    }

    Instr* clone() override {
        return new BinaryInstr<Operator, ResultType>(operand(0), operand(1), name());
    }

    void accept(InstructionVisitor& v) override {
        v.visit(*this);
    }

private:
    BinaryOperator operator_;
};

/**
 * Creates a PHI (phoney) instruction.
 *
 * Creates a synthetic instruction that purely informs the target register allocator
 * to allocate the very same register for all given operands,
 * which is then used across all their basic blocks.
 */
class X0_API PhiNode : public Instr {
public:
    PhiNode(const std::vector<Value*>& ops, const std::string& name);

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

class X0_API TerminateInstr : public Instr {
protected:
    TerminateInstr(const TerminateInstr& v) :
        Instr(v)
    {}

public:
    TerminateInstr(const std::vector<Value*>& ops) :
        Instr(FlowType::Void, ops, "")
    {}
};

/**
 * Conditional branch instruction.
 *
 * Creates a terminate instruction that transfers control to one of the two
 * given alternate basic blocks, depending on the given input condition.
 */
class X0_API CondBrInstr : public TerminateInstr {
public:
    /**
     * Initializes the object.
     *
     * @param cond input condition that (if true) causes \p trueBlock to be jumped to, \p falseBlock otherwise.
     * @param trueBlock basic block to run if input condition evaluated to true.
     * @param falseBlock basic block to run if input condition evaluated to false.
     */
    CondBrInstr(Value* cond, BasicBlock* trueBlock, BasicBlock* falseBlock);

    Value* condition() const { return operands()[0]; }
    BasicBlock* trueBlock() const { return (BasicBlock*) operands()[1]; }
    BasicBlock* falseBlock() const { return (BasicBlock*) operands()[2]; }

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

/**
 * Unconditional jump instruction.
 */
class X0_API BrInstr : public TerminateInstr {
public:
    explicit BrInstr(BasicBlock* targetBlock);

    BasicBlock* targetBlock() const { return (BasicBlock*) operands()[0]; }

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

/**
 * handler-return instruction.
 */
class X0_API RetInstr : public TerminateInstr {
public:
    RetInstr(Value* result);

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;
};

/**
 * Match instruction, implementing the Flow match-keyword.
 *
 * <li>operand[0] - condition</li>
 * <li>operand[1] - default block</li>
 * <li>operand[2n+2] - case label</li>
 * <li>operand[2n+3] - case block</li>
 */
class X0_API MatchInstr : public TerminateInstr {
private:
    MatchInstr(const MatchInstr&);

public:
    MatchInstr(FlowVM::MatchClass op, Value* cond);

    FlowVM::MatchClass op() const { return op_; }

    Value* condition() const { return operand(0); }

    void addCase(Constant* label, BasicBlock* code);
    std::vector<std::pair<Constant*, BasicBlock*>> cases() const;

    BasicBlock* elseBlock() const;
    void setElseBlock(BasicBlock* code);

    void dump() override;
    Instr* clone() override;
    void accept(InstructionVisitor& v) override;

private:
    FlowVM::MatchClass op_;
    std::vector<std::pair<Constant*, BasicBlock*>> cases_;
};

} // namespace x0
