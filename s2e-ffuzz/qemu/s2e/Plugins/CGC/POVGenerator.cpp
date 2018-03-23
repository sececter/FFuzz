///
/// Copyright (C) 2015-2016, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2014-2016, Cyberhaven, Inc
/// All rights reserved. Proprietary and confidential.
///
/// Distributed under the terms of S2E-LICENSE
///


#include <iomanip>
#include <cctype>
#include <zlib.h>
#include <llvm/ADT/StringMap.h>
#include <klee/util/ExprTemplates.h>

#include <s2e/S2E.h>
#include <s2e/Utils.h>
#include <s2e/S2EExecutionState.h>
#include <s2e/S2EExecutor.h>
#include <s2e/ConfigFile.h>

#include <klee/util/ExprUtil.h>

#include "POVGenerator.h"
#include "CGCMonitor.h"
#include "Recipe/RecipeDescriptor.h"

using namespace klee;

namespace s2e {
namespace plugins {

typedef POVGenerator::VariableRemapping VariableRemapping;

template<typename T> T evalExpr(const Assignment &solution, const ref<Expr> &e, Expr::Width w)
{
    ref<Expr> eval = solution.evaluate(e);
    s2e_assert(NULL, eval->getWidth() == w, "Incorrect size of expression: " << eval->getWidth() << " != " << w);

    ref<ConstantExpr> ce = dyn_cast<ConstantExpr>(eval);
    s2e_assert(NULL, !ce.isNull(), "Could not evaluate " << e);

    return ce->getZExtValue();
}

static bool evalBool(const Assignment &solution, const ref<Expr> &e)
{
    return evalExpr<bool>(solution, e, Expr::Bool);
}

static uint8_t evalByte(const Assignment &solution, const ref<Expr> &e)
{
    return evalExpr<uint8_t>(solution, e, Expr::Int8);
}

static uint32_t evalInt(const Assignment &solution, const ref<Expr> &e)
{
    return evalExpr<uint32_t>(solution, e, Expr::Int32);
}

/// \brief PoV entry which does not depend on execution state
///
/// Represents auxiliary PoV entry.
/// POVStaticEntries are created manually and
/// are not part of the POVEntry vector.
///
class POVStaticEntry {
public:
    virtual POVStaticEntry *clone() = 0;
    virtual ~POVStaticEntry() {}

    virtual void getXmlString(std::stringstream &ss) const {};
    virtual void getCString(std::stringstream &ss) const {};

    virtual void getString(std::stringstream &ss, bool xmlFormat) const {
        if (xmlFormat) {
            getXmlString(ss);
        } else {
            getCString(ss);
        }
    }
};

class POVEntry {
public:
    virtual POVEntry *clone() = 0;
    virtual ~POVEntry() {}

    virtual void getXmlString(std::stringstream &ss, const Assignment &solution, const VariableRemapping &remapping, bool debug) const {};
    virtual void getCString(std::stringstream &ss, const Assignment &solution, const VariableRemapping &remapping, bool debug) const {};

    virtual void getString(std::stringstream &ss, bool xmlFormat, const Assignment &solution, const VariableRemapping &remapping, bool debug) const {
        if (xmlFormat) {
            getXmlString(ss, solution, remapping, debug);
        } else {
            getCString(ss, solution, remapping, debug);
        }
    }
};

///
/// \brief Represents a state fork, printed as a comment in the POV.
///
/// For now, the POV generator only traces forks that depend on
/// a symbolic random variable. This is required by the fuzzer in order
/// to force execution down a specific path.
///
/// This is not part of the XML spec, so print it as a comment.
///
class POVFork : public POVEntry {
private:
    std::string m_module;
    uint64_t m_pc;
    ref<Expr> m_condition;

public:
    POVFork(const std::string &module, uint64_t pc, const ref<Expr> &condition):
        m_module(module), m_pc(pc), m_condition(condition) {}

    POVEntry *clone() {
        return new POVFork(*this);
    }

    void getXmlString(std::stringstream &ss, const Assignment &solution, const VariableRemapping &remapping, bool debug) const {
        bool outcome = evalBool(solution, m_condition);

        ss << "<!-- fork: module=" << m_module
           << " pc=" << hexval(m_pc)
           << " outcome=" << int(outcome)
           << " condition=" << m_condition
           << " -->\n\n";
    }

    void getCString(std::stringstream &ss, const Assignment &solution, const VariableRemapping &remapping, bool debug) const {
        // not needed in C pov
    }
};

///
/// \brief Represents the result of the random syscall.
///
/// This is for use by the fuzzer, which needs the concrete results
/// of the random number generator in order to replay the path.
///
/// This is not part of the XML spec, so print it as a comment.
///
class POVRandom : public POVEntry {
private:
    std::vector<ref<Expr>> m_data;

public:
    POVRandom(const std::vector<ref<Expr>> &data) {
        for (auto &e : data) {
            s2e_assert(NULL, e->getWidth() == Expr::Int8, "Variable is not of a byte size: " << e);
            m_data.push_back(e);
        }
    }

    POVEntry *clone() {
        return new POVRandom(*this);
    }

    void getXmlString(std::stringstream &ss, const Assignment &solution, const VariableRemapping &remapping, bool debug) const {
        ss << "<!-- random: value=";
        for (auto e: m_data) {
            ss << charval(evalByte(solution, e));
        }
        ss << " -->\n\n";
    }

    void getCString(std::stringstream &ss, const Assignment &solution, const VariableRemapping &remapping, bool debug) const {
        // not needed in C pov
    }
};

class POVDeclaration : public POVStaticEntry {
private:
    std::string m_name;
    std::string m_var;
    unsigned m_begin;
    unsigned m_end;

public:
    POVDeclaration(const std::string &newVarName, const std::string &existingVar, unsigned b, unsigned e) :
        m_name(newVarName), m_var(existingVar), m_begin(b), m_end(e) {}

    POVStaticEntry *clone() { return new POVDeclaration(*this); }

