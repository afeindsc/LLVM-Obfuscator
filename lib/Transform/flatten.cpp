//=== flatten.cpp - Flatten Control Flow Pass  ----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// http://ac.inf.elte.hu/Vol_030_2009/003.pdf
#define DEBUG_TYPE "flatten"
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/User.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CFG.h"
#include <algorithm>
#include <vector>
#include <chrono>
#include <random>

using namespace llvm;

static cl::list<std::string>
flattenFunc("flattenFunc", cl::CommaSeparated,
            cl::desc("Insert Bogus Control Flow only for some functions: "
                     "flattenFunc=\"func1,func2\""));

static cl::opt<std::string> flattenSeed(
    "flattenSeed", cl::init(""),
    cl::desc("Seed for random number generator. Defaults to system time"));

namespace {
struct Flatten : public FunctionPass {
  static char ID;
  std::minstd_rand engine;

  Flatten() : FunctionPass(ID) {}

  inline Value *findBlock(LLVMContext &context,
                          std::vector<BasicBlock *> &blocks,
                          BasicBlock *block) {
    auto iterator = std::find(blocks.begin(), blocks.end(), block);
    assert(iterator != blocks.end() && "Block does not exist in vector!");
    unsigned index = iterator - blocks.begin();
    return ConstantInt::get(Type::getInt32Ty(context), index, false);
  }

  // Initialise and check options
  virtual bool doInitialization(Module &M) {
    // Seed engine and create distribution
    if (!flattenSeed.empty()) {
      std::seed_seq seed(flattenSeed.begin(), flattenSeed.end());
      engine.seed(seed);
    } else {
      unsigned seed =
          std::chrono::system_clock::now().time_since_epoch().count();
      engine.seed(seed);
    }

    return false;
  }

