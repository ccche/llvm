//===-- UnrollLoop.cpp - Loop unrolling utilities -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements some loop unrolling utilities. It does not define any
// actual pass or policy, but provides a single function to perform loop
// unrolling.
//
// The process of unrolling can produce extraneous basic blocks linked with
// unconditional branches.  This will be corrected in the future.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/OptimizationDiagnosticInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/SimplifyIndVar.h"
using namespace llvm;

#define DEBUG_TYPE "loop-unroll"

// TODO: Should these be here or in LoopUnroll?
STATISTIC(NumCompletelyUnrolled, "Number of loops completely unrolled");
STATISTIC(NumUnrolled, "Number of loops unrolled (completely or otherwise)");

static cl::opt<bool>
UnrollRuntimeEpilog("unroll-runtime-epilog", cl::init(false), cl::Hidden,
                    cl::desc("Allow runtime unrolled loops to be unrolled "
                             "with epilog instead of prolog."));

/// Convert the instruction operands from referencing the current values into
/// those specified by VMap.
static inline void remapInstruction(Instruction *I,
                                    ValueToValueMapTy &VMap) {
  for (unsigned op = 0, E = I->getNumOperands(); op != E; ++op) {
    Value *Op = I->getOperand(op);
    ValueToValueMapTy::iterator It = VMap.find(Op);
    if (It != VMap.end())
      I->setOperand(op, It->second);
  }

  if (PHINode *PN = dyn_cast<PHINode>(I)) {
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
      ValueToValueMapTy::iterator It = VMap.find(PN->getIncomingBlock(i));
      if (It != VMap.end())
        PN->setIncomingBlock(i, cast<BasicBlock>(It->second));
    }
  }
}

/// Folds a basic block into its predecessor if it only has one predecessor, and
/// that predecessor only has one successor.
/// The LoopInfo Analysis that is passed will be kept consistent.  If folding is
/// successful references to the containing loop must be removed from
/// ScalarEvolution by calling ScalarEvolution::forgetLoop because SE may have
/// references to the eliminated BB.  The argument ForgottenLoops contains a set
/// of loops that have already been forgotten to prevent redundant, expensive
/// calls to ScalarEvolution::forgetLoop.  Returns the new combined block.
static BasicBlock *
foldBlockIntoPredecessor(BasicBlock *BB, LoopInfo *LI, ScalarEvolution *SE,
                         SmallPtrSetImpl<Loop *> &ForgottenLoops,
                         DominatorTree *DT) {
  // Merge basic blocks into their predecessor if there is only one distinct
  // pred, and if there is only one distinct successor of the predecessor, and
  // if there are no PHI nodes.
  BasicBlock *OnlyPred = BB->getSinglePredecessor();
  if (!OnlyPred) return nullptr;

  if (OnlyPred->getTerminator()->getNumSuccessors() != 1)
    return nullptr;

  DEBUG(dbgs() << "Merging: " << *BB << "into: " << *OnlyPred);

  // Resolve any PHI nodes at the start of the block.  They are all
  // guaranteed to have exactly one entry if they exist, unless there are
  // multiple duplicate (but guaranteed to be equal) entries for the
  // incoming edges.  This occurs when there are multiple edges from
  // OnlyPred to OnlySucc.
  FoldSingleEntryPHINodes(BB);

  // Delete the unconditional branch from the predecessor...
  OnlyPred->getInstList().pop_back();

  // Make all PHI nodes that referred to BB now refer to Pred as their
  // source...
  BB->replaceAllUsesWith(OnlyPred);

  // Move all definitions in the successor to the predecessor...
  OnlyPred->getInstList().splice(OnlyPred->end(), BB->getInstList());

  // OldName will be valid until erased.
  StringRef OldName = BB->getName();

  // Erase the old block and update dominator info.
  if (DT)
    if (DomTreeNode *DTN = DT->getNode(BB)) {
      DomTreeNode *PredDTN = DT->getNode(OnlyPred);
      SmallVector<DomTreeNode *, 8> Children(DTN->begin(), DTN->end());
      for (auto *DI : Children)
        DT->changeImmediateDominator(DI, PredDTN);

      DT->eraseNode(BB);
    }

  // ScalarEvolution holds references to loop exit blocks.
  if (SE) {
    if (Loop *L = LI->getLoopFor(BB)) {
      if (ForgottenLoops.insert(L).second)
        SE->forgetLoop(L);
    }
  }
  LI->removeBlock(BB);

  // Inherit predecessor's name if it exists...
  if (!OldName.empty() && !OnlyPred->hasName())
    OnlyPred->setName(OldName);

  BB->eraseFromParent();

  return OnlyPred;
}