    void getXmlString(std::stringstream &ss) const {
        ss  << "<decl>\n"
            << "  <var>" << m_name << "</var>\n"
            << "  <value>\n"
            << "    <substr>\n"
            << "      <var>" << m_var << "</var>\n"
            << "      <begin>" << m_begin << "</begin>\n"
            << "      <end>" << (m_end + 1) << "</end>\n"
            << "    </substr>\n"
            << "  </value>\n"
            << "</decl>\n";
    }

    void getCString(std::stringstream &ss) const {
        s2e_warn_assert(NULL, false, "POVDeclaration must not be used in C PoV");
    }
};

class POVEntryReadWrite : public POVEntry {
protected:
    // Data size, can be symbolic
    ref<Expr> m_sizeExpr;

public:
    POVEntryReadWrite(const ref<Expr> &size) :
        m_sizeExpr(size) {}

    POVEntryReadWrite(uint32_t size) :
        m_sizeExpr(E_CONST(size, Expr::Int32)) {}
};

/* Data that is read by the CB from stdin */
class POVEntryWrite : public POVEntryReadWrite {
protected:
    std::vector<std::pair<ref<Expr> /* expr */, std::string> /* name */> m_input;

public:
    POVEntryWrite(const std::vector<std::pair<std::vector<ref<Expr>>, std::string>> &data, const ref<Expr> &sizeExpr) :
        POVEntryReadWrite(sizeExpr)
    {
        for (auto &it : data) {
            s2e_assert(NULL, it.first.size() == 1, "Same name " << it.second << " is used for " << it.first.size() << " variables");
            s2e_assert(NULL, it.first[0]->getWidth() == Expr::Int8, "Variable is not of a byte size: " << it.first[0]);
            m_input.push_back(std::make_pair(it.first[0], it.second));
        }
    }

    POVEntryWrite(const std::vector<uint8_t> &d) :
        POVEntryReadWrite(d.size())
    {
        for (auto &v : d) {
            m_input.push_back(std::make_pair(E_CONST(v, Expr::Int8), ""));
        }
    }

    POVEntry *clone() {
        return new POVEntryWrite(*this);
    }

    std::string getCVarName(std::string name) const {
        std::map<std::string /* recipe var name */, std::string /* C var name */> map;
        map[recipe::VARNAME_EIP] = "g_neg_t1.ipval";
        map[recipe::VARNAME_GP] = "g_neg_t1.regval";
        map[recipe::VARNAME_ADDR] = "g_neg_t2.region_addr";
        map[recipe::VARNAME_SIZE] = "g_neg_t2.read_size";

        for (auto kv: map) {
            if (name.find(kv.first) == 0) {
                return "GET_BYTE(" + kv.second + ", " + name[kv.first.length() + 1] + ")";
            }
        }

        return "g_var_" + name;
    }

    void getString(std::stringstream &ss, bool xmlFormat, const Assignment &solution, const VariableRemapping &remapping, bool debug) const {
        uint32_t concreteSize = evalInt(solution, m_sizeExpr);
        s2e_assert(NULL, concreteSize <= m_input.size(), "Symbolic size expression is solved to have invalid concrete value");

        ss << (xmlFormat ? "<!-- " : "  // ");
        ss << "write size: " << concreteSize << "/" << m_input.size();
        ss << (xmlFormat ? " -->\n" : "\n");

        if (!concreteSize) {
            ss << "\n";
            return;
        }

        const int allocThreshold = 4096;
        bool allocBuf = concreteSize > allocThreshold;

        if (xmlFormat) {
            ss << "<write>\n";
        } else {
            ss << "  do {\n";
            ss << "    size_t count = " << concreteSize << ";\n";
            if (allocBuf) {
                ss << "    uint8_t *buf = NULL;\n";
                ss << "    allocate(count, 0, (void**) &buf);\n";
            } else {
                ss << "    uint8_t buf[count];\n";
            }
            ss << "    uint8_t *p = buf;\n";
        }

        unsigned count = 0;

        for (auto &it : m_input) {
            const ref<Expr> &e = it.first;
            const std::string &name = it.second;

            uint8_t byte = evalByte(solution, e);
            auto remappedVar = remapping.find(name);

            if (remappedVar != remapping.end()) {
                if (xmlFormat) {
                    ss << "  <var>" << remappedVar->second << "</var>";

                    // This is for the fuzzer. It wants concrete value for the nonce.
                    // This is useful because cgcmonitor uses a fixed rng seed.
                    // Comment location is important.
                    ss << " <!-- fuzzer: " << charval(byte) << " -->";
                } else {
                    ss << "    *p++ = " << getCVarName(remappedVar->second) << ";";
                }
            } else {
                if (xmlFormat) {
                    ss << "  <data>" << charval(byte) << "</data>";
                } else {
                    ss << "    *p++ = " << cbyte(byte) << ";";
                }
            }

            if (debug && !isa<ConstantExpr>(e)) {
                ss << (xmlFormat ? " <!-- " : " // ");
                ss << e;
                ss << (xmlFormat ? " -->" : "");
            }

            ss << "\n";

            ++count;
            if (count == concreteSize) {
                break;
            }
        }

        if (xmlFormat) {
            ss << "</write>\n";
        } else {
            ss << "    transmit_all(STDOUT, buf, count);\n";
            if (allocBuf) {
                ss << "    deallocate(buf, count);\n";
            }
            ss << "  } while (0);\n";
        }

        ss << "\n";

        s2e_assert(NULL, count == concreteSize, "Processed " << count << " bytes instead of " << concreteSize);
    }
};

/* Data that is written by the CB to stdout */
class POVEntryRead: public POVEntryReadWrite {
protected:
    std::vector<ref<Expr>> m_output;

public:
    POVEntryRead(const std::vector<ref<Expr>> &data, ref<Expr> sizeExpr) :
        POVEntryReadWrite(sizeExpr)
    {
        for (auto &e : data) {
            s2e_assert(NULL, e->getWidth() == Expr::Int8, "Variable is not of a byte size: " << e);
            m_output.push_back(e);
        }
    }

