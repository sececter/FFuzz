///
/// Copyright (C) 2010-2016, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2014-2016, Cyberhaven, Inc
/// All rights reserved. Proprietary and confidential.
///
/// Distributed under the terms of S2E-LICENSE
///


#include <s2e/cpu.h>

#ifdef CONFIG_WIN32
#include <windows.h>
#endif

#include "BaseInstructions.h"
#include <s2e/S2E.h>
#include <s2e/S2EExecutor.h>
#include <s2e/S2EExecutionState.h>
#include <s2e/ConfigFile.h>
#include <s2e/Utils.h>
#include <s2e/Plugins/Opcodes.h>

#include <iostream>
#include <sstream>

#include <llvm/Support/TimeValue.h>
#include <llvm/ADT/DenseSet.h>
#include <klee/Searcher.h>
#include <klee/Solver.h>

#include <llvm/Support/CommandLine.h>

extern llvm::cl::opt<bool> ConcolicMode;

extern "C" {
    /**
     * In some cases, it may be useful to forbid guest apps to use
     * s2e instructions, e.g., when doing malware analysis.
     */
    int g_s2e_allow_custom_instructions = 0;
}

namespace s2e {
namespace plugins {

using namespace std;
using namespace klee;

S2E_DEFINE_PLUGIN(BaseInstructions, "Default set of custom instructions plugin", "",);

namespace {
class BaseInstructionsState: public PluginState {
    llvm::DenseSet<uint64_t> m_allowedPids;
public:
    bool allowed(uint64_t pid) {
        return m_allowedPids.count(pid);
    }

    inline bool empty() {
        return m_allowedPids.empty();
    }

    inline void allow(uint64_t pid) {
        m_allowedPids.insert(pid);
    }

    inline void disallow(uint64_t pid) {
        m_allowedPids.erase(pid);
    }

    static PluginState *factory(Plugin *p, S2EExecutionState *s) {
        return new BaseInstructionsState();
    }

