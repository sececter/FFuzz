///
/// Copyright (C) 2010-2016, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2016, Cyberhaven, Inc
/// All rights reserved. Proprietary and confidential.
///
/// Distributed under the terms of S2E-LICENSE
///


#include <s2e/cpu.h>

#include "CacheSim.h"

#include <s2e/S2E.h>
#include <s2e/ConfigFile.h>
#include <s2e/S2EExecutionState.h>
#include <s2e/Utils.h>

#include <llvm/Support/TimeValue.h>

#include <iostream>
#include <stdlib.h>
#include <stdio.h>

#define CACHESIM_LOG_SIZE 4096

namespace s2e {
namespace plugins {

using namespace std;
using namespace klee;

/** Returns the floor form of binary logarithm for a 32 bit integer.
    (unsigned) -1 is returned if n is 0. */
uint64_t floorLog2(uint64_t n);
uint64_t floorLog2(uint64_t n) {
    int pos = 0;
    if (n >= 1<<16) { n >>= 16; pos += 16; }
    if (n >= 1<< 8) { n >>=  8; pos +=  8; }
    if (n >= 1<< 4) { n >>=  4; pos +=  4; }
    if (n >= 1<< 2) { n >>=  2; pos +=  2; }
    if (n >= 1<< 1) {           pos +=  1; }
    return ((n == 0) ? ((uint64_t)-1) : pos);
}

/* Model of n-way accosiative write-through LRU cache */
class Cache {
protected:
    uint64_t m_size;
    uint64_t m_associativity;
    uint64_t m_lineSize;

    uint64_t m_indexShift; // log2(m_lineSize)
    uint64_t m_indexMask;  // 1 - setsCount

    uint64_t m_tagShift;   // m_indexShift + log2(setsCount)

    std::vector<uint64_t> m_lines;

    std::string m_name;
    uint8_t m_cacheId;

    Cache* m_upperCache;

public:
    uint64_t getSize() const {
        return m_size;
    }

    uint64_t getAssociativity() const {
        return m_associativity;
    }

    uint64_t getLineSize() const {
        return m_lineSize;
    }

    uint8_t getId() const {
        return m_cacheId;
    }

    void setId(uint8_t id) {
        m_cacheId = id;
    }


    Cache(const Cache &c) {
        m_size = c.m_size;
        m_associativity = c.m_associativity;
        m_lineSize = c.m_lineSize;
        m_indexShift = c.m_indexShift;
        m_indexMask = c.m_indexMask;
        m_tagShift = c.m_tagShift;
        m_lines = c.m_lines;
        m_name = c.m_name;
        m_cacheId = c.m_cacheId;
        m_upperCache = NULL;
    }

    Cache(const std::string& name,
          uint64_t size, uint64_t associativity,
          uint64_t lineSize, uint64_t cost = 1, Cache* upperCache = NULL)
        : m_size(size), m_associativity(associativity), m_lineSize(lineSize),
          m_name(name), m_upperCache(upperCache)
    {
        assert(size && associativity && lineSize);

        assert(uint64_t(1LL<<floorLog2(associativity)) == associativity);
        assert(uint64_t(1LL<<floorLog2(lineSize)) == lineSize);

        uint64_t setsCount = (size / lineSize) / associativity;
        assert(setsCount && uint64_t(1LL << floorLog2(setsCount)) == setsCount);

        m_indexShift = floorLog2(m_lineSize);
        m_indexMask = setsCount-1;

        m_tagShift = floorLog2(setsCount) + m_indexShift;

        m_lines.resize(setsCount * associativity, (uint64_t) -1);
    }

    const std::string& getName() const { return m_name; }

    Cache* getUpperCache() { return m_upperCache; }
    void setUpperCache(Cache* cache) { m_upperCache = cache; }

