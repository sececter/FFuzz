///
/// Copyright (C) 2016, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2016, Cyberhaven, Inc
/// All rights reserved. Proprietary and confidential.
///
/// Distributed under the terms of S2E-LICENSE
///


#include <s2e/S2E.h>

#include <unordered_map>

#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

#include "RecipeDescriptor.h"

namespace s2e {
namespace plugins {
namespace recipe {

// This allows using per-plugin output streams.
// Don't use g_s2e, because it does not obey log verbosity levels.
static Plugin *s_plugin;

// TODO: design a clean solution so that classes that are
// related to a plugin but not derived from it can use
// its output functions (note that it also ties these classes
// to that plugin).
static llvm::raw_ostream &getDebugStream()
{
    if (!s_plugin) {
        s_plugin = g_s2e->getPlugin("Recipe");
    }

    return s_plugin->getDebugStream() << "RecipeDescriptor: ";
}

static llvm::raw_ostream &getWarningsStream()
{
    if (!s_plugin) {
        s_plugin = g_s2e->getPlugin("Recipe");
    }

    return s_plugin->getWarningsStream() << "RecipeDescriptor: ";
}

static std::string ltrim(std::string s, const char* t = " \t\n\r\f\v")
{
    s.erase(0, s.find_first_not_of(t));
    return s;
}

static std::string rtrim(std::string s, const char* t = " \t\n\r\f\v")
{
    s.erase(s.find_last_not_of(t) + 1);
    return s;
}

static std::string trim(std::string s, const char* t = " \t\n\r\f\v")
{
    return ltrim(rtrim(s, t), t);
}

static bool ReadLines(const std::string &recipeFile, std::vector<std::string> &lines)
{
    std::ifstream fs(recipeFile);

    if (!fs.is_open()) {
        getWarningsStream() << "Failed to read recipe\n";
        return false;
    }

    std::string line;

    while (std::getline(fs, line)) {
        lines.push_back(trim(line));
    }

    fs.close();
    return true;
}


RecipeDescriptor* RecipeDescriptor::fromFile(const std::string &recipeFile)
{
    std::vector<std::string> lines;
    Preconditions eipPreconditions;
    std::unordered_map<uint8_t, uint8_t> byteValues;

    if (!ReadLines(recipeFile, lines)) {
        return NULL;
    }

    RecipeDescriptor *ret = new RecipeDescriptor();

    for (std::string line : lines) {
        /* Skip comments */
        if (line.size() == 0 || line.at(0) == '#') {
            continue;
        }

        if (ret->parseSettingsLine(line)) {
            continue;
        }

        if (!ret->parsePreconditionLine(line)) {
            goto err;
        }
    }

    if (!ret->isValid()) {
        goto err;
    }

    /* Check category */
    /* First, extract preconditions on EIP */
    foreach2(it, ret->preconditions.begin(), ret->preconditions.end()) {
        if (it->left.type == Left::Type::REGBYTE && it->left.reg == Register::Reg::REG_EIP) {
            eipPreconditions.push_back(*it);
        }
    }

    if (eipPreconditions.size() > 4) {
        getWarningsStream() << "Invalid set of preconditions on EIP\n";
        goto err;
    }

    foreach2(it, eipPreconditions.begin(), eipPreconditions.end()) {
        if (it->right.type != Right::Type::CONCRETE) {
            break;
        } else {
            if (byteValues.find(it->left.reg.byteIdx) == byteValues.end()) {
                assert(it->right.valueWidth == klee::Expr::Int8 && "Only 8bit values must be used in recipe");
                byteValues[it->left.reg.byteIdx] = it->right.value;
            } else {
                getWarningsStream() << "Multiple preconditions for byte " << int(it->left.reg.byteIdx) << " of EIP\n";
                goto err;
            }
        }
    }

    if (byteValues.size() == 4) {
        ret->eipType = EIPType::CONCRETE_EIP;
        // We know that we can't have byte indexes outside [0:3], since we
        // checked at parse time. Furthermore, if we get here, we know we have
        // 4 distinct indexes, thus we can assume we have all we need to build
        // the target eip
        ret->concreteTargetEIP = (byteValues[3] << 24 | byteValues[2] << 16 | byteValues[1] << 8 | byteValues[0]);
        return ret;
    }

    /* ATM, we do not have any other case */
    ret->eipType = EIPType::SYMBOLIC_EIP;

    return ret;

err:
    delete ret;
    return NULL;
}

bool RecipeDescriptor::isValid() const
{
    if (settings.type == PovType::POV_GENERAL) {
        getWarningsStream() << "Recipe has invalid or unset type!\n";
        return false;
    }

    if (settings.type == PovType::POV_TYPE1 && settings.gp.reg == Register::REG_INV) {
        getWarningsStream() << "Type 1 recipe has invalid or unset GP!\n";
        return false;
    }

    return true;
}

bool RecipeDescriptor::parseSettingsLine(const std::string &line)
{
    static const std::string regNameRegex = "EAX|EBX|ECX|EDX|EBP|ESP|ESI|EDI|EIP";
    static const std::string numberRegex = "0x[[:xdigit:]]+|[[:digit:]]+";

    static const boost::regex typeRegex("\\*type[[:space:]]*(1|2){1}\\*");
    static const boost::regex gpRegex("\\*gp[[:space:]]*=[[:space:]]*(" + regNameRegex + "){1}\\*");
    static const boost::regex regMaskRegex("\\*regMask[[:space:]]*=[[:space:]]*(" + numberRegex + "){1}\\*");
    static const boost::regex ipMaskRegex("\\*ipMask[[:space:]]*=[[:space:]]*(" + numberRegex + "){1}\\*");
    static const boost::regex skipRegex("\\*skip[[:space:]]*=[[:space:]]*(" + numberRegex + "){1}\\*");
    // We accept everything as cbid name. Regex is greedy so *s will be taken in too, except for the last one.
    static const boost::regex cbidRegex("\\*cbid[[:space:]]*=[[:space:]]*(.*)\\*");

    boost::smatch match;

    if (boost::regex_match(line, match, typeRegex) && match.size() == 2) {
        settings.type = PovType(std::stoull(match[1], nullptr, 0));
        getDebugStream() << "Type: " << settings.type << "\n";
    } else if (boost::regex_match(line, match, gpRegex) && match.size() == 2) {
        settings.gp = Register(Register::regFromStr(match[1]));
        getDebugStream() << "Gp: " << settings.gp.regName() << "\n";
    } else if (boost::regex_match(line, match, regMaskRegex) && match.size() == 2) {
        settings.regMask = std::stoull(match[1], nullptr, 0);
        getDebugStream() << "RegMask: " << hexval(settings.regMask) << "\n";
    } else if (boost::regex_match(line, match, ipMaskRegex) && match.size() == 2) {
        settings.ipMask = std::stoull(match[1], nullptr, 0);
        getDebugStream() << "IpMask: " << hexval(settings.ipMask) << "\n";
    } else if (boost::regex_match(line, match, skipRegex) && match.size() == 2) {
        settings.skip = std::stoull(match[1], nullptr, 0);
        getDebugStream() << "Skip: " << hexval(settings.skip) << "\n";
    } else if (boost::regex_match(line, match, cbidRegex) && match.size() == 2) {
        settings.cbid = match[1];
        getDebugStream() << "CBID: " << settings.cbid << "\n";
    } else {
        return false;
    }

    return true;
}

/// \brief Escape special characters for regex
///
/// Use this function to escape special characters in string that
/// will be later used in regex.
///
/// \param s input string
/// \return escaped string
///
static std::string esc(const std::string &s) {
    std::string ret;
    for (auto c: s) {
        if (c == '$') {
            ret += '\\';
        }
        ret += c;
    }
    return ret;
}

bool RecipeDescriptor::parsePreconditionLine(const std::string &line)
{
    const std::string inputStr = trim(line);

    const std::string varNameRegex = esc(VARNAME_EIP) + "|" + esc(VARNAME_GP) + "|" + esc(VARNAME_ADDR) + "|" + esc(VARNAME_SIZE);
    const std::string regNameRegex = "EAX|EBX|ECX|EDX|EBP|ESP|ESI|EDI|EIP";
    const std::string numberRegex = "0x[[:xdigit:]]+|[[:digit:]]+";
    const std::string byteOffsetRegex = "[0-3]{1}";

    const boost::regex assignRegex("([^=]+)==([^=]+)");
    const boost::regex execRegex("\\*(" + regNameRegex + ") points to executable memory\\*");
    const boost::regex addrRegex("\\[(" + numberRegex + ")\\]");

    const boost::regex regOffsetRegex("(" + regNameRegex + ")\\[(" + byteOffsetRegex + ")\\]");
    const boost::regex regPtrRegex("\\[(" + regNameRegex + ")\\+(" + numberRegex + ")\\]");
    const boost::regex regPtrPtrOffsetRegex("\\[(" + regNameRegex + ")\\+(" + numberRegex + ")\\]" +
                                            "\\[(" + numberRegex + ")\\]");
    const boost::regex valRegex("(" + numberRegex + ")");
    const boost::regex charRegex("'([[:print:]]{1})'");
    const boost::regex tagRegex("((" + varNameRegex + ")\\[" + byteOffsetRegex + "\\])");

    boost::smatch match;

    /* Register must point to executable memory */
    if (boost::regex_match(inputStr, match, execRegex) && match.size() == 2) {
        Left l;
        l.type = Left::REGPTR_EXEC;
        l.reg = Register::regFromStr(match[1]);
        preconditions.push_back(Precondition(l, Right()));
        return true;
    }

    /* Split into Left and Right */
    if (!boost::regex_match(inputStr, match, assignRegex) || match.size() != 3) {
        getDebugStream() << "Not an assign expression: '" << inputStr << "'\n";
        return false;
    }

    const std::string leftStr = trim(match[1]);
    const std::string rightStr = trim(match[2]);

    /* Parse Left */
    Left left;
    if (boost::regex_match(leftStr, match, regOffsetRegex) && match.size() == 3) {
        uint8_t byteIdx = static_cast<uint8_t>(std::stoul(match[2], nullptr, 0));
        left = Left(match[1], byteIdx);
    } else if (boost::regex_match(leftStr, match, addrRegex) && match.size() == 2) {
        left = Left(std::stoull(match[1], nullptr, 0));
    } else if (boost::regex_match(leftStr, match, regPtrRegex) && match.size() == 3) {
        left = Left(match[1], static_cast<off_t>(std::stoull(match[2], nullptr, 0)));
    } else if (boost::regex_match(leftStr, match, regPtrPtrOffsetRegex) && match.size() == 4) {
        left = Left(match[1], std::stoull(match[2], nullptr, 0), std::stoull(match[3], nullptr, 0));
    } else {
        getWarningsStream() << "Invalid left expression: '" << leftStr << "'\n";
        return false;
    }

    /* Parse Right */
    Right right;
    if (boost::regex_match(rightStr, match, valRegex) && match.size() == 2) {
        unsigned long long v = std::stoull(match[1], nullptr, 0);
        if (v != klee::bits64::truncateToNBits(v, klee::Expr::Int8)) {
            getWarningsStream() << "Value must fit 8bit: " << hexval(v) << "\n";
            return false;
        }
        right = Right(static_cast<uint8_t>(v), klee::Expr::Int8);
    } else if (boost::regex_match(rightStr, match, charRegex) && match.size() == 2) {
        right = Right(static_cast<uint8_t>(match[1].str()[0]), klee::Expr::Int8);
    } else if (boost::regex_match(rightStr, match, tagRegex) && match.size() == 3) {
        right = Right(match[1]);
    } else if (boost::regex_match(rightStr, match, regOffsetRegex) && match.size() == 3) {
        unsigned long long idx = std::stoull(match[2], nullptr, 0);
        right = Right(match[1], static_cast<uint8_t>(idx));
    } else {
        getWarningsStream() << "Invalid right expression: '" << rightStr << "'\n";
        return false;
    }

    /* Done */
    preconditions.push_back(Precondition(left, right));
    return true;
}

bool RecipeDescriptor::mustTryRecipe(const RecipeDescriptor &recipe, const std::string &recipeName,
                                     const StateConditions& sc, uint64_t eip)
{
    // If cbid is not set in the recipe, the recipe must be tried for every cbid (moduleName)
    if (recipe.settings.cbid.size() == 0 || recipe.settings.cbid == sc.module.Name) {

        // If we have a symbolic EIP, we can try recipes with both symbolic and concrete EIP
        if (sc.eipType == EIPType::SYMBOLIC_EIP) {
            return true;
        }

        // If both the current EIP and recipe EIP are concrete, check if they match
        if (sc.eipType == EIPType::CONCRETE_EIP && recipe.eipType == EIPType::CONCRETE_EIP &&
            recipe.concreteTargetEIP == eip) {
            return true;
        }
    } else {
        getDebugStream() << "Recipe.settings.cbid (" << recipe.settings.cbid << ") != (" << sc.module.Name
                         << ") moduleName. Skipping.\n";
    }

    return false;
}

}
}
}