    virtual ~BaseInstructionsState() {}
    virtual BaseInstructionsState* clone() const {
        return new BaseInstructionsState(*this);
    }
};
}

void BaseInstructions::initialize()
{
    ConfigFile *cfg = s2e()->getConfig();

    m_monitor = NULL;
    if (cfg->getBool(getConfigKey() + ".restrict", false)) {
        m_monitor = dynamic_cast<OSMonitor*>(s2e()->getPlugin("Interceptor"));
        if (!m_monitor) {
            getWarningsStream() << "You must enable an os monitoring plugin to use restricted mode\n";
            exit(-1);
        }

        getWarningsStream() << "Restriction enabled\n";
        s2e()->getCorePlugin()->onTranslateBlockStart.connect(
                sigc::mem_fun(*this, &BaseInstructions::onTranslateBlockStart));
    }

    s2e()->getCorePlugin()->onCustomInstruction.connect(
            sigc::mem_fun(*this, &BaseInstructions::onCustomInstruction));

    g_s2e_allow_custom_instructions = 1;
}

void BaseInstructions::onTranslateBlockStart(ExecutionSignal *signal,
                                             S2EExecutionState *state,
                                             TranslationBlock *tb,
                                             uint64_t pc)
{
    if ((tb->flags >> VM_SHIFT) & 1) {
        g_s2e_allow_custom_instructions = 1;
        return;
    }

    if (m_monitor->isKernelAddress(pc)) { // XXX make it configurable
        g_s2e_allow_custom_instructions = 1;
        return;
    }

    DECLARE_PLUGINSTATE(BaseInstructionsState, state);
    if (plgState->empty()) {
        /**
         * The first process will have to register itself
         */
        g_s2e_allow_custom_instructions = 1;
        return;
    }

    uint64_t pid = m_monitor->getPid(state, pc);
    g_s2e_allow_custom_instructions = plgState->allowed(pid);
}

void BaseInstructions::allowCurrentPid(S2EExecutionState *state)
{
    if (!m_monitor) {
        getWarningsStream(state) << "Please enable the restrict option to control access to custom instructions\n";
        exit(-1);
        return;
    }

    DECLARE_PLUGINSTATE(BaseInstructionsState, state);
    uint64_t pid = m_monitor->getPid(state, state->getPc());
    plgState->allow(pid);

    getDebugStream(state) << "Allowing custom instructions for pid " << hexval(pid) << "\n";

    se_tb_safe_flush();
}

void BaseInstructions::makeSymbolic(S2EExecutionState *state, uintptr_t address,
                                    unsigned size, const std::string &nameStr,
                                    bool makeConcolic, std::vector<klee::ref<Expr> > *varData, std::string *varName)
{
    std::vector<unsigned char> concreteData;
    std::vector<klee::ref<Expr> > symb;

    std::stringstream valueSs;
    if (makeConcolic) {
        valueSs << "='";
        for (unsigned i = 0; i< size; ++i) {
            uint8_t byte = 0;
            if (!state->readMemoryConcrete8(address + i, &byte, VirtualAddress, false)) {
                getWarningsStream(state)
                    << "Can not concretize/read symbolic value"
                    << " at " << hexval(address + i) << ". System state not modified.\n";
                return;
            }
            concreteData.push_back(byte);
            valueSs << charval(byte);
        }
        valueSs << "'";
        symb = state->createConcolicArray(nameStr, size, concreteData, varName);
    } else {
        symb = state->createSymbolicArray(nameStr, size, varName);
    }

    getInfoStream(state) << "Inserted symbolic data @" << hexval(address)
                         << " of size " << hexval(size)
                         << ": " << (varName ? *varName : nameStr)
                         << valueSs.str()
                         << "\n";

    for(unsigned i = 0; i < size; ++i) {
        if(!state->writeMemory8(address + i, symb[i])) {
            getWarningsStream(state)
                << "Can not insert symbolic value"
                << " at " << hexval(address + i)
                << ": can not write to memory\n";
        }
    }

    state->setInputSize(size);

    if(varData) {
        *varData = symb;
    }
}

void BaseInstructions::makeSymbolic(S2EExecutionState *state, bool makeConcolic)
{
    target_ulong address, size, name;
    bool ok = true;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]),
                                         &address, sizeof address);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                         &size, sizeof size);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                         &name, sizeof name);

    if(!ok) {
        getWarningsStream(state)
            << "ERROR: symbolic argument was passed to s2e_op "
               " insert_symbolic opcode\n";
        return;
    }

    std::string nameStr = "unnamed";
    if(name && !state->mem()->readString(name, nameStr)) {
        getWarningsStream(state)
                << "Error reading string from the guest\n";
    }

    makeSymbolic(state, address, size, nameStr, makeConcolic);
}

void BaseInstructions::isSymbolic(S2EExecutionState *state)
{
    target_ulong address;
    target_ulong size;
    target_ulong result;

    bool ok = true;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                         &address, sizeof(address));

    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]),
                                         &size, sizeof(size));

    if(!ok) {
        getWarningsStream(state)
            << "ERROR: symbolic argument was passed to s2e_op is_symbolic\n";
        return;
    }

    // readMemoryConcrete fails if the value is symbolic
    result = 0;
    for (unsigned i=0; i<size; ++i) {
        klee::ref<klee::Expr> ret = state->readMemory8(address + i);
        if (!isa<ConstantExpr>(ret)) {
            result = 1;
        }
    }

    getInfoStream(state)
            << "Testing whether data at " << hexval(address)
            << " and size " << size << " is symbolic: "
            << (result ? " true" : " false") << '\n';

    state->writeCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]), &result, sizeof(result));
}

