#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <string>
#include <fstream>
#include <map>
#include <set>
#include <cmath>

using namespace llvm;
using namespace std;

#define DEBUG_TYPE "ConstantPropagation"

namespace {

struct ConstantPropagation : public FunctionPass {
    static char ID;

    // Set to track inactive (unreachable) basic blocks
    std::set<BasicBlock*> inactiveBlocks;

    // Map to track values associated with each basic block's instruction indices
    std::map<BasicBlock*, std::map<int, std::map<Value*, double>>> blockValues;

    ConstantPropagation() : FunctionPass(ID) {}

    bool runOnFunction(Function &F) override {
        int instructionIndex = 0;
        for (BasicBlock &BB : F) {
            handleBranchMerging(&BB); // Merge branch values for the block
            errs() << "After merging block: " << BB.getName() << "\n";
            printBlockValues(&BB);

            bool isActiveBlock = inactiveBlocks.find(&BB) == inactiveBlocks.end();
            std::map<int, std::map<Value*, double>> instructionValues; // Local instruction values for this block
            instructionValues = blockValues[&BB];
            for (Instruction &I : BB) {
                instructionIndex++;
                // errs() << "  Visiting instruction " << instructionIndex << ": " << I << "\n";
                if (!isActiveBlock) continue;

                std::map<Value*, double> lhsValues;

                if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
                    if (BO->getOpcode() == Instruction::Add ||
                            BO->getOpcode() == Instruction::Sub ||
                            BO->getOpcode() == Instruction::Mul ||
                            BO->getOpcode() == Instruction::SDiv) {
                        double result = evaluateBinaryOperation(BO);
                        lhsValues[BO] = result;
                    }
                }

                if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    Value *loadedLocation = LI->getOperand(0);
                    double value = getOperandValue(&BB, loadedLocation);
                    lhsValues[LI] = value;
                }

                if (isa<AllocaInst>(&I)) {
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
                        double value = getOperandValue(&BB, storedValue);
                        for (auto& entry : instructionValues) {
                            auto it = entry.second.find(storedLocation);
                            if (it != entry.second.end()) {
                                it->second = value;
                                break;
                            }
                        }
                    }
                }

                if (auto *ICI = dyn_cast<ICmpInst>(&I)) {
                    double LHS = getOperandValue(&BB, ICI->getOperand(0));
                    double RHS = getOperandValue(&BB, ICI->getOperand(1));

                    if (!std::isnan(LHS) && !std::isnan(RHS)) {
                        if (evaluateComparison(ICI)) {
                            inactiveBlocks.insert(ICI->getParent()->getTerminator()->getSuccessor(1));
                        } else {
                            inactiveBlocks.insert(ICI->getParent()->getTerminator()->getSuccessor(0));
                        }
                    }
                }

                if (!lhsValues.empty()) {
                    instructionValues[instructionIndex] = lhsValues;
                }
                blockValues[&BB] = instructionValues; // Store instruction values for this block
            }
            errs() << "After processing block: " << BB.getName() << "\n";
            printBlockValues(&BB);
       
        }

        // Print final values for all active blocks
        printActiveBlockValues();

        return false;
    }

