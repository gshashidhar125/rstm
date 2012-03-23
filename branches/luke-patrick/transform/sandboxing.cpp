/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

#define DEBUG_TYPE "sandbox"
#include "llvm/Pass.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Value.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TypeBuilder.h"
#include "tanger.h"                     // TangerTransform pass
#include <set>
using namespace llvm;
using std::set;

// ----------------------------------------------------------------------------
// Stringification macro.
// ----------------------------------------------------------------------------
#define STRINGIFY(n) #n
#define STRINGIFY2(n) STRINGIFY(n)

// ----------------------------------------------------------------------------
// The type for our validation function. This should appear in the STM library
// archive.
// ----------------------------------------------------------------------------
extern "C" void stm_validation_full(void);
extern "C" void enter_waiver(void);
extern "C" void leave_waiver(void);

// ----------------------------------------------------------------------------
// Provide some template specializations needed by TypeBuilder. These need to
// live in the llvm namespace.
// ----------------------------------------------------------------------------
namespace llvm {
  template <bool xcompile>
  class TypeBuilder<bool, xcompile> {
    public:
      static const IntegerType* get(LLVMContext& context) {
          return TypeBuilder<types::i<1>, xcompile>::get(context);
      }
  };
}  // namespace llvm


namespace {
  STATISTIC(validations, "Number of stm_validation_full barriers inserted.");
  STATISTIC(waivers, "Number of waivers handled.");
  // --------------------------------------------------------------------------
  // A utility that gets the target function of either a call or invoke
  // instruction.
  // --------------------------------------------------------------------------
  static Function* GetTarget(CallSite call) {
      // Get the called function (NULL if this is an indirect call).
      Function* target = call.getCalledFunction();

      // There's something else to worry about here. We could have a called
      // value that's a pointer cast, in which case we need to strip the
      // pointer casts to see if we can figure out the function target.
      if (!target) {
          Value* v = call.getCalledValue();
          target = dyn_cast<Function>(v->stripPointerCasts());
      }

      return target;
  }

  // --------------------------------------------------------------------------
  // Finds the called function for a call instruction.
  // --------------------------------------------------------------------------
  static Function* GetTarget(CallInst* call) {
      if (!call)
          return NULL;

      // Treat inline asm as an opaque block of code that is equivalent to an
      // indirect call.
      if (call->isInlineAsm())
          return NULL;

      return GetTarget(CallSite(call));
  }

  // --------------------------------------------------------------------------
  // Abstracts a transactional ABI. Pure-virtual superclass so that we can
  // adapt sandboxing quickly to other ABIs.
  // --------------------------------------------------------------------------
  class TransactionRecognizer {
    public:
      virtual ~TransactionRecognizer();

      virtual bool init(Module& m) = 0;

      virtual bool isBeginMarker(Instruction*) const = 0;
      virtual bool isEndMarker(Instruction*) const = 0;
      virtual bool isReadBarrier(Instruction*) const = 0;
      virtual bool isWriteBarrier(Instruction*) const = 0;
      virtual bool isABI(Instruction*) const = 0;
      virtual bool isTransactionalClone(Function*) const = 0;
      virtual bool isWaiver(Function*) const = 0;
      virtual Function* getGetTx() const = 0;

      bool isWaiverCall(Instruction*) const;
  };

  // --------------------------------------------------------------------------
  // Recognizes the tanger-specific ABI.
  // --------------------------------------------------------------------------
  class TangerRecognizer : public TransactionRecognizer {
    public:
      TangerRecognizer();
      ~TangerRecognizer();

      bool init(Module& m);
      bool isBeginMarker(Instruction*) const;
      bool isEndMarker(Instruction*) const;
      bool isReadBarrier(Instruction*) const;
      bool isWriteBarrier(Instruction*) const;
      bool isABI(Instruction*) const;
      bool isTransactionalClone(Function*) const;
      bool isWaiver(Function*) const;
      Function* getGetTx() const;

    private:
      Function* get_tx;

      // During initialization we grab pointers to the transactional marker
      // functions that we need. These include the begin and end markers, and
      // the read and write barriers.
      SmallPtrSet<Function*, 2> begins;
      SmallPtrSet<Function*, 2> ends;