void BaseInstructions::killState(S2EExecutionState *state)
{
    std::string message;
    target_ulong messagePtr;

#ifdef TARGET_X86_64
    const klee::Expr::Width width = klee::Expr::Int64;
#else
    const klee::Expr::Width width = klee::Expr::Int32;
#endif

    bool ok = true;
    klee::ref<klee::Expr> status =
                                state->readCpuRegister(CPU_OFFSET(regs[R_EAX]),
                                                       width);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]), &messagePtr,
                                         sizeof messagePtr);

    if (!ok) {
        getWarningsStream(state)
            << "ERROR: symbolic argument was passed to s2e_kill_state \n";
    } else {
        message="<NO MESSAGE>";
        if(messagePtr && !state->mem()->readString(messagePtr, message)) {
            getWarningsStream(state)
                << "Error reading message string from the guest\n";
        }
    }

    //Kill the current state
    getInfoStream(state) << "Killing state "  << state->getID() << '\n';
    std::ostringstream os;
    os << "State was terminated by opcode\n"
       << "            message: \"" << message << "\"\n"
       << "            status: " << status;
    s2e()->getExecutor()->terminateStateEarly(*state, os.str());
}

void BaseInstructions::printExpression(S2EExecutionState *state)
{
    //Print the expression
#ifdef TARGET_X86_64
    const klee::Expr::Width width = klee::Expr::Int64;
#else
    const klee::Expr::Width width = klee::Expr::Int32;
#endif

    target_ulong name;
    bool ok = true;
    klee::ref<Expr> val = state->readCpuRegister(
                offsetof(CPUX86State, regs[R_EAX]), width);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                         &name, sizeof name);

    if(!ok) {
        getWarningsStream(state)
            << "ERROR: symbolic argument was passed to s2e_op "
               "print_expression opcode\n";
        return;
    }

    std::string nameStr = "<NO NAME>";
    if(name && !state->mem()->readString(name, nameStr)) {
        getWarningsStream(state)
                << "Error reading string from the guest\n";
    }


    getInfoStream() << "SymbExpression " << nameStr << " - "
                               <<val << '\n';

    if (ConcolicMode && !isa<klee::ConstantExpr>(val)) {
        klee::ref<klee::Expr> concrete = state->concolics->evaluate(val);
        getInfoStream() << "SymbExpression " << nameStr << " - Value: "
                                   << concrete << '\n';
    }
}

void BaseInstructions::printMemory(S2EExecutionState *state)
{
    target_ulong address, size, name;
    bool ok = true;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]),
                                         &address, sizeof address);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                         &size, sizeof size);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                         &name, sizeof name);

    if(!ok) {
        getWarningsStream(state)
            << "ERROR: symbolic argument was passed to s2e_op "
               "print_expression opcode\n";
        return;
    }

    std::string nameStr = "<NO NAME>";
    if(name && !state->mem()->readString(name, nameStr)) {
        getWarningsStream(state)
                << "Error reading string from the guest\n";
    }

    getInfoStream() << "Symbolic memory dump of " << nameStr << '\n';

    for (uint32_t i=0; i<size; ++i) {

        getInfoStream() << hexval(address+i) << ": ";
        klee::ref<Expr> res = state->readMemory8(address+i);
        if (res.isNull()) {
            getInfoStream() << "Invalid pointer\n";
        }else {
            getInfoStream() << res << '\n';
        }
    }
}

void BaseInstructions::hexDump(S2EExecutionState *state)
{
    target_ulong address, size, name;
    bool ok = true;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]),
                                         &address, sizeof address);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                         &size, sizeof size);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                         &name, sizeof name);

    if(!ok) {
        getWarningsStream(state)
            << "ERROR: symbolic argument was passed to s2e_op "
               "hexDump opcode\n";
        return;
    }

    std::string nameStr = "<NO NAME>";
    if(name && !state->mem()->readString(name, nameStr)) {
        getWarningsStream(state)
                << "Error reading string from the guest\n";
    }

    llvm::raw_ostream &os = getDebugStream(state);

    os << "Hexdump of " << nameStr << '\n';

    unsigned i;
    char buff[17];

    // Process every byte in the data.
    for (i = 0; i < size; i++) {
        uint8_t data = 0;
        state->mem()->readMemoryConcrete8(address + i, &data);

        // Multiple of 16 means new line (with line offset).

        if ((i % 16) == 0) {
            // Just don't print ASCII for the zeroth line.
            if (i != 0) {
                os << "  " << buff << "\n";
            }
            // Output the offset.
            os << hexval(address + i, 8);
        }

        // Now the hex code for the specific character.
        os << " " << hexval(data, 2);

        // And store a printable ASCII character for later.
        if ((data < 0x20) || (data > 0x7e))
            buff[i % 16] = '.';
        else
            buff[i % 16] = data;
        buff[(i % 16) + 1] = '\0';
    }

    // Pad out last line if not exactly 16 characters.
    while ((i % 16) != 0) {
        os << "   ";
        i++;
    }

    // And print the final ASCII bit.
    os << "  " << buff << "\n";
}


