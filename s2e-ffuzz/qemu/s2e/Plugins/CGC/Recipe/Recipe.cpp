///
/// Copyright (C) 2016, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2016, Cyberhaven, Inc
/// All rights reserved. Proprietary and confidential.
///
/// Distributed under the terms of S2E-LICENSE
///


#include <s2e/cpu.h>

/// We currenlty try recipes in these cases:
///   - on every function call, in this case recipe must have
/// EIP precondition that matches call target
///   - when EIP becomes symbolic
///
/// After EIP becomes symbolic, we continue state execution. It could come
/// from jump table, in which case we need to explore all possible values.

#include <s2e/S2E.h>
#include <s2e/S2EExecutor.h>
#include <klee/util/ExprUtil.h>
#include <klee/util/ExprTemplates.h>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include <algorithm>

#include "Recipe.h"
#include "MemoryHelpers.h"

using namespace klee;

namespace s2e {
namespace plugins {
namespace recipe {

S2E_DEFINE_PLUGIN(Recipe, "Support PoV recipes", "",
        "ProcessExecutionDetector", "ModuleExecutionDetector", "StackMonitor", "SeedSearcher", "KeyValueStore");

/// \brief Group ID for CUPASearcherGroupClass
/// Used by CUPA searcher to group states where symbolic address vulnerability was handled.
/// Actual value doesn't matter, the only requirement is to be unique. Chosen at random.
#define CUPA_GROUP_SYMBOLIC_ADDRESS     "28153"
#define CUPA_GROUP_FLAG_PAGE_ADDRESS    "28154"
#define CUPA_GROUP_FLAG_READ_SYM_MEM    "28155"

class RecipeState: public PluginState
{
public:
    RecipeState()
    {
    }

    RecipeState(const RecipeState &o)
    {
    }

    virtual RecipeState* clone() const
    {
        return new RecipeState(*this);
    }

