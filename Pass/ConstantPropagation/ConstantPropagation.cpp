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

    // Map a block to the index of the last instruction in that block
    std::map<BasicBlock*, int> blockAndItsLastIndex;

    // Lattice definition implicitly applied here:
    // - `any`: Represented by std::nan or uninitialized.
    // - `constant`: Represented by integer values.
    // - `not a constant`: Represented by a combination of std::nan and membership in `definitelyNotConstant`.
    // Note: There are definitely better ways to implement lattices.
    // The current implementation is chosen due to the rusty C skill of the author.
    std::set<Value*> definitelyNotConstant;

    ConstantPropagation() : FunctionPass(ID) {}

    bool runOnFunction(Function &F) override {
        bool hasChanged = false;
        bool isFixedPoint = false;
        int i = 1;

        while (!isFixedPoint) {
            int instructionIndex = 0;
            // errs() <<"Fixed point is false" << "\n";
            // errs()<< "Value of i is: " << i << "\n";
            isFixedPoint = true;
            for (BasicBlock &BB : F) {
                //printBlockValues(&BB);
                handleBranchMerging(&BB); // Merge branch values for the block
                //errs() << "After merging " << "\n";
                //printBlockValues(&BB);
                bool isActiveBlock = inactiveBlocks.find(&BB) == inactiveBlocks.end();
                std::map<int, std::map<Value*, double>> instructionValues; // Local instruction values for this block
                instructionValues = blockValues[&BB];
                for (Instruction &I : BB) {
                    instructionIndex++;
                    // errs() << "  Visiting instruction " << instructionIndex << ": " << I << "\n";
                    if (!isActiveBlock) {
                        //           errs() << "Inactive blocks: " << BB.getName() << "\n";
                        continue;
                    }

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
                                if (BB.getName() != "while.cond") {

                                    inactiveBlocks.insert(ICI->getParent()->getTerminator()->getSuccessor(1));
                                }
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
                blockAndItsLastIndex[&BB] = instructionIndex;
                // errs() << "At the end: " << "\n";
                if (isActiveBlock) {
                    //    printBlockValues(&BB);
                } else {
                    //  errs() << "Block: " << BB.getName() << "\n";
                }
            }
            if ( i < 5) {
                i = i + 1;
                isFixedPoint = false;
            }
            else {
                isFixedPoint = true;
            }
            // errs() << "After processing block: " << BB.getName() << "\n";
            // printBlockValues(&BB);

        }

        for (BasicBlock &BB : F) {
            printBlockValues(&BB);
        }
        printNotAConstantValues();
        return false;
    }

private:

    void handleBranchMerging(BasicBlock *BB) {
        if (pred_begin(BB) == pred_end(BB)) return; // Entry block (no predecessors)


        std::map<int, std::map<Value*, double>> mergedValues;
        bool firstPredecessor = true;
        // errs() << "For block: " << BB->getName() << "\n";
        // errs() << "Preds are: " << "\n";
        for (auto predBB : predecessors(BB)) {
            // below code will set the value of mergedValues equal to first visited predecessor before comparing with other predecessors. This approach will fail if the first predecessor is uninitialized.
            bool unInitialized = blockValues.find(predBB) == blockValues.end();
            if (unInitialized) {
                continue;
            }
            if (inactiveBlocks.find(predBB) != inactiveBlocks.end()) {
                continue;
            }

            // errs() << predBB -> getName() << "\n";
            const auto &predValues = blockValues[predBB];

            for (const auto &instEntry : predValues) {
                int instIdx = instEntry.first;

                if (firstPredecessor) {
                    // errs()<<"Regarding block " << BB -> getName() << "\n";
                    // errs()<<"First predecessor is: " << predBB -> getName() << "\n";
                    //printBlockValues(predBB);
                    mergedValues[instIdx] = instEntry.second;
                } else {
                    // For subsequent predecessors, compare values and handle conflicts
                    for (const auto &varEntry : instEntry.second) {
                        Value *var = varEntry.first;
                        double predValue = varEntry.second;

                        // Not-a-constant /\ anything = Not-a-constant
                        if (definitelyNotConstant.find(var) != definitelyNotConstant.end()) {
                            continue;
                        }

                        // Check if the variable already exists in mergedValues
                        auto it = mergedValues.find(instIdx);
                        if (it != mergedValues.end()) {
                            // If the variable exists in mergedValues, compare the values
                            double &mergedValue = it->second[var];

                            if (!std::isnan(predValue) && mergedValue != predValue) {
                                // If there's a conflict, set the value to NaN
                                mergedValue = std::nan("1");
                                definitelyNotConstant.insert(var); // Mark variable as non-constant
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
        int lastIndex = blockAndItsLastIndex[BB];
        errs() << "Last index: " << lastIndex << "\n";
        for (const auto &instEntry : blockInstrValues) {
            int instIdx = instEntry.first;
            if (instIdx > lastIndex) {
                errs() << "Reached last index" << "\n";
                return;
            }
            for (const auto &varEntry : instEntry.second) {
                if (!std::isnan(varEntry.second)) {
                    errs() << "  Inst " << instIdx << ": " << *varEntry.first << " = ";
                    errs() << (int)varEntry.second << "\n";
                }
                else {
                    errs() << "  Inst " << instIdx << ": " << *varEntry.first << " = ";
                    errs() << "NAN" << "\n";
                }
            }
        }
    }

    void printNotAConstantValues() {
        errs() << "----- Not-A-Constant Values -----\n";
        for (Value *V : definitelyNotConstant) {
            errs() << "Value: ";
            if (V->hasName()) {
                errs() << V->getName();
            } else {
                V->print(errs());
            }
            errs() << "\n";
        }
        errs() << "---------------------------------\n";
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

            errs() << "-----" << BB->getName() << "-----" << "\n";

            if (inactiveBlocks.find(BB) != inactiveBlocks.end()) {
                continue;
            }

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