/// Check if unrolling created a situation where we need to insert phi nodes to
/// preserve LCSSA form.
/// \param Blocks is a vector of basic blocks representing unrolled loop.
/// \param L is the outer loop.
/// It's possible that some of the blocks are in L, and some are not. In this
/// case, if there is a use is outside L, and definition is inside L, we need to
/// insert a phi-node, otherwise LCSSA will be broken.
/// The function is just a helper function for llvm::UnrollLoop that returns
/// true if this situation occurs, indicating that LCSSA needs to be fixed.
static bool needToInsertPhisForLCSSA(Loop *L, std::vector<BasicBlock *> Blocks,
                                     LoopInfo *LI) {
  for (BasicBlock *BB : Blocks) {
    if (LI->getLoopFor(BB) == L)
      continue;
    for (Instruction &I : *BB) {
      for (Use &U : I.operands()) {
        if (auto Def = dyn_cast<Instruction>(U)) {
          Loop *DefLoop = LI->getLoopFor(Def->getParent());
          if (!DefLoop)
            continue;
          if (DefLoop->contains(L))
            return true;
        }
      }
    }
  }
  return false;
}

/// Unroll the given loop by Count. The loop must be in LCSSA form. Returns true
/// if unrolling was successful, or false if the loop was unmodified. Unrolling
/// can only fail when the loop's latch block is not terminated by a conditional
/// branch instruction. However, if the trip count (and multiple) are not known,
/// loop unrolling will mostly produce more code that is no faster.
///
/// TripCount is generally defined as the number of times the loop header
/// executes. UnrollLoop relaxes the definition to permit early exits: here
/// TripCount is the iteration on which control exits LatchBlock if no early
/// exits were taken. Note that UnrollLoop assumes that the loop counter test
/// terminates LatchBlock in order to remove unnecesssary instances of the
/// test. In other words, control may exit the loop prior to TripCount
/// iterations via an early branch, but control may not exit the loop from the
/// LatchBlock's terminator prior to TripCount iterations.
///
/// Similarly, TripMultiple divides the number of times that the LatchBlock may
/// execute without exiting the loop.
///
/// If AllowRuntime is true then UnrollLoop will consider unrolling loops that
/// have a runtime (i.e. not compile time constant) trip count.  Unrolling these
/// loops require a unroll "prologue" that runs "RuntimeTripCount % Count"
/// iterations before branching into the unrolled loop.  UnrollLoop will not
/// runtime-unroll the loop if computing RuntimeTripCount will be expensive and
/// AllowExpensiveTripCount is false.
///
/// The LoopInfo Analysis that is passed will be kept consistent.
///
/// This utility preserves LoopInfo. It will also preserve ScalarEvolution and
/// DominatorTree if they are non-null.
bool llvm::UnrollLoop(Loop *L, unsigned Count, unsigned TripCount, bool Force,
                      bool AllowRuntime, bool AllowExpensiveTripCount,
                      unsigned TripMultiple, LoopInfo *LI, ScalarEvolution *SE,
                      DominatorTree *DT, AssumptionCache *AC,
                      OptimizationRemarkEmitter *ORE, bool PreserveLCSSA) {
  BasicBlock *Preheader = L->getLoopPreheader();
  if (!Preheader) {
    DEBUG(dbgs() << "  Can't unroll; loop preheader-insertion failed.\n");
    return false;
  }

  BasicBlock *LatchBlock = L->getLoopLatch();
  if (!LatchBlock) {
    DEBUG(dbgs() << "  Can't unroll; loop exit-block-insertion failed.\n");
    return false;
  }

  // Loops with indirectbr cannot be cloned.
  if (!L->isSafeToClone()) {
    DEBUG(dbgs() << "  Can't unroll; Loop body cannot be cloned.\n");
    return false;
  }

  BasicBlock *Header = L->getHeader();
  BranchInst *BI = dyn_cast<BranchInst>(LatchBlock->getTerminator());

  if (!BI || BI->isUnconditional()) {
    // The loop-rotate pass can be helpful to avoid this in many cases.
    DEBUG(dbgs() <<
             "  Can't unroll; loop not terminated by a conditional branch.\n");
    return false;
  }

  if (Header->hasAddressTaken()) {
    // The loop-rotate pass can be helpful to avoid this in many cases.
    DEBUG(dbgs() <<
          "  Won't unroll loop: address of header block is taken.\n");
    return false;
  }

  if (TripCount != 0)
    DEBUG(dbgs() << "  Trip Count = " << TripCount << "\n");
  if (TripMultiple != 1)
    DEBUG(dbgs() << "  Trip Multiple = " << TripMultiple << "\n");

  // Effectively "DCE" unrolled iterations that are beyond the tripcount
  // and will never be executed.
  if (TripCount != 0 && Count > TripCount)
    Count = TripCount;

  // Don't enter the unroll code if there is nothing to do. This way we don't
  // need to support "partial unrolling by 1".
  if (TripCount == 0 && Count < 2)
    return false;

  assert(Count > 0);
  assert(TripMultiple > 0);
  assert(TripCount == 0 || TripCount % TripMultiple == 0);

  // Are we eliminating the loop control altogether?
  bool CompletelyUnroll = Count == TripCount;
  SmallVector<BasicBlock *, 4> ExitBlocks;
  L->getExitBlocks(ExitBlocks);
  std::vector<BasicBlock*> OriginalLoopBlocks = L->getBlocks();

  // Go through all exits of L and see if there are any phi-nodes there. We just
  // conservatively assume that they're inserted to preserve LCSSA form, which
  // means that complete unrolling might break this form. We need to either fix
  // it in-place after the transformation, or entirely rebuild LCSSA. TODO: For
  // now we just recompute LCSSA for the outer loop, but it should be possible
  // to fix it in-place.
  bool NeedToFixLCSSA = PreserveLCSSA && CompletelyUnroll &&
                        any_of(ExitBlocks, [](const BasicBlock *BB) {
                          return isa<PHINode>(BB->begin());
                        });

  // We assume a run-time trip count if the compiler cannot
  // figure out the loop trip count and the unroll-runtime
  // flag is specified.
  bool RuntimeTripCount = (TripCount == 0 && Count > 0 && AllowRuntime);

  // Loops containing convergent instructions must have a count that divides
  // their TripMultiple.
  DEBUG(
      {
        bool HasConvergent = false;
        for (auto &BB : L->blocks())
          for (auto &I : *BB)
            if (auto CS = CallSite(&I))
              HasConvergent |= CS.isConvergent();
        assert((!HasConvergent || TripMultiple % Count == 0) &&
               "Unroll count must divide trip multiple if loop contains a "
               "convergent operation.");
      });
  // Don't output the runtime loop remainder if Count is a multiple of
  // TripMultiple.  Such a remainder is never needed, and is unsafe if the loop
  // contains a convergent instruction.
  if (RuntimeTripCount && TripMultiple % Count != 0 &&
      !UnrollRuntimeLoopRemainder(L, Count, AllowExpensiveTripCount,
                                  UnrollRuntimeEpilog, LI, SE, DT, 
                                  PreserveLCSSA)) {
    if (Force)
      RuntimeTripCount = false;
    else
      return false;
  }

  // Notify ScalarEvolution that the loop will be substantially changed,
  // if not outright eliminated.
  if (SE)
    SE->forgetLoop(L);

  // If we know the trip count, we know the multiple...
  unsigned BreakoutTrip = 0;
  if (TripCount != 0) {
    BreakoutTrip = TripCount % Count;
    TripMultiple = 0;
  } else {
    // Figure out what multiple to use.
    BreakoutTrip = TripMultiple =
      (unsigned)GreatestCommonDivisor64(Count, TripMultiple);
  }

  // Report the unrolling decision.
  if (CompletelyUnroll) {
    DEBUG(dbgs() << "COMPLETELY UNROLLING loop %" << Header->getName()
          << " with trip count " << TripCount << "!\n");
    ORE->emitOptimizationRemark(DEBUG_TYPE, L,
                                Twine("completely unrolled loop with ") +
                                    Twine(TripCount) + " iterations");
  } else {
    auto EmitDiag = [&](const Twine &T) {
      ORE->emitOptimizationRemark(
          DEBUG_TYPE, L, "unrolled loop by a factor of " + Twine(Count) + T);
    };

    DEBUG(dbgs() << "UNROLLING loop %" << Header->getName()
          << " by " << Count);
    if (TripMultiple == 0 || BreakoutTrip != TripMultiple) {
      DEBUG(dbgs() << " with a breakout at trip " << BreakoutTrip);
      EmitDiag(" with a breakout at trip " + Twine(BreakoutTrip));
    } else if (TripMultiple != 1) {
      DEBUG(dbgs() << " with " << TripMultiple << " trips per branch");
      EmitDiag(" with " + Twine(TripMultiple) + " trips per branch");
    } else if (RuntimeTripCount) {
      DEBUG(dbgs() << " with run-time trip count");
      EmitDiag(" with run-time trip count");
    }
    DEBUG(dbgs() << "!\n");
  }

  bool ContinueOnTrue = L->contains(BI->getSuccessor(0));
  BasicBlock *LoopExit = BI->getSuccessor(ContinueOnTrue);

  // For the first iteration of the loop, we should use the precloned values for
  // PHI nodes.  Insert associations now.
  ValueToValueMapTy LastValueMap;
  std::vector<PHINode*> OrigPHINode;
  for (BasicBlock::iterator I = Header->begin(); isa<PHINode>(I); ++I) {
    OrigPHINode.push_back(cast<PHINode>(I));
  }

  std::vector<BasicBlock*> Headers;
  std::vector<BasicBlock*> Latches;
  Headers.push_back(Header);
  Latches.push_back(LatchBlock);

  // The current on-the-fly SSA update requires blocks to be processed in
  // reverse postorder so that LastValueMap contains the correct value at each
  // exit.
  LoopBlocksDFS DFS(L);
  DFS.perform(LI);

  // Stash the DFS iterators before adding blocks to the loop.
  LoopBlocksDFS::RPOIterator BlockBegin = DFS.beginRPO();
  LoopBlocksDFS::RPOIterator BlockEnd = DFS.endRPO();

  std::vector<BasicBlock*> UnrolledLoopBlocks = L->getBlocks();

  // Loop Unrolling might create new loops. While we do preserve LoopInfo, we
  // might break loop-simplified form for these loops (as they, e.g., would
  // share the same exit blocks). We'll keep track of loops for which we can
  // break this so that later we can re-simplify them.
  SmallSetVector<Loop *, 4> LoopsToSimplify;
  for (Loop *SubLoop : *L)
    LoopsToSimplify.insert(SubLoop);

  for (unsigned It = 1; It != Count; ++It) {
    std::vector<BasicBlock*> NewBlocks;
    SmallDenseMap<const Loop *, Loop *, 4> NewLoops;
    NewLoops[L] = L;

    for (LoopBlocksDFS::RPOIterator BB = BlockBegin; BB != BlockEnd; ++BB) {
      ValueToValueMapTy VMap;
      BasicBlock *New = CloneBasicBlock(*BB, VMap, "." + Twine(It));
      Header->getParent()->getBasicBlockList().push_back(New);

      // Tell LI about New.
      if (*BB == Header) {
        assert(LI->getLoopFor(*BB) == L && "Header should not be in a sub-loop");
        L->addBasicBlockToLoop(New, *LI);
      } else {
        // Figure out which loop New is in.
        const Loop *OldLoop = LI->getLoopFor(*BB);
        assert(OldLoop && "Should (at least) be in the loop being unrolled!");

        Loop *&NewLoop = NewLoops[OldLoop];
        if (!NewLoop) {
          // Found a new sub-loop.
          assert(*BB == OldLoop->getHeader() &&
                 "Header should be first in RPO");

          Loop *NewLoopParent = NewLoops.lookup(OldLoop->getParentLoop());
          assert(NewLoopParent &&
                 "Expected parent loop before sub-loop in RPO");
          NewLoop = new Loop;
          NewLoopParent->addChildLoop(NewLoop);
          LoopsToSimplify.insert(NewLoop);

          // Forget the old loop, since its inputs may have changed.
          if (SE)
            SE->forgetLoop(OldLoop);
        }
        NewLoop->addBasicBlockToLoop(New, *LI);
      }

      if (*BB == Header)
        // Loop over all of the PHI nodes in the block, changing them to use
        // the incoming values from the previous block.
        for (PHINode *OrigPHI : OrigPHINode) {
          PHINode *NewPHI = cast<PHINode>(VMap[OrigPHI]);
          Value *InVal = NewPHI->getIncomingValueForBlock(LatchBlock);
          if (Instruction *InValI = dyn_cast<Instruction>(InVal))
            if (It > 1 && L->contains(InValI))
              InVal = LastValueMap[InValI];
          VMap[OrigPHI] = InVal;
          New->getInstList().erase(NewPHI);
        }

      // Update our running map of newest clones
      LastValueMap[*BB] = New;
      for (ValueToValueMapTy::iterator VI = VMap.begin(), VE = VMap.end();
           VI != VE; ++VI)
        LastValueMap[VI->first] = VI->second;

      // Add phi entries for newly created values to all exit blocks.
      for (BasicBlock *Succ : successors(*BB)) {
        if (L->contains(Succ))
          continue;
        for (BasicBlock::iterator BBI = Succ->begin();
             PHINode *phi = dyn_cast<PHINode>(BBI); ++BBI) {
          Value *Incoming = phi->getIncomingValueForBlock(*BB);
          ValueToValueMapTy::iterator It = LastValueMap.find(Incoming);
          if (It != LastValueMap.end())
            Incoming = It->second;
          phi->addIncoming(Incoming, New);
        }
      }
      // Keep track of new headers and latches as we create them, so that
      // we can insert the proper branches later.
      if (*BB == Header)
        Headers.push_back(New);
      if (*BB == LatchBlock)
        Latches.push_back(New);

      NewBlocks.push_back(New);
      UnrolledLoopBlocks.push_back(New);

      // Update DomTree: since we just copy the loop body, and each copy has a
      // dedicated entry block (copy of the header block), this header's copy
      // dominates all copied blocks. That means, dominance relations in the
      // copied body are the same as in the original body.
      if (DT) {
        if (*BB == Header)
          DT->addNewBlock(New, Latches[It - 1]);
        else {
          auto BBDomNode = DT->getNode(*BB);
          auto BBIDom = BBDomNode->getIDom();
          BasicBlock *OriginalBBIDom = BBIDom->getBlock();
          DT->addNewBlock(
              New, cast<BasicBlock>(LastValueMap[cast<Value>(OriginalBBIDom)]));
        }
      }
    }

    // Remap all instructions in the most recent iteration
    for (BasicBlock *NewBlock : NewBlocks)
      for (Instruction &I : *NewBlock)
        ::remapInstruction(&I, LastValueMap);
  }

  // Loop over the PHI nodes in the original block, setting incoming values.
  for (PHINode *PN : OrigPHINode) {
    if (CompletelyUnroll) {
      PN->replaceAllUsesWith(PN->getIncomingValueForBlock(Preheader));
      Header->getInstList().erase(PN);
    }
    else if (Count > 1) {
      Value *InVal = PN->removeIncomingValue(LatchBlock, false);
      // If this value was defined in the loop, take the value defined by the
      // last iteration of the loop.
      if (Instruction *InValI = dyn_cast<Instruction>(InVal)) {
        if (L->contains(InValI))
          InVal = LastValueMap[InVal];
      }
      assert(Latches.back() == LastValueMap[LatchBlock] && "bad last latch");
      PN->addIncoming(InVal, Latches.back());
    }
  }

  // Now that all the basic blocks for the unrolled iterations are in place,
  // set up the branches to connect them.
  for (unsigned i = 0, e = Latches.size(); i != e; ++i) {
    // The original branch was replicated in each unrolled iteration.
    BranchInst *Term = cast<BranchInst>(Latches[i]->getTerminator());

    // The branch destination.
    unsigned j = (i + 1) % e;
    BasicBlock *Dest = Headers[j];
    bool NeedConditional = true;

    if (RuntimeTripCount && j != 0) {
      NeedConditional = false;
    }

    // For a complete unroll, make the last iteration end with a branch
    // to the exit block.
    if (CompletelyUnroll) {
      if (j == 0)
        Dest = LoopExit;
      NeedConditional = false;
    }

    // If we know the trip count or a multiple of it, we can safely use an
    // unconditional branch for some iterations.
    if (j != BreakoutTrip && (TripMultiple == 0 || j % TripMultiple != 0)) {
      NeedConditional = false;
    }

    if (NeedConditional) {
      // Update the conditional branch's successor for the following
      // iteration.
      Term->setSuccessor(!ContinueOnTrue, Dest);
    } else {
      // Remove phi operands at this loop exit
      if (Dest != LoopExit) {
        BasicBlock *BB = Latches[i];
        for (BasicBlock *Succ: successors(BB)) {
          if (Succ == Headers[i])
            continue;
          for (BasicBlock::iterator BBI = Succ->begin();
               PHINode *Phi = dyn_cast<PHINode>(BBI); ++BBI) {
            Phi->removeIncomingValue(BB, false);
          }
        }
      }
      // Replace the conditional branch with an unconditional one.
      BranchInst::Create(Dest, Term);
      Term->eraseFromParent();
    }
  }
  // Update dominators of blocks we might reach through exits.
  // Immediate dominator of such block might change, because we add more
  // routes which can lead to the exit: we can now reach it from the copied
  // iterations too. Thus, the new idom of the block will be the nearest
  // common dominator of the previous idom and common dominator of all copies of
  // the previous idom. This is equivalent to the nearest common dominator of
  // the previous idom and the first latch, which dominates all copies of the
  // previous idom.
  if (DT && Count > 1) {
    for (auto *BB : OriginalLoopBlocks) {
      auto *BBDomNode = DT->getNode(BB);
      SmallVector<BasicBlock *, 16> ChildrenToUpdate;
      for (auto *ChildDomNode : BBDomNode->getChildren()) {
        auto *ChildBB = ChildDomNode->getBlock();
        if (!L->contains(ChildBB))
          ChildrenToUpdate.push_back(ChildBB);
      }
      BasicBlock *NewIDom = DT->findNearestCommonDominator(BB, Latches[0]);
      for (auto *ChildBB : ChildrenToUpdate)
        DT->changeImmediateDominator(ChildBB, NewIDom);
    }
  }

  // Merge adjacent basic blocks, if possible.
  SmallPtrSet<Loop *, 4> ForgottenLoops;
  for (BasicBlock *Latch : Latches) {
    BranchInst *Term = cast<BranchInst>(Latch->getTerminator());
    if (Term->isUnconditional()) {
      BasicBlock *Dest = Term->getSuccessor(0);
      if (BasicBlock *Fold =
              foldBlockIntoPredecessor(Dest, LI, SE, ForgottenLoops, DT)) {
        // Dest has been folded into Fold. Update our worklists accordingly.
        std::replace(Latches.begin(), Latches.end(), Dest, Fold);
        UnrolledLoopBlocks.erase(std::remove(UnrolledLoopBlocks.begin(),
                                             UnrolledLoopBlocks.end(), Dest),
                                 UnrolledLoopBlocks.end());
      }
    }
  }

  // FIXME: We could register any cloned assumptions instead of clearing the
  // whole function's cache.
  AC->clear();

  // FIXME: We only preserve DT info for complete unrolling now. Incrementally
  // updating domtree after partial loop unrolling should also be easy.
  if (DT && !CompletelyUnroll)
    DT->recalculate(*L->getHeader()->getParent());
  else if (DT)
    DEBUG(DT->verifyDomTree());

  // Simplify any new induction variables in the partially unrolled loop.
  if (SE && !CompletelyUnroll) {
    SmallVector<WeakVH, 16> DeadInsts;
    simplifyLoopIVs(L, SE, DT, LI, DeadInsts);

    // Aggressively clean up dead instructions that simplifyLoopIVs already
    // identified. Any remaining should be cleaned up below.
    while (!DeadInsts.empty())
      if (Instruction *Inst =
              dyn_cast_or_null<Instruction>(&*DeadInsts.pop_back_val()))
        RecursivelyDeleteTriviallyDeadInstructions(Inst);
  }

  // At this point, the code is well formed.  We now do a quick sweep over the
  // inserted code, doing constant propagation and dead code elimination as we
  // go.
  const DataLayout &DL = Header->getModule()->getDataLayout();
  const std::vector<BasicBlock*> &NewLoopBlocks = L->getBlocks();
  for (BasicBlock *BB : NewLoopBlocks) {
    for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ) {
      Instruction *Inst = &*I++;

      if (Value *V = SimplifyInstruction(Inst, DL))
        if (LI->replacementPreservesLCSSAForm(Inst, V))
          Inst->replaceAllUsesWith(V);
      if (isInstructionTriviallyDead(Inst))
        BB->getInstList().erase(Inst);
    }
  }

  NumCompletelyUnrolled += CompletelyUnroll;
  ++NumUnrolled;

  Loop *OuterL = L->getParentLoop();
  // Update LoopInfo if the loop is completely removed.
  if (CompletelyUnroll)
    LI->markAsRemoved(L);

  // After complete unrolling most of the blocks should be contained in OuterL.
  // However, some of them might happen to be out of OuterL (e.g. if they
  // precede a loop exit). In this case we might need to insert PHI nodes in
  // order to preserve LCSSA form.
  // We don't need to check this if we already know that we need to fix LCSSA
  // form.
  // TODO: For now we just recompute LCSSA for the outer loop in this case, but
  // it should be possible to fix it in-place.
  if (PreserveLCSSA && OuterL && CompletelyUnroll && !NeedToFixLCSSA)
    NeedToFixLCSSA |= ::needToInsertPhisForLCSSA(OuterL, UnrolledLoopBlocks, LI);

  // If we have a pass and a DominatorTree we should re-simplify impacted loops
  // to ensure subsequent analyses can rely on this form. We want to simplify
  // at least one layer outside of the loop that was unrolled so that any
  // changes to the parent loop exposed by the unrolling are considered.
  if (DT) {
    if (!OuterL && !CompletelyUnroll)
      OuterL = L;
    if (OuterL) {
      // OuterL includes all loops for which we can break loop-simplify, so
      // it's sufficient to simplify only it (it'll recursively simplify inner
      // loops too).
      // TODO: That potentially might be compile-time expensive. We should try
      // to fix the loop-simplified form incrementally.
      simplifyLoop(OuterL, DT, LI, SE, AC, PreserveLCSSA);

      // LCSSA must be performed on the outermost affected loop. The unrolled
      // loop's last loop latch is guaranteed to be in the outermost loop after
      // LoopInfo's been updated by markAsRemoved.
      Loop *LatchLoop = LI->getLoopFor(Latches.back());
      if (!OuterL->contains(LatchLoop))
        while (OuterL->getParentLoop() != LatchLoop)
          OuterL = OuterL->getParentLoop();

      if (NeedToFixLCSSA)
        formLCSSARecursively(*OuterL, *DT, LI, SE);
      else
        assert(OuterL->isLCSSAForm(*DT) &&
               "Loops should be in LCSSA form after loop-unroll.");
    } else {
      // Simplify loops for which we might've broken loop-simplify form.
      for (Loop *SubLoop : LoopsToSimplify)
        simplifyLoop(SubLoop, DT, LI, SE, AC, PreserveLCSSA);
    }
  }

  return true;
}

/// Given an llvm.loop loop id metadata node, returns the loop hint metadata
/// node with the given name (for example, "llvm.loop.unroll.count"). If no
/// such metadata node exists, then nullptr is returned.
MDNode *llvm::GetUnrollMetadata(MDNode *LoopID, StringRef Name) {
  // First operand should refer to the loop id itself.
  assert(LoopID->getNumOperands() > 0 && "requires at least one operand");
  assert(LoopID->getOperand(0) == LoopID && "invalid loop id");

  for (unsigned i = 1, e = LoopID->getNumOperands(); i < e; ++i) {
    MDNode *MD = dyn_cast<MDNode>(LoopID->getOperand(i));
    if (!MD)
      continue;

    MDString *S = dyn_cast<MDString>(MD->getOperand(0));
    if (!S)
      continue;

    if (Name.equals(S->getString()))
      return MD;
  }
  return nullptr;
}
