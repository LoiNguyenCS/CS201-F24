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
  std::vector<int> instructions;
  ConstantPropagation() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override {
    for (llvm::BasicBlock &BB : F) {
      llvm::errs() << "-----" << BB.getName() << "-----" << "\n"; 

      for (llvm::Instruction &I : BB) {
	i = i + 1;
        if (llvm::BinaryOperator *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
          if (BO->getOpcode() == llvm::Instruction::Add ||
              BO->getOpcode() == llvm::Instruction::Sub ||
              BO->getOpcode() == llvm::Instruction::Mul ||
              BO->getOpcode() == llvm::Instruction::SDiv) {
	    // a binary operator has this form: s = A op B
            instructions.push_back(i);
          }
        }
        if (llvm::isa<llvm::LoadInst>(&I) ||   // Load instruction (s = *A, where A is an existing value)
          llvm::isa<llvm::AllocaInst>(&I)) {   // Allocation instruction (s = alloca, used when new value is declared)
          instructions.push_back(i);  
        }
      }
      for (int instID : instructions) {
        llvm::errs() << instID << ": " << "\n";
      }
    }

    return false;
  }

}; // end of struct ReachingDefinition
} // end of anonymous namespace

char ConstantPropagation::ID = 0;
static RegisterPass<ConstantPropagation> X("ConstantPropagation", "Constant Propagation Pass",
                                      false /* Only looks at CFG */,
                                      true /* Analysis Pass */);
