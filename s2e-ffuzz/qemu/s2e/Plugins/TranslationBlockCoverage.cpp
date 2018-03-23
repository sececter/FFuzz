///
/// Copyright (C) 2016, Cyberhaven, Inc
/// All rights reserved. Proprietary and confidential.
///
/// Distributed under the terms of S2E-LICENSE
///


#include <s2e/cpu.h>

extern "C" {
#include "qlist.h"
#include "qint.h"
#include "qdict.h"
#include "qjson.h"
}

#include <s2e/S2E.h>
#include <s2e/ConfigFile.h>
#include <s2e/Utils.h>

#include "TranslationBlockCoverage.h"

namespace s2e {
namespace plugins {
namespace coverage {

S2E_DEFINE_PLUGIN(TranslationBlockCoverage, "TranslationBlockCoverage S2E plugin", "", "ModuleExecutionDetector");

namespace {

struct TBCoverageState : public PluginState
{
    ModuleTBs coverage;

    static PluginState* factory(Plugin* p, S2EExecutionState*) {
        return new TBCoverageState();
    }

    virtual TBCoverageState* clone() const {
        return new TBCoverageState(*this);
    }
};

}

void TranslationBlockCoverage::initialize()
{
    m_detector = static_cast<ModuleExecutionDetector*>(s2e()->getPlugin("ModuleExecutionDetector"));

    auto cfg = s2e()->getConfig();

    // This is mainly for debugging, in normal use would generate too many files
    bool writeCoverageOnStateKill = cfg->getBool(getConfigKey() + ".writeCoverageOnStateKill");
    if (writeCoverageOnStateKill) {
        s2e()->getCorePlugin()->onStateKill.connect(
                sigc::mem_fun(*this, &TranslationBlockCoverage::onStateKill));
    }

    m_detector->onModuleTranslateBlockComplete.connect(
            sigc::mem_fun(*this, &TranslationBlockCoverage::onModuleTranslateBlockComplete));

    s2e()->getCorePlugin()->onTimer.connect(
            sigc::mem_fun(*this, &TranslationBlockCoverage::onTimer));

    s2e()->getCorePlugin()->onUpdateStates.connect(
            sigc::mem_fun(*this, &TranslationBlockCoverage::onUpdateStates));
}

void TranslationBlockCoverage::onModuleTranslateBlockComplete(S2EExecutionState *state,
                                                   const ModuleDescriptor &module,
                                                   TranslationBlock *tb, uint64_t last_pc)
{
    TB ntb;
    ntb.startPc = module.ToNativeBase(tb->pc);
    ntb.lastPc = module.ToNativeBase(last_pc);
    ntb.startOffset = ntb.startPc - module.NativeBase;
    ntb.size = tb->size;

    DECLARE_PLUGINSTATE(TBCoverageState, state);
    auto &tbs = plgState->coverage[module.Name];
    auto newTbs = tbs.insert(ntb);
    plgState->coverage[module.Name] = newTbs;

    // Also save aggregated coverage info
    // and keep track of the states that discovered
    // new blocks so that it is easier to retrieve
    // them, e.g., every few minutes.
    bool newBlock = false;
    auto mit = m_localCoverage.find(module.Name);
    if (mit == m_localCoverage.end()) {
        newBlock = true;
    } else {
        newBlock = (*mit).second.count(ntb) == 0;
    }

    unsigned moduleidx;
    bool wasCovered = false;
    if (m_detector->getModuleId(module, &moduleidx)) {
        Bitmap *bmp = m_globalCoverage.acquire();
        bmp->setCovered(moduleidx, ntb.startOffset, ntb.size, wasCovered);
        m_globalCoverage.release();
    }

    if (newBlock) {
        m_localCoverage[module.Name].insert(ntb);
        m_newBlockStates.insert(state);
        if (!wasCovered) {
            onNewBlockCovered.emit(state);
        }
    }
}

void TranslationBlockCoverage::onUpdateStates(
        S2EExecutionState *currentState,
        const klee::StateSet &addedStates,
        const klee::StateSet &removedStates
)
{
    for (auto it: removedStates) {
        m_newBlockStates.erase(it);
    }
}

void TranslationBlockCoverage::onStateKill(S2EExecutionState *state)
{
   generateJsonCoverageFile(state);
}

void TranslationBlockCoverage::onTimer()
{
    // TODO: print number of TBs
}

const ModuleTBs& TranslationBlockCoverage::getCoverage(S2EExecutionState *state)
{
    DECLARE_PLUGINSTATE(TBCoverageState, state);
    return plgState->coverage;
}

std::string TranslationBlockCoverage::generateJsonCoverageFile(S2EExecutionState *state)
{
    std::string path;

    std::stringstream fileName;
    fileName << "tbcoverage-" << state->getID() << ".json";
    path = s2e()->getOutputFilename(fileName.str());

    generateJsonCoverageFile(state, path);

    return path;
}

void TranslationBlockCoverage::generateJsonCoverageFile(S2EExecutionState *state, const std::string &path)
{
    std::stringstream coverage;
    generateJsonCoverage(state, coverage);

    std::ofstream o(path.c_str());
    o << coverage.str();
    o.close();
}

void TranslationBlockCoverage::generateJsonCoverage(S2EExecutionState *state, std::stringstream &coverage)
{
    QDict *pt = qdict_new();

    const ModuleTBs &tbs = getCoverage(state);
    for (auto module : tbs) {
        QList *blocks = qlist_new();
        for (auto &tb : module.second) {

            QList *info = qlist_new();
            qlist_append_obj(info, QOBJECT(qint_from_int(tb.startPc)));
            qlist_append_obj(info, QOBJECT(qint_from_int(tb.lastPc)));
            qlist_append_obj(info, QOBJECT(qint_from_int(tb.size)));

            qlist_append_obj(blocks, QOBJECT(info));
        }

        qdict_put_obj(pt, module.first.c_str(), QOBJECT(blocks));
    }

    QString *json = qobject_to_json(QOBJECT(pt));

    coverage << qstring_get_str(json) << "\n";

    QDECREF(json);
    QDECREF(pt);
}

bool mergeCoverage(ModuleTBs &dest, const ModuleTBs &source)
{
    bool ret = false;

    for (const auto it : source) {
        const std::string &mod = it.first;
        const auto tbs = it.second;
        if (dest.count(mod) == 0) {
            ret = true;
        }

        unsigned prevCount = dest[mod].size();
        for (const auto &tb : tbs) {
            dest[mod] = dest[mod].insert(tb);
        }

        if (prevCount < dest[mod].size()) {
            ret = true;
        }
    }

    return ret;
}

} // namespace coverage
} // namespace plugins
} // namespace s2e