    POVEntry *clone() {
        return new POVEntryRead(*this);
    }

    bool hasNonces() const {
        for (auto e : m_output) {
            if (POVGenerator::isRandomRead(e)) {
                return true;
            }
        }
        return false;
    }

    void getString(std::stringstream &ss, bool xmlFormat, const Assignment &solution, const VariableRemapping &remapping, bool debug) const {
        uint32_t concreteSize = evalInt(solution, m_sizeExpr);
        s2e_assert(NULL, concreteSize <= m_output.size(), "Symbolic size expression is solved to have invalid concrete value");

        if (debug) {
            ss << (xmlFormat ? "<!-- " : "  // ");
            for (auto e : m_output) {
                if (isa<ConstantExpr>(e)) {
                    ss << charval(dyn_cast<ConstantExpr>(e)->getZExtValue());
                } else {
                    ss << e;
                }
                ss << " ";
            }
            ss << "|| ";
            for (auto e : m_output) {
                ss << charval(evalByte(solution, e));
            }
            ss << (xmlFormat ? " -->\n" : "\n");
        }

        ss << (xmlFormat ? "<!-- " : "  // ");
        ss << "read size: " << concreteSize << "/" << m_output.size();
        ss << (xmlFormat ? " -->\n" : "\n");

        if (!concreteSize) {
            ss << "\n";
            return;
        }

        if (!hasNonces()) {
            if (xmlFormat) {
                ss << "<read><length>" << concreteSize << "</length></read>\n";
            } else {
                ss << "  receive_null(STDIN, " << concreteSize << ");\n";
            }
            ss << "\n";
            return;
        }

        unsigned count = 0;

        for (auto e : m_output) {
            if (POVGenerator::isRandomRead(e)) {
                ref<ReadExpr> re = dyn_cast<ReadExpr>(e);
                const Array *root = re->getUpdates().getRoot();

                if (xmlFormat) {
                    ss << "<read>\n";
                    ss << "  <length>1</length>\n";
                    ss << "  <assign> <var>" << root->getName() << "</var> <slice begin=\"0\" end=\"1\" />";

                    // This comment is required by the fuzzer
                    ss << " <!-- fuzzer: " << charval(evalByte(solution, e)) << " -->";

                    ss << " </assign>\n";
                    ss << "</read>\n";
                } else {
                    ss << "  uint8_t g_var_" << root->getName() << " = 0;\n";
                    ss << "  receive_all(STDIN, &g_var_" << root->getName() << ", 1);\n";
                }
            } else {
                if (xmlFormat) {
                    ss << "<read><length>1</length></read>\n";
                } else {
                    ss << "  receive_null(STDIN, 1);\n";
                }
            }

            count++;
            if (count == concreteSize) {
                break;
            }
        }

        ss << "\n";

        s2e_assert(NULL, count == concreteSize, "Processed " << count << " bytes instead of " << concreteSize);
    }
};

class POVEntryDelay: public POVStaticEntry {
protected:
    int m_timeout;

public:
    POVEntryDelay(int timeout) : m_timeout(timeout) {}

    POVStaticEntry *clone() {
        return new POVEntryDelay(*this);
    }

    void getXmlString(std::stringstream &ss) const {
        ss << "<delay>" << m_timeout << "</delay>\n\n";
    }

    void getCString(std::stringstream &ss) const {
        ss << "  delay(" << m_timeout << ");\n\n";
    }
};

/* Compare ReadExpr by their string name, e.g.: v1_receive_1 < v2_receive_2 */
struct ReadExprComparator {
  bool operator() (const ref<ReadExpr> &lhs, const ref<ReadExpr> &rhs) const {
      return lhs->getUpdates().getRoot()->getName() < rhs->getUpdates().getRoot()->getName();
  }
};

class POVGeneratorState: public PluginState {
private:
    std::vector<POVEntry*> m_entries;

public:
    POVGeneratorState(const POVGeneratorState &o) {
        for (unsigned i = 0; i < o.m_entries.size(); ++i) {
            m_entries.push_back(o.m_entries[i]->clone());
        }
    }

    POVGeneratorState* clone() const {
        return new POVGeneratorState(*this);
    }

    POVGeneratorState() {
    }

    static PluginState *factory(Plugin *p, S2EExecutionState *s) {
        return new POVGeneratorState();
    }

    ~POVGeneratorState() {
        for (unsigned i = 0; i < m_entries.size(); ++i) {
            delete m_entries[i];
        }
    }

    void addEntry(POVEntry *entry) {
        m_entries.push_back(entry);
    }

    void getString(std::stringstream &ss, bool xmlFormat, const Assignment &solution, const VariableRemapping &remapping, bool debug) const {
        for (unsigned i = 0; i < m_entries.size(); ++i) {
            m_entries[i]->getString(ss, xmlFormat, solution, remapping, debug);
        }
    }

    std::string getReadName(const ref<ReadExpr> &re) const {
        return re->getUpdates().getRoot()->getName();
    }

    /* Collect the sets containing the ReadExpr for random and receive system calls */
    void collect(const ref<Expr> &root,
                 std::set<ref<ReadExpr>, ReadExprComparator> &receive,
                 std::set<ref<ReadExpr>, ReadExprComparator> &random) const
    {
        std::vector<ref<ReadExpr>> reads;
        findReads(root, false, reads);

        for (auto it : reads) {
            if (POVGenerator::isRandomRead(it)) {
                random.insert(it);
            } else if (POVGenerator::isReceiveRead(it)) {
                receive.insert(it);
            }
        }
    }

