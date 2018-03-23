///
/// Copyright (C) 2013-2015, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2016, Cyberhaven, Inc
/// All rights reserved. Proprietary and confidential.
///
/// Distributed under the terms of S2E-LICENSE
///


#include <s2e/S2E.h>
#include <s2e/ConfigFile.h>
#include <s2e/Utils.h>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include "StackClustering.h"

namespace s2e {
namespace plugins {

S2E_DEFINE_PLUGIN(StackClustering, "Aggregates the call stack in all states", "", "StackMonitor", "ModuleExecutionDetector",
                  "ControlFlowGraph", "Interceptor");

void StackClustering::initialize()
{
    m_stackMonitor = static_cast<StackMonitor*>(s2e()->getPlugin("StackMonitor"));
    m_detector = static_cast<ModuleExecutionDetector*>(s2e()->getPlugin("ModuleExecutionDetector"));
    m_monitor = static_cast<OSMonitor*>(s2e()->getPlugin("Interceptor"));
    m_cfg = static_cast<ControlFlowGraph*>(s2e()->getPlugin("ControlFlowGraph"));

    s2e()->getCorePlugin()->onStateFork.connect(
        sigc::mem_fun(*this, &StackClustering::onStateFork)
    );

    s2e()->getCorePlugin()->onTimer.connect(
        sigc::mem_fun(*this, &StackClustering::onTimer)
    );

    s2e()->getCorePlugin()->onUpdateStates.connect(
        sigc::mem_fun(*this, &StackClustering::onUpdateStates)
    );


    m_timer = 0;
    m_interval = s2e()->getConfig()->getInt(getConfigKey() + ".interval", 10);
}

void StackClustering::computeCallStack(S2EExecutionState *originalState, calltree::CallStack &cs, calltree::Location &loc)
{
    assert(false && "Upgrade to new apis");

#if 0 // This does not compile anymore

    if (!originalState->regs()->initialized()) {
        //Handle the case when the initial state is not initialized yet
        return;
    }

    //Retrieve the updated stack in the current state
    StackMonitor::CallStacks callStacks;
    m_stackMonitor->getCallStacks(originalState, callStacks);

    uint64_t pc = originalState->getPc();
    loc.first = "<unknown>";
    loc.second = pc;
    const ModuleDescriptor *desc = m_detector->getModule(originalState, originalState->getPc(), false);
    if (desc) {
        pc = desc->ToNativeBase(pc);
        loc.second = pc;
        loc.first = desc->Name;
    }

    uint64_t stackBase, stackSize;
    unsigned currentStackIndex = 0;


    if (originalState->isActive() && originalState->getTb()) {
        //S2E can read virtual memory from active states only
        if (m_monitor->getCurrentStack(originalState, &stackBase, &stackSize)) {
            //States are clustered in priority by the current call stack
            foreach2 (cit, callStacks.begin(), callStacks.end()) {
                const StackMonitor::CallStack &smcs = *cit;
                foreach2(sfit, smcs.begin(), smcs.end()) {
                    const StackFrameInfo &sf = *sfit;
                    if (sf.StackBase == stackBase && sf.StackSize == stackSize) {
                        goto next;
                    }
                    break;
                }
                currentStackIndex++;
            }
        }
    }

    next:

    getInfoStream(originalState)
            << "StackClustering: computeCallStack: "
            << " stack_index: " << currentStackIndex
            << " stack_count: " << callStacks.size() << "\n";

    unsigned index = 0;
    foreach2 (cit, callStacks.begin(), callStacks.end()) {
        if (index != currentStackIndex) {
            ++index;
            continue;
        }

        const StackMonitor::CallStack &smcs = *cit;

        //Translate the call frames to the CallTree format
        foreach2(sfit, smcs.begin(), smcs.end()) {
            const StackFrameInfo &sf = *sfit;
            calltree::CallStackEntry entry;

            /** Convert the function address to relative */
            entry.Module = "<unknown>";
            entry.FunctionAddress = sf.FrameFunction;
            desc = m_detector->getModule(originalState, sf.FrameFunction, false);
            if (desc) {
                entry.FunctionAddress = desc->ToNativeBase(entry.FunctionAddress);
                entry.Module = desc->Name;
            }

            /** Convert the return address to relative */
            entry.ReturnAddress = sf.FramePc;
            desc = m_detector->getModule(originalState, sf.FramePc, false);
            if (desc) {
                entry.ReturnAddress = desc->ToNativeBase(entry.ReturnAddress);
            }

            entry.FunctionName = "<unknown>";
            m_cfg->getFunctionName(entry.Module, entry.FunctionAddress, entry.FunctionName);

            getInfoStream(originalState) << "  Module: " << entry.Module <<
                    " FunctionAddress: " << hexval(entry.FunctionAddress) <<
                    " (" << entry.FunctionName << ")" <<
                    " ReturnAddress: " << hexval(entry.ReturnAddress) << "\n";

            cs.push_back(entry);
        }

        //Will add the first call stack only, CallTree does not support states
        //with multiple call stacks for now.
        break;
    }
#endif
}


void StackClustering::onStateFork(S2EExecutionState *originalState,
                            const std::vector<S2EExecutionState*>& newStates,
                            const std::vector<klee::ref<klee::Expr> >& newConditions)
{

    m_callTree.remove(originalState);

    calltree::CallStack cs;
    calltree::Location loc;

    computeCallStack(originalState, cs, loc);

    foreach2(it, newStates.begin(), newStates.end()) {
        S2EExecutionState *state = *it;
        m_callTree.add(state, cs, loc);
    }
}

void StackClustering::print()
{
    std::stringstream ss;
    ss << "cfg" << m_timer << ".dot";

    std::string filename = s2e()->getOutputFilename(ss.str());

    std::error_code error;
    llvm::raw_fd_ostream ofs(filename.c_str(), error, llvm::sys::fs::F_None);
    if (error) {
        getWarningsStream() << "Could not open " << filename << " - " << error.message() << "\n";
        return;
    }

    calltree::CallTreeDotPrinter<S2EExecutionState*>(ofs).visit(m_callTree);
    ofs.flush();
}

void StackClustering::onTimer()
{
    ++m_timer;
    if ((m_timer % m_interval) != 0) {
        return;
    }

    print();
}

void StackClustering::onUpdateStates(S2EExecutionState *state,
                                     const klee::StateSet &addedStates,
                                     const klee::StateSet &removedStates)
{
    foreach2(it, addedStates.begin(), addedStates.end()) {
        S2EExecutionState *ss = static_cast<S2EExecutionState*>(*it);
        calltree::CallStack cs;
        calltree::Location loc;

        computeCallStack(ss, cs, loc);
        m_callTree.add(ss, cs, loc);
    }

    foreach2(it, removedStates.begin(), removedStates.end()) {
        S2EExecutionState *ss = static_cast<S2EExecutionState*>(*it);
        m_callTree.remove(ss);
    }
}

void StackClustering::add(S2EExecutionState *state)
{
    calltree::CallStack cs;
    calltree::Location loc;

    computeCallStack(state, cs, loc);
    m_callTree.add(state, cs, loc);
}

} // namespace plugins
} // namespace s2e