      SmallPtrSet<Function*, 64> reads;
      SmallPtrSet<Function*, 64> writes;

      SmallPtrSet<Function*, 128> all;
  };

  // --------------------------------------------------------------------------
  // Hardcode some strings that I need to deal with tanger-transactified code.
  // --------------------------------------------------------------------------
  static const char* clone_prefix = "tanger_txnal_";
  static const char* waiver_prefix = "rstm_waiver_";

  static const char* get_transaction_marker = "tanger_stm_get_tx";

  static const char* begin_transaction_markers[] = {
      "_ITM_beginTransaction"
  };

  static const char* end_transaction_markers[] = {
      "_ITM_commitTransaction",
      "_ITM_commitTransactionToId"
  };

  static const char* other_abi_markers[] = {
      "tanger_stm_constructor",
      "tanger_stm_destructor",
      "tanger_stm_get_tx",
      "tanger_stm_indirect_nb_targets_max_multi",
      "tanger_stm_indirect_nb_targets_multi",
      "tanger_stm_indirect_nb_versions.b",
      "tanger_stm_indirect_resolve_multiple",
      "tanger_stm_indirect_target_pairs_multi",
      "tanger_stm_save_restore_stack",
      "tanger_stm_std_memmove",
      "tanger_stm_std_memset",
      "tanger_stm_std_qsort",
      "_ITM_abortTransaction",
      "_ITM_beginTransaction",
      "_ITM_calloc",
      "_ITM_changeTransactionMode",
      "_ITM_commitTransaction",
      "_ITM_finalizeProcess",
      "_ITM_finalizeThread",
      "_ITM_free",
      "_ITM_getTransaction",
      "_ITM_getTransactionId",
      "_ITM_initializeProcess",
      "_ITM_initializeThread",
      "_ITM_malloc",
      "_ITM_memcpyRnWt",
      "_ITM_memcpyRnWtaR",
      "_ITM_memcpyRnWtaW",
      "_ITM_memcpyRtWn",
      "_ITM_memcpyRtWt",
      "_ITM_memcpyRtWtaR",
      "_ITM_memcpyRtWtaW",
      "_ITM_memcpyRtaRWn",
      "_ITM_memcpyRtaRWt",
      "_ITM_memcpyRtaRWtaR",
      "_ITM_memcpyRtaRWtaW",
      "_ITM_memcpyRtaWWn",
      "_ITM_memcpyRtaWWt",
      "_ITM_memcpyRtaWWtaR",
      "_ITM_memcpyRtaWWtaW",
      "_ITM_memmoveRnWt",
      "_ITM_memmoveRnWtaR",
      "_ITM_memmoveRnWtaW",
      "_ITM_memmoveRtWn",
      "_ITM_memmoveRtWt",
      "_ITM_memmoveRtWtaR",
      "_ITM_memmoveRtWtaW",
      "_ITM_memmoveRtaRWn",
      "_ITM_memmoveRtaRWt",
      "_ITM_memmoveRtaRWtaR",
      "_ITM_memmoveRtaRWtaW",
      "_ITM_memmoveRtaWWn",
      "_ITM_memmoveRtaWWt",
      "_ITM_memmoveRtaWWtaR",
      "_ITM_memmoveRtaWWtaW",
      "_ITM_memsetW",
      "_ITM_memsetWaR",
      "_ITM_memsetWaW"
  };

  static const char* read_barriers[] = {
      "_ITM_RCD",
      "_ITM_RCE",
      "_ITM_RCF",
      "_ITM_RD",
      "_ITM_RE",
      "_ITM_RF",
      "_ITM_RM128",
      "_ITM_RM64",
      "_ITM_RU1",
      "_ITM_RU2",
      "_ITM_RU4",
      "_ITM_RU8",
      "_ITM_RaRCD",
      "_ITM_RaRCE",
      "_ITM_RaRCF",
      "_ITM_RaRD",
      "_ITM_RaRE",
      "_ITM_RaRF",
      "_ITM_RaRM128",
      "_ITM_RaRM64",
      "_ITM_RaRU1",
      "_ITM_RaRU2",
      "_ITM_RaRU4",
      "_ITM_RaRU8",
      "_ITM_RaWCD",
      "_ITM_RaWCE",
      "_ITM_RaWCF",
      "_ITM_RaWD",
      "_ITM_RaWE",
      "_ITM_RaWF",
      "_ITM_RaWM128",
      "_ITM_RaWM64",
      "_ITM_RaWU1",
      "_ITM_RaWU2",
      "_ITM_RaWU4",
      "_ITM_RaWU8",
      "_ITM_RfWCD",
      "_ITM_RfWCE",
      "_ITM_RfWCF",
      "_ITM_RfWD",
      "_ITM_RfWE",
      "_ITM_RfWF",
      "_ITM_RfWM128",
      "_ITM_RfWM64",
      "_ITM_RfWU1",
      "_ITM_RfWU2",
      "_ITM_RfWU4",
      "_ITM_RfWU8"
  };

