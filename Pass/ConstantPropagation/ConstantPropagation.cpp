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

#include "llvm/IR/InstrTypes.h"

using namespace llvm;
using namespace std;

#define DEBUG_TYPE "ReachingDefinition"

namespace
{



struct ConstantPropagation : public FunctionPass
{
    static char ID;
    int i = 0;

    // Map to track instruction indices and the corresponding LHS variables with their computed values
    std::map<int, std::map<Value*, int>> instructionValues;


    ConstantPropagation() :

        FunctionPass(ID) {}

    bool runOnFunction(llvm::Function &F) override {
        for (llvm::BasicBlock &BB : F) {
            llvm::errs() << "-----" << BB.getName() << "-----" << "\n";

            for (llvm::Instruction &I : BB) {
                i = i + 1;
                // Map the instruction index to the LHS variable and its computed value
                std::map<Value*, int> lhsValues;

                if (llvm::BinaryOperator *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
                    if (BO->getOpcode() == llvm::Instruction::Add ||
                            BO->getOpcode() == llvm::Instruction::Sub ||
                            BO->getOpcode() == llvm::Instruction::Mul ||
                            BO->getOpcode() == llvm::Instruction::SDiv) {
                        // a binary operator has this form: s = A op B
                        int result = evaluateBinaryOperation(BO);
                        lhsValues[BO] = result;
                    }
                }

                if (llvm::isa<llvm::LoadInst>(&I)) {
                    llvm::LoadInst *LI = llvm::cast<llvm::LoadInst>(&I);

                    // If the loaded value is a constant, we can directly store it
                    if (llvm::ConstantInt *C = llvm::dyn_cast<llvm::ConstantInt>(LI->getOperand(0))) {
                        lhsValues[LI] = C->getSExtValue();
                    } else {
                        // If the loaded value is not a constant, check if it's a variable
                        Value *loadedLocation = LI->getOperand(0);  // Get the variable being loaded

                        // Look for the value of the variable in instructionValues
                        int value = 0;  // Default value if the variable is not found
                        for (const auto& entry : instructionValues) {
                            auto it = entry.second.find(loadedLocation);
                            if (it != entry.second.end()) {
                                value = it->second;  // If found, use the stored value
                                break;
                            }
                        }

                        // Store the value of the variable in the instructionValues map
                        lhsValues[LI] = value;
                    }
                }




                if (llvm::isa<llvm::AllocaInst>(&I)) {
                    lhsValues[&I] = 0;
                }

                if (llvm::isa<llvm::StoreInst>(&I)) {
                    llvm::StoreInst *SI = llvm::cast<llvm::StoreInst>(&I);

                    // The value being stored is the operand to the store instruction
                    Value *storedValue = SI->getValueOperand();

                    if (llvm::ConstantInt *C = llvm::dyn_cast<llvm::ConstantInt>(storedValue)) {
                        // We need to update the value of the variable being written to
                        Value *storedLocation = SI->getPointerOperand();

                        // We don't add StoreInst to the map, but we update the value of the variable in the map
                        // Now, we need to search for the variable in the instructionValues map
                        for (auto& entry : instructionValues) {
                            // Check if the storedLocation matches the LHS variable in this instruction's map
                            auto it = entry.second.find(storedLocation);
                            if (it != entry.second.end()) {
                                // We found the variable, so we update its value
                                it->second = C->getSExtValue();
                                break;  // Once updated, we can exit the loop as we've found and updated the value
                            }
                        }
                    }
                }


                if (!lhsValues.empty()) {
                    instructionValues[i] = lhsValues;
                }
            }


            for (const auto& entry : instructionValues) {
                llvm::errs() << entry.first << ": ";
                for (const auto& lhsEntry : entry.second) {
                    llvm::errs() << "[" << *lhsEntry.first << "]: " << lhsEntry.second << " ";
                }
                llvm::errs() << "\n";
            }
        }
        return false;
    }

private:
    // Helper function to evaluate binary operations
    int evaluateBinaryOperation(llvm::BinaryOperator *BO) {
        // Try to evaluate operands dynamically
        int result = 0;

        int Op0Val = getOperandValue(BO->getOperand(0)); // Get value of first operand
        int Op1Val = getOperandValue(BO->getOperand(1)); // Get value of second operand

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

    // Helper function to get the value of an operand from the map
    int getOperandValue(Value *V) {
        // If the operand is a constant integer, return its value
        if (llvm::ConstantInt *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
            return CI->getSExtValue();
        }

        // If the operand is a variable that we have already seen (i.e., it's in the map)
        if (llvm::isa<llvm::Instruction>(V)) {
            for (const auto& entry : instructionValues) {
                auto it = entry.second.find(V);
                if (it != entry.second.end()) {
                    return it->second;  // Return the computed value from the map
                }
            }
        }

        // If the operand is not found, return 0 (unknown value)
        return 0;
    }


}; // end of struct ReachingDefinition
} // end of anonymous namespace

char ConstantPropagation::ID = 0;
static RegisterPass<ConstantPropagation> X("ConstantPropagation", "Constant Propagation Pass",
        false /* Only looks at CFG */,
        true /* Analysis Pass */);
