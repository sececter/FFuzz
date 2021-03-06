///
/// Copyright (C) 2015-2016, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2015-2016, Cyberhaven, Inc
/// All rights reserved. Proprietary and confidential.
///
/// Distributed under the terms of S2E-LICENSE
///

#ifndef S2E_PLUGINS_CGC_INTERFACE_H
#define S2E_PLUGINS_CGC_INTERFACE_H

#include <s2e/Plugin.h>
#include <s2e/Plugins/ModuleExecutionDetector.h>
#include <s2e/Plugins/BaseInstructions.h>
#include <s2e/Plugins/BasicBlockCoverage.h>
#include <s2e/Plugins/TranslationBlockCoverage.h>
#include <s2e/Plugins/ControlFlowGraph.h>
#include <s2e/Plugins/CallSiteMonitor.h>
#include <s2e/Plugins/StaticLibraryFunctionModels.h>
#include <s2e/Plugins/Searchers/SeedSearcher.h>
#include <s2e/Plugins/CGC/ExploitGenerator.h>
#include <s2e/Plugins/CGC/Recipe/Recipe.h>
#include <s2e/Synchronization.h>

#include <unordered_set>
#include <vector>

namespace s2e {
namespace plugins {

enum TC_TYPES { TC_EXP1, TC_EXP2, TC_TEST, TC_CRASH, TC_TIMEOUT };
enum BB_TYPES { FUNC_ENTRY = 1, ICALL_TGT = 1 << 1, IJMP_TGT = 1 << 2 , JIT_ENTRY = 1 << 3};


class POVGenerator;
class CGCMonitor;
class ModuleExecutionDetector;


struct CBStats {
    /* Whether the CB called the random() syscall at some point */
    bool calledRandom;

    /* Stores program counters of branches that depend on a random value */
    llvm::DenseSet<uint64_t> randomBranchesPc;
};

class CGCInterface : public Plugin
{
    S2E_PLUGIN

    typedef ExploitGenerator::TestCaseType TestCaseType;

    enum ExplorationState {
        /*
         * Let S2E find crashes in the first seconds before trying
         * to use any seeds.
         */
        WARM_UP,

        /* Wait for new seeds to become available */
        WAIT_FOR_NEW_SEEDS,

        /* Wait for the guest to actually fetch the seed */
        WAIT_SEED_SCHEDULING,

        /* Wait for a while that the seeds executes */
        WAIT_SEED_EXECUTION
    };

private:
    friend class CGCInterfaceState;
    CGCMonitor* m_monitor;
    ModuleExecutionDetector* m_detector;
    ProcessExecutionDetector* m_procDetector;
    POVGenerator* m_povGenerator;
    ExploitGenerator *m_exploitGenerator;
    coverage::BasicBlockCoverage *m_coverage;
    coverage::TranslationBlockCoverage *m_tbcoverage;
    ControlFlowGraph *m_cfg;
    CallSiteMonitor *m_csTracker;
    seeds::SeedSearcher *m_seedSearcher;
    recipe::Recipe *m_recipe;
    StaticLibraryFunctionModels *m_models;


    typedef std::set<u_int64_t> AddressSet;
    typedef std::unordered_map<uint64_t, uint32_t> BBFlags;
    BBFlags m_bbFlags;

    uint64_t m_maxPovCount;
    bool m_recordAllPaths;
    bool m_recordConstraints;
    bool m_disableSendingExtraDataToDB; // data is used by fuzzer
    unsigned m_lowPrioritySeedThreshold;

    std::unordered_map<std::string, CBStats> m_cbStats;
    uint64_t m_cbStatsLastSent;
    unsigned m_cbStatsUpdateInterval;
    bool m_cbStatsChanged;

    // Completed paths since last notification
    unsigned m_completedPaths;

    // Number of completed seeds since last notification
    unsigned m_completedSeeds;

    unsigned m_maxCompletedPathDepth;
    unsigned m_maxPathDepth;
    int m_lastReportedStateCount;

    ExplorationState m_explorationState;
    uint64_t m_stateMachineTimeout;
    uint64_t m_coverageTimeout;
    uint64_t m_timeOfLastCoverageReport;
    uint64_t m_timeOfLastCoveredBlock;
    uint64_t m_timeOfLastCrash;
    uint64_t m_timeOfLastHighPrioritySeed;
    uint64_t m_timeOfLastFetchedSeed;

    coverage::GlobalCoverage m_coveredTbs;
    coverage::ModuleTBs m_localCoveredTbs;

    void onSeed(const seeds::Seed &seed, seeds::SeedEvent event);

    typedef POVGenerator::PovOptions PovOptions;
    typedef POVGenerator::PovType PovType;

    bool sendCoveragePov(S2EExecutionState *state, TestCaseType tctype);

    void onStateKill(S2EExecutionState* state);

    void onRandom(S2EExecutionState *state, uint64_t pid, const std::vector<klee::ref<klee::Expr>> &);

    void onRandomInputFork(S2EExecutionState *state,
                           const ModuleDescriptor *module);

    void onProcessForkComplete(bool isChild);

    void onTimer();

    void onPovReady(S2EExecutionState* state, const PovOptions &opt,
                    const std::string &recipeName,
                    const std::string &xmlFilename,
                    const std::string &cFilename,
                    TestCaseType tcType);

    void sendTestcase(S2EExecutionState *state, const std::string &xmlPovPath, const std::string &cPovPath,
                      TestCaseType tcType,
                      const PovOptions &opt, const std::string &recipeName = "");

    void constraintsToJson(S2EExecutionState* state, std::stringstream &output);
    std::string constraintsToJsonFile(S2EExecutionState* state);

    void onNewBlockCovered(S2EExecutionState *state);
    void onSegFault(S2EExecutionState *state, uint64_t pid, uint64_t address);

    void processSeedStateMachine(uint64_t currentTime);
    void processIntermediateCoverage(uint64_t currentTime);

    bool updateCoverage(S2EExecutionState *state);

public:
    CGCInterface(S2E* s2e) : Plugin(s2e) {}
    void initialize();
}; // class CGCInterface

} // namespace plugins
} // namespace s2e

#endif // S2E_PLUGINS_CGC_INTERFACE_H