  static const char* write_barriers[] = {
      "_ITM_WCD",
      "_ITM_WCE",
      "_ITM_WCF",
      "_ITM_WD",
      "_ITM_WE",
      "_ITM_WF",
      "_ITM_WM128",
      "_ITM_WM64",
      "_ITM_WU1",
      "_ITM_WU2",
      "_ITM_WU4",
      "_ITM_WU8",
      "_ITM_WaRCD",
      "_ITM_WaRCE",
      "_ITM_WaRCF",
      "_ITM_WaRD",
      "_ITM_WaRE",
      "_ITM_WaRF",
      "_ITM_WaRM128",
      "_ITM_WaRM64",
      "_ITM_WaRU1",
      "_ITM_WaRU2",
      "_ITM_WaRU4",
      "_ITM_WaRU8",
      "_ITM_WaWCD",
      "_ITM_WaWCE",
      "_ITM_WaWCF",
      "_ITM_WaWD",
      "_ITM_WaWE",
      "_ITM_WaWF",
      "_ITM_WaWM128",
      "_ITM_WaWM64",
      "_ITM_WaWU1",
      "_ITM_WaWU2",
      "_ITM_WaWU4",
      "_ITM_WaWU8"
  };

  static const char* known_dangerous[] = {
      "__assert_fail"
  };

  // --------------------------------------------------------------------------
  // Implements the simple sandboxing pass from Transact. Looks for
  // transactionalize functions and top-level transactions to
  // instrument. Assumes that all functions and basic blocks are tainted on
  // entry.
  // --------------------------------------------------------------------------
  struct SRVEPass : public FunctionPass, public TangerRecognizer {
      static char ID;
      SRVEPass();

      bool doInitialization(Module&);
      bool doFinalization(Module&);
      bool runOnFunction(Function&);

    private:
      void visit(BasicBlock*, int);     // recursive traversal
      bool isDangerous(Instruction*) const;

      set<BasicBlock*> blocks;          // used during visit() recursion
      set<Function*> funcs;             // populated with txnly interesting fs
      IRBuilder<>* ir;                  // used to inject instrumentation
      Constant* do_validate;            // the validation function we're using
      Constant* do_enter_waiver;
      Constant* do_leave_waiver;
      SmallPtrSet<Function*, 1> dangerous; // set of functions we know are bad
      // (including things that we've waivered)
  };

  char SRVEPass::ID = 0;
  RegisterPass<SRVEPass> S("sandbox-tm", "Sandbox Tanger's Output", false,
                           false);
}

SRVEPass::SRVEPass()
  : FunctionPass(ID), TangerRecognizer(),
    blocks(), funcs(), ir(NULL),
    do_validate(NULL), do_enter_waiver(NULL), do_leave_waiver(NULL) {
}