void BaseInstructions::concretize(S2EExecutionState *state, bool addConstraint)
{
    target_ulong address, size;

    bool ok = true;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]),
                                         &address, sizeof address);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EDX]),
                                         &size, sizeof size);

    if(!ok) {
        getWarningsStream(state)
            << "ERROR: symbolic argument was passed to s2e_op "
               " get_example opcode\n";
        return;
    }

    for(unsigned i = 0; i < size; ++i) {
        uint8_t b = 0;
        if (!state->readMemoryConcrete8(address + i, &b, VirtualAddress, addConstraint)) {
            getWarningsStream(state)
                << "Can not concretize memory"
                << " at " << hexval(address + i) << '\n';
        } else {
            //readMemoryConcrete8 does not automatically overwrite the destination
            //address if we choose not to add the constraint, so we do it here
            if (!addConstraint) {
                if (!state->writeMemoryConcrete(address + i, &b, sizeof(b))) {
                    getWarningsStream(state)
                        << "Can not write memory"
                        << " at " << hexval(address + i) << '\n';
                }
            }
        }
    }
}

void BaseInstructions::sleep(S2EExecutionState *state)
{
    long duration = 0;
    state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]), &duration,
                                   sizeof(duration));
    getDebugStream() << "Sleeping " << duration << " seconds\n";

    llvm::sys::TimeValue startTime = llvm::sys::TimeValue::now();

    while (llvm::sys::TimeValue::now().seconds() - startTime.seconds() < duration) {
        #ifdef _WIN32
        Sleep(1000);
        #else
        ::sleep(1);
        #endif
    }
}

void BaseInstructions::printMessage(S2EExecutionState *state, bool isWarning)
{
    target_ulong address = 0;
    bool ok = state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]),
                                                &address, sizeof address);
    if(!ok) {
        getWarningsStream(state)
            << "ERROR: symbolic argument was passed to s2e_op "
               " message opcode\n";
        return;
    }

    std::string str="";
    if(!address || !state->mem()->readString(address, str)) {
        getWarningsStream(state)
                << "Error reading string message from the guest at address "
                << hexval(address) << '\n';
    } else {
        llvm::raw_ostream *stream;
        if(isWarning)
            stream = &getWarningsStream(state);
        else
            stream = &getInfoStream(state);
        (*stream) << "Message from guest (" << hexval(address) <<
                     "): " <<  str;

        /* Avoid doubling end of lines */
        if (str[str.length() - 1] != '\n') {
            *stream << "\n";
        }
    }
}

void BaseInstructions::invokePlugin(S2EExecutionState *state)
{
    BaseInstructionsPluginInvokerInterface *iface = NULL;
    Plugin *plugin;
    std::string pluginName;
    target_ulong pluginNamePointer = 0;
    target_ulong dataPointer = 0;
    target_ulong dataSize = 0;
    target_ulong result = 0;
    bool ok = true;

    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]), &pluginNamePointer, sizeof(pluginNamePointer));
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]), &dataPointer, sizeof(dataPointer));
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EDX]), &dataSize, sizeof(dataSize));
    if(!ok) {
        getWarningsStream(state)
            << "ERROR: symbolic arguments was passed to s2e_op invokePlugin opcode\n";
        result = 1;
        goto fail;
    }


    if (!state->mem()->readString(pluginNamePointer, pluginName)) {
        getWarningsStream(state)
            << "ERROR: invokePlugin could not read name of plugin to invoke\n";
        result = 2;
        goto fail;
    }

    plugin = s2e()->getPlugin(pluginName);
    if (!plugin) {
        getWarningsStream(state)
            << "ERROR: invokePlugin could not find plugin " << pluginName << "\n";
        result = 3;
        goto fail;
    }

    iface = dynamic_cast<BaseInstructionsPluginInvokerInterface*>(plugin);

    if (!iface) {
        getWarningsStream(state)
            << "ERROR: " << pluginName << " is not an instance of BaseInstructionsPluginInvokerInterface\n";
        result = 4;
        goto fail;
    }

    iface->handleOpcodeInvocation(state, dataPointer, dataSize);

 fail:
    state->writeCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]), &result, sizeof(result));
}

