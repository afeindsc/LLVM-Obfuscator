//=== metrics.cpp - Potency analysis metrics ================================//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "metrics"
#include "Transform/metrics.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include <algorithm>

using namespace llvm;

char Metrics::ID = 0;

static cl::opt<std::string> metricsOutput(
    "metrics-output", cl::init(""),
    cl::desc("Write metrics to an output file instead of stderr"));

static cl::opt<bool>
    metricsOutputAppend("metrics-output-append", cl::init(true),
                        cl::desc("Append output to file. Default to true"));

static cl::opt<std::string> metricsFormat(
    "metrics-format", cl::init("%lu %lu %lu\n"),
    cl::desc("String format for results. If none, will be verbose output"));

bool Metrics::runOnModule(Module &M) {
  unsigned long programLength = 0;
  unsigned long cyclomatic = 0;
  unsigned long nesting = 0;

  for (auto &F : M) {
    if (F.isDeclaration()) {
      continue;
    }

    LoopInfo &loopInfo = getAnalysis<LoopInfo>(F);

    for (auto &BB : F) {

      std::vector<Loop *> loops;
      for (auto &inst : BB) {
        ++programLength;
        programLength += inst.getNumOperands();
      }

      TerminatorInst *terminator = BB.getTerminator();

      if (BranchInst *branch = dyn_cast<BranchInst>(terminator)) {
        if (branch->isConditional()) {
          ++cyclomatic;
        }
      } else if (SwitchInst *switchInst = dyn_cast<SwitchInst>(terminator)) {
        cyclomatic += switchInst->getNumCases();
      } else if (isa<ReturnInst>(terminator)) {
        ++cyclomatic;
      }

      if (Loop *loop = loopInfo.getLoopFor(&BB)) {
        if (std::find(loops.begin(), loops.end(), loop) == loops.end()) {
          loops.push_back(loop);
          ++cyclomatic;
          nesting += loop->getLoopDepth() - 1;
        }
      }
    }

    unsigned nestCalc = calculateNest(F.getEntryBlock(), loopInfo);
    nesting += nestCalc == 0 ? 0 : nestCalc - 1;
    cyclomatic += 2;
  }

  nesting += cyclomatic;
  if (!metricsOutput.empty()) {
    std::string errorInfo;
    raw_fd_ostream output(
        metricsOutput.c_str(), errorInfo,
        metricsOutputAppend ? sys::fs::F_Append : sys::fs::F_None);

    if (!errorInfo.empty()) {
      LLVMContext &ctx = getGlobalContext();
      ctx.emitError("Metrics: Unable to write to output file");
    }

    output << format(metricsFormat.c_str(), programLength, cyclomatic, nesting);
  } else {
    errs() << format(metricsFormat.c_str(), programLength, cyclomatic, nesting);
  }
  return false;
}

unsigned Metrics::calculateNest(BasicBlock &BB, LoopInfo &loopInfo) {
  if (loopInfo.getLoopFor(&BB)) {
    // In a loop -- skipping
    return 0;
  }

  bool isConditional = false;
  TerminatorInst *terminator = BB.getTerminator();

  if (BranchInst *branch = dyn_cast<BranchInst>(terminator)) {
    if (branch->isConditional()) {
      isConditional = true;
    }
  } else if (isa<SwitchInst>(terminator)) {
    isConditional = true;
  }

  unsigned nest = isConditional ? 1 : 0;

  for (unsigned i = 0, successors = terminator->getNumSuccessors();
       i < successors; ++i) {
    nest =
        std::max(nest, calculateNest(*(terminator->getSuccessor(i)), loopInfo));
  }

  return nest;
}

static RegisterPass<Metrics> X("metrics", "Potency analysis metrics pass",
                               false, true);
