#include <stdio.h>
#include <stdlib.h>

#include <llvm/Pass.h>
#include <llvm/IR/Module.h>
#include "llvm/IR/LLVMContext.h"
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/IR/Attributes.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Casting.h>
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include <list>
#include <vector>

#include "reomp_IR_pass.h"
#include "reomp_rr.h"
#include "reomp_mem.h"
#include "mutil.h"



#define REOMP_VERSION (1)
bool ReOMP::on_main_function(Function &F)
{
#if REOMP_VERSION == 0
  Module *M = F.getParent();
  const Module::GlobalListType &G_list = M->getGlobalList();
  for (const GlobalValue &G: G_list) {
    
    errs() << "--> " << G.getName() << "\n";
  }
  //  errs() << "--> " << M->getGlobalVariable("sum_global")->getName() << "\n";
#elif REOMP_VERSION == 1
#else
#endif
  return false;
}

bool ReOMP::responsible_arg_var(Argument *A) 
{
  Type *ATy = A->getType();
  uint64_t dbytes = A->getDereferenceableBytes();
  A->print(errs());
  errs() << "\n";
  errs() << A->getName() << "\n";
  if (ATy->isPointerTy()) {
    if (dbytes > 0) { 
      errs() << "We record this\n";
      return true;
    }
  } else {
    errs() << "Non-pointer arguments passed to .omp_outlined\n";
    exit(0);
  }
  return false;
}

bool ReOMP::responsible_global_var(Value* value)
{
  StringRef var_name;
  if (!value->getType()->isPointerTy()) {
    errs() << "Non pointer global variables referenced in .omp_outlined\n";
    exit(0);
  }

  var_name = value->getName();
  if (var_name.startswith("llvm.")) return false;
  if (var_name.startswith("__kmpc_")) return false;
  if (var_name.startswith(".")) return false;
  if (var_name == "") return false;
  return true;
}

reomp_omp_rr_data* ReOMP::create_omp_rr_data()
{
  reomp_omp_rr_data *omp_rr_data = new reomp_omp_rr_data();
  omp_rr_data->global_var_uset = new unordered_set<GlobalVariable*>();
  omp_rr_data->arg_list = new list<Value*>();
  return omp_rr_data;
}
void ReOMP::free_omp_rr_data(reomp_omp_rr_data* omp_rr_data)
{
  delete omp_rr_data->global_var_uset;
  delete omp_rr_data->arg_list;
  delete omp_rr_data;
  return;
}



void ReOMP::get_responsible_global_vars(Function* omp_outlined_F, unordered_set<GlobalVariable*> *omp_global_vars_uset)
{
  if (omp_global_vars_uset->size() != 0) {
    errs() << "omp_global_vars_uset is not empty\n";
    exit(0);
  }

  for (BasicBlock &BB : *omp_outlined_F) {
    for (Instruction &I : BB) {
      for (Value *Op : I.operands()) {
	if (GlobalVariable *G = dyn_cast<GlobalVariable>(Op)) {
	  /* Get only user defined global variable */
	  //if (is_user_variable(G->getName())) {
	  if (responsible_global_var(G)) {
	    if (omp_global_vars_uset->find(G) == omp_global_vars_uset->end()) {
	      omp_global_vars_uset->insert(G);
	    }
	  }
	}
      }
    }
  }
  return;
}

void ReOMP::get_responsible_data(CallInst *kmpc_fork_CI, reomp_omp_rr_data *omp_rr_data)
{
  Function *omp_outlined_F;

  /* Get all function arguments to be recorded and replayed */
  extract_omp_function(kmpc_fork_CI, &omp_outlined_F, omp_rr_data->arg_list);
  /* Get all global variables to be recorded and replayed */
  get_responsible_global_vars(omp_outlined_F, omp_rr_data->global_var_uset);

  return;
}




bool ReOMP::on_omp_function(Function &F)
{
    

  return false;
}

bool ReOMP::is_fork_call(CallInst *CI)
{
  return CI->getCalledFunction()->getName() == "__kmpc_fork_call";
}