// ----------------------------------------------------------------------------
// Populate the set of functions that we care about (i.e., those that have a
// call to get the transaction descriptor).
// ----------------------------------------------------------------------------
bool
SRVEPass::doInitialization(Module& m) {
    // init() will return false if the TangerRecognizer doesn't find the tanger
    // ABI in the module.
    if (!init(m))
        return false;

    // Find all of the uses of the get_tx ABI call (this appears in all lexical
    // transactions as well as in transactionalized functions).
    Function* f = getGetTx();
    for (Value::use_iterator i = f->use_begin(), e = f->use_end(); i != e; ++i)
    {
        CallInst* call = dyn_cast<CallInst>(*i);
        if (!call)
            report_fatal_error("User of marker is not a call instruction");

        funcs.insert(call->getParent()->getParent());
    }

    if (funcs.size() == 0)
        return false;

    // If we found any functions to transactionalize, then we initialize our
    // instruction builder and inject the validation function into the module.
    ir = new IRBuilder<>(m.getContext());
    do_validate = m.getOrInsertFunction("stm_validation_full",
        TypeBuilder<typeof(stm_validation_full), false>::get(m.getContext()));

    do_enter_waiver = m.getOrInsertFunction("stm_sandbox_set_in_lib",
        TypeBuilder<typeof(enter_waiver), false>::get(m.getContext()));

    do_leave_waiver = m.getOrInsertFunction("stm_sandbox_clear_in_lib",
        TypeBuilder<typeof(leave_waiver), false>::get(m.getContext()));

    // See if there are any of the known dangerous functions in the module.
    // Find markers that we don't care about.
    for (int i = 0, e = array_lengthof(known_dangerous); i < e; ++i)
        if (Function* f = m.getFunction(known_dangerous[i]))
            dangerous.insert(f);

    return true;
}

// ----------------------------------------------------------------------------
// Clean up the ir builder that we newed in doInitialization.
// ----------------------------------------------------------------------------
bool
SRVEPass::doFinalization(Module& m) {
    delete ir;
    return false;
}



// ----------------------------------------------------------------------------
// Process a function---called for every function in the module.
// ----------------------------------------------------------------------------
bool
SRVEPass::runOnFunction(Function& f) {
    if (isWaiver(&f)) {
        ir->SetInsertPoint(f.getEntryBlock().begin());
        ir->CreateCall(do_validate);
        ir->CreateCall(do_enter_waiver);

        for (inst_iterator i = inst_begin(f), e = inst_end(f); i != e; ++i) {
            if (isa<ReturnInst>(*i)) {
                ir->SetInsertPoint(&*i);
                ir->CreateCall(do_leave_waiver);
            }
        }
        waivers++;
        DEBUG(outs() << "handled waiverd function: " << f.getName() << "\n");
    }

    // do we care about this function?
    if (funcs.find(&f) == funcs.end())
        return false;

    // We're doing a depth-first search, and we check some assumptions about
    // the proper nesting of begin and end transaction markers. Setting the
    // depth to 1 for clones makes the logic work correctly.
    int depth = (isTransactionalClone(&f)) ? 1 : 0;
    DEBUG(if (depth) outs() << "transactional clone: " << f.getName() << "\n");

    // DFS (recursive) of the blocks in the function.
    blocks.clear();
    blocks.insert(&f.getEntryBlock());
    visit(&f.getEntryBlock(), depth);

    return true;
}