    /// \brief Match random and receive bytes in two expressions that must be equal
    ///
    /// Figure out when receive variable must equal random variable and store
    /// their names in \param remapping.
    ///
    /// If both expressions are of Concat type, this function will traverse their children.
    ///
    /// \param a first expression
    /// \param b second expression
    /// \param visited set of already visited expressions
    /// \param remapping receive to random variables map
    /// \returns false if nonces were detected, but we can't handle them. Otherwise true
    ///
    bool matchNoncePairs(const ref<Expr> &a, const ref<Expr> &b, std::set<ref<Expr>> &visited, VariableRemapping &remapping) const
    {
        if (visited.count(a) || visited.count(b)) {
            g_s2e->getDebugStream(g_s2e_state) << "Circular dependency in expression\n";
            return false;
        }
        visited.insert(a);
        visited.insert(b);

        if (a->getWidth() != b->getWidth()) {
            g_s2e->getDebugStream(g_s2e_state) << "Expressions have different width\n";
            return false;
        }

        if (isa<ConcatExpr>(a) && isa<ConcatExpr>(b)) {
            for (int i = 0; i < 2; i++) {
                if (!matchNoncePairs(a->getKid(i), b->getKid(i), visited, remapping)) {
                    return false;
                }
            }
        } else {
            std::set<ref<ReadExpr>, ReadExprComparator > aReceive, bReceive;
            std::set<ref<ReadExpr>, ReadExprComparator > aRandom, bRandom;

            collect(a, aReceive, aRandom);
            collect(b, bReceive, bRandom);

            if ((aReceive.size() == 1 && bRandom.size() == 1) || (bReceive.size() == 1 && aRandom.size() == 1)) {
                // Each expression contains either one random or one receive byte, they must be equal

                std::string rcv = getReadName(*(aReceive.size() ? aReceive.begin() : bReceive.begin()));
                std::string rnd = getReadName(*(aRandom.size() ? aRandom.begin() : bRandom.begin()));

                if (remapping.count(rcv)) {
                    g_s2e->getDebugStream(g_s2e_state) << "Can't remap " << rcv << " to " << rnd << ": already remapped to " << remapping[rcv] << "\n";
                    return false;
                }
                remapping[rcv] = rnd;
            } else if ((aReceive.size() || bReceive.size()) && (aRandom.size() || bRandom.size())) {
                // Both random and receive bytes are present, have no idea how to match them

                g_s2e->getDebugStream(g_s2e_state) << "Don't know how to match nonce pairs in two expressions\nFirst: " << a << "\nSecond: " << b << "\n";
                return false;
            }
        }

        return true;
    }