void BaseInstructions::assume(S2EExecutionState *state)
{
    klee::ref<klee::Expr> expr = state->readCpuRegister(CPU_OFFSET(regs[R_EAX]), klee::Expr::Int32);
    assumeInternal(state, expr);
}

void BaseInstructions::assumeRange(S2EExecutionState *state)
{
    klee::ref<klee::Expr> value;
    klee::ref<klee::Expr> lower;
    klee::ref<klee::Expr> upper;

    value = state->readCpuRegister(CPU_OFFSET(regs[R_EAX]), klee::Expr::Int32);
    lower = state->readCpuRegister(CPU_OFFSET(regs[R_ECX]), klee::Expr::Int32);
    upper = state->readCpuRegister(CPU_OFFSET(regs[R_EDX]), klee::Expr::Int32);

    klee::ref<klee::Expr> condition =
            klee::AndExpr::create(
                klee::UgeExpr::create(value, lower),
                klee::UleExpr::create(value, upper)
            );

    assumeInternal(state, condition);
}

void BaseInstructions::assumeDisjunction(S2EExecutionState *state)
{
    uint64_t sp = state->getSp();
    uint32_t count;
    bool ok = true;

    static unsigned STACK_ELEMENT_SIZE = state->getPointerSize();

    target_ulong currentParam = sp + STACK_ELEMENT_SIZE * 2;

    klee::ref<klee::Expr> variable = state->readMemory(currentParam, STACK_ELEMENT_SIZE * 8);
    if (variable.isNull()) {
        getWarningsStream(state) << "BaseInstructions: assumeDisjunction could not read the variable\n";
        return;
    }

    currentParam += STACK_ELEMENT_SIZE;
    ok &= state->readMemoryConcrete(currentParam, &count, sizeof(count));
    if (!ok) {
        getWarningsStream(state) << "BaseInstructions: assumeDisjunction could not read number of disjunctions\n";
        return;
    }

    if (count == 0) {
        getDebugStream(state) << "BaseInstructions: assumeDisjunction got 0 disjunctions\n";
        return;
    }

    currentParam += STACK_ELEMENT_SIZE;

    klee::ref<klee::Expr> expr;
    for (unsigned i = 0; i < count; ++i) {
        //XXX: 64-bits mode!!!
        klee::ref<klee::Expr> value = state->readMemory(currentParam, STACK_ELEMENT_SIZE * 8);
        if (i == 0) {
            expr = klee::EqExpr::create(variable, value);
        } else {
            expr = klee::OrExpr::create(expr, klee::EqExpr::create(variable, value));
        }
        currentParam += STACK_ELEMENT_SIZE;
    }

    getDebugStream(state) << "BaseInstructions: assuming expression " << expr << "\n";
    assumeInternal(state, expr);
}

void BaseInstructions::assumeInternal(S2EExecutionState *state, klee::ref<klee::Expr> expr)
{


    klee::ref<klee::Expr> zero = klee::ConstantExpr::create(0, expr.get()->getWidth());
    klee::ref<klee::Expr> boolExpr = klee::NeExpr::create(expr, zero);


    //Check that the added constraint is consistent with
    //the existing path constraints
    bool isValid = true;
    if (ConcolicMode) {
        klee::ref<klee::Expr> ce = state->concolics->evaluate(boolExpr);
        assert(isa<klee::ConstantExpr>(ce) && "Expression must be constant here");
        if (!ce->isTrue()) {
            isValid = false;
        }
    } else {
        bool truth;
        Solver *solver = s2e()->getExecutor()->getSolver(*state);
        Query query(state->constraints, boolExpr);
        bool res = solver->mustBeTrue(query.negateExpr(), truth);
        if (!res || truth) {
            isValid = false;
        }
    }

    if (!isValid) {
        std::stringstream ss;
        ss << "BaseInstructions: specified assume expression cannot be true. "
                << boolExpr;
        g_s2e->getExecutor()->terminateStateEarly(*state, ss.str());
    }

    state->addConstraint(boolExpr);
}

