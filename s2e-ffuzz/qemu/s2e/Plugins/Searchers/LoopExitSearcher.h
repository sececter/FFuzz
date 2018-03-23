///
/// Copyright (C) 2014-2016, Cyberhaven, Inc
/// All rights reserved. Proprietary and confidential.
///
/// Distributed under the terms of S2E-LICENSE
///


#ifndef S2E_PLUGINS_LoopExitSearcher_H
#define S2E_PLUGINS_LoopExitSearcher_H

#include <s2e/Plugin.h>
#include <s2e/Plugins/CorePlugin.h>
#include <s2e/Plugins/ModuleExecutionDetector.h>
#include <s2e/S2EExecutionState.h>
#include <s2e/Plugins/BaseInstructions.h>
#include <s2e/Plugins/LoopDetector.h>
#include <s2e/Plugins/ControlFlowGraph.h>
#include <s2e/Plugins/EdgeDetector.h>
#include <s2e/Plugins/BasicBlockCoverage.h>
#include <s2e/Plugins/EdgeCoverage.h>

#include <klee/Searcher.h>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include "Common.h"

namespace s2e {
namespace plugins {


class LoopExitSearcher : public Plugin, public klee::Searcher
{
    S2E_PLUGIN

private:


    typedef llvm::DenseMap<uint64_t, uint64_t> ForkCounts;
    typedef std::map<std::string, ForkCounts> ModuleForkCounts;

    ModuleForkCounts m_forkCount;

    LoopDetector *m_loopDetector;
    ControlFlowGraph *m_cfg;
    ModuleExecutionDetector *m_detector;
    coverage::BasicBlockCoverage *m_bbcov;
    EdgeCoverage *m_ecov;

    searchers::MultiStates m_states;
    searchers::MultiStates m_waitingStates;
    S2EExecutionState *m_currentState;

    unsigned m_timerTicks;

    void onFork(S2EExecutionState *state,
                const std::vector<S2EExecutionState*>& newStates,
                const std::vector<klee::ref<klee::Expr> >& newConditions
                );

    void onTimer();

    void increasePriority(S2EExecutionState *state, int64_t p);

public:
    LoopExitSearcher(S2E* s2e): Plugin(s2e) {}
    void initialize();


    virtual klee::ExecutionState& selectState();


    virtual void update(klee::ExecutionState *current,
                        const klee::StateSet &addedStates,
                        const klee::StateSet &removedStates);

    virtual bool empty();

private:

};


} // namespace plugins
} // namespace s2e

#endif
