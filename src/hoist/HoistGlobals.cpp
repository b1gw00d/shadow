/**
 * The Shadow Simulator
 *
 * Copyright (c) 2012 David Chisnall, Rob Jansen
 *
 * This file is part of Shadow.
 *
 * Shadow is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shadow is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Shadow.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "llvm/Pass.h"
#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/LLVMContext.h"
#include "llvm/Instructions.h"
#include "llvm/Constants.h"
#include "llvm/GlobalVariable.h"
#include "llvm/GlobalValue.h"
#include "llvm/Support/CallSite.h"
#include "llvm/DataLayout.h"

#ifdef DEBUG
#include "llvm/Support/raw_ostream.h"
#endif

#include <string>

#include "llvm/Analysis/Verifier.h"

using namespace llvm;
using std::string;

namespace {
class HoistGlobalsPass: public ModulePass {

private:
	// require DataLayout so we can get variable sizes
	void getAnalysisUsage(AnalysisUsage &AU) const {
		AU.addRequired<DataLayout>();
	}

public:
	static char ID;
	HoistGlobalsPass() : ModulePass(ID) {}

	bool runOnModule(Module &M) {
		bool modified = false;

		// lists to store our globals, their types, and their initial values
		SmallVector<GlobalVariable*, 16> Globals;
		SmallVector<Type*, 16> GlobalTypes;
		SmallVector<Constant*, 16> GlobalInitializers;

		for (Module::global_iterator i = M.global_begin(), e = M.global_end();
				i != e; ++i) {
			// Skip anything that isn't a global variable
			GlobalVariable *GV = dyn_cast<GlobalVariable>(i);
			if (GV) {
				// If this is not a definition, skip it
				if (GV->isDeclaration())
					continue;
				// If it's constant, it can be shared
				if (GV->isConstant())
					continue;

				/* linkage types:
				 * global -> llvm::GlobalValue::CommonLinkage
				 * static -> llvm::GlobalValue::InternalLinkage
				 * extern -> llvm::GlobalValue::ExternalLinkage
				 */
//				if(GV->hasLocalLinkage() || GV->hasExternalLinkage()) {
//					errs() << "local or extern '" << GV->getName() << "'\n";
//					GV->setLinkage(GlobalValue::CommonLinkage);
//				}

				// We found a global variable, keep track of it
				modified = true;

//				GV->setThreadLocalMode(GlobalVariable::GeneralDynamicTLSModel);
				GlobalTypes.push_back(cast<PointerType>(GV->getType())->getElementType());
				GlobalInitializers.push_back(GV->getInitializer());
				Globals.push_back(GV);
			}
		}

		// There is nothing to do if we have no globals
		if (!modified)
			return false;

		// Now we need a new structure to store all of the globals we found
		// its type is a combination of the types of all of its elements
		// we will initialize each of the elements as done previously
		// choice of: PrivateLinkage InternalLinkage ExternalLinkage CommonLinkage
		//
		// lets use thread-local storage so each thread gets its own copy
		// choice of: NotThreadLocal, GeneralDynamicTLSModel, LocalDynamicTLSModel,
		//				InitialExecTLSModel, LocalExecTLSModel

		Type *Int32Ty = Type::getInt32Ty(M.getContext());

		StructType *HoistedStructType = StructType::create(GlobalTypes,
				"hoisted_globals");
		Constant *HoistedStructInitializer = ConstantStruct::get(
				HoistedStructType, GlobalInitializers);
		GlobalVariable *HoistedStruct = new GlobalVariable(M, HoistedStructType,
				false, GlobalValue::ExternalLinkage, HoistedStructInitializer,
				"__hoisted_globals", 0, GlobalVariable::NotThreadLocal, 0);

		// and we need the size of the struct so we know how much to copy in
		// and out for each node

		uint64_t rawsize = getAnalysis<DataLayout>().getTypeStoreSize(
				HoistedStructType);
		Constant *HoistedStructSize = ConstantInt::get(Int32Ty, rawsize, false);
		GlobalVariable *HoistedSize = new GlobalVariable(M, Int32Ty, true,
				GlobalValue::ExternalLinkage, HoistedStructSize,
				"__hoisted_globals_size", 0, GlobalVariable::NotThreadLocal, 0);


#ifdef DEBUG
		errs() << "Hoisting globals: ";
#endif
		// replace all accesses to the original variables with pointer indirection
		// this replaces uses with pointers into the global struct
		uint64_t Field = 0;
		for (GlobalVariable **i = Globals.begin(), **e = Globals.end(); i != e;
				++i) {
			GlobalVariable *GV = *i;

			Constant *GEPIndexes[] = { ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, Field++) };
			Constant *GEP = ConstantExpr::getGetElementPtr(HoistedStruct, GEPIndexes, true);
//			GetElementPtrInst *GEP2 = GetElementPtrInst::Create(HoistedStruct, GEPIndexes);

			// we have to do this manually so we can preserve debug info
			GV->replaceAllUsesWith(GEP);

//			for (Value::use_iterator ui = GV->use_begin(); ui != GV->use_end(); ++ui) {
//				User *u = dyn_cast < User > (*ui);
//				if (Instruction *ins = dyn_cast < Instruction > (*ui)) {
//					MDNode *N = ins->getMetadata("dbg");
//				}
//				u->replaceUsesOfWith(GV, GEP); // this doesn't work !
//			}

#ifdef DEBUG
			errs() << GV->getName() << ", ";
#endif

			assert(GV->use_empty());
			GV->eraseFromParent();
		}

#ifdef DEBUG
		errs() << "\n";
#endif

		// now create a new pointer variable that will be loaded and evaluated
		// before accessing the hoisted globals struct

		PointerType *HoistedPointerType = PointerType::get(HoistedStructType, 0);

		GlobalVariable *HoistedPointer = new GlobalVariable(M,
				HoistedPointerType, false, GlobalValue::ExternalLinkage,
				HoistedStruct, "__hoisted_globals_pointer", 0,
				GlobalVariable::NotThreadLocal, 0);

//      Constant *GEPIndexes[] = {ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, 0)};
//      Constant *GEP = ConstantExpr::getGetElementPtr(HoistedPointer, GEPIndexes, true);
//      Constant *NewPtr = ConstantExpr::getBitCast(HoistedPointer, HoistedStruct->getType());
//      HoistedStruct->replaceAllUsesWith(GEP);

#ifndef NDEBUG
		verifyModule(M);
#endif
		return true;
	}
};

char HoistGlobalsPass::ID = 0;
RegisterPass<HoistGlobalsPass> X("hoist-globals", "Turn globals into indirections via a single pointer");
} // namespace

ModulePass *createHoistGlobalsPass() {
	return new HoistGlobalsPass();
}