/**
 * Copies a guest memory buffer from one place to another, disregarding
 * any page protections. Can be used to hack kernel memory from user apps.
 * Use with caution.
 */
void BaseInstructions::writeBuffer(S2EExecutionState *state)
{
    target_ulong source, destination, size;
    bool ok = true;

    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ESI]), &source, sizeof(source));
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EDI]), &destination, sizeof(destination));
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]), &size, sizeof(size));

    getDebugStream(state) << "BaseInstructions: copying " << size << " bytes from "
            << hexval(source) << " to " << hexval(destination) << "\n";

    uint32_t remaining = (uint32_t) size;

    while (remaining > 0) {
        uint8_t byte;
        if (!state->mem()->readMemoryConcrete(source, &byte, sizeof(byte))) {
            getDebugStream(state) << "BaseInstructions: could not read byte at "
                    << hexval(source) << "\n";
            break;
        }

        if (!state->mem()->writeMemoryConcrete(destination, &byte, sizeof(byte))) {
            getDebugStream(state) << "BaseInstructions: could not write byte to "
                    << hexval(destination) << "\n";
            break;
        }

        source++;
        destination++;
        remaining--;
    }

    target_ulong written = size - remaining;
    state->writeCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]), &written, sizeof(written));
}

void BaseInstructions::getRange(S2EExecutionState *state)
{
    klee::ref<klee::Expr> value;
    std::pair< klee::ref<klee::Expr>, klee::ref<klee::Expr> > range;
    target_ulong low = 0, high = 0;

    unsigned size = state->getPointerSize();
    value = state->readCpuRegister(CPU_OFFSET(regs[R_EAX]), size * 8);
    state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]), &low, size);
    state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EDX]), &high, size);

    if (!low || !high) {
        getDebugStream(state) << "BaseInstructions: invalid arguments for range\n";
        return;
    }

    klee::Solver *solver = g_s2e->getExecutor()->getSolver(*state);

    Query query(state->constraints, value);
    range = solver->getRange(query);

    getDebugStream(state) << "BaseInstructions: range " << range.first << " to " << range.second << "\n";

    state->mem()->writeMemory(low, range.first);
    state->mem()->writeMemory(high, range.second);
}

void BaseInstructions::getConstraintsCountForExpression(S2EExecutionState *state)
{
    klee::ref<klee::Expr> value;

    unsigned size = state->getPointerSize();
    value = state->readCpuRegister(CPU_OFFSET(regs[R_EAX]), size * 8);

    Query query(state->constraints, value);
    std::vector< klee::ref<klee::Expr> > requiredConstraints;
    klee::getIndependentConstraintsForQuery(query, requiredConstraints);

    target_ulong result = requiredConstraints.size();
    state->regs()->write(CPU_OFFSET(regs[R_EAX]), &result, sizeof(result));
}

/**
 * Forks count times without adding constraints.
 */
void BaseInstructions::forkCount(S2EExecutionState *state)
{
    bool ok = true;
    target_ulong count;
    target_ulong nameptr;

    state->jumpToSymbolicCpp();

    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]),
                                         &count, sizeof count);

    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                         &nameptr, sizeof nameptr);

    std::string name;

    if (!state->mem()->readString(nameptr, name)) {
        getWarningsStream(state) << "Could not read string at address "
                                 << hexval(nameptr) << "\n";

        state->regs()->write<target_ulong>(CPU_OFFSET(regs[R_EAX]), -1);
        return;
    }

    klee::ref<klee::Expr> var = state->createConcolicValue<uint32_t>(name, 0);

    state->regs()->write(CPU_OFFSET(regs[R_EAX]), var);
    state->regs()->write<target_ulong>(CPU_OFFSET(eip), state->getPc() + 10);

    getDebugStream(state) << "s2e_fork: will fork " << count << " times with variable "
                          << var << "\n";

    for (unsigned i = 1; i < count; ++i) {
        klee::ref<klee::Expr> val = klee::ConstantExpr::create(i, var->getWidth());
        klee::ref<klee::Expr> cond = klee::NeExpr::create(var, val);

        klee::Executor::StatePair sp = s2e()->getExecutor()->forkCondition(state, cond, true, true);
        assert(sp.first == state);
        assert(sp.second && sp.second != sp.first);
    }

    klee::ref<klee::Expr> cond = klee::EqExpr::create(var, klee::ConstantExpr::create(0, var->getWidth()));
    state->addConstraint(cond);
}