/*
  e.g.) Input: 
  
  CallInst fork_CI = 
      call 
      void (%ident_t*, i32, void (i32*, i32*, ...)*, ...) 
      fork_func = @__kmpc_fork_call(
        %ident_t* nonnull %7, 
	i32 6, 
	void (i32*, i32*, ...)* bitcast (void (i32*, i32*, i32*, double*, i32*, i32*, [10 x double]*, [10 x double]*)* @.omp_outlined. to void (i32*, i32*, ...)*), 
        i32* nonnull %1, 
	double* nonnull %6, 
	i32* nonnull %2, 
	i32* nonnull %5, 
	[10 x double]* nonnull %3, 
	[10 x double]* nonnull %4
    ) !dbg !98

*/
void ReOMP::extract_omp_function(CallInst *fork_CI, Function **omp_func, list<Value*> *omp_func_arg_list)
{
  Function *fork_func;
  int first_arg_index = -1;
  fork_func = dyn_cast<Function>(fork_CI->getCalledValue());
  /*
    fork_func = @__kmpc_fork_call(
        %ident_t* nonnull %7, 
	i32 6, 
	void (i32*, i32*, ...)* bitcast (void (i32*, i32*, i32*, double*, i32*, i32*, [10 x double]*, [10 x double]*)* @.omp_outlined. to void (i32*, i32*, ...)*), 
        i32* nonnull %1, 
	double* nonnull %6, 
	i32* nonnull %2, 
	i32* nonnull %5, 
	[10 x double]* nonnull %3, 
	[10 x double]* nonnull %4
    )
   */


  *omp_func = NULL;
  for (unsigned op = 0, Eop = fork_CI->getNumArgOperands(); op < Eop; ++op) {
    Value *vv =fork_CI->getArgOperand(op);
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(vv)) {
      /*
	CE = void (i32*, i32*, ...)* bitcast (void (i32*, i32*, i32*, double*, i32*, i32*, [10 x double]*, [10 x double]*)* @.omp_outlined. to void (i32*, i32*, ...)*), 
       */
      Function *omp_func_v = dyn_cast<Function>(CE->getOperand(0));
      /*
	omp_func_v = void (i32*, i32*, i32*, double*, i32*, i32*, [10 x double]*, [10 x double]*)* @.omp_outlined.
       */
      if (omp_func_v) {
	if (*omp_func == NULL) {
	  *omp_func = omp_func_v;
	  first_arg_index = op + 1;
	} else {
	  errs() << "Two .omp_outlined functions\n";
	  exit(0);
	}
      }
    }      
  }

  if (*omp_func == NULL) {
    errs() << "Could not find .omp_outlined. function\n";
    exit(0);    
  }


  for (unsigned op = first_arg_index, Eop = fork_CI->getNumArgOperands(); op < Eop; ++op) {
    Value *arg =fork_CI->getArgOperand(op);
    omp_func_arg_list->push_back(arg);
  }
  return;
}

void ReOMP::insert_func(Instruction *I, BasicBlock *BB, int offset, string func_name, vector<Value*> &arg_vec)
{
  IRBuilder<> builder(I);
  builder.SetInsertPoint(BB, (offset)? ++builder.GetInsertPoint():builder.GetInsertPoint());
  Constant* func = reomp_func_umap.at(func_name);
  builder.CreateCall(func, arg_vec);
  return;
}

void ReOMP::insert_func(Instruction *I, BasicBlock *BB, int offset, string func_name)
{
  IRBuilder<> builder(I);
  builder.SetInsertPoint(BB, (offset)? ++builder.GetInsertPoint():builder.GetInsertPoint());
  //  builder.SetInsertPoint(BB, builder.GetInsertPoint());
  Constant* func = reomp_func_umap.at(func_name);
  builder.CreateCall(func);
  return;
}

int ReOMP::insert_rr(BasicBlock *BB, CallInst *kmpc_fork_CI, reomp_omp_rr_data *omp_rr_data)
{
  int is_created = 0;
  LLVMContext &ctx = BB->getContext();
  vector<Value*> arg_vec;

  for (Value* GV: *(omp_rr_data->global_var_uset)) {
    arg_vec.clear();
    if (GV->getType()->isPointerTy()) {
      arg_vec.push_back(GV);
      arg_vec.push_back(ConstantInt::get(Type::getInt64Ty(ctx), 0));
      insert_func(kmpc_fork_CI, BB, REOMP_IR_PASS_INSERT_AFTER, "reomp_rr", arg_vec);
      is_created++;
    }
  }
  
  for (Value* AG: *(omp_rr_data->arg_list)) {
    arg_vec.clear();
    arg_vec.push_back(AG);
    arg_vec.push_back(ConstantInt::get(Type::getInt64Ty(ctx), 0));
    insert_func(kmpc_fork_CI, BB, REOMP_IR_PASS_INSERT_AFTER, "reomp_rr", arg_vec);
    is_created++;
  }
  return is_created;
}

/* Outer Handlers */
int ReOMP::handle_function(Function &F)
{
  int modified_counter = 0;
  modified_counter += ci_mem_memory_hook_on_main(F);
  return modified_counter;
}