private:

    void handleBranchMerging(BasicBlock *BB) {
        if (pred_begin(BB) == pred_end(BB)) return; // Entry block (no predecessors)


        std::map<int, std::map<Value*, double>> mergedValues;
        bool firstPredecessor = true;

        for (auto predBB : predecessors(BB)) {
            const auto &predValues = blockValues[predBB];

            for (const auto &instEntry : predValues) {
                int instIdx = instEntry.first;

                if (firstPredecessor) {
                    mergedValues[instIdx] = instEntry.second;
                } else {
                    // For subsequent predecessors, compare values and handle conflicts
                    for (const auto &varEntry : instEntry.second) {
                        Value *var = varEntry.first;
                        double predValue = varEntry.second;

                        // Check if the variable already exists in mergedValues
                        auto it = mergedValues.find(instIdx);
                        if (it != mergedValues.end()) {
                            // If the variable exists in mergedValues, compare the values
                            double &mergedValue = it->second[var];

                            if (std::isnan(mergedValue)) {
                                // If it is NaN, we keep the previous value
                                continue;
                            } else if (!std::isnan(predValue) && mergedValue != predValue) {
                                // If there's a conflict, set the value to NaN
                                mergedValue = std::nan("1");
                            }
                        }
                    }
                }
            }
            // After processing the first predecessor, set the flag to false
            firstPredecessor = false;
        }

        // Update the current block with the merged values
        blockValues[BB] = mergedValues;
    }

    void printBlockValues(BasicBlock *BB) {
        errs() << "Block: " << BB->getName() << "\n";
        const auto &blockInstrValues = blockValues[BB];
        for (const auto &instEntry : blockInstrValues) {
            int instIdx = instEntry.first;
            for (const auto &varEntry : instEntry.second) {
                if (!std::isnan(varEntry.second)) {
                    errs() << "  Inst " << instIdx << ": " << *varEntry.first << " = ";
                    errs() << (int)varEntry.second << "\n";
                }
            }
        }
    }
    double evaluateBinaryOperation(BinaryOperator *BO) {
        double Op0Val = getOperandValue(BO->getParent(), BO->getOperand(0));
        double Op1Val = getOperandValue(BO->getParent(), BO->getOperand(1));

        switch (BO->getOpcode()) {
        case Instruction::Add:
            return Op0Val + Op1Val;
        case Instruction::Sub:
            return Op0Val - Op1Val;
        case Instruction::Mul:
            return Op0Val * Op1Val;
        case Instruction::SDiv:
            return Op0Val / Op1Val;
        default:
            return std::nan("1");
        }
    }

    bool evaluateComparison(ICmpInst *ICI) {
        double LHS = getOperandValue(ICI->getParent(), ICI->getOperand(0));
        double RHS = getOperandValue(ICI->getParent(), ICI->getOperand(1));

        switch (ICI->getPredicate()) {
        case ICmpInst::ICMP_EQ:
            return LHS == RHS;
        case ICmpInst::ICMP_NE:
            return LHS != RHS;
        case ICmpInst::ICMP_SLT:
            return LHS < RHS;
        case ICmpInst::ICMP_SLE:
            return LHS <= RHS;
        case ICmpInst::ICMP_SGT:
            return LHS > RHS;
        case ICmpInst::ICMP_SGE:
            return LHS >= RHS;
        default:
            return false;
        }
    }

    double getOperandValue(BasicBlock *BB, Value *V) {
        if (auto *CI = dyn_cast<ConstantInt>(V)) {
            return (double)CI->getSExtValue();
        }

        if (isa<Instruction>(V)) {
            const auto &blockInstrValues = blockValues[BB];
            for (const auto &entry : blockInstrValues) {
                auto it = entry.second.find(V);
                if (it != entry.second.end()) {
                    return it->second;
                }
            }
        }

        return std::nan("1");
    }

    void updateStoredValue(BasicBlock *BB, Value *location, double value) {
        auto &blockInstrValues = blockValues[BB];
        for (auto &entry : blockInstrValues) {
            auto it = entry.second.find(location);
            if (it != entry.second.end()) {
                it->second = value;
                return;
            }
        }
    }

    void printActiveBlockValues() {
        for (const auto &blockEntry : blockValues) {
            BasicBlock *BB = blockEntry.first;

            // Only print active blocks
            if (inactiveBlocks.find(BB) != inactiveBlocks.end()) {
                continue;
            }

            errs() << "-----" << BB->getName() << "-----" << "\n";
            const auto &blockInstrValues = blockEntry.second;
            for (const auto &instEntry : blockInstrValues) {
                int instIdx = instEntry.first;
                for (const auto &varEntry : instEntry.second) {
                    if (!std::isnan(varEntry.second)) {
                        errs() << instIdx << ": " << *varEntry.first << " = ";
                        errs() << (int)varEntry.second << "\n";
                    }
                }
            }
        }
    }
};

} // end of anonymous namespace

char ConstantPropagation::ID = 0;
static RegisterPass<ConstantPropagation> X("ConstantPropagation", "Constant Propagation Pass",
        false /* Only looks at CFG */,
        true /* Analysis Pass */);