  virtual bool runOnFunction(Function &F) {
    // If the function is declared elsewhere in other translation unit
    // we should not modify it here
    if (F.isDeclaration()) {
      return false;
    }
    DEBUG(errs() << "flatten: Function '" << F.getName() << "'\n");

    // Check if function is requested
    auto funcListStart = flattenFunc.begin(), funcListEnd = flattenFunc.end();
    if (flattenFunc.size() != 0 &&
        std::find(funcListStart, funcListEnd, F.getName()) == funcListEnd) {
      DEBUG(errs() << "\tFunction not requested -- skipping\n");
      return false;
    }

    LLVMContext &context = F.getContext();

    // Use a vector to store the list of blocks
    std::vector<BasicBlock *> blocks;
    blocks.reserve(F.size());

    DEBUG(errs() << "\t" << F.size() << " basic blocks found\n");
    Twine blockPrefix = "block_";
    unsigned i = 0;
    DEBUG(errs() << "\tListing and filtering blocks\n");
    // Get original list of blocks
    for (auto &block : F) {
      DEBUG(if (!block.hasName()) { block.setName(blockPrefix + Twine(i++)); });

      DEBUG(errs() << "\tBlock " << block.getName() << "\n");
      BasicBlock::iterator inst1 = block.begin();
      if (block.getFirstNonPHIOrDbgOrLifetime()) {
        inst1 = block.getFirstNonPHIOrDbgOrLifetime();
      }
      if (block.isLandingPad()) {
        DEBUG(errs() << "\t\tSkipping: Landing pad block\n");
        continue;
      }
      if (&block == &F.getEntryBlock()) {
        DEBUG(errs() << "\t\tSkipping: Entry block\n");
        continue;
      }

      if (isa<IndirectBrInst>(block.getTerminator())) {
        // TODO Maybe handle this
        DEBUG(errs() << "\tSkipping function -- IndirectBrInst encountered");
        return false;
      }

      if (isa<SwitchInst>(block.getTerminator())) {
        // TODO Maybe handle this
        DEBUG(errs() << "\tSkipping function -- SwitchInst encountered");
        return false;
      }

      DEBUG(errs() << "\t\tAdding block\n");
      blocks.push_back(&block);
    }

    DEBUG(errs() << "\t" << blocks.size() << " basic blocks remaining\n");
    if (blocks.size() < 2) {
      DEBUG(errs() << "\tNothing left to flatten\n");
      return false;
    }
    // Setup other variables
    BasicBlock &entryBlock = F.getEntryBlock();

    if (entryBlock.getTerminator()->getNumSuccessors() == blocks.size() ||
        entryBlock.getTerminator()->getNumSuccessors() == 0) {
      DEBUG(errs() << "\tFunction is trivial -- already flat control flow\n");
      return false;
    }

    DEBUG(F.viewCFG());
    // Demote all the PHI Nodes to stack
    DEBUG(errs() << "\tDemoting PHI Nodes to stack\n");
    for (auto block : blocks) {
      std::vector<PHINode *> phis;
      for (auto &inst : *block) {
        if (PHINode *phiInst = dyn_cast<PHINode>(&inst)) {
          phis.push_back(phiInst);
        }
      }
      for (auto phiInst : phis) {
        DemotePHIToStack(phiInst);
      }
    }

    BasicBlock *initialBlock;
    // Going to have to split the entry block into 2 blocks
    if (entryBlock.getTerminator()->getNumSuccessors() > 1) {
      DEBUG(errs() << "\tSplitting entry block\n");
      initialBlock = entryBlock.splitBasicBlock(entryBlock.getTerminator());
      blocks.push_back(initialBlock);
    } else {
      initialBlock = entryBlock.getTerminator()->getSuccessor(0);
    }
    DEBUG(entryBlock.setName("entry_block"));
    DEBUG(initialBlock->setName("initial_block"));

    entryBlock.getTerminator()->eraseFromParent();

    // Entry Block builder
    IRBuilder<> entryBuilder(&entryBlock);

    BasicBlock *jumpBlock = BasicBlock::Create(context, "", &F);
    DEBUG(jumpBlock->setName("jump_block"));

    // Jump Block builder
    IRBuilder<> jumpBuilder(jumpBlock);

    Twine jumpIndexName("");
    DEBUG(jumpIndexName = jumpIndexName.concat("jump_index"));
    PHINode *jumpIndex = jumpBuilder.CreatePHI(
        Type::getInt32Ty(context), blocks.size() + 1, jumpIndexName);

    DEBUG(errs() << "\tCreating jump table:\n");

    Twine jumpTableName("");
    DEBUG(jumpTableName = jumpTableName.concat("jump_table"));
    Value *jumpTableSize =
        ConstantInt::get(Type::getInt32Ty(context), blocks.size(), false);
    AllocaInst *jumpTable = entryBuilder.CreateAlloca(
        Type::getInt8PtrTy(context), jumpTableSize, jumpTableName);

    // Create indirect branch
    Twine jumpAddrName("");
    DEBUG(jumpAddrName = jumpAddrName.concat("jump_addr"));
    Value *jumpAddress =
        jumpBuilder.CreateGEP(jumpTable, jumpIndex, jumpAddrName);
    IndirectBrInst *indirectBranch =
        jumpBuilder.CreateIndirectBr(jumpAddress, blocks.size());
    assert(indirectBranch && "IndirectBranchInst cannot be null!");

    for (unsigned i = 0, iEnd = blocks.size(); i < iEnd; ++i) {
      BasicBlock *block = blocks[i];
      assert(block != &entryBlock && "Entry block should not be processed!");
      DEBUG(errs() << "\t" << block->getName() << ":\n");
      Value *index = ConstantInt::get(Type::getInt32Ty(context), i, false);

      // Create jump index
      if (block == initialBlock) {
        jumpIndex->addIncoming(index, &entryBlock);
      }

      TerminatorInst *terminator = block->getTerminator();
      bool hasSuccessor = terminator->getNumSuccessors() > 0;
      if (terminator->getNumSuccessors() == 0) {
        // No need to do anything
        DEBUG(errs() << "\t\t0 Successor\n");
        // ReturnInst, ResumeInst, UnreachableInst
      } else if (terminator->getNumSuccessors() == 1) {
        // Trivial
        DEBUG(errs() << "\t\t1 Successor\n");
        BasicBlock *destination = terminator->getSuccessor(0);
        Value *destinationIndexValue = findBlock(context, blocks, destination);
        jumpIndex->addIncoming(destinationIndexValue, block);

        terminator->eraseFromParent();
        BranchInst::Create(jumpBlock, block);
      } else { // > 1 succesors
        DEBUG(errs() << "\t\t" << terminator->getNumSuccessors()
                     << " Successors\n");
        // Conditional branch
        if (BranchInst *branch = dyn_cast<BranchInst>(terminator)) {
          DEBUG(errs() << "\t\tConditional branch\n");
          BasicBlock *trueBlock = branch->getSuccessor(0);
          BasicBlock *falseBlock = branch->getSuccessor(1);
          Value *trueIndex = findBlock(context, blocks, trueBlock);
          Value *falseIndex = findBlock(context, blocks, falseBlock);
          SelectInst *select = SelectInst::Create(
              branch->getCondition(), trueIndex, falseIndex, "", terminator);

          jumpIndex->addIncoming(select, block);

          terminator->eraseFromParent();
          BranchInst::Create(jumpBlock, block);
        } else if (InvokeInst *invoke = dyn_cast<InvokeInst>(terminator)) {
          // InvokeInst
          DEBUG(errs() << "\t\tInvoke Terminator\n");
          Value *destination =
              findBlock(context, blocks, invoke->getNormalDest());
          BasicBlock *newDestination = BasicBlock::Create(context, "", &F);
          invoke->setNormalDest(newDestination);
          jumpIndex->addIncoming(destination, newDestination);
          BranchInst::Create(jumpBlock, newDestination);
        } else {
          llvm_unreachable("Unexpected TerminatorInst encountered!");
        }
      }

      // Add to jump table
      Value *ptr = entryBuilder.CreateGEP(jumpTable, index);
      BlockAddress *blockAddress = BlockAddress::get(block);
      entryBuilder.CreateStore((Value *)blockAddress, ptr);

      indirectBranch->addDestination(block);

      if (hasSuccessor) {
        DEBUG(errs() << "\t\tHandling successor use\n");
        for (auto &inst : *block) {
          DEBUG(errs() << "\t\t\t" << inst << "\n");
          std::vector<User *> users;
          PHINode *phi = nullptr;
          bool isUsed = false;
          // Find the phi node in jumpBlock if it's there
          for (auto user = inst.use_begin(), useEnd = inst.use_end();
               user != useEnd; ++user) {
            Instruction *userInst = dyn_cast<Instruction>(*user);
            assert(userInst && "User is not an instruction");
            BasicBlock *userBlock = userInst->getParent();
            if (userBlock == jumpBlock) {
              phi = dyn_cast<PHINode>(userInst);
              if (phi && phi != jumpIndex) {
                isUsed = true;
                break;
              }
            } else if (userBlock != block) {
              isUsed = true;
              users.push_back(*user);
              DEBUG(errs() << "\t\t\t\tUsed in " << userBlock->getName()
                           << "\n");
            }
          }
          if (isUsed && !phi) {
            phi = jumpBuilder.CreatePHI(inst.getType(), users.size(), "");
            phi->moveBefore(jumpBlock->begin());
          }
          if (isUsed) {
            phi->addIncoming(&inst, block);
            for (User *user : users) {
              user->replaceUsesOfWith(&inst, phi);
            }
            DemotePHIToStack(phi);
          }
        }
      }
    }

    assert(jumpTable->isArrayAllocation() && "Jump table should be static!");
    entryBuilder.CreateBr(jumpBlock);

    for (auto inst = jumpBlock->begin(), instEnd = jumpBlock->end();
         inst != instEnd; ++inst) {
      PHINode *phi = dyn_cast<PHINode>((Instruction *)inst);
      if (phi == jumpIndex)
        continue;
      if (!phi)
        continue;
      if ((Instruction *)inst == jumpBlock->getFirstNonPHIOrDbgOrLifetime())
        break;
      for (auto pred = pred_begin(jumpBlock), predEnd = pred_end(jumpBlock);
           pred != predEnd; ++pred) {
        if (phi->getBasicBlockIndex(*pred) == -1) {
          phi->addIncoming(phi, *pred);
        }
      }
    }

    DEBUG(F.viewCFG());
    // DEBUG_WITH_TYPE("cfg", F.viewCFG());

    return true;
  }

  // Finalisation will add the necessary opaque predicates
  // virtual bool doFinalization(Module &M) { return false; }
  // virtual void getAnalysisUsage(AnalysisUsage &Info) const {}
};
}

char Flatten::ID = 0;
static RegisterPass<Flatten> X("flatten", "Flatten function control flow",
                               false, false);

// http://homes.cs.washington.edu/~bholt/posts/llvm-quick-tricks.html
static RegisterStandardPasses Y(PassManagerBuilder::EP_OptimizerLast,
                                [](const PassManagerBuilder &,
                                   PassManagerBase &PM) {
  PM.add(new Flatten());
});
