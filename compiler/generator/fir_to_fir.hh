/************************************************************************
 ************************************************************************
    FAUST compiler
	Copyright (C) 2003-2004 GRAME, Centre National de Creation Musicale
    ---------------------------------------------------------------------
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 ************************************************************************
 ************************************************************************/

#ifndef _FIR_TO_FIR_H
#define _FIR_TO_FIR_H

#include "instructions.hh"
#include "code_container.hh"

#ifdef _WIN32
bool sortArrayDeclarations(StatementInst* a, StatementInst* b);
#else
bool sortArrayDeclarations(StatementInst* a, StatementInst* b);
#endif

bool sortTypeDeclarations(StatementInst* a, StatementInst* b);

// Change stack access for struct access
struct Stack2StructAnalyser : public DispatchVisitor {
    
    string fName;
    
    void visit(NamedAddress* address)
    {
        if (address->fAccess == Address::kStack && address->fName.find(fName) != string::npos) {
            address->fAccess = Address::kStruct;
        }
    }
    
    Stack2StructAnalyser(const string& name):fName(name)
    {}
};

// Analysis to promote stack variables to struct variables
struct Stack2StructAnalyser1 : public DispatchVisitor {
    
    CodeContainer* fContainer;
    string fName;
    
    // TODO : also rewrite value memory access
    void visit(DeclareVarInst* inst)
    {
        BasicCloneVisitor cloner;
        string name = inst->fAddress->getName();
        
        if (inst->fAddress->getAccess() == Address::kStack && name.find(fName) != string::npos) {
            
            // Variable moved to the Struct
            fContainer->pushDeclare(InstBuilder::genDecStructVar(name, inst->fType->clone(&cloner)));
            
            // For local thread access (in compute), rewrite the Declare instruction by a Store
            if (inst->fValue) {
                fContainer->pushComputeBlockMethod(InstBuilder::genStoreStructVar(name, inst->fValue->clone(&cloner)));
            }
            
            // Mark inst to be removed
            inst->fAddress->setAccess(Address::kLink);
        }
        
        // Then dispatch and possibly rewrite 'value' access
        DispatchVisitor::visit(inst);
    }
    
    void visit(NamedAddress* address)
    {
        if (address->fAccess == Address::kStack && address->fName.find(fName) != string::npos) {
            address->fAccess = Address::kStruct;
        }
    }
    
    Stack2StructAnalyser1(CodeContainer* container, const string& name)
        :fContainer(container), fName(name)
    {}
    
};

struct VariableMover {
    
    static void Move(CodeContainer* container, const string& name)
    {
        // Transform stack variables in struct variables
        Stack2StructAnalyser1 analyser1(container, name);
        container->generateComputeBlock(&analyser1);
        
        // Variable access stack ==> struct
        Stack2StructAnalyser analyser2(name);
        container->transformDAG(&analyser2);
    }
};

// Remove all variable declarations marked as "Address::kLink"
struct RemoverCloneVisitor : public BasicCloneVisitor {

    // Rewrite Declare as a no-op (DropInst)
    StatementInst* visit(DeclareVarInst* inst)
    {
        if (inst->fAddress->getAccess() == Address::kLink) {
            return InstBuilder::genDropInst();
        } else {
            return BasicCloneVisitor::visit(inst);
        }
    }
};

/*
 For subcontainers table generation : rename 'sig' in 'dsp' and remove 'dsp' allocation
 (using in ASMJavaScript and Interpreter backend)
*/

struct DspRenamer : public BasicCloneVisitor {
    
    DspRenamer()
    {}
    
    // change access
    virtual Address* visit(NamedAddress* named)
    {
        if (startWith(named->getName(), "sig")) {
            return InstBuilder::genNamedAddress("dsp", named->fAccess);
        } else {
            return BasicCloneVisitor::visit(named);
        }
    }
    
    // remove allocation
    virtual StatementInst* visit(DeclareVarInst* inst)
    {
        if (startWith(inst->fAddress->getName(), "sig")) {
            return InstBuilder::genDropInst();
        } else {
            BasicCloneVisitor cloner;
            return inst->clone(&cloner);
        }
    }
    
    BlockInst* getCode(BlockInst* src)
    {
        return dynamic_cast<BlockInst*>(src->clone(this));
    }
    
};

// Replace a void function call with the actual inlined function code

struct InlineVoidFunctionCall : public BasicCloneVisitor {
    
    DeclareFunInst* fFunction;
    
    InlineVoidFunctionCall(DeclareFunInst* function):fFunction(function)
    {}
    
    BlockInst* ReplaceParametersByArgs(BlockInst* code, list<NamedTyped*> args_type, list<ValueInst*> args, bool ismethod)
    {
        //std::cout << "ReplaceParametersByArgs " << fFunction->fName << std::endl;
        
        list<NamedTyped*>::iterator it1 = args_type.begin();
        list<ValueInst*>::iterator it2 = args.begin(); if (ismethod) { it2++; }
        
        for (; it1 != args_type.end(); it1++, it2++) {
            code = ReplaceParameterByArg(code, *it1, *it2);
        }
        
        return code;
    }
    
    BlockInst* ReplaceParameterByArg(BlockInst* code, NamedTyped* type, ValueInst* arg)
    {
        //std::cout << "ReplaceParameterByArg " << type->fName << std::endl;
        
        struct InlineValue : public BasicCloneVisitor {
            
            string fName;
            ValueInst* fArg;
            
            InlineValue(const string& name, ValueInst* arg):fName(name), fArg(arg)
            {}
            
            ValueInst* visit(LoadVarInst* inst)
            {
                BasicCloneVisitor cloner;
                //std::cout << "ReplaceParameterByArg LoadVarInst " << fName << " " << inst->fAddress->getName()<< std::endl;
                
                if (inst->fAddress->getName() == fName) {
                    return fArg->clone(&cloner);
                } else {
                    return inst->clone(&cloner);
                }
            }
            
            StatementInst* visit(StoreVarInst* inst)
            {
                BasicCloneVisitor cloner;
                //std::cout << "ReplaceParameterByArg StoreVarInst " << fName << " " << inst->fAddress->getName()<< std::endl;
                
                LoadVarInst* arg;
                if ((inst->fAddress->getName() == fName) && (arg = dynamic_cast<LoadVarInst*>(fArg))) {
                    Address* cloned_address = inst->fAddress->clone(this);
                    cloned_address->setName(arg->fAddress->getName());
                    return new StoreVarInst(cloned_address, inst->fValue->clone(this));
                } else {
                    return inst->clone(&cloner);
                }
            }
            
            
            BlockInst* getCode(BlockInst* src)
            {
                return dynamic_cast<BlockInst*>(src->clone(this));
            }
        };
        
        InlineValue value_inliner(type->fName, arg);
        return value_inliner.getCode(code);
    }
    
    virtual StatementInst* visit(DropInst* inst)
    {
        FunCallInst* fun_call;
        if (inst->fResult
            && (fun_call = dynamic_cast<FunCallInst*>(inst->fResult))
            && fun_call->fName == fFunction->fName) {
            return ReplaceParametersByArgs(fFunction->fCode, fFunction->fType->fArgsTypes, fun_call->fArgs, fun_call->fMethod);
        } else {
            BasicCloneVisitor cloner;
            return inst->clone(&cloner);
        }
    }
   
    BlockInst* getCode(BlockInst* src)
    {
        return dynamic_cast<BlockInst*>(src->clone(this));
    }
};

#endif