    /** Models a cache access. A misCount is an array for miss counts (will be
        passed to the upper caches), misCountSize is its size. Array
        must be zero-initialized. */
    void access(uint64_t address, uint64_t size,
            bool isWrite, unsigned* misCount, unsigned misCountSize)
    {

        uint64_t s1 = address >> m_indexShift;
        uint64_t s2 = (address+size-1) >> m_indexShift;

        if(s1 != s2) {
            /* Cache access spawns multiple lines */
            uint64_t size1 = m_lineSize - (address & (m_lineSize - 1));
            access(address, size1, isWrite, misCount, misCountSize);
            access((address & ~(m_lineSize-1)) + m_lineSize, size-size1,
                                   isWrite, misCount, misCountSize);
            return;
        }

        uint64_t set = s1 & m_indexMask;
        uint64_t l = set * m_associativity;
        uint64_t tag = address >> m_tagShift;

        for(unsigned i = 0; i < m_associativity; ++i) {
            if(m_lines[l + i] == tag) {
                /* Cache hit. Move line to MRU. */
                for(unsigned j = i; j > 0; --j)
                    m_lines[l + j] = m_lines[l + j - 1];
                m_lines[l] = tag;
                return;
            }
        }

        //g_s2e->getDebugStream() << "Miss at 0x" << std::hex << address << '\n';
        /* Cache miss. Install new tag as MRU */
        misCount[0] += 1;
        for(unsigned j = m_associativity-1; j > 0; --j)
            m_lines[l + j] = m_lines[l + j - 1];
        m_lines[l] = tag;

        if(m_upperCache) {
            assert(misCountSize > 1);
            m_upperCache->access(address, size, isWrite,
                                 misCount+1, misCountSize-1);
        }
    }
};

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

CacheSimState::CacheSimState()
{
    m_i1_length = 0;
    m_d1_length = 0;
    m_i1 = NULL;
    m_d1 = NULL;
}

CacheSimState::CacheSimState(S2EExecutionState *s, Plugin *p)
{
    CacheSim *csp = dynamic_cast<CacheSim*>(p);
    S2E *s2e = csp->s2e();

    const std::string &cfgKey = p->getConfigKey();
    ConfigFile* conf = s2e->getConfig();

    if (csp->m_d1_connection.connected()) {
        csp->m_d1_connection.disconnect();
    }

    if (csp->m_i1_connection.connected()) {
        csp->m_i1_connection.disconnect();
    }


    //Initialize the cache configuration
    vector<string> caches = conf->getListKeys(cfgKey + ".caches");
    foreach2(it, caches.begin(), caches.end()) {
        string key = cfgKey + ".caches." + (*it);
        Cache* cache = new Cache(*it,
                                 conf->getInt(key + ".size"),
                                 conf->getInt(key + ".associativity"),
                                 conf->getInt(key + ".lineSize"));

        m_caches.insert(make_pair(*it, cache));
    }

    foreach2(ci, m_caches.begin(), m_caches.end()) {
        string key = cfgKey + ".caches." + (*ci).first + ".upper";
        if(conf->hasKey(key))
            (*ci).second->setUpperCache(getCache(conf->getString(key)));
    }

    if(conf->hasKey(cfgKey + ".i1"))
        m_i1 = getCache(conf->getString(cfgKey + ".i1"));

    if(conf->hasKey(cfgKey + ".d1"))
        m_d1 = getCache(conf->getString(cfgKey + ".d1"));

    m_i1_length = 0;
    m_d1_length = 0;


    s2e->getInfoStream() << "Instruction cache hierarchy:";
    for(Cache* c = m_i1; c != NULL; c = c->getUpperCache()) {
        m_i1_length += 1;
        s2e->getInfoStream() << " -> " << c->getName();
    }
    s2e->getInfoStream() << " -> memory" << '\n';

    s2e->getInfoStream() << "Data cache hierarchy:";
    for(Cache* c = m_d1; c != NULL; c = c->getUpperCache()) {
        m_d1_length += 1;
        s2e->getInfoStream() << " -> " << c->getName();
    }
    s2e->getInfoStream() << " -> memory" << '\n';


    if (csp->m_execDetector && csp->m_startOnModuleLoad){
        s2e->getDebugStream()  << "Connecting to onModuleTranslateBlockStart" << '\n';
        csp->m_ModuleConnection = csp->m_execDetector->onModuleTranslateBlockStart.connect(
                sigc::mem_fun(*csp, &CacheSim::onModuleTranslateBlockStart));

    }else {
        if(m_d1) {
            s2e->getDebugStream()  << "CacheSim: connecting to onAfterSymbolicDataMemoryAccess" << '\n';
            s2e->getCorePlugin()->onAfterSymbolicDataMemoryAccess.connect(
                sigc::mem_fun(*csp, &CacheSim::onAfterSymbolicDataMemoryAccess));
        }

        if(m_i1) {
            s2e->getDebugStream()  << "CacheSim: connecting to onTranslateBlockStart" << '\n';
            s2e->getCorePlugin()->onTranslateBlockStart.connect(
             sigc::mem_fun(*csp, &CacheSim::onTranslateBlockStart));
        }
    }

}

CacheSimState::~CacheSimState()
{
    foreach2(ci, m_caches.begin(), m_caches.end()) {
        delete (*ci).second;
    }
}

PluginState *CacheSimState::factory(Plugin *p, S2EExecutionState *s)
{
    p->getDebugStream() << "Creating initial CacheSimState" << '\n';
    CacheSimState *ret = new CacheSimState(s, p);
    return ret;

}

PluginState *CacheSimState::clone() const
{
    CacheSimState *ret = new CacheSimState(*this);

    //Clone the caches first
    CachesMap::iterator newCaches;
    for (newCaches = ret->m_caches.begin(); newCaches != ret->m_caches.end(); ++newCaches) {
        (*newCaches).second = new Cache(*(*newCaches).second);
    }

    //Update the upper cache mappings
    CachesMap::const_iterator oldCaches;
    for (oldCaches = m_caches.begin(); oldCaches != m_caches.end(); ++oldCaches) {
        Cache *u;
        if (!(u = (*oldCaches).second->getUpperCache())) {
            //Don't need to reset the upper cache pointer because
            //we clear it in the copy constructor
            continue;
        }

        CachesMap::iterator newCache = ret->m_caches.find(u->getName());
        assert(newCache != ret->m_caches.end());
        (*newCache).second->setUpperCache((*newCache).second);
    }

    ret->m_d1 = ret->m_caches[m_d1->getName()];
    assert(ret->m_d1);

    ret->m_i1 = ret->m_caches[m_i1->getName()];
    assert(ret->m_i1);

    return ret;
}

inline Cache* CacheSimState::getCache(const std::string& name)
{
    CachesMap::iterator it = m_caches.find(name);
    if(it == m_caches.end()) {
        cerr << "ERROR: cache " << name << " undefined" << endl;
        exit(1);
    }
    return it->second;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////


S2E_DEFINE_PLUGIN(CacheSim, "Cache simulator", "", "ExecutionTracer");

CacheSim::~CacheSim()
{

}


void CacheSim::initialize()
{
    assert(false && "Need to implement onConcreteDataMemoryAccess");

    ConfigFile* conf = s2e()->getConfig();

    m_execDetector = (ModuleExecutionDetector*)s2e()->getPlugin("ModuleExecutionDetector");
    m_Tracer = (ExecutionTracer*)s2e()->getPlugin("ExecutionTracer");

    if (!m_execDetector) {
        getInfoStream() << "ModuleExecutionDetector not found, will profile the whole system" << '\n';
    }

    //Report misses form the entire system
    m_reportWholeSystem = conf->getBool(getConfigKey() + ".reportWholeSystem");

    //Option to write only those instructions that cause misses
    m_reportZeroMisses = conf->getBool(getConfigKey() + ".reportZeroMisses");

    //Profile only for the selected modules
    m_profileModulesOnly = conf->getBool(getConfigKey() + ".profileModulesOnly");

    //Start cache profiling when the first configured module is loaded
    m_startOnModuleLoad = conf->getBool(getConfigKey() + ".startOnModuleLoad");

    //Determines whether to address the cache physically of virtually
    m_physAddress = conf->getBool(getConfigKey() + ".physicalAddressing");

    m_cacheStructureWrittenToLog = false;

    ////////////////////
    //XXX: trick to force the initialization of the cache upon first memory access.
    m_d1_connection = s2e()->getCorePlugin()->onAfterSymbolicDataMemoryAccess.connect(
         sigc::mem_fun(*this, &CacheSim::onAfterSymbolicDataMemoryAccess));

    m_i1_connection = s2e()->getCorePlugin()->onTranslateBlockStart.connect(
         sigc::mem_fun(*this, &CacheSim::onTranslateBlockStart));


    m_cacheLog.reserve(CACHESIM_LOG_SIZE);


}

void CacheSim::writeCacheDescriptionToLog(S2EExecutionState *state)
{
    if (m_cacheStructureWrittenToLog) {
        return;
    }

    DECLARE_PLUGINSTATE(CacheSimState, state);

    //Output the names of the caches
    uint8_t cacheId = 0;
    foreach2(it, plgState->m_caches.begin(), plgState->m_caches.end()) {
        (*it).second->setId(cacheId++);
        uint32_t retsize;
        ExecutionTraceCacheSimName *n = ExecutionTraceCacheSimName::allocate((*it).second->getId(),
                                                                             (*it).first, &retsize);

        m_Tracer->writeData(state, n, retsize, TRACE_CACHESIM);
        ExecutionTraceCacheSimName::deallocate(n);
        m_Tracer->flush();
    }

    //Output the configuration of the caches
    foreach2(it, plgState->m_caches.begin(), plgState->m_caches.end()) {
        ExecutionTraceCacheSimParams p;
        p.type = CACHE_PARAMS;
        p.size = (*it).second->getSize();
        p.associativity = (*it).second->getAssociativity();
        p.lineSize = (*it).second->getLineSize();

        if ((*it).second->getUpperCache()) {
            p.upperCacheId = (*it).second->getUpperCache()->getId();
        }else {
            p.upperCacheId = (unsigned)-1;
        }

        p.cacheId = (*it).second->getId();
        m_Tracer->writeData(state, &p, sizeof(p), TRACE_CACHESIM);
        m_Tracer->flush();
    }

    m_Tracer->flush();
    m_cacheStructureWrittenToLog = true;
}

//Connect the tracing when the first module is loaded
void CacheSim::onModuleTranslateBlockStart(
    ExecutionSignal* signal,
    S2EExecutionState *state,
    const ModuleDescriptor &desc,
    TranslationBlock *tb, uint64_t pc)
{

    DECLARE_PLUGINSTATE(CacheSimState, state);

    getDebugStream() << "Module translation CacheSim " << desc.Name << "  " <<
        pc <<'\n';

    if(plgState->m_d1)
        s2e()->getCorePlugin()->onAfterSymbolicDataMemoryAccess.connect(
            sigc::mem_fun(*this, &CacheSim::onAfterSymbolicDataMemoryAccess));

    if(plgState->m_i1)
        s2e()->getCorePlugin()->onTranslateBlockStart.connect(
            sigc::mem_fun(*this, &CacheSim::onTranslateBlockStart));

    //We connected ourselves, do not need to monitor modules anymore.
    getDebugStream()  << "Disconnecting module translation cache sim" << '\n';
    m_ModuleConnection.disconnect();
}


bool CacheSim::profileAccess(S2EExecutionState *state) const
{
    //Check whether to profile only known modules
    if (!m_reportWholeSystem) {
        if (m_execDetector && m_profileModulesOnly) {
            if (!m_execDetector->getCurrentDescriptor(state)) {
                return false;
            }
        }
    }

    return true;
}

bool CacheSim::reportAccess(S2EExecutionState *state) const
{
    bool doLog = m_reportWholeSystem;

    if (!m_reportWholeSystem) {
        if (m_execDetector) {
            return (m_execDetector->getCurrentDescriptor(state) != NULL);
        }else {
            return false;
        }
    }

    return doLog;
}

void CacheSim::onMemoryAccess(S2EExecutionState *state,
                              uint64_t address, unsigned size,
                              bool isWrite, bool isIO, bool isCode)
{
    if(isIO) /* this is only an estimation - should look at registers! */
        return;

    DECLARE_PLUGINSTATE(CacheSimState, state);

    if (!profileAccess(state)) {
        return;
    }


    Cache* cache = isCode ? plgState->m_i1 : plgState->m_d1;
    if(!cache)
        return;

    //Done only on the first invocation
    writeCacheDescriptionToLog(state);

    unsigned missCountLength = isCode ? plgState->m_i1_length : plgState->m_d1_length;
    unsigned missCount[missCountLength];
    memset(missCount, 0, sizeof(missCount));
    cache->access(address, size, isWrite, missCount, missCountLength);

    //Decide whether to log the access in the database
    if (!reportAccess(state)) {
        return;
    }

    unsigned i = 0;
    for(Cache* c = cache; c != NULL; c = c->getUpperCache(), ++i) {
        if (m_reportZeroMisses || missCount[i]) {
                ExecutionTraceCacheSimEntry e;
                e.type = CACHE_ENTRY;
                e.cacheId = c->getId();
                e.pc = state->getPc();
                e.address = address;
                e.size = size;
                e.isWrite = isWrite;
                e.isCode = isCode;
                e.missCount = missCount[i];
                m_Tracer->writeData(state, &e, sizeof(e), TRACE_CACHESIM);
        }

        if(missCount[i] == 0)
            break;
    }
}

void CacheSim::onAfterSymbolicDataMemoryAccess(S2EExecutionState *state,
                              klee::ref<klee::Expr> address,
                              klee::ref<klee::Expr> hostAddress,
                              klee::ref<klee::Expr> value,
                              unsigned flags)
{
    if(!isa<ConstantExpr>(hostAddress)) {
        getWarningsStream()
                << "Warning: CacheSim do not support symbolic addresses"
                << '\n';
        return;
    }

    uint64_t constAddress;
    unsigned size = Expr::getMinBytesForWidth(value->getWidth());

    if (m_physAddress) {
        constAddress = cast<ConstantExpr>(hostAddress)->getZExtValue(64);
  //      getDebugStream() << "acc pc=" << std::hex << state->getPc() << " ha=" << constAddress << '\n';
    }else {
        constAddress = cast<ConstantExpr>(address)->getZExtValue(64);
    }

    onMemoryAccess(state, constAddress, size,
                   flags & MEM_TRACE_FLAG_WRITE,
                   flags & MEM_TRACE_FLAG_IO, false);
}

void CacheSim::onExecuteBlockStart(S2EExecutionState *state, uint64_t pc,
                                   TranslationBlock *tb, uint64_t hostAddress)
{
//    getDebugStream() << "exec pc=" << std::hex << pc << " ha=" << hostAddress << '\n';
    onMemoryAccess(state, m_physAddress ? hostAddress : pc, tb->size, false, false, true);
}

void CacheSim::onTranslateBlockStart(ExecutionSignal *signal,
                                     S2EExecutionState *state,
                                     TranslationBlock *tb,
                                     uint64_t)
{
    uint64_t newPc;

    if (m_physAddress) {
        newPc = state->getHostAddress(tb->pc);
        //getDebugStream() << "tb pc=" << std::hex << tb->pc << " ha=" << newPc << '\n';
    }else {
        newPc = tb->pc;
    }

    signal->connect(sigc::bind(
            sigc::mem_fun(*this, &CacheSim::onExecuteBlockStart), tb, newPc));
}


} // namespace plugins
} // namespace s2e