int ReOMP::ci_mem_memory_hook_on_main(Function &F)
{
  int modified_counter = 0;
  int is_hook_enabled = 0;
  if (F.getName() == "main") {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
	if (!is_hook_enabled) {
	  modified_counter += insert_mem_enable_hook(BB, I);
	  is_hook_enabled = 1;
	}
	if (dyn_cast<ReturnInst>(&I)) {
	  modified_counter += insert_mem_disable_hook(BB, I);
	} 
      }
    }
  } 
  return modified_counter;
}

int ReOMP::handle_basicblock(Function &F, BasicBlock &BB)
{
  return 0;
}

int ReOMP::handle_instruction(Function &F, BasicBlock &BB, Instruction &I)
{
  int modified_counter = 0;
  modified_counter += ci_mem_register_local_var_addr_on_alloca(F, BB, I);
  modified_counter += ci_rr_insert_rr_on_omp_func(F, BB, I);  
  return modified_counter;
}

int ReOMP::ci_rr_insert_rr_on_omp_func(Function &F, BasicBlock &BB, Instruction &I)
{
  CallInst *CI;
  string name;
  if (!(CI = dyn_cast<CallInst>(&I))) return 0;
  name = CI->getCalledValue()->getName();
  if (F.getName().startswith(".omp_outlined.")) return 0;
  if (!(name == "__kmpc_fork_call")) return 0;

  //  omp_rr_data = create_omp_rr_data();
  //  get_responsible_data(CI, omp_rr_data);
  //  insert_rr(&BB, CI, omp_rr_data);
  vector<Value*> arg_vec;
  arg_vec.push_back(ConstantPointerNull::get(Type::getInt64PtrTy(*REOMP_CTX)));
  arg_vec.push_back(ConstantInt::get(Type::getInt64Ty(*REOMP_CTX), 0));
  insert_func(CI, &BB, REOMP_IR_PASS_INSERT_AFTER, "reomp_rr", arg_vec);

  return 1;
}


int ReOMP::ci_mem_register_local_var_addr_on_alloca(Function &F, BasicBlock &BB, Instruction &I)
{
  AllocaInst *AI;
  if (!(AI = dyn_cast<AllocaInst>(&I))) { 
    return 0;  
  }
  if (F.getName().startswith(".omp_outlined.")) return 0;

  //  Module *M = F.getParent();
  //  DataLayout *DL = new DataLayout(REO);

  /* TODO: reuse vector */
  vector<Value*> arg_vec;
  size_t type_size;

  arg_vec.push_back(AI);
  type_size = REOMP_DL->getTypeSizeInBits(AI->getAllocatedType());
  arg_vec.push_back(ConstantInt::get(Type::getInt64Ty(*REOMP_CTX), type_size));
  insert_func(AI, &BB, REOMP_IR_PASS_INSERT_AFTER, REOMP_MEM_REGISTER_LOCAL_VAR_ADDR, arg_vec);

  return 1;
}

howto_type ReOMP::get_howto_handle(Function &F, Instruction &I, int *meta)
{
  // string name;

  // if (F.getName() == "main") {
  //   if (dyn_cast<ReturnInst>(&I)) {
  //     return HOWTO_TYPE_DISABLE_HOOK;
  //   } else if (!is_hook_enabled) {
  //     return HOWTO_TYPE_ENABLE_HOOK;
  //   }
  // }  

  // if (CallInst *CI = dyn_cast<CallInst>(&I)) {
  //   name = CI->getCalledValue()->getName();
  //   if (name == "__kmpc_fork_call") {
  //     return HOWTO_TYPE_OMP_FUNC;
  //   } else if (name == "malloc") {
  //     return HOWTO_TYPE_DYN_ALLOC;
  //   } else if (name == "calloc") {
  //     return HOWTO_TYPE_DYN_ALLOC;
  //   } else if (name == "realloc") {
  //     return HOWTO_TYPE_DYN_ALLOC;
  //   }
  // }
  return HOWTO_TYPE_OTHERS;
}

int ReOMP::handle_omp_func(BasicBlock &BB, Instruction &I)
{
  reomp_omp_rr_data *omp_rr_data;
  CallInst *CI;
  bool is_modified;
  omp_rr_data = create_omp_rr_data();
  CI = dyn_cast<CallInst>(&I);
  get_responsible_data(CI, omp_rr_data);
  is_modified = insert_rr(&BB, CI, omp_rr_data);
  free_omp_rr_data(omp_rr_data);
  return 1;
}