    static PluginState *factory(Plugin *p, S2EExecutionState *s)
    {
        return new RecipeState();
    }
};

void Recipe::initialize()
{
    ConfigFile *cfg = s2e()->getConfig();

    m_recipesDir = cfg->getString(getConfigKey() + ".recipes_dir");

    loadRecipesFromDirectory(m_recipesDir);

    m_flagPage = cfg->getInt(getConfigKey() + ".flag_page", 0x4347c000);

    m_monitor = s2e()->getPlugin<CGCMonitor>();
    m_process = s2e()->getPlugin<ProcessExecutionDetector>();
    m_detector = s2e()->getPlugin<ModuleExecutionDetector>();
    m_stackMonitor = s2e()->getPlugin<StackMonitor>();
    m_seedSearcher = s2e()->getPlugin<seeds::SeedSearcher>();
    m_keyValueStore = s2e()->getPlugin<KeyValueStore>();

    m_monitor->onSymbolicBuffer.connect(sigc::mem_fun(*this, &Recipe::onSymbolicBuffer));

    s2e()->getCorePlugin()->onTranslateJumpStart.connect(sigc::mem_fun(*this, &Recipe::onTranslateJumpStart));
    s2e()->getCorePlugin()->onTranslateBlockEnd.connect(sigc::mem_fun(*this, &Recipe::onTranslateBlockEnd));
    s2e()->getCorePlugin()->onSymbolicAddress.connect(sigc::mem_fun(*this, &Recipe::onSymbolicAddress));
    s2e()->getCorePlugin()->onTranslateICTIStart.connect(sigc::mem_fun(*this, &Recipe::onTranslateICTIStart));
    s2e()->getCorePlugin()->onBeforeSymbolicDataMemoryAccess.connect(sigc::mem_fun(*this, &Recipe::onBeforeSymbolicDataMemoryAccess));

    s2e()->getCorePlugin()->onTimer.connect(sigc::mem_fun(*this, &Recipe::onTimer));
}


///
/// \brief Loads all recipes from the given directory
///
/// This function can be called multiple times to load
/// recipes that have been added since the last invocation.
///
/// Any file in the directory is assumed to be a recipe.
///
/// \param directory Location of the recipes
///
void Recipe::loadRecipesFromDirectory(const std::string &directory)
{
    if (!llvm::sys::fs::exists(directory) || !llvm::sys::fs::is_directory(directory)) {
        getWarningsStream() << directory << " does not exist/is not a dir\n";
        exit(-1);
    }

    std::error_code error;

    for (llvm::sys::fs::directory_iterator i(directory, error), e; i != e; i.increment(error)) {
        std::string entry = i->path();
        llvm::sys::fs::file_status status;
        error = i->status(status);

        if (error) {
            getWarningsStream() << "Error when querying " << entry << " - "
                                << error.message() << '\n';
            continue;
        }

        // Do not descend recursively for now
        if (status.type() == llvm::sys::fs::file_type::directory_file) {
            continue;
        }

        std::string recipeName = llvm::sys::path::filename(entry);

        // Don't reload recipes needlessly.
        if (m_recipes.count(recipeName)) {
            continue;
        }

        RecipeDescriptor *desc = RecipeDescriptor::fromFile(entry);
        if (!desc) {
            // Do not modify the string, it will be reused by the service to check for failures
            getWarningsStream() << "Failed to parse recipe: " << recipeName << "\n";
            ++m_stats.invalidRecipeCount;
            continue;
        }

        getDebugStream() << "Loaded recipe " << recipeName << "\n";

        m_recipes[recipeName] = desc;
    }
}

void Recipe::onTimer()
{
    uint64_t curTime = llvm::sys::TimeValue::now().seconds();

    if ((curTime - m_lastRecipeLoadTime) > 5) {
        getDebugStream() << m_stats << "\n";
        getDebugStream() << "Scanning for new recipes...\n";
        loadRecipesFromDirectory(m_recipesDir);
        m_lastRecipeLoadTime = llvm::sys::TimeValue::now().seconds();
    }
}

/* This handler is called before jump is made */
void Recipe::onTranslateJumpStart(ExecutionSignal *signal, S2EExecutionState *state, TranslationBlock *tb, uint64_t pc,
        int jump_type)
{
    if ((tb->flags & HF_CPL_MASK) != 3) {
        return;
    }

    if (!m_process->isTracked(state)) {
        return;
    }

    if (jump_type == JT_RET || jump_type == JT_LRET) {
        signal->connect(sigc::mem_fun(*this, &Recipe::onBeforeRet));
    }
}

/* This handler is called after jump is made */
void Recipe::onTranslateBlockEnd(ExecutionSignal *signal, S2EExecutionState *state, TranslationBlock *tb, uint64_t pc,
                                 bool isStatic, uint64_t staticTarget)
{
    if ((tb->flags & HF_CPL_MASK) != 3) {
        return;
    }

    if (!m_process->isTracked(state)) {
        return;
    }

    if (tb->se_tb_type == TB_CALL_IND || tb->se_tb_type == TB_CALL) {
        signal->connect(sigc::mem_fun(*this, &Recipe::onAfterCall));
    }
}

void Recipe::onBeforeRet(S2EExecutionState *state, uint64_t pc)
{
    // This is handled in onSymbolicPc
}

void Recipe::onAfterCall(S2EExecutionState *state, uint64_t callInstructionPc)
{
    getDebugStream(state) << "Handling call " << hexval(callInstructionPc) << " -> " << hexval(state->getPc()) << "\n";

    RecipeConditions recipeConditions;
    StateConditions stateConditions;
    stateConditions.eipType = EIPType::CONCRETE_EIP;

    // NOTE: use call instruction PC, state->getPc() may contain concretized target address
    if (!getCurrentModule(state, callInstructionPc, stateConditions.module)) {
        return; // Can't try recipes without current module!
    }

    tryRecipes(state, stateConditions, recipeConditions);
}

void Recipe::onTranslateICTIStart(ExecutionSignal *signal, S2EExecutionState *state, TranslationBlock *tb,
                                  uint64_t pc, int rm, int op, int offset)
{

    if ((tb->flags & HF_CPL_MASK) != 3) {
        return;
    }

    if (!m_process->isTracked(state)) {
        return;
    }

    signal->connect(sigc::bind(sigc::mem_fun(*this, &Recipe::handleICTI), rm, op, offset));
}

void Recipe::handleICTI(S2EExecutionState *state, uint64_t pc, unsigned rm, int op, int offset)
{
    getDebugStream(state) << "handleICTI\n";

    if (rm > CPU_OFFSET(eip)) {
        getWarningsStream(state) << "handleICTI: Invalid rm value: " << rm << "\n";
        return;
    }

    ref<Expr> regExpr = state->readCpuRegister(CPU_OFFSET(regs[rm]), state->getPointerWidth());
    s2e_assert(state, !regExpr.isNull(), "Failed to read register " << rm);

    Register r = Register(static_cast<Register::Reg>(rm));

    if (op == 2) {
        getDebugStream(state) << hexval(pc) << ": call   DWORD PTR [" << r.regName() << "+" << hexval(offset) << "]\n";
    } else if (op == 4) {
        getDebugStream(state) << hexval(pc) << ": jmp   DWORD PTR [" << r.regName() << "+" << hexval(offset) << "]\n";
    } else {
        getWarningsStream(state) << "Unhandled indirect CTI @ " << hexval(pc) << "!\n";
        return;
    }

    if (!isa<ConstantExpr>(regExpr)) {
        tryRecipesOnICTI(state, regExpr, r, offset);
    } // else is handled by onSymbolicAddress
}

void Recipe::tryRecipesOnICTI(S2EExecutionState *state, ref<Expr> regExpr, Register reg, int ictiOffset)
{
    StateConditions stateConditions;
    if (!getCurrentModule(state, state->getPc(), stateConditions.module)) {
        return;
    }

    std::vector<AddrSize> sequences;
    FindSequencesOfSymbolicData(state, m_monitor->getMemoryMap(state), false, sequences);

    if (!sequences.size()) {
        getDebugStream(state) << "No symbolic byte sequences to handle ICTI\n";
        return;
    }

    std::sort(sequences.rbegin(), sequences.rend());

    // Try every suitable memory location for our address. We can't verify if
    // the identified 4 bytes for eip will be ok, since we still don't know
    // what bytes will be put there for the recipe. As a consequence, if no
    // recipe succeeds, let's move on to the next byte. ATM, we stop as soon as
    // at least one successfull recipe is found, but it might be that more
    // recipes would be successfull if we bruteforced the whole memory
    // sequence. FIXME: this is really inefficient.
    bool success = false;
    foreach2(it, sequences.begin(), sequences.end()) {
        AddrSize &seq = *it;

        getDebugStream(state) << "Trying seq: " << hexval(seq.addr) << "\n";

        if (seq.size < state->getPointerSize()) {
            continue;
        }

        for(uint32_t i = 0; i <= seq.size - state->getPointerSize(); i++) { // Try recipe at offset i
            getDebugStream(state) << "Trying mem area @ " << hexval(seq.addr) << "+" << i << " (size: " << seq.size << ")\n";

            // Wraparound register value
            uint64_t regVal = seq.addr + i - ictiOffset;
            if (state->getPointerWidth() < Expr::Int64) {
                regVal = (uint32_t) regVal;
            }

            ref<Expr> c = E_EQ(regExpr, E_CONST(regVal, state->getPointerWidth()));
            if (isa<ConstantExpr>(c) && dyn_cast<ConstantExpr>(c)->isFalse()) {
                // This can happen if regExpr is a read from const array that does not contain desired address
                continue;
            }

            RecipeConditions recipeConditions;

            recipeConditions.constraints.push_back(c);
            recipeConditions.usedRegs.push_back(reg);

            ref<Expr> targetEip = m_detector->readMemory(state, seq.addr + i, state->getPointerWidth());
            s2e_assert(state, !targetEip.isNull(), "Failed to read memory at " << hexval(seq.addr + i));
            extractSymbolicBytes(targetEip, recipeConditions.usedExprs);

            getWarningsStream(state) << "Handling ICTI at pc=" << hexval(state->getPc()) << "\n";
            getDebugStream(state) << "  nextEIP = " << targetEip << "\n";

            stateConditions.nextEip = targetEip;
            stateConditions.eipType = EIPType::SYMBOLIC_EIP;
            success = tryRecipes(state, stateConditions, recipeConditions);
            if (success) {
                break;
            }
        }
        if (success) {
            break;
        }
    }
    if (!success) {
        getDebugStream(state) << "No suitable memory area to handle ICTI\n";
    }
}

void Recipe::suppressExecutionWithInvalidAddress(S2EExecutionState *state, ref<Expr> addr, bool isWrite, int accessSize)
{
    std::unordered_set<uint64_t> validPages;
    const CGCMonitor::MemoryMap &memMap = m_monitor->getMemoryMap(state);
    CGCMonitor::FindMemoryPages(memMap, isWrite, false, validPages);

    // Allow read/write to one page below stack
    uint64_t stackPage = state->getSp() & TARGET_PAGE_MASK;
    if (validPages.find(stackPage) != validPages.end() && stackPage > TARGET_PAGE_SIZE) {
        validPages.insert(stackPage - TARGET_PAGE_SIZE);
    }

    // Address is valid if access does not cause segfault
    // 0 || ( min0 <= addr && addr <= max0) || ( min1 <= addr && addr <= max1) || ...
    ref<Expr> addrIsValid = E_CONST(0, Expr::Bool);
    foreach2(it, validPages.begin(), validPages.end()) {
        // TODO: max = (*it + (TARGET_PAGE_SIZE - accessSize)) for last page of contiguous region
        ref<Expr> min = E_CONST(*it, state->getPointerWidth());
        ref<Expr> max = E_CONST(*it + (TARGET_PAGE_SIZE - 1), state->getPointerWidth());
        addrIsValid = E_OR(addrIsValid, E_AND(E_GE(addr, min), E_LE(addr, max)));
    }

    S2EExecutor::StatePair sp = s2e()->getExecutor()->forkCondition(state, addrIsValid);

    if (sp.second) {
        S2EExecutionState *invalidState = static_cast<S2EExecutionState*>(sp.second);

        getDebugStream(invalidState) << "Memory map:\n";
        for(auto m: memMap) {
            getDebugStream(invalidState) << "  " << m << "\n";
        }
        getDebugStream(invalidState) << "Valid pages:\n";
        for(auto v: validPages) {
            getDebugStream(invalidState) << "  " << hexval(v) << "\n";
        }

        ref<Expr> eval = invalidState->concolics->evaluate(addr);
        getWarningsStream(invalidState) << "expecting " << (isWrite ? "write to" : "read from") << " invalid addr=" << eval << " at pc="<< hexval(state->getPc()) << "\n";

        // NOTE: This will produce non replayable crash PoV in case address was valid
        m_monitor->onSegFault.emit(invalidState, m_monitor->getPid(state), state->getPc());
        s2e()->getExecutor()->terminateStateEarly(*invalidState, "Suppress execution with invalid memory address");
    }
}

void Recipe::onBeforeSymbolicDataMemoryAccess(S2EExecutionState *state, ref<Expr> addr, ref<Expr> value, bool isWrite)
{
    if (!m_process->isTracked(state)) {
        return;
    }

    getDebugStream(state) << "onBeforeSymbolicDataMemoryAccess pid=" << hexval(m_monitor->getPid(state))
            << ", EIP=" << hexval(state->getPc()) << ", isWrite=" << isWrite << "\n";
    getDebugStream(state) << "  addr=" << addr << "\n";
    getDebugStream(state) << "  value=" << value << "\n";

    // Fork new states with interesting address values (e.g. overwrite EIP on stack)
    if(isWrite) {
        handleSymbolicWrite(state, addr);
    } else {
        handleSymbolicRead(state, addr);
    }

    // Do not fork and concretize all possible invalid address values
    int accessSize = Expr::getMinBytesForWidth(value->getWidth());
    suppressExecutionWithInvalidAddress(state, addr, isWrite, accessSize);
}

void Recipe::handleSymbolicWrite(S2EExecutionState *state, ref<Expr> addr)
{
    bool isSeedState = m_seedSearcher->isSeedState(state);

    /*
     * For every EIP stored on the stack, fork a state that will overwrite it.
     */

    StackMonitor::CallStack callStack;
    if (!m_stackMonitor->getCallStack(state, m_monitor->getPid(state), m_monitor->getTid(state), callStack)) {
        getWarningsStream(state) << "Failed to get call stack\n";
        return;
    }

    std::vector<ref<Expr>> retAddrLocations;
    for (unsigned i = 1; i < callStack.size(); i++) { // always skip first dummy frame
        retAddrLocations.push_back(E_CONST(callStack[i].FrameTop, state->getPointerWidth()));
    }

    std::vector<ExecutionState*> states = s2e()->getExecutor()->forkValues(state, isSeedState, addr, retAddrLocations);
    assert(states.size() == retAddrLocations.size());

    for (unsigned i = 0; i < states.size(); i++) {
        S2EExecutionState *valuableState = static_cast<S2EExecutionState*>(states[i]);

        if (valuableState) {
            // We've forked a state where symbolic address will be concretized to one exact value.
            // Put the state into separate CUPA group to distinguish it from other states that were
            // produced by fork and concretise at the same PC.
            m_keyValueStore->setProperty(valuableState, "group", CUPA_GROUP_SYMBOLIC_ADDRESS);
        }

        std::ostringstream os;
        os << "Stack frame " << i << " retAddr at " << retAddrLocations[i];
        if (valuableState) {
            os << " overriden in state " << valuableState->getID();
        } else {
            os << " can not be overriden";
        }
        getDebugStream(state) << os.str() << "\n";
    }
}

/// \brief Handle symbolic reads
///
/// \note handles symbolic reads by creating n new states, one where the source
/// of the read operation points to the hardcoded address of the flag page and
/// n-1 where it is constrained to fall within each memory area containing
/// symbolic values.
///
/// \param state Original state to fork from
/// \param expr Expression corresponding to the source of the symbolic read
/// operation
///
void Recipe::handleSymbolicRead(S2EExecutionState *state, ref<Expr> addr)
{
    bool isSeedState = m_seedSearcher->isSeedState(state);

    // Add static location for flag page
    std::vector<ref<Expr>> staticLocations;
    staticLocations.push_back(E_CONST(m_flagPage, state->getPointerWidth()));
    std::vector<ExecutionState*> states = s2e()->getExecutor()->forkValues(state, isSeedState, addr, staticLocations);

    if (states[0] != NULL) {
        getDebugStream(state) << "Successfully forked for flag page address (" << hexval(m_flagPage) << ")\n";

        m_keyValueStore->setProperty(static_cast<S2EExecutionState*>(states[0]), "group",
                                     CUPA_GROUP_FLAG_PAGE_ADDRESS);
    }

    // Add a new state for every symbolic sequence we find
    std::vector<AddrSize> symSequences;
    FindSequencesOfSymbolicData(state, m_monitor->getMemoryMap(state), false, symSequences);

    if (symSequences.size()) {
        ref<Expr> rangeId = E_CONST(UINT32_MAX, Expr::Int32); // range = UINT32_MAX by default
        std::vector<ref<Expr>> rangeValues = { rangeId };

        for (unsigned i = 0; i < symSequences.size(); i++) {
            getDebugStream(state) << "==== " << hexval(symSequences[i].addr) << " <= addr < "
                                  << hexval(symSequences[i].addr+symSequences[i].size) << " ====\n";

            ref<Expr> seqMin = E_CONST(symSequences[i].addr, state->getPointerWidth());
            ref<Expr> seqMax = E_CONST(symSequences[i].addr+symSequences[i].size, state->getPointerWidth());
            ref<Expr> iExpr  = E_CONST(i, Expr::Int32);

            // range = i if (min[i] <= addr && addr < max[i])
            rangeId = E_ITE(E_AND(E_GE(addr, seqMin), E_LT(addr, seqMax)), iExpr, rangeId);
            rangeValues.push_back(iExpr);
        }

        getDebugStream(state) << "Forking for " << int(rangeValues.size()) << " values\n";
        std::vector<ExecutionState*> newStates = s2e()->getExecutor()->forkValues(state, isSeedState, rangeId,
                                                                                  rangeValues);

        int newStatesCount = 0;
        foreach2(it, newStates.begin(), newStates.end()) {
            if (*it != NULL) {
                // We must not add the state for index = 0xffffffff to the
                // group, it would make the group useless
                if (it != newStates.begin()) {
                    m_keyValueStore->setProperty(static_cast<S2EExecutionState*>(*it), "group",
                                                 CUPA_GROUP_FLAG_READ_SYM_MEM);
                }

                getDebugStream(state) << "Created state " << int(static_cast<S2EExecutionState*>(*it)->getID())<< " ("
                                      << static_cast<S2EExecutionState*>(*it)->concolics->evaluate(rangeId)
                                      << ")\n";
                newStatesCount++;

            }
        }
        getDebugStream(state) << "Done, created: " << int(newStatesCount) << " states!\n";
    } else {
        getDebugStream(state) << "No symbolic byte sequences to handle symbolic read\n";
    }
}

void Recipe::onSymbolicBuffer(S2EExecutionState *state, uint64_t pid, SymbolicBufferType type, ref<Expr> ptr, ref<Expr> size)
{
    if (!m_process->isTracked(state, pid)) {
        return;
    }

    // use CGCMonitor::bufferMustBeWritable to decide whether buffer requires write access
}

void Recipe::onSymbolicAddress(S2EExecutionState *state,
                               ref<Expr> virtualAddress,
                               uint64_t concreteAddress,
                               bool &concretize,
                               CorePlugin::symbolicAddressReason reason)
{
    if (!m_process->isTrackedPc(state, state->getPc(), true)) {
        return;
    }

    if (reason != CorePlugin::symbolicAddressReason::PC) {
        return;
    }

    ref<Expr> nextEip = virtualAddress;
    if (isa<ConstantExpr>(nextEip)) {
        return;
    }

    getWarningsStream(state) << "Handling symbolic EIP at pc=" << hexval(state->getPc()) << "\n";
    getDebugStream(state) << "  nextEIP = " << nextEip << "\n";

    StateConditions stateConditions;
    stateConditions.eipType = EIPType::SYMBOLIC_EIP;
    stateConditions.nextEip = nextEip;
    if (!getCurrentModule(state, state->getPc(), stateConditions.module)) {
        return; // Can't try recipes without current module!
    }

    RecipeConditions recipeConditions;
    tryRecipes(state, stateConditions, recipeConditions);
}

bool Recipe::exprIn(ref<Expr> expr, const ExprList &a)
{
    foreach2(it, a.begin(), a.end())
    {
        if (expr.compare(*it) == 0) {
            return true;
        }
    }

    return false;
}

void Recipe::extractSymbolicBytes(const ref<Expr> &e, ExprList &bytes)
{
    for (unsigned i = 0; i < e->getWidth() / CHAR_BIT; ++i) {
        ref<Expr> byte = E_EXTR(e, i * 8, Expr::Int8);
        if (!isa<ConstantExpr>(byte)) {
            bytes.push_back(byte);
        }
    }
}

ref<Expr> Recipe::getRegExpr(S2EExecutionState *state, const StateConditions &sc, Register reg)
{
    if (reg.reg == Register::REG_EIP) {
        if (!sc.nextEip.isNull()) {
            return sc.nextEip;
        } else {
            return E_CONST(state->getPc(), state->getPointerWidth());
        }
    }

    ref<Expr> value = state->regs()->readSymbolicRegion(CPU_OFFSET(regs[reg.reg]), state->getPointerWidth());
    s2e_assert(state, !value.isNull(), "Failed to read register " << reg.regName());

    return value;
}

ref<Expr> Recipe::getRegbyteExpr(S2EExecutionState *state, const StateConditions &sc, Register reg)
{
    ref<Expr> val = getRegExpr(state, sc, reg);
    return E_EXTR(val, reg.byteIdx * 8, Expr::Int8);
}

bool Recipe::getLeftExpr(S2EExecutionState *state, const StateConditions &sc, const Precondition &p, ref<Expr> &val)
{
    if (p.left.type == Left::REGBYTE) {
        val = getRegbyteExpr(state, sc, p.left.reg);
    } else if (p.left.type == Left::ADDR) {
        val = m_detector->readMemory8(state, p.left.addr);
        if (val.isNull()) {
            getWarningsStream(state) << "Failed to read memory at " << hexval(p.left.addr) << "\n";
            return false;
        }
    } else if (p.left.type == Left::REGPTR) {
        ref<Expr> base = getRegExpr(state, sc, p.left.reg);
        s2e_assert(state, isa<ConstantExpr>(base), "Symbolic memory pointers are handled separately");

        uint64_t addr = dyn_cast<ConstantExpr>(base)->getZExtValue() + p.left.offset;
        val = m_detector->readMemory8(state, addr);
        if (val.isNull()) {
            getWarningsStream(state) << "Failed to read memory at " << hexval(addr) << "\n";
            return false;
        }

    } else if (p.left.type == Left::REGPTR_PTR) {
        ref<Expr> base = getRegExpr(state, sc, p.left.reg);
        s2e_assert(state, isa<ConstantExpr>(base), "Symbolic memory pointers are handled separately");

        uint64_t addr = dyn_cast<ConstantExpr>(base)->getZExtValue() + p.left.offset;
        ref<Expr> ptr = m_detector->readMemory(state, addr, state->getPointerWidth());
        if (ptr.isNull()) {
            getWarningsStream(state) << "Failed to read memory at " << hexval(addr) << "\n";
            return false;
        }

        s2e_assert(state, isa<ConstantExpr>(ptr), "Symbolic memory pointers are handled separately");

        addr = dyn_cast<ConstantExpr>(ptr)->getZExtValue() + p.left.mem_offset;
        val = m_detector->readMemory8(state, addr);
        if (val.isNull()) {
            getWarningsStream(state) << "Failed to read memory at " << hexval(addr) << "\n";
            return false;
        }
    } else if (p.left.type == Left::REGPTR_EXEC) {
        s2e_assert(state, false, "This is handled separately");
    }

    return true;
}

bool Recipe::isSymbolicRegPtr(S2EExecutionState *state, const StateConditions &sc, const Left &l, ref<Expr> &ptrExpr)
{
    if (l.type == Left::REGPTR || l.type == Left::REGPTR_EXEC) {
        ptrExpr = getRegExpr(state, sc, l.reg);
        if (!isa<ConstantExpr>(ptrExpr)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Recipe::pruneSymbolicSequences
 * Removes bytes containing specific expressions from symbolic sequences.
 * We assume that contiguous symbolic byte sequence can be used for storing arbitrary values.
 * Thus, if some memory byte is already used to store recipe value,
 * it is not allowed to be a part of any sequence.
 *
 * E.g. if you have sequence if size 5 and byte 1 is known to be used somewhere else,
 * the function will produce two new seqences of size 1 and 3: 00000 -> 0, 000
 *
 * @param state
 * @param usedExprs
 * @param list in/out parameter, contains the filtered list when the function returns.
 */
void Recipe::pruneSymbolicSequences(S2EExecutionState *state, const ExprList &usedExprs, std::vector<AddrSize> &list)
{
    std::vector<AddrSize> filtered;

    foreach2(it, list.begin(), list.end())
    {
        BitArray concreteMask(it->size);

        for (size_t i = 0; i < it->size; i++) {
            ref<Expr> memExpr = m_detector->readMemory8(state, it->addr + i);
            s2e_assert(state, !memExpr.isNull(), "Failed to read memory at " << hexval(it->addr + i));

            if (exprIn(memExpr, usedExprs)) {
                // mark used expressions as concrete values
                concreteMask.set(i);
            }
        }

        FindSequencesOfSymbolicData(&concreteMask, it->addr, NULL, filtered);
    }

    list = filtered;
}

bool Recipe::applySimplePrecondition(S2EExecutionState *state, const StateConditions &sc, const ref<Expr> &left,
                                     const Right &right, RecipeConditions &recipeConditions)
{
    s2e_assert(state, !left.isNull(), "left expr must not be null!");

    RecipeConditions l_recipeConditions = recipeConditions;

    extractSymbolicBytes(left, l_recipeConditions.usedExprs);

    if (right.type == Right::NEGOTIABLE) {
        // Check width
        if(left->getWidth() != Expr::Int8) {
            getWarningsStream(state) << "Invalid left value width " << left->getWidth() << "\n";
            return false;
        }

        if (isa<ConstantExpr>(left)) {
            return false;
        }

        std::vector<const Array*> vars;
        findSymbolicObjects(left, vars);

        if (vars.size() != 1) {
            getDebugStream(state) << "Expression contains multiple variables, can't remap it.\n";
            getDebugStream(state) << "ee: " << left << "\n";
            foreach2(it, vars.begin(), vars.end()) {
                getDebugStream(state) << "var: " << (*it)->getName() << "\n";
            }
            return false;
        }

        if (vars[0]->getSize() != 1) {
            getDebugStream(state) << "Variable " << vars[0]->getName() << " spans " << vars[0]->getSize() << " bytes, can't remap it.\n";
            return false;
        }

        if (l_recipeConditions.remappings.find(vars[0]->getName()) != l_recipeConditions.remappings.end()) {
            getDebugStream(state) << "Variable " << vars[0]->getName() << " has already been remapped\n";
            return false;
        }

        l_recipeConditions.remappings[vars[0]->getName()] = right.name();
    } else if (right.type == Right::REGBYTE) {
        ref<Expr> ee_right = getRegbyteExpr(state, sc, right.reg);

        // Check width
        if(left->getWidth() != Expr::Int8) {
            getWarningsStream(state) << "Invalid left value width " << left->getWidth() << "\n";
            return false;
        }

        ref<Expr> c = E_EQ(left, ee_right);
        if (isa<ConstantExpr>(c) && dyn_cast<ConstantExpr>(c)->isFalse()) {
            // This can happen if left is const and its value does not match right, or
            // if left is a read from const array that does not contain right
            return false;
        }

        l_recipeConditions.constraints.push_back(c);
    } else if (right.type == Right::CONCRETE) {
        // Check width
        if (left->getWidth() != right.valueWidth) {
            getWarningsStream(state) << "Value width mismatch: " << left->getWidth() << " != " << right.valueWidth << "\n";
            return false;
        }

        ref<Expr> c = E_EQ(left, E_CONST(right.value, right.valueWidth));
        if (isa<ConstantExpr>(c) && dyn_cast<ConstantExpr>(c)->isFalse()) {
            // This can happen if left is const and its value does not match right, or
            // if left is a read from const array that does not contain right
            return false;
        }

        l_recipeConditions.constraints.push_back(c);
    } else {
        s2e_assert(state, false, "Invalid right type " << (int) right.type);
    }

    recipeConditions = l_recipeConditions;
    return true;
}

// Test a sequence of memory by adding one precondition at a time and verifying
// satisfiability. If true, *offset* contains the value where the precondition
// should start to be applied. If false, offset is undefined.
bool Recipe::testMemPrecondition(S2EExecutionState *state, const StateConditions &sc, const MemPrecondition &p,
                                 AddrSize sequence, const RecipeConditions &recipeConditions, uint32_t &offset)
{
    for(uint32_t i = 0; i < (sequence.size - p.requiredMemSize); i++) {
        // Constraints
        RecipeConditions l_recipeConditions = recipeConditions;

        // Test preconditions at offset i
        bool satisfiable = true;
        foreach2(it, p.preconditions.begin(), p.preconditions.end()) {
            ref<Expr> left = m_detector->readMemory8(state, sequence.addr + i + it->left.offset);
            if (left.isNull()) {
                getDebugStream(state) << "Mem precondition " << *it << " points to invalid memory!\n";
                satisfiable = false;
                break;
            }

            if (!applySimplePrecondition(state, sc, left, it->right, l_recipeConditions)) {
                getDebugStream(state) << "Mem precondition " << *it << " is not satisfiable\n";
                satisfiable = false;
                break;
            }
            // Test temporary constraints
            if (!state->testConstraints(l_recipeConditions.constraints, NULL, NULL)) {
                satisfiable = false;
                break;
            }
        }

        if (satisfiable) {
            getInfoStream(state) << "Found a good memory area @ " << hexval(sequence.addr) << " + " << i << "\n";
            offset = i;
            // NOTE: this function just tests memory condition, we DO NOT need to alter original ones!
            return true;
        }
    }


    return false;
}

bool Recipe::applyMemPrecondition(S2EExecutionState *state, const StateConditions &sc, const MemPrecondition &p,
                                  RecipeConditions &recipeConditions)

{
    RecipeConditions l_recipeConditions = recipeConditions;

    std::vector<AddrSize> sequences;
    FindSequencesOfSymbolicData(state, m_monitor->getMemoryMap(state), p.exec, sequences);

    extractSymbolicBytes(p.ptrExpr, l_recipeConditions.usedExprs);
    pruneSymbolicSequences(state, l_recipeConditions.usedExprs, sequences);

    if (!sequences.size()) {
        getDebugStream(state) << "No symbolic byte sequences to handle memory precondition\n";
        return false;
    }

    std::sort(sequences.rbegin(), sequences.rend());

    bool satisfiable = false;
    foreach2(it, sequences.begin(), sequences.end()) {
        // Conditions must be restored for each sequence, enclose for body
        RecipeConditions inner_recipeConditions = l_recipeConditions;

        const AddrSize &seq = *it;

        if (seq.size < p.requiredMemSize) {
            getDebugStream(state) << "Symbolic byte sequence is too short\n";
            continue;
        }

        uint32_t offset;
        satisfiable = testMemPrecondition(state, sc, p, seq, inner_recipeConditions, offset);

        if (!satisfiable) {
            getDebugStream(state) << "Memory area at " << hexval(seq.addr) << " is not good, tying next one\n";
            continue;
        } else {
            getDebugStream(state) << "Found valid memory area at " << hexval(seq.addr+offset) << "\n";
        }


        if (!applySimplePrecondition(state, sc, p.ptrExpr, Right(seq.addr+offset, state->getPointerWidth()), inner_recipeConditions)) {
            getDebugStream(state) << "Symbolic pointer precondition is not satisfiable\n";
            continue;
        }

        // If we get here, we're sure it's satisfiable => Apply preconditions.
        foreach2(it2, p.preconditions.begin(), p.preconditions.end()) {
            uint64_t addr = seq.addr + offset + it2->left.offset;
            ref<Expr> left = m_detector->readMemory8(state, addr);
            s2e_assert(state, !left.isNull(), "Failed to read memory at " << hexval(addr));

            // It should never fail, but better keep it to avoid headaches later on
            if (!applySimplePrecondition(state, sc, left, it2->right, inner_recipeConditions)) {
                getDebugStream(state) << "Mem precondition " << *it2 << " is not satisfiable\n";
                satisfiable = false;
                break;
            }
        }

        if (satisfiable) {
            l_recipeConditions = inner_recipeConditions;
            break;
        }
    }

    if (satisfiable)
        recipeConditions = l_recipeConditions;
    else {
        getDebugStream(state) << "No suitable memory area to handle memory precondition\n";
    }
    return satisfiable;
}

void Recipe::classifyPreconditions(S2EExecutionState *state, const StateConditions& sc, const Preconditions &p,
                                   Preconditions &simple, std::map<Register::Reg, MemPrecondition> &memory)
{
    foreach2(it, p.begin(), p.end())
    {
        ref<Expr> ptrExpr;
        if (isSymbolicRegPtr(state, sc, it->left, ptrExpr)) {
            MemPrecondition &mp = memory[it->left.reg.reg];
            mp.ptrExpr = ptrExpr;
            mp.requiredMemSize = std::max(mp.requiredMemSize, unsigned(it->left.offset + 1));
            if (it->left.type == Left::REGPTR_EXEC) {
                mp.exec = true;
            } else {
                mp.preconditions.push_back(*it);
            }
        } else {
            simple.push_back(*it);
        }
    }
}


bool Recipe::checkUsedRegs(S2EExecutionState *state, const Left &left, const RegList &usedRegs)
{
    if (left.type == Left::REGBYTE || left.type == Left::REGPTR || left.type == Left::REGPTR_PTR) {
        if (std::find(std::begin(usedRegs), std::end(usedRegs), left.reg) != usedRegs.end()) {
            getDebugStream(state) << "Register " << left.reg.regName() << " cannot be used\n";
            return true;
        }
    }
    return false;
}

bool Recipe::getCurrentModule(S2EExecutionState *state, uint64_t eip, ModuleDescriptor& module)
{
    const ModuleDescriptor *p_module = m_detector->getModule(state, eip);
    if (!p_module) {
        getWarningsStream(state) << "Could not find module at pc=" << hexval(eip) << "\n";
        print_stacktrace(s2e_warning_print, "Could not find module");
        return false;
    }

    module = *p_module;

    return true;
}

bool Recipe::applyPreconditions(S2EExecutionState *state, PovType type, const StateConditions &sc, const Preconditions &p,
                                RecipeConditions &recipeConditions)
{
    std::unordered_set<uint64_t> executablePages;
    CGCMonitor::FindMemoryPages(m_monitor->getMemoryMap(state), false, true, executablePages);

    Preconditions simple;
    std::map<Register::Reg, MemPrecondition> memory;
    classifyPreconditions(state, sc, p, simple, memory);

    RecipeConditions l_recipeConditions = recipeConditions;

    foreach2(it, simple.begin(), simple.end())
    {
        if (it->left.type == Left::REGPTR_EXEC) {
            ref<Expr> reg = getRegExpr(state, sc, it->left.reg);
            uint64_t regVal = dyn_cast<ConstantExpr>(reg)->getZExtValue();

            if (executablePages.find(regVal & TARGET_PAGE_MASK) == executablePages.end()) {
                getDebugStream(state) << "Precondition " << *it << " is not satisfiable\n";
                return false;
            }
        } else {
            if (checkUsedRegs(state, it->left, l_recipeConditions.usedRegs)) {
                return false;
            }

            ref<Expr> left;
            if (!getLeftExpr(state, sc, *it, left)) {
                getDebugStream(state) << "Cannot get left expr, " << *it << " is not satisfiable\n";
                return false;
            }

            if (!applySimplePrecondition(state, sc, left, it->right, l_recipeConditions)) {
                getDebugStream(state) << "Precondition " << *it << " is not satisfiable\n";
                return false;
            }
        }
    }

    if (memory.size()) {
        s2e_assert(state, memory.size() == 1, "TODO: support multiple memory regions");
        if (!applyMemPrecondition(state, sc, memory.begin()->second, l_recipeConditions)) {
            getDebugStream(state) << "Memory precondition is not satisfiable\n";
            return false;
        }
    }

    if (!state->testConstraints(l_recipeConditions.constraints, NULL, NULL)) {
        getDebugStream(state) << "Constraints are not solvable\n";
        return false;
    }

    recipeConditions = l_recipeConditions;
    return true;
}

bool Recipe::tryRecipes(S2EExecutionState *state, const StateConditions &sc, RecipeConditions &recipeConditions)
{
    bool success = false;

    foreach2(it, m_recipes.begin(), m_recipes.end())
    {
        const RecipeDescriptor &recipe = *it->second;
        const std::string &recipeName = it->first;
        const RecipeSettings &settings = recipe.settings;

        if (!RecipeDescriptor::mustTryRecipe(recipe, recipeName, sc, state->getPc())) {
            continue;
        }

        getDebugStream(state) << "Trying recipe '" << recipeName << "'\n";
        RecipeConditions l_recipeConditions = recipeConditions;

        if (!applyPreconditions(state, settings.type, sc, recipe.preconditions, l_recipeConditions)) {
            continue;
        }

        getWarningsStream(state) << "Recipe '" << recipeName << "' ready\n";

        success = true;

        PovOptions opt;

        opt.m_type = settings.type;
        opt.m_faultAddress = state->getPc();
        opt.m_extraConstraints = l_recipeConditions.constraints;
        opt.m_remapping = l_recipeConditions.remappings;

        if (settings.type == PovType::POV_TYPE1) {
            opt.m_ipMask = settings.ipMask;
            opt.m_regMask = settings.regMask;
            opt.m_regNum = settings.gp.reg;
        } else if (settings.type == PovType::POV_TYPE2) {
            opt.m_bytesBeforeSecret = settings.skip;
        }

        onPovReady.emit(state, opt, recipeName);
    }

    if (success) {
        ++m_stats.successfulRecipeTries;
    } else {
        ++m_stats.failedRecipeTries;
    }

    // TODO: Could not find any working recipe, save the test case
    // for current path in case we get better recipes next time.

    // Returns true if at least one recipe was tried successfully
    return success;
}

} // namespace recipe
} // namespace plugins
} // namespace s2e