/** Handle s2e_op instruction. Instructions:
    0f 3f XX XX XX XX XX XX XX XX
    XX: opcode
 */
void BaseInstructions::handleBuiltInOps(S2EExecutionState* state, uint64_t opcode)
{
    switch((opcode>>8) & 0xFF) {
        case BASE_S2E_CHECK: { /* s2e_check */
                target_ulong v = 1;
                state->writeCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]), &v,
                                                sizeof v);
            }
            break;
        case BASE_S2E_ENABLE_SYMBEX: state->enableSymbolicExecution(); break;
        case BASE_S2E_DISABLE_SYMBEX: state->disableSymbolicExecution(); break;

        case BASE_S2E_MAKE_SYMBOLIC: { /* s2e_make_symbolic */
            makeSymbolic(state, false);
            break;
        }

        case BASE_S2E_IS_SYMBOLIC: { /* s2e_is_symbolic */
            isSymbolic(state);
            break;
        }

        case BASE_S2E_GET_PATH_ID: { /* s2e_get_path_id */
            const klee::Expr::Width width = sizeof (target_ulong) << 3;
            state->writeCpuRegister(offsetof(CPUX86State, regs[R_EAX]),
                klee::ConstantExpr::create(state->getID(), width));
            break;
        }

        case BASE_S2E_KILL_STATE: { /* s2e_kill_state */
            killState(state);
            break;
            }

        case BASE_S2E_PRINT_EXR: { /* s2e_print_expression */
            printExpression(state);
            break;
        }

        case BASE_S2E_PRINT_MEM: { //Print memory contents
            printMemory(state);
            break;
        }

        case BASE_S2E_ENABLE_FORK: {
            state->enableForking();
            break;
        }

        case BASE_S2E_DISABLE_FORK: {
            state->disableForking();
            break;
        }

        case BASE_S2E_INVOKE_PLUGIN: {
            invokePlugin(state);
            break;
        }

        case BASE_S2E_ASSUME: {
            assume(state);
            break;
        }

        case BASE_S2E_ASSUME_DISJ: {
            assumeDisjunction(state);
            break;
        }

        case BASE_S2E_ASSUME_RANGE: {
            assumeRange(state);
            break;
        }

        case BASE_S2E_YIELD: {
            s2e()->getExecutor()->yieldState(*state);
            break;
        }

        case BASE_S2E_PRINT_MSG: { /* s2e_print_message */
            printMessage(state, opcode >> 16);
            break;
        }

        case BASE_S2E_MAKE_CONCOLIC: { /* s2e_make_concolic */
            makeSymbolic(state, true);
            break;
        }

        case BASE_S2E_BEGIN_ATOMIC: { /* s2e_begin_atomic */
            getDebugStream(state) << "BaseInstructions: s2e_begin_atomic\n";
            state->setStateSwitchForbidden(true);
            break;
        }

        case BASE_S2E_END_ATOMIC: { /* s2e_end_atomic */
            state->setStateSwitchForbidden(false);
            getDebugStream(state) << "BaseInstructions: s2e_end_atomic\n";
            break;
        }

        case BASE_S2E_CONCRETIZE: /* concretize */
            concretize(state, true);
            break;

        case BASE_S2E_EXAMPLE: { /* replace an expression by one concrete example */
            concretize(state, false);
            break;
        }

        case BASE_S2E_STATE_COUNT: { /* Get number of active states */
            target_ulong count = s2e()->getExecutor()->getStatesCount();
            state->writeCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]), &count,
                                            sizeof(count));
            break;
        }

        case BASE_S2E_INSTANCE_COUNT: { /* Get number of active S2E instances */
            target_ulong count = s2e()->getCurrentProcessCount();
            state->writeCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]), &count,
                                            sizeof(count));
            break;
        }

        case BASE_S2E_SLEEP: { /* Sleep for a given number of seconds */
           sleep(state);
           break;
        }

        case BASE_S2E_WRITE_BUFFER: { /* Write the given buffer to some guest memory location */
           writeBuffer(state);
           break;
        }

        case BASE_S2E_GET_RANGE: { /* Computes the upper and lower bounds for a symbolic variable */
           getRange(state);
           break;
        }

        case BASE_S2E_CONSTR_CNT: {
           getConstraintsCountForExpression(state);
           break;
        }

        case BASE_S2E_HEX_DUMP: {
           hexDump(state);
           break;
        }

        case BASE_S2E_SET_TIMER_INT: { /* disable/enable timer interrupt */
            uint8_t disabled = opcode >> 16;
            if(disabled)
                getDebugStream(state) << "Disabling timer interrupt\n";
            else
                getDebugStream(state) << "Enabling timer interrupt\n";
            state->regs()->write(CPU_OFFSET(timer_interrupt_disabled), disabled);
            break;
        }
        case BASE_S2E_SET_APIC_INT: { /* disable/enable all apic interrupts */
            uint8_t disabled = opcode >> 16;
            if(disabled)
                getDebugStream(state) << "Disabling all apic interrupt\n";
            else
                getDebugStream(state) << "Enabling all apic interrupt\n";
            state->regs()->write(CPU_OFFSET(all_apic_interrupts_disabled), disabled);
            break;
        }

        case BASE_S2E_GET_OBJ_SZ: { /* Gets the current S2E memory object size (in power of 2) */
            target_ulong size = SE_RAM_OBJECT_BITS;
            state->writeCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]), &size,
                                            sizeof size);
            break;
        }

        case BASE_S2E_CLEAR_TEMPS: {
            /**
             * Clear all temporary flags.
             * Useful to force concrete mode from guest code.
             */
            target_ulong val = 0;
            state->regs()->write(CPU_OFFSET(cc_op), val);
            state->regs()->write(CPU_OFFSET(cc_src), val);
            state->regs()->write(CPU_OFFSET(cc_dst), val);
            state->regs()->write(CPU_OFFSET(cc_tmp), val);
        } break;

        case BASE_S2E_FORK_COUNT: {
            forkCount(state);
        } break;

        default:
            getWarningsStream(state)
                << "BaseInstructions: Invalid built-in opcode " << hexval(opcode) << '\n';
            break;
    }
}