// ----------------------------------------------------------------------------
// Manages both the depth-first traversal of blocks, and the instrumentation of
// the block. Doing this DFS is the only way that we know if a block should be
// transactional or not.
// ----------------------------------------------------------------------------
void
SRVEPass::visit(BasicBlock* bb, int depth) {
    // We always assume that a basic block starts tainted.
    bool tainted = true;

    // We want to know if this basic block had a begin-transaction in it,
    // because we want to avoid instrumenting the serial-irrevocable code
    // path, if possible.
    bool had_begin = false;

    // We want to use some domain-specific knowledge to avoid instrumentation
    // on the serial-irrevocable path.

    for (BasicBlock::iterator i = bb->begin(), e = bb->end(); i != e; ++i) {
        // If we are terminating with a return, the depth should be 0 if we're
        // not processing a transactional clone. Otherwise, we're processing a
        // transactional clone and the depth should be 1. This just does error
        // checking, because we believe all returns to be safe (see paper).
        if (isa<ReturnInst>(i)) {
            if (isTransactionalClone(bb->getParent())) {
                if (depth != 1)
                    report_fatal_error("Unmatched transaction begin marker");
            } else if (depth != 0) {
                report_fatal_error("Unmatched transaction begin marker");
            }
        }

        // Begin markers increment our nesting depth. Testing for overflow can
        // help us find analysis loops.
        if (isBeginMarker(i)) {
            DEBUG(outs() << "begin transaction: " << *i << "\n");
            if (INT_MAX - depth < 1)
                report_fatal_error("Nesting error in search (overflow).");
            ++depth;
            had_begin = true;
        }

        // End marker decrements nesting depth. Underflow signifies unmatched
        // end marker along some path.
        if (isEndMarker(i)) {
            DEBUG(outs() << "end transaction: " << *i << "\n");
            if (--depth < 0)
                report_fatal_error("Unbalanced transactional end marker");
        }

        if (depth) {
            // read barriers introduce taint
            if (isReadBarrier(i)) {
                tainted = true;
                continue;
            }

            // other ABI calls are neutral
            if (isABI(i))
                continue;

            // dangerous operations cannot be executed from a potentially
            // tainted context
            if (isDangerous(i)) {
                if (tainted) {
                    ir->SetInsertPoint(i);
                    ir->CreateCall(do_validate);
                    tainted = false;
                    validations++;
                    DEBUG(outs() << " INSTRUMENTED: " << validations << "\n");
                } else {
                    DEBUG(outs() << " SRVE Suppressed.\n");
                }
            }

            // // Waivers need special handling.
            // if (isWaiverCall(i)) {
            //     ir->SetInsertPoint(i);
            //     ir->CreateCall(do_enter_waiver);
            //     BasicBlock::iterator n = i;
            //     ir->SetInsertPoint(++n);
            //     ir->CreateCall(do_leave_waiver);
            //     waivers++;
            //     // don't process the call to do_leave_waiver
            //     ++i;
            //     continue;               // waivers do not introduce taint
            // }

            // function calls and invokes introduce taint, but only after we
            // have pre-validated them
            if (isa<CallInst>(i) || isa<InvokeInst>(i))
                tainted = true;
        }
    }

    // Special case for blocks with begin transaction instructions---mark the
    // "default" target as visited. This is the serial-irrevocable block for
    // tanger transactions.
    //
    // TODO: We should a) verify this is always the case and b) abstract this
    //       into the TangerRecognizer class.
    if (had_begin) {
        SwitchInst* sw = dyn_cast<SwitchInst>(bb->getTerminator());
        assert(sw && "Expected a _ITM_beginTransaction block to terminate "
                      "with a switch");
        DEBUG(outs() << "eliding serial-irrevocable instrumentation\n");
        blocks.insert(sw->getDefaultDest());
    }

    // Done this block, continue depth first search.
    for (succ_iterator bbn = succ_begin(bb), e = succ_end(bb); bbn != e; ++bbn)
        if (blocks.insert(*bbn).second)
            visit(*bbn, depth);
}

// ----------------------------------------------------------------------------
// Encodes instruction types that we consider dangerous.
// ----------------------------------------------------------------------------
bool
SRVEPass::isDangerous(Instruction* i) const {
    // stores are always dangerous
    if (isa<StoreInst>(i)) {
        DEBUG(outs() << "dangerous store: " << *i << "... ");
        return true;
    }

    if (isa<LoadInst>(i)) {
        DEBUG(outs() << "dangerous load: " << *i << "... ELIDED\n");
        return false;
    }

    // dynamically sized allocas are dangerous
    if (AllocaInst* a = dyn_cast<AllocaInst>(i)) {
        if (a->isArrayAllocation() && !isa<Constant>(a->getArraySize())) {
            DEBUG(outs() << "dangerous alloca: " << *i << "... ");
            return true;
        }
    }

    // Indirect calls and invokes are *not* dangerous, because the tanger
    // mapping instrumentation already does a check to see if the target is
    // transactional, and goes serial irrevocable (hence validates) if it
    // isn't.

    if (CallInst* call = dyn_cast<CallInst>(i)) {
        if (call->isInlineAsm()) {
            DEBUG(outs() << "dangerous inline asm: " << *i << "... ");
            return true;
        }

        Function* target = GetTarget(call);

        if (!target) {
            // DEBUG(outs() << "indirect call: " << *i << "... ");
            // return true;
            DEBUG(outs() << "indirect call: " << *i << "... ELIDED\n");
            return false;
        }

        if (dangerous.count(target)) {
            DEBUG(outs() << "dangerous call to: " << target->getName()
                         << "... ");
            return true;
        }

        // if (isWaiver(target)) {
        //     DEBUG(outs() << "waivered call to: " << target->getName()
        //                  << "... ");
        //     return true;
        // }
    }

    if (InvokeInst* invoke = dyn_cast<InvokeInst>(i)) {
        Function* target = GetTarget(invoke);
        if (!target) {
            // DEBUG(outs() << "indirect call: " << *i << "... ");
            // return true;
            DEBUG(outs() << "indirect call: " << *i << "... ELIDED\n");
            return false;
        }

        if (dangerous.count(target)) {
            DEBUG(outs() << "dangerous invoke to: " << target->getName()
                         << "... ");
            return true;
        }

        // if (isWaiver(target)) {
        //     DEBUG(outs() << "waivered call to: " << target->getName()
        //                  << "... ");
        //     return true;
        // }
    }

    // Used to implement switches. Right now we consider these dangerous.
    if (dyn_cast<IndirectBrInst>(i)) {
        DEBUG(outs() << "dangerous indirect branch: " << *i << "... ");
        return true;
    }

    return false;
}

