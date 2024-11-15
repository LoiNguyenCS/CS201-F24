#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <string>
#include <fstream>
#include <map>
#include <set>
#include <queue>
#include <cmath>

using namespace llvm;
using namespace std;

#define DEBUG_TYPE "ConstantPropagation"

namespace
{

struct ConstantPropagation : public FunctionPass
{
    static char ID;
    int i = 0;

    // Map to track instruction indices and the corresponding LHS variables with their computed values
    std::map<int, std::map<Value*, double>> instructionValues;

    // Set to track inactive (unreachable) basic blocks
    std::set<BasicBlock*> inactiveBlocks;

    ConstantPropagation() : FunctionPass(ID) {}

    bool runOnFunction(llvm::Function &F) override {
        for (llvm::BasicBlock &BB : F) {
            llvm::errs() << "-----" << BB.getName() << "-----" << "\n";
            bool isActiveBlock = false;

            if (inactiveBlocks.find(&BB) != inactiveBlocks.end()) {
                isActiveBlock = true;
            }

            for (llvm::Instruction &I : BB) {
                i = i + 1;
                if (isActiveBlock) {
                    continue;
                }
                std::map<Value*, double> lhsValues;

                if (llvm::BinaryOperator *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
                    if (BO->getOpcode() == llvm::Instruction::Add ||
                            BO->getOpcode() == llvm::Instruction::Sub ||
                            BO->getOpcode() == llvm::Instruction::Mul ||
                            BO->getOpcode() == llvm::Instruction::SDiv) {
                        double result = evaluateBinaryOperation(BO);
                        lhsValues[BO] = result;
                    }
                }

                if (llvm::isa<llvm::LoadInst>(&I)) {
                    llvm::LoadInst *LI = llvm::cast<llvm::LoadInst>(&I);
                    if (llvm::ConstantInt *C = llvm::dyn_cast<llvm::ConstantInt>(LI->getOperand(0))) {
                        lhsValues[LI] = (double)C->getSExtValue();
                    } else {
                        Value *loadedLocation = LI->getOperand(0);
                        double value = getOperandValue(loadedLocation);
                        lhsValues[LI] = value;
                    }
                }

                if (llvm::isa<llvm::AllocaInst>(&I)) {
                    lhsValues[&I] = std::nan("1");
                }

                if (llvm::isa<llvm::StoreInst>(&I)) {
                    llvm::StoreInst *SI = llvm::cast<llvm::StoreInst>(&I);
                    Value *storedValue = SI->getValueOperand();
                    Value *storedLocation = SI->getPointerOperand();
                    if (llvm::ConstantInt *C = llvm::dyn_cast<llvm::ConstantInt>(storedValue)) {
                        for (auto& entry : instructionValues) {
                            auto it = entry.second.find(storedLocation);
                            if (it != entry.second.end()) {
                                it->second = (double)C->getSExtValue();
                                break;
                            }
                        }
                    }
                    else {
                        double value = getOperandValue(storedValue);
                        for (auto& entry : instructionValues) {
                            auto it = entry.second.find(storedLocation);
                            if (it != entry.second.end()) {
                                it->second = value;
                                break;
                            }
                        }
                    }
                }

                // Process comparison instructions
                if (llvm::ICmpInst *ICI = llvm::dyn_cast<llvm::ICmpInst>(&I)) {
                    double LHS = getOperandValue(ICI->getOperand(0));
                    double RHS = getOperandValue(ICI->getOperand(1));

                    if (!std::isnan(LHS) && !std::isnan(RHS)) {
                        if (evaluateComparison(ICI)) {
                            llvm::BasicBlock *ElseBB = ICI->getParent()->getTerminator()->getSuccessor(1);
                            inactiveBlocks.insert(ElseBB);
                        } else {
                            llvm::BasicBlock *ThenBB = ICI->getParent()->getTerminator()->getSuccessor(0);
                            inactiveBlocks.insert(ThenBB);  // Mark the 'then' block as inactive
                        }
                    }
                }

                // Record computed values for each instruction
                if (!lhsValues.empty()) {
                    instructionValues[i] = lhsValues;
                }
            }
            if (isActiveBlock) {
                continue;
            }

            // Output the final results (values for each instruction)
            for (const auto& entry : instructionValues) {
                for (const auto& lhsEntry : entry.second) {
                    if (!std::isnan(lhsEntry.second)) {
                        llvm::errs() << entry.first << ": ";
                        llvm::errs() << "[" << *lhsEntry.first << "]: " << (int)lhsEntry.second << " ";
                        llvm::errs() << "\n";
                    }
                }
            }
        }

        return false;
    }

private:

    // Helper function to evaluate binary operations
    double evaluateBinaryOperation(llvm::BinaryOperator *BO) {
        // Try to evaluate operands dynamically
        double result = 0;

        double Op0Val = getOperandValue(BO->getOperand(0)); // Get value of first operand
        double Op1Val = getOperandValue(BO->getOperand(1)); // Get value of second operand

        switch (BO->getOpcode()) {
        case llvm::Instruction::Add:
            result = Op0Val + Op1Val;
            break;
        case llvm::Instruction::Sub:
            result = Op0Val - Op1Val;
            break;
        case llvm::Instruction::Mul:
            result = Op0Val * Op1Val;
            break;
        case llvm::Instruction::SDiv:
            result = Op0Val / Op1Val; // Assuming no division by zero
            break;
        default:
            break;
        }
        return result;
    }

    // Helper function to evaluate integer comparison operations (ICmpInst)
    bool evaluateComparison(llvm::ICmpInst *ICI) {
        // Get the operands (LHS and RHS) of the comparison
        double LHS = getOperandValue(ICI->getOperand(0));
        double RHS = getOperandValue(ICI->getOperand(1));

        // Determine the comparison result based on the operator
        switch (ICI->getPredicate()) {
        case llvm::ICmpInst::ICMP_EQ:
            return LHS == RHS;  // Equal
        case llvm::ICmpInst::ICMP_NE:
            return LHS != RHS;  // Not equal
        case llvm::ICmpInst::ICMP_SLT:
            return LHS < RHS;   // Less than
        case llvm::ICmpInst::ICMP_SLE:
            return LHS <= RHS;  // Less than or equal
        case llvm::ICmpInst::ICMP_SGT:
            return LHS > RHS;   // Greater than
        case llvm::ICmpInst::ICMP_SGE:
            return LHS >= RHS;  // Greater than or equal
        default:
            // Unsupported comparison predicate
            return false;
        }
    }

    // Helper function to get the value of an operand (for integer values)
    double getOperandValue(Value *V) {
        if (llvm::ConstantInt *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
            return (double)CI->getSExtValue();
        }

        // If the operand is an instruction, check if it has been evaluated
        if (llvm::isa<llvm::Instruction>(V)) {
            for (const auto& entry : instructionValues) {
                auto it = entry.second.find(V);
                if (it != entry.second.end()) {
                    return it->second;
                }
            }
        }

        return std::nan("1");  // Use NaN to indicate an unknown value;
    }
};

} // end of anonymous namespace

char ConstantPropagation::ID = 0;
static RegisterPass<ConstantPropagation> X("ConstantPropagation", "Constant Propagation Pass",
        false /* Only looks at CFG */,
        true /* Analysis Pass */);