void BaseInstructions::onCustomInstruction(S2EExecutionState* state,
        uint64_t opcode)
{
    uint8_t opc = (opcode>>8) & 0xFF;
    if (opc <= 0x70) {
        handleBuiltInOps(state, opcode);
    }
}

void BaseInstructions::handleOpcodeInvocation(S2EExecutionState *state,
                                        uint64_t guestDataPtr,
                                        uint64_t guestDataSize)
{
    S2E_BASEINSTRUCTION_COMMAND command;

    if (guestDataSize != sizeof(command)) {
        getWarningsStream(state) <<
                "mismatched S2E_BASEINSTRUCTION_COMMAND size\n";
        exit(-1);
    }

    if (!state->mem()->readMemoryConcrete(guestDataPtr, &command, guestDataSize)) {
        getWarningsStream(state) <<
                "could not read transmitted data\n";
        exit(-1);
    }

    switch (command.Command) {
        case ALLOW_CURRENT_PID: {
            allowCurrentPid(state);
        } break;

        case GET_HOST_CLOCK_MS: {
            llvm::sys::TimeValue t = llvm::sys::TimeValue::now();
            command.Milliseconds = t.seconds() *  1000 + t.milliseconds();
            state->mem()->writeMemoryConcrete(guestDataPtr, &command, guestDataSize);
        } break;
    }
}

}
}