int ReOMP::insert_mem_enable_hook(BasicBlock &BB, Instruction &I)
{
  insert_func(&I, &BB, REOMP_IR_PASS_INSERT_BEFORE, REOMP_RR_INIT_STR);
  return 1;
}

int ReOMP::insert_mem_disable_hook(BasicBlock &BB, Instruction &I)
{
  insert_func(&I, &BB, REOMP_IR_PASS_INSERT_BEFORE, REOMP_RR_FINALIZE_STR);    
  return 1;
}


int ReOMP::handle_dyn_alloc(BasicBlock &BB, Instruction &I)
{
  LLVMContext &ctx = BB.getContext();
  vector<Value*> arg_vec;
  Value *Vptr, *Vsize;
  CallInst *CI;

  Vptr  = dyn_cast<Value>(&I);
  CI    = dyn_cast<CallInst>(&I);
  Vsize = CI->getArgOperand(0);
  errs() << "====" << *Vsize << "\n";
  arg_vec.push_back(Vptr);
  arg_vec.push_back(Vsize);
  insert_func(CI, &BB, REOMP_IR_PASS_INSERT_AFTER, "reomp_mem_on_alloc", arg_vec);
  return 1;
}

void ReOMP::init_inserted_functions(Module &M)
{

  LLVMContext &ctx = M.getContext();
  reomp_func_umap["reomp_rr"] = M.getOrInsertFunction("reomp_rr", 
							 Type::getInt64PtrTy(ctx), 
							 Type::getInt64Ty(ctx), 
							 NULL);

  // reomp_func_umap["reomp_mem_on_alloc"] = M.getOrInsertFunction("reomp_mem_on_alloc", 
  // 							 Type::getInt64PtrTy(ctx), 
  // 							 Type::getInt64Ty(ctx), 
  // 							 NULL);

  // reomp_func_umap["reomp_mem_on_free"] = M.getOrInsertFunction("reomp_mem_on_free", 
  // 							 Type::getInt64PtrTy(ctx), 
  //							 NULL);

  reomp_func_umap[REOMP_MEM_REGISTER_LOCAL_VAR_ADDR] = M.getOrInsertFunction(REOMP_MEM_REGISTER_LOCAL_VAR_ADDR,
							 Type::getInt64PtrTy(ctx), 
							 Type::getInt64Ty(ctx), 
							 NULL);
  
  reomp_func_umap[REOMP_RR_INIT_STR] = M.getOrInsertFunction(REOMP_RR_INIT_STR,
							     Type::getVoidTy(ctx),
							     NULL);

  reomp_func_umap[REOMP_RR_FINALIZE_STR] = M.getOrInsertFunction(REOMP_RR_FINALIZE_STR,
							     Type::getVoidTy(ctx),
							     NULL);

  reomp_func_umap[REOMP_MEM_ENABLE_HOOK] = M.getOrInsertFunction(REOMP_MEM_ENABLE_HOOK, 
								 Type::getVoidTy(ctx), 
								 NULL);

  reomp_func_umap[REOMP_MEM_DISABLE_HOOK] = M.getOrInsertFunction(REOMP_MEM_DISABLE_HOOK,
								 Type::getVoidTy(ctx), 
								 NULL);

  reomp_func_umap[REOMP_MEM_PRINT_ADDR] = M.getOrInsertFunction(REOMP_MEM_PRINT_ADDR,
								  Type::getInt64PtrTy(ctx),
								  NULL);

  return;
}

bool ReOMP::doInitialization(Module &M)
{
  REOMP_M   = &M;
  REOMP_CTX = &(M.getContext());
  REOMP_DL  = new DataLayout(REOMP_M);


  init_inserted_functions(M);


  return true;
}

bool ReOMP::runOnFunction(Function &F)
{
  int modified_counter = 0;
  modified_counter += handle_function(F);
  for (BasicBlock &BB : F) {
    modified_counter += handle_basicblock(F, BB);
    for (Instruction &I : BB) {
      modified_counter += handle_instruction(F, BB, I);
    }
  }
  return modified_counter > 0;
}


void ReOMP::getAnalysisUsage(AnalysisUsage &AU) const
{
  //  AU.setPreservesCFG();
  // addReauired cause errors
  //  AU.addRequired<DependenceAnalysisWrapperPass>();
}

char ReOMP::ID = 0;
//static RegisterPass<DASample> A("dasample", "dasample", false, false);

static void registerReOMP(const PassManagerBuilder &, legacy::PassManagerBase &PM)
{
  PM.add(new ReOMP);
}

static RegisterStandardPasses RegisterReOMP(PassManagerBuilder::EP_EarlyAsPossible, registerReOMP);


/* ============== End of class ================================= */