TransactionRecognizer::~TransactionRecognizer() {
}

bool
TransactionRecognizer::isWaiverCall(Instruction* i) const {
    return isWaiver(GetTarget(dyn_cast<CallInst>(i)));
}


TangerRecognizer::TangerRecognizer()
  : TransactionRecognizer(),
    get_tx(NULL), begins(), ends(), reads(), writes(), all() {
}

TangerRecognizer::~TangerRecognizer() {
}

bool
TangerRecognizer::init(Module& m) {
    // Check to see if there are any transactions in the module. We do this
    // using the get_transaction_marker.
    get_tx = m.getFunction(get_transaction_marker);
    if (!get_tx)
        return false;
    all.insert(get_tx);

    // Find the begin markers.
    for (int i = 0, e = array_lengthof(begin_transaction_markers); i < e; ++i) {
        if (Function* begin = m.getFunction(begin_transaction_markers[i])) {
        begins.insert(begin);
        all.insert(begin);
        }
    }

    // Find the end markers.
    for (int i = 0, e = array_lengthof(end_transaction_markers); i < e; ++i) {
        if (Function* end = m.getFunction(end_transaction_markers[i])) {
        ends.insert(end);
        all.insert(end);
    }
    }

    // Find the read barriers that are used in the module.
    for (int i = 0, e = array_lengthof(read_barriers); i < e; ++i) {
        if (Function* read = m.getFunction(read_barriers[i])) {
            reads.insert(read);
            all.insert(read);
        }
    }

    // Find the write barriers that are used in the module.
    for (int i = 0, e = array_lengthof(write_barriers); i < e; ++i) {
        if (Function* write = m.getFunction(write_barriers[i])) {
            writes.insert(write);
            all.insert(write);
        }
    }

    // Find markers that we don't care about.
    for (int i = 0, e = array_lengthof(other_abi_markers); i < e; ++i) {
        if (Function* f = m.getFunction(other_abi_markers[i])) {
            all.insert(f);
        }
    }

    return true;
}

bool
TangerRecognizer::isBeginMarker(Instruction* i) const {
    return begins.count(GetTarget(dyn_cast<CallInst>(i)));
}

bool
TangerRecognizer::isEndMarker(Instruction* i) const {
    return ends.count(GetTarget(dyn_cast<CallInst>(i)));
}

bool
TangerRecognizer::isReadBarrier(llvm::Instruction* i) const{
    return reads.count(GetTarget(dyn_cast<CallInst>(i)));
}

bool
TangerRecognizer::isWriteBarrier(llvm::Instruction* i) const {
    return writes.count(GetTarget(dyn_cast<CallInst>(i)));
}

bool
TangerRecognizer::isABI(llvm::Instruction* i) const {
    return all.count(GetTarget(dyn_cast<CallInst>(i)));
}

bool
TangerRecognizer::isTransactionalClone(Function* f) const {
    if (!f)
        return false;
    return f->getName().startswith(clone_prefix);
}

bool
TangerRecognizer::isWaiver(Function* f) const {
    if (!f)
        return false;
    return f->getName().startswith(waiver_prefix);
}

Function*
TangerRecognizer::getGetTx() const {
    return get_tx;
}