    /// \brief Match random and receive bytes in constraints set
    ///
    /// Figure out when receive variable must equal random variable and store
    /// their names in \param remapping.
    ///
    /// \param constraints constraints set
    /// \param remapping receive to random variables map
    /// \returns false if nonces were detected, but we can't handle them. Otherwise true
    ///
    bool matchNoncePairs(const ConstraintManager &constraints, VariableRemapping &remapping) const
    {
        std::set<ref<Expr>> cset = constraints.getConstraintSet();

        for (auto e : cset) {
            std::set<ref<ReadExpr>, ReadExprComparator> receive;
            std::set<ref<ReadExpr>, ReadExprComparator> random;

            collect(e, receive, random);

            if (receive.size() == 1 && random.size() == 1) {
                // Expression contains one random and one receive byte, assume these bytes must be equal.
                // TODO: Handle a more complex dependency between those bytes.

                std::string rcv = getReadName(*receive.begin());
                std::string rnd = getReadName(*random.begin());

                if (remapping.count(rcv)) {
                    g_s2e->getDebugStream(g_s2e_state) << "Can't remap " << rcv << " to " << rnd << ": already remapped to " << remapping[rcv] << "\n";
                    return false;
                }
                remapping[rcv] = rnd;
            } else if (random.size() && receive.size() == random.size()) {
                // Expression contains equal number of random and receive bytes

                ConcatExprPairs concatExprs;
                if (!getConcatExprPairs(e, concatExprs)) {
                    g_s2e->getDebugStream(g_s2e_state) << "Nonces are not stored in concat expressions\n" << e << "\n";
                    return false;
                }

                for (auto p: concatExprs) {
                    std::set<ref<Expr>> visited;
                    if (!matchNoncePairs(p.first, p.second, visited, remapping)) {
                        g_s2e->getDebugStream(g_s2e_state) << "Failed to match nonce pairs for concat expressions\nFirst: " << p.first << "\nSecond: " << p.second << "\n";
                        return false;
                    }
                }
            } else if (random.size() && receive.size()) {
                // Both random and receive bytes are present, have no idea how to match them

                g_s2e->getDebugStream(g_s2e_state) << "Don't know how to match nonce pairs in expression\n" << e << "\n";
                return false;
            }
        }

        return true;
    }
};

const std::string POVGenerator::XML_HEADER =
        "<?xml version=\"1.0\" standalone=\"no\" ?>\n"
        "<!DOCTYPE cfepov SYSTEM \"/usr/share/cgc-docs/cfe-pov.dtd\">\n"
        "<cfepov>\n"
        "  <cbid>service</cbid>\n"
        "    <replay>\n\n";

const std::string POVGenerator::XML_FOOTER =
        "    </replay>\n"
        "</cfepov>\n";

const std::string POVGenerator::C_HEADER =
        "#include <libcgc.h>\n"
        "\n"
        "typedef unsigned int uint32_t;\n"
        "typedef unsigned char uint8_t;\n"
        "\n"
        "#define MIN(a, b)       ( (a) < (b) ? (a) : (b) )\n"
        "#define GET_BYTE(v, i)  ( ( ((uint32_t)(v)) >> ((i) * 8) ) & 0xFF )\n"
        "\n"
        "#define NEG_FD  3\n"
        "\n"
        "typedef struct type1_vals_ {\n"
        "  uint32_t ipval;\n"
        "  uint32_t regval;\n"
        "} type1_vals;\n"
        "\n"
        "typedef struct type2_vals_ {\n"
        "  uint32_t region_addr;\n"
        "  uint32_t region_size;\n"
        "  uint32_t read_size;\n"
        "} type2_vals;\n"
        "\n"
        "void transmit_all(int fd, void *buf, size_t count) {\n"
        "  size_t total = 0;\n"
        "  while (total < count) {\n"
        "    size_t s = 0;\n"
        "    transmit(fd, buf + total, count - total, &s);\n"
        "    total += s;\n"
        "  }\n"
        "}\n"
        "\n"
        "void receive_all(int fd, void *buf, size_t count) {\n"
        "  size_t total = 0;\n"
        "  while (total < count) {\n"
        "    size_t s = 0;\n"
        "    receive(fd, buf + total, count - total, &s);\n"
        "    total += s;\n"
        "  }\n"
        "}\n"
        "\n"
        "void receive_null(int fd, size_t count) {\n"
        "  uint8_t buf[256];\n"
        "  while (count) {\n"
        "    size_t s = MIN(count, sizeof(buf));\n"
        "    receive_all(fd, buf, s);\n"
        "    count -= s;\n"
        "  }\n"
        "}\n"
        "\n"
        "void type1_negotiate(uint32_t ipmask, uint32_t regmask, uint32_t regnum, type1_vals *t1vals)\n"
        "{\n"
        "  uint32_t povType = 1;\n"
        "  transmit_all(NEG_FD, &povType, sizeof(povType));\n"
        "  transmit_all(NEG_FD, &ipmask, sizeof(ipmask));\n"
        "  transmit_all(NEG_FD, &regmask, sizeof(regmask));\n"
        "  transmit_all(NEG_FD, &regnum, sizeof(regnum));\n"
        "  receive_all(NEG_FD, t1vals, sizeof(*t1vals));\n"
        "}\n"
        "\n"
        "void type2_negotiate(type2_vals *t2vals)\n"
        "{\n"
        "  uint32_t povType = 2;\n"
        "  transmit_all(NEG_FD, &povType, sizeof(povType));\n"
        "  receive_all(NEG_FD, t2vals, sizeof(*t2vals));\n"
        "}\n"
        "\n"
        "void delay(uint32_t msec) {\n"
        "  struct timeval timeout;\n"
        "  timeout.tv_sec = msec / 1000;\n"
        "  timeout.tv_usec = (msec % 1000) * 1000;\n"
        "  fdwait(STDIN, NULL, NULL, &timeout, NULL);\n"
        "}\n"
        "\n"
        "void memset(void *b, int c, size_t len)\n"
        "{\n"
        "  for (int i = 0; i < len; i++ ) {\n"
        "    ((unsigned char*)b)[i] = c;\n"
        "  }\n"
        "}\n"
        "\n"
        "int main(void)\n"
        "{\n";

const std::string POVGenerator::C_FOOTER =
        "}\n";

S2E_DEFINE_PLUGIN(POVGenerator, "POVGenerator plugin", "POVGenerator", "ExecutionTracer", "CGCMonitor", "ProcessExecutionDetector",
                  "ModuleExecutionDetector");

POVGenerator::POVGenerator(S2E* s2e)
        : Plugin(s2e)
{
    m_numPOVs = 0;
}

void POVGenerator::initialize()
{
    m_monitor = s2e()->getPlugin<CGCMonitor>();
    m_detector = s2e()->getPlugin<ProcessExecutionDetector>();
    m_modules = s2e()->getPlugin<ModuleExecutionDetector>();
    m_seedSearcher = s2e()->getPlugin<seeds::SeedSearcher>();

    ConfigFile *cfg = s2e()->getConfig();

    m_compress = cfg->getBool(getConfigKey() + ".compress", false);

    m_monitor->onWrite.connect(sigc::mem_fun(*this, &POVGenerator::onWrite));
    m_monitor->onSymbolicRead.connect(sigc::mem_fun(*this, &POVGenerator::onSymbolicRead));
    m_monitor->onConcreteRead.connect(sigc::mem_fun(*this, &POVGenerator::onConcreteRead));
    m_monitor->onRandom.connect(sigc::mem_fun(*this, &POVGenerator::onRandom));

    s2e()->getCorePlugin()->onStateFork.connect_front(sigc::mem_fun(*this, &POVGenerator::onStateFork));
}

void POVGenerator::onRandom(S2EExecutionState *state,
                            uint64_t pid, const std::vector<ref<Expr>> &data)
{
    DECLARE_PLUGINSTATE(POVGeneratorState, state);
    POVRandom *rnd = new POVRandom(data);
    plgState->addEntry(rnd);
}

/// Catch fork that branch on random input in order to let the fuzzer know about it.
void POVGenerator::onStateFork(S2EExecutionState *state,
                               const std::vector<S2EExecutionState*> &newStates,
                               const std::vector<ref<Expr> > &newConditions)
{
    bool hasRandom = false;
    bool hasReceive = false;
    for (auto e : newConditions) {
        std::vector<const Array*> results;
        findSymbolicObjects(e, results);
        for (auto a : results) {
            hasRandom |= POVGenerator::isRandom(a);
            hasReceive |= POVGenerator::isReceive(a);
        }
    }

    if (!(hasRandom && hasReceive)){
        return;
    }

    getDebugStream(state) << "Found branch on symbolic random value at " << hexval(state->getPc()) << "\n";
    const ModuleDescriptor *module = m_modules->getCurrentDescriptor(state);
    if (!module) {
        // XXX: the kernel forks sometimes on the input and the random value, need to investigate
        getWarningsStream(state) << "Could not fetch module";
        return;
    }

    uint64_t pc = module->ToNativeBase(state->getPc());

    for (unsigned i = 0; i < newStates.size(); ++i) {
        DECLARE_PLUGINSTATE(POVGeneratorState, newStates[i]);
        POVFork *forkEntry = new POVFork(module->Name, pc, newConditions[i]);
        plgState->addEntry(forkEntry);
    }

    onRandomInputFork.emit(state, module);
}

bool POVGenerator::isReceive(const Array *array)
{
    return array->getRawName() == "receive";
}

bool POVGenerator::isRandom(const Array *array)
{
    return array->getRawName() == "random";
}

bool POVGenerator::isRandomRead(const ref<Expr> &e) {
    ref<ReadExpr> re = dyn_cast<ReadExpr>(e);
    if (re.isNull()) {
        return false;
    }
    return isRandom(re->getUpdates().getRoot());
}

bool POVGenerator::isReceiveRead(const ref<Expr> &e) {
    ref<ReadExpr> re = dyn_cast<ReadExpr>(e);
    if (re.isNull()) {
        return false;
    }
    return isReceive(re->getUpdates().getRoot());
}

void POVGenerator::onWrite(S2EExecutionState *state,
                           uint64_t pid, uint64_t fd,
                           const std::vector<ref<Expr>> &data,
                           ref<Expr> sizeExpr)
{
    if (!m_detector->isTracked(state, pid)) {
        return;
    }

    s2e_assert(state, CGCMonitor::isWriteFd(fd), "Invalid write fd " << hexval(fd));

    DECLARE_PLUGINSTATE(POVGeneratorState, state);
    POVEntryRead *entry = new POVEntryRead(data, sizeExpr);
    plgState->addEntry(entry);
}

void POVGenerator::onSymbolicRead(S2EExecutionState* state, uint64_t pid, uint64_t fd, uint64_t size,
        const std::vector<std::pair<std::vector<ref<Expr>>, std::string> > &data,
        ref<Expr> sizeExpr)
{
    if (!m_detector->isTracked(state, pid)) {
        return;
    }

    s2e_assert(state, CGCMonitor::isReadFd(fd), "Invalid read fd " << hexval(fd));

    if (!size) {
        return;
    }

    DECLARE_PLUGINSTATE(POVGeneratorState, state);
    POVEntry *entry = new POVEntryWrite(data, sizeExpr);
    plgState->addEntry(entry);
}

void POVGenerator::onConcreteRead(S2EExecutionState *state,
                           uint64_t pid, uint64_t fd,
                           const std::vector<uint8_t> &data)
{
    if (!m_detector->isTracked(state, pid)) {
        return;
    }

    s2e_assert(state, CGCMonitor::isReadFd(fd), "Invalid concrete read fd " << hexval(fd));

    if (!data.size()) {
        return;
    }

    DECLARE_PLUGINSTATE(POVGeneratorState, state);
    POVEntry *entry = new POVEntryWrite(data);
    plgState->addEntry(entry);
}

/**
 * Save PoV to file.
 * PoV submission has to be carried out in a separate plugin.
 */
std::string POVGenerator::writeToFile(S2EExecutionState *state, const PovOptions &opt,
                                      const std::string &filePrefix, const std::string &fileExtWithoutDot,
                                      const std::string &pov)
{
    std::vector<uint8_t> contents;
    if(m_compress) {
        contents = compress(pov);
    } else {
        contents.insert(contents.end(), pov.begin(), pov.end());
    }

    std::stringstream povFileNameSS;

    if (filePrefix.length() > 0) {
        povFileNameSS << filePrefix << "-";
    }

    switch (opt.m_type) {
        case POV_TYPE1: povFileNameSS << "pov-type1-"; break;
        case POV_TYPE2: povFileNameSS << "pov-type2-"; break;
        default: povFileNameSS << "pov-unknown-"; break;
    }

    povFileNameSS << state->getID() << "." << fileExtWithoutDot;
    if (m_compress) {
        povFileNameSS << ".zlib"; // use `zlib-flate -uncompress`
    }

    std::string povFileName = g_s2e->getOutputFilename(povFileNameSS.str());

    FILE* povFile = fopen(povFileName.c_str(), "wb");

    if (!povFile) {
        g_s2e->getWarningsStream(state) << "Could not create POV file" << '\n';
        exit(-1);
    }

    if (fwrite(&contents[0], 1, contents.size(), povFile) != contents.size()) {
        int err = errno;
        g_s2e->getWarningsStream(state) << "Could not write POV file (" << strerror(err) << ")\n";
        exit(-1);
    }

    if (fclose(povFile) != 0) {
        g_s2e->getWarningsStream(state) << "Error closing POV file" << '\n';
        exit(-1);
    }

    g_s2e->getInfoStream(state) << "POV saved to file " << povFileName << "\n";

    return povFileName;
}

bool POVGenerator::solveConstraints(S2EExecutionState *state, const PovOptions &opt, Assignment &assignment)
{
    using namespace klee;

    if (opt.m_extraConstraints.size() > 0) {
        ConstraintManager constraints = state->constraints;
        for (auto it : opt.m_extraConstraints) {
            constraints.addConstraint(it);
        }

        // TODO: all this constraints to assignment could be moved to klee
        // TODO: try to preserve existing concolics as much as possible

        // Extract symbolic objects
        std::vector<const Array*> symbObjects;
        for (unsigned i = 0; i < state->symbolics.size(); ++i) {
            symbObjects.push_back(state->symbolics[i].second);
        }

        std::vector<std::vector<unsigned char> > concreteObjects;

        //XXX: Not sure about passing state, we may have new constraints
        Solver *solver = s2e()->getExecutor()->getSolver(*state);
        Query query(constraints, ConstantExpr::alloc(0, Expr::Bool));
        if (!solver->getInitialValues(query, symbObjects, concreteObjects)) {
            getWarningsStream() << "Could not get symbolic solution\n";
            return false;
        }

        for (unsigned i = 0; i < symbObjects.size(); ++i) {
            assignment.add(symbObjects[i], concreteObjects[i]);
        }
    } else {
        assignment = *state->concolics;
    }

    return true;
}

///
/// \brief Extracts all the conditions from select statements
/// and adds their true form to the unmerged list.
///
/// Given an appropriate variable assignment and the following expression:
/// (Eq (w32 0x0)
/// (Select w32 (Not (Eq N0:(Read w8 0x0 v1_random_1) N1:(Read w8 0x0 v5_receive_5)))
///             (Sub w32 (ZExt w32 N0) (ZExt w32 N1))
///             (Select w32 (Not (Eq N2:(Read w8 0x0 v2_random_2) N3:(Read w8 0x0 v6_receive_6)))
///                         (Sub w32 (ZExt w32 N2) (ZExt w32 N3))
///                         (Select w32 (Not (Eq N4:(Read w8 0x0 v3_random_3) N5:(Read w8 0x0 v7_receive_7)))
///                                     (Sub w32 (ZExt w32 N4) (ZExt w32 N5))
///                                     (Select w32 (Not (Eq N6:(Read w8 0x0 v4_random_4) N7:(Read w8 0x0 v8_receive_8)))
///                                                 (Sub w32 (ZExt w32 N6) (ZExt w32 N7))
///                                                 (w32 0x0))))))
///
/// this function will return in the unmerged list the following expressions:
///
/// (Eq N0:(Read w8 0x0 v1_random_1) N1:(Read w8 0x0 v5_receive_5))
/// (Eq N2:(Read w8 0x0 v2_random_2) N3:(Read w8 0x0 v6_receive_6))
/// (Eq N4:(Read w8 0x0 v3_random_3) N5:(Read w8 0x0 v7_receive_7))
/// (Eq N6:(Read w8 0x0 v4_random_4) N7:(Read w8 0x0 v8_receive_8))
///
/// \param unmerged the list of extracted expressions
/// \param explored the set of already explored expressions
/// \param assignment variable assignment
/// \param e the expression whose select statements are to be extracted
///
static void UnmergeSelect(std::vector<ref<Expr>> &unmerged,
                          std::set<ref<Expr>> &explored,
                          const Assignment &assignment,
                          const ref<Expr> &e)
{
    if (explored.count(e)) {
        return;
    }

    explored.insert(e);

    ref<SelectExpr> se = dyn_cast<SelectExpr>(e);
    if (se.isNull()) {
        for (unsigned i = 0; i < e->getNumKids(); ++i) {
            UnmergeSelect(unmerged, explored, assignment, e->getKid(i));
        }
        return;
    }

    auto cond = se->getKid(0);
    auto t = se->getKid(1);
    auto f = se->getKid(2);

    bool outcome = evalBool(assignment, cond);

    ref<Expr> ne;
    if (outcome) {
        ne = t;
        unmerged.push_back(cond);
    } else {
        ne = f;
        unmerged.push_back(NotExpr::create(cond));
    }

    UnmergeSelect(unmerged, explored, assignment, ne);
}

///
/// \brief UnmergeSelects unmerges all select statement in the given
/// constraint manager
///
/// This function simplifies the task of matching random variables
/// with symbolic input.
///
/// Preconditions:
///
///   - Every constraint must evaluate to true using the given variable assignment
///
/// \param mgr the constraint manager
/// \param variable assignment
///
void POVGenerator::unmergeSelects(ConstraintManager &mgr, const Assignment &assignment)
{
    std::vector<ref<Expr>> unmergedSelects;
    std::set<ref<Expr>> explored;

    for (auto cs : mgr.getConstraintSet()) {
        UnmergeSelect(unmergedSelects, explored, assignment, cs);
    }

    for (auto cs : unmergedSelects) {
        bool outcome = evalBool(assignment, cs);
        s2e_assert(NULL, outcome, "Constraint did not evaluate to true with given assignment");

        getDebugStream() << "unmergeSelects: adding " << cs << "\n";
        mgr.addConstraint(cs);
    }
}

std::string POVGenerator::generatePoV(bool xmlFormat, uint64_t seedIndex, const POVGeneratorState *plgState,
                                      const PovOptions &opt, const VariableRemapping &remapping,
                                      const Assignment &solution, const ConstraintManager &constraints)
{
    // TODO: check if this is really needed
    POVEntryDelay delay(100);

    std::stringstream ss;

    ss << (xmlFormat ? XML_HEADER : C_HEADER);
    if ((int) seedIndex != -1) {
        ss << (xmlFormat ? "<!-- " : "  // ");
        ss << "seed index: " << seedIndex;
        ss << (xmlFormat ? " -->\n" : "\n");
        ss << "\n";
    }
    generateNegotiate(ss, xmlFormat, opt);
    plgState->getString(ss, xmlFormat, solution, remapping, getLogLevel() <= LOG_DEBUG);
    delay.getString(ss, xmlFormat);
    generateReadSecret(ss, xmlFormat, opt);
    ss << (xmlFormat ? XML_FOOTER : C_FOOTER);

    return ss.str();
}

void POVGenerator::generatePoV(S2EExecutionState *state, const PovOptions &opt, std::string &xmlPov, std::string &cPov)
{
    DECLARE_PLUGINSTATE_CONST(POVGeneratorState, state);

    Assignment solution;
    if (!solveConstraints(state, opt, solution)) {
        return;
    }

    unmergeSelects(state->constraints, solution);

    // Detect nonces
    VariableRemapping remapping = opt.m_remapping;
    if (!plgState->matchNoncePairs(state->constraints, remapping)) {
        getDebugStream(state) << "Can't match nonce pairs\n";
        return;
    }

    uint64_t seedIndex = -1;
    if (m_seedSearcher) {
        seedIndex = m_seedSearcher->getSubtreeSeedIndex(state);
    }

    xmlPov = generatePoV(true, seedIndex, plgState, opt, remapping, solution, state->constraints);
    cPov = generatePoV(false, seedIndex, plgState, opt, remapping, solution, state->constraints);
}

/* http://stackoverflow.com/questions/4538586/how-to-compress-a-buffer-with-zlib */
void POVGenerator::compress(void *in_data, size_t in_data_size, std::vector<uint8_t> &out_data)
{
    std::vector<uint8_t> buffer;

    const size_t BUFSIZE = 128 * 1024;
    uint8_t temp_buffer[BUFSIZE];

    z_stream strm;
    strm.zalloc = 0;
    strm.zfree = 0;
    strm.next_in = reinterpret_cast<uint8_t *>(in_data);
    strm.avail_in = in_data_size;
    strm.next_out = temp_buffer;
    strm.avail_out = BUFSIZE;

    deflateInit(&strm, Z_BEST_COMPRESSION);

    while (strm.avail_in != 0) {
        int res = deflate(&strm, Z_NO_FLUSH);
        s2e_assert(NULL, res == Z_OK, "Deflate error");
        if (strm.avail_out == 0) {
            buffer.insert(buffer.end(), temp_buffer, temp_buffer + BUFSIZE);
            strm.next_out = temp_buffer;
            strm.avail_out = BUFSIZE;
        }
    }

    int deflate_res = Z_OK;
    while (deflate_res == Z_OK) {
        if (strm.avail_out == 0) {
            buffer.insert(buffer.end(), temp_buffer, temp_buffer + BUFSIZE);
            strm.next_out = temp_buffer;
            strm.avail_out = BUFSIZE;
        }
        deflate_res = deflate(&strm, Z_FINISH);
    }

    s2e_assert(NULL, deflate_res == Z_STREAM_END, "Deflate error");
    buffer.insert(buffer.end(), temp_buffer, temp_buffer + BUFSIZE - strm.avail_out);
    deflateEnd(&strm);

    out_data.swap(buffer);
}

std::vector<uint8_t> POVGenerator::compress(const std::string &s)
{
    std::vector<uint8_t> compressed;
    compress((void *) s.c_str(), s.size(), compressed);
    return compressed;
}

void POVGenerator::generateNegotiate(std::stringstream &ss, bool xmlFormat, const PovOptions &opt)
{
    if (opt.m_type == POV_GENERAL) {
        /**
         * This is useful to generate crash povs, that don't have an exploit.
         * Masks should have enough bits so that cb-test does not complain.
         */
        if (xmlFormat) {
            ss << "<!-- dummy negotiation -->\n";
            ss << "<negotiate>\n";
            ss << "  <type1>\n";
            ss << "    <ipmask>" << 0xffffffff << "</ipmask>\n";
            ss << "    <regmask>" << 0xffffffff << "</regmask>\n";
            ss << "    <regnum>" << 0 << "</regnum>\n";
            ss << "  </type1>\n";
            ss << "</negotiate>\n";
        } else {
            ss << "  // dummy negotiation\n";
            ss << "  type1_vals g_neg_t1 = { 0 };\n";
            ss << "  type1_negotiate(0xffffffff, 0xffffffff, 0, &g_neg_t1);\n";
        }
    } else if (opt.m_type == POV_TYPE1) {
        if (xmlFormat) {
            ss << "<negotiate>\n";
            ss << "  <type1>\n";
            ss << "    <ipmask>" << hexval(opt.m_ipMask) << "</ipmask>\n";
            ss << "    <regmask>" << hexval(opt.m_regMask) << "</regmask>\n";
            ss << "    <regnum>" << opt.m_regNum << "</regnum>\n";
            ss << "  </type1>\n";
            ss << "</negotiate>\n\n";

            /**
             * Slice variables into bytes so that it is easier to
             * reuse them in the pov (read elements often have only one byte).
             */

            for (unsigned i = 0; i < 4; ++i) {
                std::stringstream ss1;
                ss1 << recipe::VARNAME_EIP << "[" << i << "]";
                POVDeclaration decl(ss1.str(), "TYPE1_IP", i, i);
                decl.getString(ss, true);
            }

            for (unsigned i = 0; i < 4; ++i) {
                std::stringstream ss1;
                ss1 << recipe::VARNAME_GP << "[" << i << "]";
                POVDeclaration decl(ss1.str(), "TYPE1_REG", i, i);
                decl.getString(ss, true);
            }
        } else {
            ss << "  type1_vals g_neg_t1 = { 0 };\n";
            ss << "  type1_negotiate("
                    << hexval(opt.m_ipMask) << ", "
                    << hexval(opt.m_regMask) << ", "
                    << opt.m_regNum << ", "
                    << "&g_neg_t1);\n";
        }
    } else if (opt.m_type == POV_TYPE2) {
        if (xmlFormat) {
            ss << "<negotiate>\n";
            ss << "  <type2/>\n";
            ss << "</negotiate>\n\n";

            /**
             * Slice variables into bytes so that it is easier to
             * reuse them in the pov (read elements often have only one byte).
             */

            for (unsigned i = 0; i < 4; ++i) {
                std::stringstream ss1;
                ss1 << recipe::VARNAME_ADDR << "[" << i << "]";
                POVDeclaration decl(ss1.str(), "TYPE2_ADDR", i, i);
                decl.getString(ss, true);
            }

            for (unsigned i = 0; i < 4; ++i) {
                std::stringstream ss1;
                ss1 << recipe::VARNAME_SIZE << "[" << i << "]";
                POVDeclaration decl(ss1.str(), "TYPE2_LENGTH", i, i);
                decl.getString(ss, true);
            }
        } else {
            ss << "  type2_vals g_neg_t2 = { 0 };\n";
            ss << "  type2_negotiate(&g_neg_t2);\n";
        }
    }

    ss << "\n";
}

void POVGenerator::generateReadSecret(std::stringstream &ss, bool xmlFormat, const PovOptions &opt)
{
    if (opt.m_type == POV_TYPE2) {
        if (xmlFormat) {
            ss << "<!-- skip bytes before secret -->\n";
            ss << "<read><length>" << opt.m_bytesBeforeSecret <<  "</length></read>\n";
            ss << "\n";
            ss << "<read>\n";
            ss << "  <length isvar=\"true\">TYPE2_LENGTH</length>\n";
            ss << "  <assign>\n";
            ss << "    <var>TYPE2_VALUE</var>\n";
            ss << "    <slice/>\n";
            ss << "  </assign>\n";
            ss << "</read>\n";
            ss << "\n";
            ss << "<submit><var>TYPE2_VALUE</var></submit>\n";
        } else {
            ss << "  // skip bytes before secret\n";
            ss << "  receive_null(STDIN, " << opt.m_bytesBeforeSecret << ");\n";
            ss << "\n";
            ss << "  do {\n";
            ss << "    uint8_t data[g_neg_t2.read_size];\n";
            ss << "    receive_all(STDIN, data, sizeof(data));\n";
            ss << "    transmit_all(NEG_FD, data, sizeof(data));\n";
            ss << "  } while (0);\n";
        }
    }

    ss << "  \n";
}

}
}
