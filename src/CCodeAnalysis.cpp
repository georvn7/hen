#include <regex>
#include <sstream>

#include "CCodeNode.h"
#include "Node.hpp"
#include "Project.h"
#include "Utils.h"
#include "CCodeProject.h"
#include "Client.h"
#include "Graph.hpp"
#include "UtilsCodeAnalysis.h"
#include "Client.h"
#include "Debugger.h"

using namespace web;
using namespace utility;

namespace stdrave {

std::set<std::string> g_cppKeywords = {
    // C++ keywords
    "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit", "atomic_noexcept",
    "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char", "char8_t", "char16_t",
    "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
    "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
    "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend",
    "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
    "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "reflexpr", "register",
    "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "synchronized", "template", "this", "thread_local", "throw",
    "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "wchar_t", "while", "xor", "xor_eq", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
};

//Wrapper to let us capture stuff
void visitChildren(CXCursor cursor, CodeInspector function)
{
    clang_visitChildren(cursor, [](CXCursor c, CXCursor parent, CXClientData clientData) {
        CodeInspector* func = (CodeInspector*)clientData;
        (*func)(c, parent);
        return CXChildVisit_Recurse;
    }, &function);
}

// Function to extract text from a source range
std::string getTextFromRange(CXSourceRange range, const std::string& code, unsigned int& offsetStart, unsigned int& length) {
    CXSourceLocation startLocation = clang_getRangeStart(range);
    CXSourceLocation endLocation = clang_getRangeEnd(range);

    unsigned int startOffset, endOffset;
    clang_getSpellingLocation(startLocation, nullptr, nullptr, nullptr, &startOffset);
    clang_getSpellingLocation(endLocation, nullptr, nullptr, nullptr, &endOffset);

    if (startOffset < endOffset && endOffset <= code.size()) {
        offsetStart = startOffset;
        length = endOffset - startOffset;
        return code.substr(startOffset, endOffset - startOffset);
    }

    offsetStart = 0;
    length = 0;
    return "";  // Return empty string if the range is invalid
}

std::string getCursorSourceCode(CXCursor c, const std::string& code) {
    CXSourceRange range = clang_getCursorExtent(c);
    unsigned int offset, length;
    return getTextFromRange(range, code, offset, length);
}

struct CXCursorComparator {
    bool operator()(const CXCursor& lhs, const CXCursor& rhs) const {
        CXString lhsUSR = clang_getCursorUSR(lhs);
        CXString rhsUSR = clang_getCursorUSR(rhs);
        
        std::string lhsUSRStr = clang_getCString(lhsUSR);
        std::string rhsUSRStr = clang_getCString(rhsUSR);
        
        clang_disposeString(lhsUSR);
        clang_disposeString(rhsUSR);

        // Handle cases where USR might be empty
        if (lhsUSRStr.empty() && rhsUSRStr.empty()) {
            return false; // Both are considered equal
        } else if (lhsUSRStr.empty()) {
            return true; // Empty string is considered less than any non-empty string
        } else if (rhsUSRStr.empty()) {
            return false; // Non-empty string is considered greater than any empty string
        } else {
            return lhsUSRStr < rhsUSRStr; // Regular string comparison
        }
    }
};

// Custom hash function for CXCursor
struct CXCursorHash {
    std::size_t operator()(const CXCursor& cursor) const {
        return clang_hashCursor(cursor);
    }
};

// Custom equality function for CXCursor
struct CXCursorEqual {
    bool operator()(const CXCursor& lhs, const CXCursor& rhs) const {
        return clang_equalCursors(lhs, rhs);
    }
};

void listOnMessage(std::stringstream& review, const std::string& message, const std::set<std::string>& list, const std::string& recommendation=std::string())
{
    if(!list.size()) return;

    review << message << ": ";
    auto last = *--list.end();
    for(auto item : list) {
        review << item;
        if(item != last) {
            review << ", ";
        }
    }
    if(!recommendation.empty())
    {
        review << std::endl << recommendation;
    }
    review << std::endl << std::endl;
}


class Linter
{
    CCodeNode::CodeType m_type;
    const CCodeNode*    m_ccNode;
    static std::string  m_code;
    std::string         m_codeForAnalysis;
    uint32_t            m_startOffset;
    uint32_t            m_endOffset;
    std::string         m_functionToImpl;
    CXSourceRange       m_functionToImplRange;
    std::string         m_functionBeingDefined;
    
    bool m_checkFuncDecl;
    bool m_checkFuncImpl;
    bool m_checkDataDef;
    uint32_t            m_hasAncestorDepth;
    
    std::set<std::string> m_appFunctions;
    std::set<std::string> m_calledFunctions;
    std::set<std::string> m_calledMemberFunctions;
    std::set<std::string> m_calledAppFunctions;
    std::set<std::string> m_declaredFunctions;
    std::set<std::string> m_definedFunctions;
    std::set<std::string> m_definedFunctionSignatures;
    std::set<std::string> m_noEffectFunctions;
    std::set<std::string> m_declaredStructs;
    std::set<std::string> m_definedStructs;
    std::set<std::string> m_declaredEnums;
    std::set<std::string> m_definedEnums;
    std::set<std::string> m_classes;
    std::set<std::string> m_autos;
    std::set<std::string> m_includes;
    std::set<std::string> m_globalVariables;
    std::set<std::string> m_emptyScope;
    std::set<std::string> m_localAndArgs;
    std::set<std::string> m_capturedVariables;
    std::set<std::string> m_structMemberFunctions;
    std::set<std::string> m_nestedStructs;
    std::set<std::string> m_inNamespace;
    std::set<std::string> m_notUIntEnums;
    std::set<std::string> m_templatedDefinitions;
    std::set<std::string> m_structsWithIneritance;
    std::set<std::string> m_todos;
    std::set<std::string> m_constAppTypes;
    std::set<std::string> m_typedefs;
    std::set<std::string> m_directives;
    std::set<std::string> m_systemIdentifiers;
    std::set<std::string> m_appFunctionsWithInitializers;
    std::set<std::string> m_unknownTypes;
    std::set<std::string> m_unknownIdentifiers;
    std::set<std::string> m_lambdas;
    std::set<std::string> m_preprocessorDefinitions;
    std::vector<std::pair<unsigned, unsigned>> m_stringLiterals;
    
    std::unordered_map<CXCursor, CXCursor, CXCursorHash, CXCursorEqual> m_parentMap;
    
public:
    Linter(const CCodeNode* ccNode, const std::string& code, CCodeNode::CodeType codeType):
    m_ccNode(ccNode),
    m_type(codeType),
    m_hasAncestorDepth(0)
    {
        std::string removedComments;
        //We need this to simplify some regex checks
        std::string codeNoComments = code;
        if(codeType != CCodeNode::EXTRACT)
        {
            codeNoComments = removeComments(code, removedComments);
            
            std::cout << "removed comments: " << std::endl << removedComments << std::endl;
            
            m_todos = findPatternsInComments(removedComments,{
                std::regex(R"(\b(?:TODO|To\s*Do)\b)", std::regex::icase),
                std::regex(R"(\bplace\s*holders?\b)", std::regex::icase),
                std::regex(R"(stubs?)", std::regex::icase),
                std::regex(R"(dummy?)", std::regex::icase)
            });
        }
        else
        {
            m_todos = findPatternsInComments(code,{
                std::regex(R"(\b(?:TODO|To\s*Do)\b)", std::regex::icase),
                std::regex(R"(\bplace\s*holders?\b)", std::regex::icase),
                std::regex(R"(stubs?)", std::regex::icase),
                std::regex(R"(dummy?)", std::regex::icase)
            });
        }
        
        std::string codeNoIncludes = extractIncludeStatements(codeNoComments, true);
        
        for(auto func : m_ccNode->m_calls.items) {
            m_appFunctions.insert(func->func_name);
        }
        m_functionToImpl = ccNode->m_brief.func_name;
        
        functionsWithoutEffect(codeNoComments);
        
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        //First pass check on instrumented code to find all non-global variable identifiers
        std::string commonIncludes = getCommonIncludes();
        std::stringstream dbgSource;
        if(m_type != CCodeNode::TEST)
        {
            m_code = commonIncludes;
            m_code += "\n";
            m_code += proj->m_common_header_eval;
            m_code += "\n";
            std::set<std::string> referencedNodes;
            if(m_type != CCodeNode::DATA_DEF) {
                
                bool defineData = m_type != CCodeNode::EXTRACT;
                std::set<std::string> owners;
                m_code += ccNode->getDataApi(true, true, defineData, owners);
            }
            
            if(m_type == CCodeNode::FUNC_IMPL ||
               m_type == CCodeNode::FUNC_CMPL ||
               m_type == CCodeNode::FUNC_FIX)
            {
                std::string functionStubs = getFunctionStubs();
                m_code += functionStubs;
            }
            
            dbgSource << m_code;
            dbgSource << "//*** Start analysis from here" << std::endl;
            m_startOffset = (uint32_t)m_code.size();
            m_code += codeNoIncludes;
            m_codeForAnalysis = codeNoIncludes;
            dbgSource << codeNoIncludes;
            dbgSource << "//*** Stop analysis here" << std::endl;
            m_endOffset = (uint32_t)m_code.size();
            
            std::cout << "//***** Code for clang begin *****" << std::endl;
            std::cout << dbgSource.str();
            std::cout << "//***** Code for clang end *****" << std::endl;
        }
        else
        {
            m_code = code;
            m_startOffset = (uint32_t)m_code.find("//*** Start analysis from here");
            m_endOffset = (uint32_t)m_code.size();
        }
        
        m_functionBeingDefined.clear();
        std::memset(&m_functionToImplRange,0,sizeof(m_functionToImplRange));
    }
    
    //TODO: I don't quite like how the instrumented code is accessed
    static const std::string& code() {return m_code;}
    
    bool checkStringLiteral(CXCursor cursor)
    {
        if (clang_getCursorKind(cursor) != CXCursor_StringLiteral
    #ifdef __APPLE__
            && clang_getCursorKind(cursor) != CXCursor_ObjCStringLiteral   // skip ObjC @"…", if you might see it
    #endif
        ) {
            return false;
        }
        
        CXTranslationUnit unit = clang_Cursor_getTranslationUnit(cursor);
        
        CXSourceRange cr = clang_getCursorExtent(cursor);
        CXSourceLocation s = clang_getRangeStart(cr);
        CXSourceLocation e = clang_getRangeEnd(cr);

        CXFile fS=nullptr, fE=nullptr; unsigned offS=0, offE=0, line=0, col=0;
        clang_getSpellingLocation(s, &fS, &line, &col, &offS);
        clang_getSpellingLocation(e, &fE, &line, &col, &offE);
        
        m_stringLiterals.emplace_back(offS, offE);
        return true;
    }
    
    void instrumentStringLiterals(CXTranslationUnit unit)
    {
        std::cout << "Replaced string literals: " << std::endl;
        for (auto edit : m_stringLiterals) {
            size_t len = edit.second - edit.first;
            std::string repl = "\"";
            repl += std::string(len-2, '*');
            repl += "\"";
            
            std::string literal = m_code.substr(edit.first, len);
            std::cout << "literal: " << literal << std::endl;
            
            m_code.replace(edit.first, len, repl);
        }
        std::cout << "End replaced string literals" << std::endl;
    }
    
    std::string getCursorType(CXCursor c)
    {
        CXString str = clang_getCursorKindSpelling(clang_getCursorKind(c));
        return getClangString(str);
    }
    
    std::set<std::string> getCalledFunctions() {
        
        //Remove detected system identifiers from application functions
        std::set<std::string> calledFunctionsNoIdentifiers;
        std::set_difference(m_calledFunctions.begin(), m_calledFunctions.end(),
                            m_systemIdentifiers.begin(), m_systemIdentifiers.end(),
                            std::inserter(calledFunctionsNoIdentifiers, calledFunctionsNoIdentifiers.begin()));
        
        //Remove member functions
        std::set<std::string> calledFunctionsNoMembers;
        std::set_difference(calledFunctionsNoIdentifiers.begin(), calledFunctionsNoIdentifiers.end(),
                            m_calledMemberFunctions.begin(), m_calledMemberFunctions.end(),
                            std::inserter(calledFunctionsNoMembers, calledFunctionsNoMembers.begin()));
        
        
        //Remove preprocessor macros functions
        std::set<std::string> appFunctions;
        std::set_difference(calledFunctionsNoMembers.begin(), calledFunctionsNoMembers.end(),
                            m_calledMemberFunctions.begin(), m_calledMemberFunctions.end(),
                            std::inserter(appFunctions, appFunctions.begin()));
        
        return appFunctions;
    }
    
    void printCursor(CXCursor c, CXCursor p, const std::string& code)
    {
        std::string type = getCursorType(c);
        std::string parentType = getCursorType(p);
        std::string name = getCursorName(c);
        CXSourceRange range = clang_getCursorExtent(c);
        unsigned int offset, length;
        std::string source = getTextFromRange(range, code, offset, length);
    
        std::cout << "type: " << type << " parentType: " << parentType << " name: " << name << " offset " << offset;
        std::cout << " length " << length << " source: " << source << std::endl;
    }
    
    std::string getCommonIncludes()
    {
        std::stringstream commonIncludes;
        for(auto inc : CCodeProject::getSTDIncludes())
        {
            commonIncludes << "#include <" << inc << ">" << std::endl;
        }
        
        return commonIncludes.str();
    }
    
    std::string getFunctionStubs()
    {
        if(m_appFunctions.empty())
        {
            return std::string();
        }
        
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::stringstream functionStubs;
        
        //TODO: Needs to be in Environment/source/Stubs.h
        std::string stubsFilePath = Client::getInstance().getEnvironmentDir() + "/Stubs.h";
        std::ifstream stubsFile(stubsFilePath);
        if(stubsFile.is_open())
        {
            std::string stubsSource((std::istreambuf_iterator<char>(stubsFile)), std::istreambuf_iterator<char>());
            functionStubs << stubsSource << std::endl;
        }
        
        for(auto func : m_appFunctions)
        {
            bool hasDeclaration = false;
            auto it = proj->nodeMap().find(func);
            if(it != proj->nodeMap().end())
            {
                auto existingFuncNode = (const CCodeNode*)it->second;
                if(!existingFuncNode->m_prototype.declaration.empty())
                {
                    functionStubs << existingFuncNode->m_prototype.declaration << std::endl;
                    hasDeclaration = true;
                }
            }
            
            if(!hasDeclaration)
            {
                functionStubs << "template <typename... Args> AnyReturn " << func << "(Args&&...) { return AnyReturn{}; }" << std::endl;
            }
        }
        
        return functionStubs.str();
    }
    
    std::string extractIncludeStatements(const std::string& code, bool remove) {
        std::istringstream iss(code);
        std::ostringstream oss;
        std::string line;
        std::regex includePattern(R"(^\s*#\s*include\s*["<][^">]+[">]\s*$)", std::regex::icase);

        while (std::getline(iss, line)) {
            std::string result = line;
            if(remove)
            {
                // Replace all matches (include statements) with an empty string, effectively removing them
                result = std::regex_replace(line, includePattern, "");
                if(result != line)
                {
                    m_includes.insert(line);
                }
            }

            // Write the line back to the output stream
            oss << result << "\n";
        }

        return oss.str();
    }
    
    // Helper function to determine if a compound statement is empty
    bool isCompoundStatementEmpty(CXCursor cursor) {
        bool isEmpty = true; // Assume empty until proven otherwise

        clang_visitChildren(cursor, [](CXCursor c, CXCursor parent, CXClientData clientData) {
            // If any child is found, the compound statement is not empty
            bool* isEmpty = reinterpret_cast<bool*>(clientData);
            *isEmpty = false;
            return CXChildVisit_Break; // Stop visiting further
        }, &isEmpty);

        return isEmpty;
    }
    
    uint32_t getCursorStart(CXCursor cursor)
    {
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation startLocation = clang_getRangeStart(range);
        uint32_t startOffset = 0;
        CXFile file = nullptr;
        clang_getSpellingLocation(startLocation, &file, nullptr, nullptr, &startOffset);
        if(file)
        {
            CXString fileName = clang_getFileName(file);
            std::string stdFileName = clang_getCString(fileName);
            clang_disposeString(fileName);
            if(stdFileName != "code.cpp")
            {
                return 0;
            }
        }
        return startOffset;
    }
    
    
    uint32_t getCursorEnd(CXCursor cursor)
    {
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation endlLocation = clang_getRangeEnd(range);
        uint32_t endOffset = 0;
        CXFile file = nullptr;
        clang_getSpellingLocation(endlLocation, &file, nullptr, nullptr, &endOffset);
        if(file)
        {
            CXString fileName = clang_getFileName(file);
            std::string stdFileName = clang_getCString(fileName);
            clang_disposeString(fileName);
            if(stdFileName != "code.cpp")
            {
                return 0;
            }
        }
        return endOffset;
    }
    
    bool isInScope(CXSourceRange scope, CXSourceRange sub)
    {
        CXSourceLocation scopeStartLocation = clang_getRangeStart(scope);
        CXSourceLocation scopeEndLocation = clang_getRangeEnd(scope);
        
        CXSourceLocation subStartLocation = clang_getRangeStart(sub);
        CXSourceLocation subEndLocation = clang_getRangeEnd(sub);
        
        unsigned int scopeStartOffset, scopeEndOffset;
        clang_getSpellingLocation(scopeStartLocation, nullptr, nullptr, nullptr, &scopeStartOffset);
        clang_getSpellingLocation(scopeEndLocation, nullptr, nullptr, nullptr, &scopeEndOffset);
        
        unsigned int subStartOffset, subEndOffset;
        CXFile file = nullptr;
        clang_getSpellingLocation(subStartLocation, &file, nullptr, nullptr, &subStartOffset);
        if(!file)
        {
            return true;
        }
        
        CXString fileName = clang_getFileName(file);
        std::string stdFileName = clang_getCString(fileName);
        clang_disposeString(fileName);
        if(stdFileName != "code.cpp")
        {
            return true;
        }
        
        clang_getSpellingLocation(subEndLocation, nullptr, nullptr, nullptr, &subEndOffset);
        
        return scopeStartOffset <= subStartOffset && subEndOffset <= scopeEndOffset;
    }

    bool isInCallstack(CXSourceRange scope, CXCursor cursor)
    {
        bool inCallstack = false;
        // Traverse up the cursor tree
        while (clang_getCursorKind(cursor) != CXCursor_TranslationUnit) 
        {
            CXSourceRange sub = clang_getCursorExtent(cursor);

            if (isInScope(scope, sub))
            {
                inCallstack = true;
                break;
            }

            cursor = clang_getCursorSemanticParent(cursor);
        }

        return inCallstack;
    }
    
    bool functionHasNoEffect(const std::string& code, const std::string& funcName)
    {
        std::string pattern = R"((?:\(void\)|;|\{|\})\s*)" + funcName + R"(\s*\(\s*\))";
        std::smatch match;
        if (std::regex_search(code, match, std::regex(pattern))) {
            return true;
        }
        
        return false;
    }
    
    void functionsWithoutEffect(const std::string& code)
    {
        std::regex pattern(R"((?:\(\s*void\s*\)|;|\{|\})\s*[a-zA-Z_][a-zA-Z0-9_]*\s*\(\s*\))");
        
        std::sregex_iterator it(code.begin(), code.end(), pattern);
        std::sregex_iterator end;

        while (it != end) {
            std::string function;
            findIdentifier(it->str(), function);
            if(m_appFunctions.find(function) != m_appFunctions.end())
            {
                m_noEffectFunctions.insert(function);
            }
            ++it;
        }
    }
    
    std::string::size_type findFunction(const std::string& source, std::string& function)
    {
        std::string pattern = R"([a-zA-Z_][a-zA-Z0-9_]*\s*\()";
        std::smatch match;
        if (std::regex_search(source, match, std::regex(pattern)))
        {
            function = match.str();
            return match.position();
        }
        
        function = std::string();
        return std::string::npos;
    }
    
    // This function finds a member function call pattern like: (identifier or )) . or -> or :: functionName(
    inline std::string::size_type findMemberFunction(const std::string& source, std::string& function) {
        // Pattern explanation:
        // (?:[a-zA-Z_][a-zA-Z0-9_]*|\))
        //   - Either a valid identifier or a closing parenthesis.
        //
        // \s*(\.|->|::)\s*
        //   - Optional whitespace, then a scope/member access operator, then optional whitespace.
        //
        // ([a-zA-Z_][a-zA-Z0-9_]*)
        //   - The actual function name.
        //
        // \s*\(
        //   - Optional whitespace followed by '('
        static const std::regex memberPattern(
            R"((?:[a-zA-Z_][a-zA-Z0-9_]*|\))\s*(\.|->|::)\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*\()"
        );

        std::smatch match;
        if (std::regex_search(source, match, memberPattern)) {
            function = match.str();
            return match.position();
        }

        function.clear();
        return std::string::npos;
    }
    
    inline bool extractMemberFunctionName(const std::string& code, std::string& funcName)
    {
        // This simplified regex looks for:
        //    .func(
        // or ->func(
        // or ::func(
        // optionally allowing whitespace in between.
        //
        // Explanation:
        // (?:\.|->|::)        - match one of ".", "->", or "::"
        // \s*                 - optional spaces
        // ([a-zA-Z_]\w*)      - the function name (captured group 1)
        // \s*\(               - optional spaces before '('
        static const std::regex memberExtract(
            R"((?:\.|->|::)\s*([a-zA-Z_]\w*)\s*\()"
        );

        std::smatch match;
        if (std::regex_search(code, match, memberExtract)) {
            // match[1] should be the function name
            funcName = match[1].str();
            return true;
        }
        return false;
    }
    
    std::string::size_type findIdentifier(const std::string& source, std::string& identifier)
    {
        std::string pattern = R"([a-zA-Z_][a-zA-Z0-9_]*)";
        std::smatch match;
        if (std::regex_search(source, match, std::regex(pattern))) {
            identifier = match.str();
            return match.position();
        }
        
        identifier = std::string();
        return std::string::npos;
    }
    
    void checkAndAddGlobal(const std::string& variable)
    {
        bool notKeyword = g_cppKeywords.find(variable) == g_cppKeywords.end();
        bool notFunction = m_calledFunctions.find(variable) == m_calledFunctions.end();
        bool notMacro = m_preprocessorDefinitions.find(variable) == m_preprocessorDefinitions.end();
        
        notFunction = notFunction && m_definedFunctions.find(variable) == m_definedFunctions.end();
        bool notLocal = m_localAndArgs.find(variable) == m_localAndArgs.end();
        
        const auto& cppTypes = CCodeProject::getCppTypes();
        bool notType = cppTypes.find(variable) == cppTypes.end();
        
        //Sometimes there are false positives for internal variables like: __begin6
        bool foundInSource = m_codeForAnalysis.find(variable) != std::string::npos;
        
        if(foundInSource && notKeyword && notFunction && notMacro && notLocal && notType)
        {
            m_globalVariables.insert(variable);
        }
    }
    
    void checkFunctionCall(const std::string& function)
    {
        std::vector<std::string> arguments = parseArguments(function);
        for(auto arg : arguments) {
            std::string argIsFunc;
            auto pos = findFunction(arg, argIsFunc);
            if(pos != std::string::npos) {
                checkFunctionCall(arg.substr(pos));
            }
        }
        
        std::string funcName;
        if(findIdentifier(function, funcName) != std::string::npos)
        {
            bool isCppKeyword = g_cppKeywords.find(funcName) != g_cppKeywords.end();
            
            if(funcName != "PRINT_TEST" && !isCppKeyword)
            {
                m_calledFunctions.insert(funcName);
                if(m_appFunctions.find(funcName) != m_appFunctions.end()) {
                    m_calledAppFunctions.insert(funcName);
                }
            }
        }
    }
    
    // NEW FUNCTION: checkMemberFunctionCall
    // Similar to checkFunctionCall but only looks for member functions.
    void checkMemberFunctionCall(const std::string& function) {
        // Recursively check arguments for *member* functions only
        std::vector<std::string> arguments = parseArguments(function);
        for (auto& arg : arguments) {
            std::string argMemberFunc;
            auto mpos = findMemberFunction(arg, argMemberFunc);
            if (mpos != std::string::npos) {
                // Found a member function in the argument
                checkMemberFunctionCall(arg.substr(mpos));
            }
            // Notice we do not call findFunction here because this is strictly for member functions
        }

        // Try to detect a member function call in 'function' itself
        std::string memberFunction;
        auto memberPos = findMemberFunction(function, memberFunction);

        if (memberPos == std::string::npos) {
            // No member function found at this level
            return;
        }

        std::string funcName;
        if (!extractMemberFunctionName(memberFunction, funcName)) {
            return;
        }

        bool isCppKeyword = (g_cppKeywords.find(funcName) != g_cppKeywords.end());
        if (!funcName.empty() && funcName != "PRINT_TEST" && !isCppKeyword) {
            m_calledMemberFunctions.insert(funcName);
        }
    }
    
    bool checkFunctionsInScope(const std::string& source)
    {
        bool isEmptyScope = true;
        std::string removedComments;
        std::string sourceNoComments = removeComments(source, removedComments);
        sourceNoComments = emptyAllStringLiterals(sourceNoComments);
        
        std::string funcSearch = sourceNoComments;
        std::string function;
        auto fpos = std::string::npos;
        while((fpos = findFunction(funcSearch, function)) != std::string::npos)
        {
            std::string funcName;
            findIdentifier(function, funcName);
            
            bool isCppKeyword = g_cppKeywords.find(funcName) != g_cppKeywords.end();
            
            if(funcName != "PRINT_TEST" && !isCppKeyword)
            {
                m_calledFunctions.insert(funcName);
                if(m_appFunctions.find(funcName) != m_appFunctions.end())
                {
                    m_calledAppFunctions.insert(funcName);
                }
            }
            funcSearch = funcSearch.substr(fpos + function.length());
            isEmptyScope = false;
        }

        return isEmptyScope;
    }
    
    // NEW FUNCTION: checkMemberFunctionsInScope
    // Similar to checkFunctionsInScope but only deals with member functions.
    bool checkMemberFunctionsInScope(const std::string& source) {
        bool isEmptyScope = true;
        std::string removedComments;
        std::string sourceNoComments = removeComments(source, removedComments);
        sourceNoComments = emptyAllStringLiterals(sourceNoComments);
        
        std::string funcSearch = sourceNoComments;

        while (true) {
            std::string memberFunction;
            auto mfpos = findMemberFunction(funcSearch, memberFunction);

            if (mfpos == std::string::npos) {
                // No more member functions
                break;
            }

            std::string funcName;
            if (!extractMemberFunctionName(memberFunction, funcName)) {
                // If we failed to extract, just move on
                funcSearch = funcSearch.substr(mfpos + memberFunction.length());
                continue;
            }

            bool isCppKeyword = (g_cppKeywords.find(funcName) != g_cppKeywords.end());
            if (!funcName.empty() && funcName != "PRINT_TEST" && !isCppKeyword) {
                m_calledMemberFunctions.insert(funcName);
            }

            // Move forward in the search string
            funcSearch = funcSearch.substr(mfpos + memberFunction.length());
            isEmptyScope = false;
        }

        return isEmptyScope;
    }
    
    bool checkSource(const std::string& source)
    {
        //TODO: What will happen if we have data declaration in this scope?
        
        bool isEmptyScope = checkFunctionsInScope(source);
        checkMemberFunctionsInScope(source);
        std::string removedComments;
        std::string sourceNoComments = removeComments(source, removedComments);
        std::string varSearch = sourceNoComments;
        std::string variable;
        auto vpos = std::string::npos;
        while((vpos = findIdentifier(varSearch, variable)) != std::string::npos)
        {
            varSearch = varSearch.substr(vpos + variable.length());
            isEmptyScope = false;
        }
        
        return isEmptyScope;
    }
    
    void captureIdentifiersInScope(const std::string& source)
    {
        std::string removedComments;
        std::string sourceNoComments = removeComments(source, removedComments);
        std::string varSearch = sourceNoComments;
        std::string variable;
        auto vpos = std::string::npos;
        int skip = 0;
        while((vpos = findIdentifier(varSearch, variable)) != std::string::npos)
        {
            if(varSearch.substr(vpos, 5) == "std::")
            {
                skip = 2;
            }
            
            checkAndAddGlobal(variable);
            varSearch = varSearch.substr(vpos + variable.length());
        }
    }
    
    void captureSTDDeclarations(const std::string& source)
    {
        std::string removedComments;
        std::string sourceNoComments = removeComments(source, removedComments);
        std::regex pattern(R"(std::[a-zA-Z_][a-zA-Z0-9_]+(<[^>]+>)*\s+([a-zA-Z_][a-zA-Z0-9_]*))");
        
        std::smatch matches;
        std::string::const_iterator searchStart(sourceNoComments.cbegin());
        while (regex_search(searchStart, sourceNoComments.cend(), matches, pattern)) {
            m_localAndArgs.insert(matches[2]);
            searchStart = matches.suffix().first;
        }
    }
    
    std::vector<std::string> parseArguments(const std::string& function) {
        std::vector<std::string> result;
        int depth = 0;
        std::string current_arg;

        // Find the first opening parenthesis to ensure only parsing inside the main function arguments
        size_t start = function.find('(');
        size_t end = function.rfind(')');
        if (start == std::string::npos || end == std::string::npos || end <= start) {
            return result; // No valid parenthesis found
        }

        // Iterate over the string within the outermost parentheses
        for (size_t i = start + 1; i < end; ++i) {
            char ch = function[i];
            if (ch == '(') {
                depth++;
            } else if (ch == ')') {
                depth--;
            }

            // If depth is zero and the character is a comma, split here
            if (depth == 0 && ch == ',') {
                result.push_back(current_arg);
                current_arg.clear();
            } else {
                current_arg += ch;
            }
        }

        // Add the last argument if there is any
        if (!current_arg.empty()) {
            result.push_back(current_arg);
        }

        return result;
    }
    
    CXCursor createInvalidCursor() 
    {
        CXCursor cursor;
        cursor.kind = CXCursor_InvalidFile;
        cursor.xdata = 0;
        cursor.data[0] = nullptr;
        cursor.data[1] = nullptr;
        cursor.data[2] = nullptr;
        return cursor;
    }
    
    bool isValidCursor(CXCursor cursor)
    {
        return !(clang_Cursor_isNull(cursor) || cursor.kind == CXCursor_InvalidFile || cursor.kind == CXCursor_FirstInvalid);
    }
    
    std::string getDeclarationFile(CXCursor cursor)
    {
        // Retrieve the declaration cursor from a reference cursor
        CXCursor declCursor = clang_getCursorReferenced(cursor);
        return getCursorFile(declCursor);
    }
    
    CXCursor getCursorDeclaration(CXCursor c)
    {
        while(!clang_isDeclaration(c.kind) && isValidCursor(c))
        {
            c = clang_getCursorReferenced(c);
            if (c.kind == CXCursor_OverloadedDeclRef) {
                unsigned numOverloaded = clang_getNumOverloadedDecls(c);
                
                CXCursor nextC = createInvalidCursor() ;
                for (unsigned i = 0; i < numOverloaded; ++i) {
                    CXCursor overloadedDecl = clang_getOverloadedDecl(c, i);
                    if(i==0 || overloadedDecl.kind == CXCursor_VarDecl || overloadedDecl.kind == CXCursor_ParmDecl)
                    {
                        nextC = overloadedDecl;
                    }
                }
                c = nextC;
            }
        }
        
        return c;
    }
    
    bool hasAncestor(CXCursor c, CXCursorKind kind)
    {
        m_hasAncestorDepth = 0;
        return hasAncestorReq(c, kind);
    }
    
    bool hasAncestorReq(CXCursor c, CXCursorKind kind)
    {
        auto parent = m_parentMap.find(c);
        if(parent == m_parentMap.end()) return false;
        if(clang_getCursorKind(parent->second) == kind) return true;
    
        if(m_hasAncestorDepth++ > 128)
        {
            return false;
        }
        
        return hasAncestorReq(parent->second, kind);
    }
    
    void getASTPath(CXCursor c, std::string& path)
    {
        std::set<unsigned> parentsMap;
        
        CXCursor parent = clang_getCursorSemanticParent(c);
        parentsMap.insert(clang_hashCursor(parent));
        while (!clang_Cursor_isNull(parent))
        {
            path += getCursorName(parent) + "/" + path;
        
            parent = clang_getCursorSemanticParent(parent);
            auto hash = clang_hashCursor(parent);
            if(parentsMap.find(hash) != parentsMap.end())
            {
                //Detect cyclic dependency
                break;
            }
            parentsMap.insert(hash);
        }
        
        path += "/" + getCursorName(c);
    }
    
    bool filter(CXCursor c, bool skipStringLiterals)
    {
        std::string cursorFile = getCursorFile(c);
        if(cursorFile != "code.cpp") return false;
        
        uint32_t srcOffset = getCursorStart(c);
        if(srcOffset < m_startOffset) return false;
        if(srcOffset >= m_endOffset) return false;
        
        if(skipStringLiterals)
        {
            if (clang_getCursorKind(c) == CXCursor_StringLiteral
#ifdef __APPLE__
                || clang_getCursorKind(c) == CXCursor_ObjCStringLiteral   // skip ObjC @"…", if you might see it
#endif
                ) {
                return false;// ignore this node, visit siblings
            }
        }
        
        return true;
    }
    
    bool visit(CXCursor c, CXCursor parent, std::stringstream& review)
    {
        if(!filter(c,true))
        {
            return false;
        }
        
        return check(c, parent, review);
    }
    
    std::string getReferencedCursorName(CXCursor referenced) {
        // Try display name first
        CXString displayName = clang_getCursorDisplayName(referenced);
        const char* dName = clang_getCString(displayName);
        std::string name;
        if (dName && *dName) {
            name = dName;
        }
        clang_disposeString(displayName);

        // If display name is empty, try spelling
        if (name.empty()) {
            CXString spelling = clang_getCursorSpelling(referenced);
            const char* sName = clang_getCString(spelling);
            if (sName && *sName) {
                name = sName;
            }
            clang_disposeString(spelling);
        }

        return name;
    }
    
    bool isSystemIdentifier(CXCursor c, const std::string& cursorName, const std::string& mainFile)
    {
        CXCursorKind kind = clang_getCursorKind(c);
        bool isDeclaration = clang_isDeclaration(kind);
        bool isReference = (kind == CXCursor_CallExpr ||
                            kind == CXCursor_MemberRefExpr ||
                            kind == CXCursor_DeclRefExpr ||
                            kind == CXCursor_TypeRef);

        // If it's a reference, get the declaration it refers to
        CXCursor referenced = c;
        if (isReference) {
            referenced = clang_getCursorReferenced(c);
            if (clang_Cursor_isNull(referenced)) {
                // Can't find a referenced declaration; treat it as not system by default
                return false;
            }
        }

        // Get the declaration location
        CXSourceLocation declLoc = clang_getCursorLocation(referenced);
        CXFile declFile;
        unsigned declLine, declColumn;
        clang_getSpellingLocation(declLoc, &declFile, &declLine, &declColumn, nullptr);

        if (!declFile) {
            // No associated file, can't determine origin reliably
            return false;
        }

        CXString declFileName = clang_getFileName(declFile);
        const char *declFileCStr = clang_getCString(declFileName);
        std::string declFileStr = declFileCStr ? declFileCStr : "";
        clang_disposeString(declFileName);

        std::string refCursorName = getReferencedCursorName(referenced);

        // If not in system header, but file differs from main file, consider it external
        if (!declFileStr.empty() && (declFileStr != mainFile)) {
            
            auto vpos = std::string::npos;
            std::string identifier;
            while((vpos = findIdentifier(refCursorName, identifier)) != std::string::npos)
            {
                refCursorName = refCursorName.substr(vpos + identifier.length());
                m_systemIdentifiers.insert(identifier);
            }
            
            return true; // External identifier
        }

        // If it's the same file, it's likely a custom identifier
        return false;
    }
    
    void checkUnknownIdentifiers(CXCursor c)
    {
        CXCursorKind kind = clang_getCursorKind(c);
        
        // We only care about references that might be unknown
        // (TypeRef, DeclRefExpr, MemberRef, etc.).
        // You can add other kinds if you want finer-grained classification.
        switch (kind) {
        case CXCursor_TypeRef:
        case CXCursor_DeclRefExpr:
        case CXCursor_MemberRef:
        case CXCursor_UnexposedExpr:
        {
            // Does this reference resolve to a known declaration?
            CXCursor referenced = clang_getCursorReferenced(c);
            if (clang_equalCursors(referenced, clang_getNullCursor())) {
                
                // It's unknown / undeclared
                CXString spelling = clang_getCursorSpelling(c);
                
                printf("Unknown or unexposed reference: %s\n", clang_getCString(spelling));

                // Classify by cursor kind
                if (kind == CXCursor_TypeRef) {
                    printf("Unknown TYPE: %s\n", clang_getCString(spelling));
                }
                else if (kind == CXCursor_DeclRefExpr) {
                    printf("Unknown VARIABLE/FUNCTION: %s\n", clang_getCString(spelling));
                }
                else if (kind == CXCursor_MemberRef) {
                    printf("Unknown MEMBER or ENUMERATOR: %s\n", clang_getCString(spelling));
                }

                clang_disposeString(spelling);
            }
            break;
        }
        default:
            // Not a reference cursor or not relevant for "unknown" detection.
            break;
        }
    }
    
    bool check(CXCursor c, CXCursor parent, std::stringstream& review)
    {
        m_parentMap[c] = parent;
        std::string stdCursorName = getCursorName(c);
        std::string stdParentName = getCursorName(parent);
        CXSourceRange range = clang_getCursorExtent(c);
        std::string stdCursorSource = getCursorSourceCode(c, m_code);
        std::string stdCursorKind = getCursorKind(c);
        std::string stdCursorType = getCursorType(c);
        
        CXType type = clang_getCursorType(c);
        
        isSystemIdentifier(c, stdCursorName, "code.cpp");
        
        if(m_functionBeingDefined == m_functionToImpl)
        {
            bool inScope = isInScope(m_functionToImplRange, range);
            if(!inScope)
            {
                m_functionBeingDefined.clear();
            }
        }
        
        // Checking for include directives
        if (clang_getCursorKind(c) == CXCursor_InclusionDirective)
        {
            CXString includeFile = clang_getFileName(clang_getIncludedFile(c));
            std::string fileName = clang_getCString(includeFile);
            if (fileName.length()) {
                m_includes.insert(fileName);
            }
            else
            {
                m_includes.insert(stdCursorName);
            }
            clang_disposeString(includeFile);
        }
        
        // Checking for class declaration/definition
        if (clang_getCursorKind(c) == CXCursor_ClassDecl) {
            m_classes.insert(stdCursorName);
        }
        
        // Checking for member functions
        if (clang_getCursorKind(c) == CXCursor_CXXMethod) {
            m_structMemberFunctions.insert(stdCursorName);
        }

        // Checking for constructors
        if (clang_getCursorKind(c) == CXCursor_Constructor) {
            m_structMemberFunctions.insert(stdCursorName);
        }

        // Checking for destructors
        if (clang_getCursorKind(c) == CXCursor_Destructor) {
            m_structMemberFunctions.insert(stdCursorName);
        }
        
        if (clang_getCursorKind(c) == CXCursor_TypedefDecl) {
            m_typedefs.insert(stdCursorName);
        }
        
        if (clang_getCursorKind(c) == CXCursor_PreprocessingDirective)
        {
            m_directives.insert(stdCursorName);
        }
        
        if (clang_getCursorKind(c) == CXCursor_MacroDefinition)
        {
            m_preprocessorDefinitions.insert(stdCursorName);
        }
        
        else if (clang_getCursorKind(c) == CXCursor_FieldDecl) {
            std::string type = extractType(stdCursorSource, stdCursorName);
            type = removeWhitespace(type);
            if(type.find("std::function<") != std::string::npos)
            {
                m_structMemberFunctions.insert(stdCursorSource);
            }
        }
        
        if (clang_getCursorKind(c) == CXCursor_FieldDecl)
        {
            CXType fieldType = clang_getCursorType(c);
            if (fieldType.kind == CXType_Pointer)
            {
                CXType pointeeType = clang_getPointeeType(fieldType);
                if (pointeeType.kind == CXType_FunctionProto)
                {
                    m_structMemberFunctions.insert(stdCursorName);
                }
            }
        }
        
        if (type.kind == CXType_Auto) {
            std::string linesWithAuto = filterLinesByPattern(stdCursorSource, "auto ");
            if(!linesWithAuto.empty())
            {
                m_autos.insert(linesWithAuto);
            }
        }
        
        if(isTemplated(c)) {
            m_templatedDefinitions.insert(stdCursorName);
        }
        
        if(m_functionToImpl != m_functionBeingDefined)
        {
            // Checking for structure declaration/definition
            if (clang_getCursorKind(c) == CXCursor_StructDecl) {
                
                if(hasAncestor(c, CXCursor_Namespace))
                {
                    m_inNamespace.insert(stdCursorName);
                }
                
                if (clang_isCursorDefinition(c)) {
                    m_definedStructs.insert(stdCursorName);
                    // Check if this struct is nested within another struct or class
                    if (hasAncestor(c, CXCursor_StructDecl) || hasAncestor(c, CXCursor_ClassDecl)) {
                        m_nestedStructs.insert(stdCursorName);
                    }
                    
                    if(hasBaseClass(c))
                    {
                        m_structsWithIneritance.insert(stdCursorName);
                    }
                    
                } else {
                    m_declaredStructs.insert(stdCursorName);
                }
            }
            
            // Checking for enum declaration/definition
            if (clang_getCursorKind(c) == CXCursor_EnumDecl) {
                
                if(hasAncestor(c, CXCursor_Namespace))
                {
                    m_inNamespace.insert(stdCursorName);
                }
                
                if (clang_isCursorDefinition(c)) {
                    m_definedEnums.insert(stdCursorName);
                    if(!isUint32Enum(c))
                    {
                        m_notUIntEnums.insert(stdCursorName);
                    }
                    // Check if this struct is nested within another struct or class
                    if (hasAncestor(c, CXCursor_StructDecl) || hasAncestor(c, CXCursor_ClassDecl)) {
                        m_nestedStructs.insert(stdCursorName);
                    }
                } else {
                    m_declaredEnums.insert(stdCursorName);
                }
            }
        }
        
        // Checking for global or external variables
        if (clang_getCursorKind(c) == CXCursor_VarDecl)
        {
            {
                //We need to verify if there are pointers or references
                //to variables from datat types defined by the application
                CXType pointeeType = clang_getPointeeType(type);
                bool isConstQualified = clang_isConstQualifiedType(pointeeType);
                bool isReference = type.kind == CXType_LValueReference;
                bool isPointer = type.kind == CXType_Pointer;
                
                if ((isReference || isPointer) && isConstQualified)
                {
                    CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
                    CXString typeName = clang_getTypeSpelling(pointeeType);
                    
                    std::string typeNameStr = getClangString(typeName);
                    std::set<std::string> appTypes = proj->getAppTypes(typeNameStr);
                    for(auto appType : appTypes)
                    {
                        std::string owningPath;
                        if(proj->findData(appType, owningPath))
                        {
                            m_constAppTypes.insert(stdCursorSource);
                        }
                    }
                }
            }
            
            CXCursorKind parentKind = clang_getCursorKind(parent);
            
            if(isExternalVariable(c))
            {
                m_globalVariables.insert(stdCursorName);
            }
            else if(m_functionToImpl == m_functionBeingDefined)
            {
                m_localAndArgs.insert(stdCursorName);
            }
            else if ((parentKind == CXCursor_TranslationUnit || parentKind == CXCursor_Namespace) &&
                     stdCursorName != m_functionToImpl)
            {
                checkAndAddGlobal(stdCursorName);
            }
            
            if(hasAncestor(c, CXCursor_Namespace))
            {
                m_inNamespace.insert(stdCursorName);
            }
        }
        
        if (clang_getCursorKind(c) == CXCursor_ParmDecl) {
            // Get the parent cursor, which should be the function declaration
            CXCursor functionCursor = clang_getCursorSemanticParent(c);
            std::string functionName = getCursorName(functionCursor);
            
            if(m_functionToImpl == functionName)
            {
                m_localAndArgs.insert(stdCursorName);
            }
            else if (functionName == "operator()") {
                // This is very likely a lambda call operator. Let's go up.
                
                // Start by getting the semantic parent of the parameter declaration
                CXCursor grandParentCursor = clang_getCursorSemanticParent(c);
                
                // Keep climbing up until we hit a translation-unit or find our target function
                int i=0;
                while (!clang_Cursor_isNull(grandParentCursor) &&
                       clang_getCursorKind(grandParentCursor) != CXCursor_TranslationUnit &&
                       i++<5)
                {
                    std::string parentName = getCursorName(grandParentCursor);
                    
                    if (parentName == m_functionToImpl) {
                        // We found it: the chain of parents eventually leads to m_functionToImpl
                        m_localAndArgs.insert(stdCursorName);
                        break;
                    }
                    
                    // Otherwise, move one level higher
                    grandParentCursor = clang_getCursorSemanticParent(grandParentCursor);
                }
            }
        }
        
        //Check for function declaration or definition
        if (clang_getCursorKind(c) == CXCursor_FunctionDecl) {
            
            std::string declaration = extractFunctionDeclaration(stdCursorSource);
            
            auto funcNamespaces = functionHasCustomNamespaces(stdCursorSource);
            if(funcNamespaces.size() > 0)
            {
                m_inNamespace.insert(declaration);
            }
            
            if(hasAncestor(c, CXCursor_Namespace))
            {
                m_inNamespace.insert(stdCursorName);
            }
            
            if(clang_isCursorDefinition(c)) {
                
                {
                    ParsedFunction signature = parseFunctionSignature(declaration);
                    std::string signatureStr = removeWhitespace(signature.str());
                    m_definedFunctionSignatures.insert(signatureStr);
                }
                
                m_functionBeingDefined = stdCursorName;
                if(m_functionBeingDefined == m_functionToImpl)
                {
                    m_functionToImplRange = range;
                }
                m_definedFunctions.insert(stdCursorName);
            } else {
                m_declaredFunctions.insert(stdCursorName);
            }
        }
        
        if(m_functionToImpl != m_functionBeingDefined)
            return true;
        
        if(clang_getCursorKind(c) == CXCursor_LambdaExpr)
        {
            CXTranslationUnit tu = clang_Cursor_getTranslationUnit(c);
            CXSourceRange  fullRange = clang_getCursorExtent(c);

            // 1) tokenize the entire lambda (captures, params, body, everything)
            CXToken *tokens = nullptr;
            unsigned numTokens = 0;
            clang_tokenize(tu, fullRange, &tokens, &numTokens);

            // 2) walk tokens, accumulate text until you see the “{” that opens the body
            std::string signature;
            for (unsigned i = 0; i < numTokens; ++i) {
                CXTokenKind kind = clang_getTokenKind(tokens[i]);
                CXString  sp    = clang_getTokenSpelling(tu, tokens[i]);
                std::string txt = clang_getCString(sp);
                clang_disposeString(sp);

                // stop once we hit the “{” that begins the body
                if (kind == CXToken_Punctuation && txt == "{")
                    break;

                // otherwise, append the token text (you may want to insert a space
                // between tokens, depending on how you want it formatted)
                signature += txt;
                signature += " ";
            }

            clang_disposeTokens(tu, tokens, numTokens);

            // 3) trim off any trailing whitespace
            auto not_space = [](char c){ return !isspace((unsigned char)c); };
            auto start_it  = std::find_if(signature.begin(), signature.end(), not_space);
            auto   end_it  = std::find_if(signature.rbegin(), signature.rend(), not_space).base();
            signature = (start_it < end_it
                         ? std::string(start_it, end_it)
                         : std::string());

            m_lambdas.insert(signature);
        }
        
        if (clang_getCursorKind(c) == CXCursor_DeclStmt) {
            
            std::string function;
            auto pos = findFunction(stdCursorSource, function);
            if(pos != std::string::npos) {
                std::string funcNoStrings = emptyAllStringLiterals(stdCursorSource.substr(pos));
                checkFunctionCall(funcNoStrings);
                checkMemberFunctionCall(funcNoStrings);
            }
        }
        
        //When a variable is used in an expression or passed as an argument in a function call,
        //the reference to this variable is typically represented by a CXCursor_DeclRefExpr.
        if (clang_getCursorKind(c) == CXCursor_DeclRefExpr) {
            //CXCursor referencedVar = clang_getCursorReferenced(c);
            CXCursor referencedVar = getCursorDeclaration(c);
            if(isValidCursor(referencedVar) &&
               clang_getCursorKind(referencedVar) != CXCursor_EnumConstantDecl &&
               clang_getCursorKind(referencedVar) != CXCursor_CXXMethod)
            {
                std::string varName = getCursorName(referencedVar);
                std::string declaredInFile = getCursorFile(referencedVar);
                if(declaredInFile == "code.cpp")
                {
                    bool isInLambda = hasAncestor(c, CXCursor_LambdaExpr);
                    if(!isInLambda)
                    {
                        std::string srcLine = getFullLineForCursor(c, m_code, "code.cpp");
                        checkAndAddGlobal(varName);
                    }
                }
            }
        }
        
        // Check for empty scopes
        if (clang_getCursorKind(c) == CXCursor_CompoundStmt) {
            std::string scopeSrc = getCursorSourceCode(c, m_code);
            
            if(!isCompoundStatementEmpty(c))
            {
                checkFunctionsInScope(scopeSrc);
                checkMemberFunctionsInScope(scopeSrc);
            }
            else
            {
                bool isEmpty = checkSource(scopeSrc);
                if(isEmpty)
                {
                    m_emptyScope.insert(scopeSrc);
                }
            }
        }

        // Checking for function calls
        if (clang_getCursorKind(c) == CXCursor_CallExpr) {
    
            //std::string functionName = getCalledFunctionName(c);
            
            std::string functionName = extractFunctionName(stdCursorSource);
            if(m_appFunctions.find(functionName) != m_appFunctions.end())
            {
                analyzeFunctionForEffect(c, m_noEffectFunctions);
                if(appFunctionHasInitializerList(c))
                {
                    m_appFunctionsWithInitializers.insert(functionName);
                }
            }
            
            if(isFunctionCallWithTemplates(c))
            {
                m_ccNode->m_stats.m_complexCode.insert(stdCursorSource);
            }
            
            std::string function;
            auto pos = findFunction(stdCursorSource, function);
            if(pos != std::string::npos) {
                std::string funcNoStrings = emptyAllStringLiterals(stdCursorSource.substr(pos));
                checkFunctionCall(funcNoStrings);
                checkMemberFunctionCall(funcNoStrings);
            }
        }
        
        //check for function pointers assignments
        if (clang_getCursorKind(c) == CXCursor_BinaryOperator) {
            CXString opSpelling = clang_getCursorSpelling(c);
            
            //For binary operations, the first child (index 0) is the LHS,
            //and the second child (index 1) is the RHS.
            
            std::string op = clang_getCString(opSpelling);
            clang_disposeString(opSpelling);
            
            if (op == "=") {
                CXCursor lhs = clang_getNullCursor();
                CXCursor rhs = clang_getNullCursor();
                
                struct VisitorData {
                    CXCursor* lhs;
                    CXCursor* rhs;
                } visitorData = {&lhs, &rhs};

                clang_visitChildren(c,
                    [](CXCursor cursor, CXCursor parent, CXClientData client_data) {
                        auto* data = reinterpret_cast<VisitorData*>(client_data);
                        if (clang_Cursor_isNull(*data->lhs)) {
                            *data->lhs = cursor;
                        } else {
                            *data->rhs = cursor;
                            return CXChildVisit_Break;
                        }
                        return CXChildVisit_Continue;
                    },
                    &visitorData
                );
                
                // Check if LHS is a member access
                if (clang_getCursorKind(lhs) == CXCursor_MemberRefExpr) {
                    if(isAssignedFunctionType(rhs)) {
                        m_structMemberFunctions.insert(stdCursorSource);
                    }
                }
            }
        }
        
        return true;
    }
    
    void checkNoFunc(std::stringstream& review, bool called, bool defined, bool decl)
    {
        if(called) {
            listOnMessage(review, "Not expected function calls", m_calledFunctions, "Remove listed function calls");
        } else {
            //TODO: make note on calld functions that aren't from the available libraries and m_appFunctions
        }
        
        if(defined) {
            listOnMessage(review, "Not expected function definitions", m_definedFunctions, "Remove listed function definitions");
        } else {
            std::set<std::string> definedFunctions = m_definedFunctions;
            definedFunctions.erase(m_functionToImpl);
            listOnMessage(review, "Not expected function definitions", definedFunctions, "Remove the definition for the listed functions. Only the '" + m_functionToImpl + "' function shuld be implemented");
        }
        
        if(decl) {
            listOnMessage(review, "Not expected function declarations", m_declaredFunctions, "Remove listed function declarations");
        }
    }
    
    void checkNoStructs(std::stringstream& review, bool defined, bool decl)
    {
        if(defined) {
            listOnMessage(review, "Not expected struct definitions", m_definedStructs, "Remove listed struct definitions");
        }
        
        if(decl) {
            listOnMessage(review, "Not expected struct declarations", m_declaredStructs, "Remove listed struct declarations. If it is for a struct we've already discussed and you know how it is defined, you can continue to use its type name in the code. Forward declaration will be added later, we don’t need forward declarations now.");
        }
    }
    
    void checkNoEnums(std::stringstream& review, bool defined, bool decl)
    {
        if(defined) {
            listOnMessage(review, "Not expected enum definitions", m_definedEnums, "Remove listed enum definitions");
        }
        
        if(decl) {
            listOnMessage(review, "Not expected enum declarations", m_declaredEnums, "Remove listed enum declarations. If it is for an existing enum that we've discussed and you know how it is defined, you can keep using its name in the code, don't replace it with uint32_t! Forward declaration will be added later.");
        }
    }
    
    bool hasExceptions(const std::string& source) {
        std::regex exceptionPattern(R"(\b(try\s*\{|\bcatch\b|\bthrow\b|\bnoexcept\b))");

        return std::regex_search(source, exceptionPattern);
    }
    
    void finishReport(std::stringstream& review)
    {
        std::set<std::string> calledFunctions;
        std::set_difference(m_calledFunctions.begin(), m_calledFunctions.end(),
                            m_localAndArgs.begin(), m_localAndArgs.end(),
                            std::inserter(calledFunctions, calledFunctions.begin()));
        
        m_calledFunctions = calledFunctions;
        
        calledFunctions.clear();
        std::set_difference(m_calledFunctions.begin(), m_calledFunctions.end(),
                            m_preprocessorDefinitions.begin(), m_preprocessorDefinitions.end(),
                            std::inserter(calledFunctions, calledFunctions.begin()));
        m_calledFunctions = calledFunctions;
        
        
        listOnMessage(review, "Not expected includes", m_includes, "Remove the listed include statements from the source. They will be added later.");
        
        listOnMessage(review, "Not expected preprocessor directives", m_directives, "Remove the directives from the source. Preprocessor directives aren't supported in this project.");
        
        listOnMessage(review, "Not expected classes", m_classes,
                      "Remove the C++ classes. This software should define or declare only struct type of data. Classes might be used only from the available libraries");
        
        listOnMessage(review, "Not expected struct member functions, constructors, destructors, std::function or function pointers", m_structMemberFunctions,
                      "Remove or replace appropriately the listed structure member functions, constructors, destructors, std::function or function pointers. Sructures should have only public data members that aren't functions or function pointers. This code must not store or call function pointers from data types, use global fuctions instead");
        
        listOnMessage(review, "Not expected nested data types definitions", m_nestedStructs,
                      "Refactor nested data types by moving them to the global scope.");
        
        listOnMessage(review, "Not expected lambda expressions", m_lambdas,
                      "Refactor the implementation to not use lambdas. If not possible to implement everything by using available functions and libraries, consider calling a new helper function. Just call the new function, we'll declare and define it later based on the usage.");
        
        listOnMessage(review, "Not expected preprocessor definitions", m_preprocessorDefinitions,
                      "Refactor the implementation to not use perprocessor definitions. If not possible to implement everything by using available functions and libraries, consider calling a new helper function. Just call the new function, we'll declare and define it later based on the usage.");
        
        if(m_autos.size() && m_ccNode->m_reviewLevel > CCodeNode::ReviewLevel_1)
        {
            //let's check for 'auto' in the source to avoid false positives
            if(findAutoKeyword(m_codeForAnalysis).size() > 0)
            {
                listOnMessage(review, "Found 'auto' keywords", m_autos,
                              "Don't use the 'auto' keyword for data types that are presumably defined in this software. Replace the 'auto' keywords with the appropriate type. Notice, you might need to figure out data produced and consumed by the functions. You have to decide what are their arguments and return type based on function descriptions. Based on your decision, we are going to declare and define those data types later.");// It is fine to use 'auto' for types from the standard library");
                //TODO: Consider to re-enable the relaxed hint for auto with STL only types.
            }
        }
        
        listOnMessage(review, "Not expected global or external variables", m_globalVariables, "Don't declare external and global variables and data structures! To pass the data to function calls use function parameters. To output form a function call, use the return result and mutable parameters. To declare return type and arguments in a function declaration, use the C++ sytax for function declaration");
        
        listOnMessage(review, "Not expected namespaces for functions or data types", m_inNamespace,
                      "Refactor the listed functions or data types according to the guidelines for allowed namespaces na STL types in the 'AVAILABLE LIBRARIES' section. Application-defined functions and data types should generally be defined in the global scope rather than within namespaces. Check ");
        
        listOnMessage(review, "Not expected typedef statement", m_typedefs,
                      "Typedefs are not allowed in this project. Replace typedefs with 'struct' or 'enum' definitions or remove them");
        
        if(hasExceptions(m_codeForAnalysis))
        {
            review << "C++ exceptions are not recommended! Replace with checks for errors in functions output." << std::endl << std::endl;
        }
        
        listOnMessage(review, "Not expected template definitions", m_templatedDefinitions,
                      "Refactor the listed definitions without tempates. Templates use in structs and function definitions is not recommended in this project");
        
        listOnMessage(review, "Not expected data types with inheritance", m_structsWithIneritance,
                      "Refactor the listed data types to not inherit other data types. You can use compopsition instead. Inheritance is not recommended in this project");
        
        switch(m_type)
        {
            //bool called, bool defined, bool decl
            case CCodeNode::FUNC_DECL:
            {
                checkNoFunc(review, true, true, false);
                checkNoStructs(review, true, true);
                checkNoEnums(review, true, true);
                
                listOnMessage(review, "Found comments that suggest not full function declaration", m_todos, "Full declaration is expected!");
                
                listOnMessage(review, "Not expected empty scopes", m_emptyScope, "Provide only implementation!");
                
                if(m_declaredFunctions.size() > 1)
                {
                    listOnMessage(review, "More than one function declarations found", m_declaredFunctions,
                                  "Exactly one function declaration must appear in the provided source, with no additional declarations or definitions.");
                }
            }
            break;
            case CCodeNode::FUNC_IMPL:
            case CCodeNode::FUNC_CMPL:
            case CCodeNode::FUNC_FIX:
            {
                checkNoFunc(review, false, false, true);
                checkNoStructs(review, true, true);
                checkNoEnums(review, true, true);
                
                {
                    CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
                    std::set<std::string> calledFunctions = getCalledFunctions();
                    std::set<std::string> dataConflicts = proj->getConflictsWithData(calledFunctions);
                    if(dataConflicts.size() > 0)
                    {
                        listOnMessage(review, "The following identifiers are detected as a calls to functions but data types with the same name already exists in the project",
                                      dataConflicts, "We can't have function names in conflict with existing data. Consider renaming the functions!");
                    }
                }
                
                if(m_definedFunctionSignatures.size() > 1)
                {
                    std::string recoForFuncDef = "Only the definition of function '" + m_functionBeingDefined + "' is expected in the code snippet";
                    listOnMessage(review, "More than one functions declared or defined. List of defined/declared function signatures", m_definedFunctionSignatures, recoForFuncDef);
                }
                
                listOnMessage(review, "Function calls that are supposed to affect the program state by accessing data via their arguments and produce or modify data via return result or mutable arguments", m_noEffectFunctions,
                              "Modify the listed functions to access data they need via their parameters. To ouput data form the functions use return result and mutable parameters.");
                
                //Handle unused function calls (as listed in m_calls)
                {
                    std::set<std::string> notCalledAppFunctions;
                    for(auto appFun : m_appFunctions)
                    {
                        if(m_calledAppFunctions.find(appFun) == m_calledAppFunctions.end())
                        {
                            bool foundInSource = m_codeForAnalysis.find(appFun) != std::string::npos;
                            if(!foundInSource && findFunctionCalls(m_codeForAnalysis, appFun).empty())
                            {
                                notCalledAppFunctions.insert(appFun);
                            }
                        }
                    }
                    
                    m_ccNode->m_stats.m_unusedFunctions = notCalledAppFunctions;
                    if(m_ccNode->m_stats.m_unusedFunctionsReportsCount == 0)
                    {
                        listOnMessage(review, "Functions not called by the implementation but were listed as potential candidates to be used ", notCalledAppFunctions, "Review the documentation of these functions if they are useful for your implementation.");
                    }
                    
                    if(!notCalledAppFunctions.empty())
                    {
                        //The selection of functions to be used in the implementation must have been suggested by the DIRECTOR model
                        //It makes no sense the DEVELOPER model to dispute this decision
                        if(Client::getInstance().getLLM() >= LLMRole::EXPERT)
                        {
                            m_ccNode->m_stats.m_unusedFunctionsReportsCount++;
                        }
                        else if(m_ccNode->m_stats.m_unusedFunctionsOccurrencesCount >= 1)
                        {
                            //After few attempts with LLMRole::DEVELOPER escalate to higher model to decide how to use suggested functions
                            Client::getInstance().escalateLLM();
                        }
                        
                        m_ccNode->m_stats.m_unusedFunctionsOccurrencesCount++;
                    }
                }

                if(!m_ccNode->m_excludeCalls.empty())
                {
                    std::string functionsToExclude;
                    for(auto func : m_calledFunctions)
                    {
                        auto itEx = m_ccNode->m_excludeCalls.find(func);
                        if(itEx != m_ccNode->m_excludeCalls.end())
                        {
                            if(!functionsToExclude.empty()) {
                                functionsToExclude += ", ";
                            }
                            functionsToExclude += func;
                        }
                    }
                    
                    if(!functionsToExclude.empty())
                    {
                        std::string recommendation = "Currently the implementation is calling: ";
                        recommendation += functionsToExclude + "\n";
                        recommendation += "Please remove the listed functions";
                        
                        listOnMessage(review, "The following functions must not be called in the implementation", m_ccNode->m_excludeCalls, recommendation);
                    }
                }
                
                std::stringstream recommendationTodos;
                recommendationTodos << "Provide full implementation, no placeholders and stubs, no \"TO DO\" comments, no empty scopes. Implement everything!";
                recommendationTodos << " If it is not possible to implement everyting in the function '" << m_ccNode->m_brief.func_name << "' ";
                recommendationTodos << " consider calling a new helper function or functions to take this responsibility. Just call the new functions, we are going to declare and define it later based on it's usage in the code. If these strings are in literals, if possible, consider changing the string to avoid this confusion\n";
                
                listOnMessage(review, "Found comments with the following strings that suggest not full implementation:", m_todos, recommendationTodos.str());
                
                listOnMessage(review, "Not expected empty scopes", m_emptyScope, "Provide full implementation, no placeholders and stubs, no \"TO DO\" comments, no empty scopes. Implement everything!");
                
                if(m_ccNode->m_reviewLevel > CCodeNode::ReviewLevel_1)
                {
                    listOnMessage(review, "The following source locations have variables (from structs defined by the application) declared as pointers or references", m_constAppTypes, "Don't declare pointers and references to structs defined by the application as constant. Refactor the source in a way all variables from data types defined by the application are non-constant");
                }
                
                listOnMessage(review, "The following custom functions defined by the application have initializers lists as their arguments", m_appFunctionsWithInitializers, "Initializers list aren't allowed as function arguments in this project. Replace with proper arguments");
            }
            break;
            case CCodeNode::DATA_DEF:
            {
                checkNoFunc(review, true, true, true);
                checkNoStructs(review, false, true);
                checkNoEnums(review, false, true);
                
                {
                    CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
                    std::set<std::string> dataDefinitions = m_definedStructs;
                    dataDefinitions.insert(m_definedEnums.begin(), m_definedEnums.end());
                    std::set<std::string> functionConflicts = proj->getConflictsWithFunctions(dataDefinitions);
                    if(functionConflicts.size() > 0)
                    {
                        listOnMessage(review, "The following identifiers are detected as data definitions but functions with the same name already exist in the project",
                                      functionConflicts, "We can't have data type names in conflict with existing functions. Consider renaming the data types!");
                    }
                }
                
                listOnMessage(review, "Found comments that suggest not full data definition", m_todos, "Provide full data definition!");
                
                listOnMessage(review, "Not expected empty scopes", m_emptyScope, "Provide full implementation, no placeholders and stubs, no \"TO DO\" comments, no empty scopes. Implement everything!");
                
                listOnMessage(review, "Enum definitions without scoped underlying uint32_t type", m_notUIntEnums,
                              "Enum data types in this projects must be defined with 'uint32_t' underlying type, for example: enum class MyEnumName : uint32_t {/*Enum fields here*/};");
            }
            break;
            case CCodeNode::TEST:
            {
                checkNoFunc(review, false, false, true);
                checkNoStructs(review, true, true);
                checkNoEnums(review, true, true);
                
                listOnMessage(review, "Function calls that are supposed to affect the program state by accessing data via their arguments and produce or modify data via return result or mutable arguments", m_noEffectFunctions,
                              "Modify the listed functions to access data they need via their parameters. To ouput data form the functions use return result and mutable parameters.");
                
                std::set<std::string> notCalledAppFunctions;
                std::set_difference(m_appFunctions.begin(), m_appFunctions.end(),
                                    m_calledAppFunctions.begin(), m_calledAppFunctions.end(),
                                    std::inserter(notCalledAppFunctions, notCalledAppFunctions.begin()));
                
                listOnMessage(review, "Functions not called by the implementation but were supposed to", notCalledAppFunctions, "Modify the source to use the listed functions. See their description for guidelines how to use these functions");
                
                listOnMessage(review, "Found comments with the following strings that suggest not full implementation:", m_todos, "Full implementation is expected! If these strings are parts of a literal, if possible, consider changing the string to avoid this confusion");
            }
            break;
            case CCodeNode::EXTRACT:
            {
                //Do something ?
            }
            break;
            default: assert(0);
        }
    }
};


std::string Linter::m_code;

std::string getCursorSourceCode(CXCursor c) {
    return getCursorSourceCode(c, Linter::code());
}

void CCodeNode::clang(const std::string& code, CodeType type, CodeInspector codeInspector)
{
    CXIndex index = clang_createIndex(0, 0);
    
    std::string sysroot = getSysRoot();
    std::string resourceDir = getClangResourceDir();
    std::string cxxInclude  = getCppInclude();
    std::string cxxIncludeOpt = "-I" + cxxInclude;
    
    //sdkPath.rfind('c');
    const char* clang_args[] = {
        "-x", "c++",
        "-std=c++17",
        "-stdlib=libc++",
        "-isysroot", sysroot.c_str(),
        "-resource-dir", resourceDir.c_str(), // ← critical for stdarg.h, stdint.h, intrinsics, etc.
        cxxIncludeOpt.c_str(), // ← libc++ headers
        "-DCOMPILE_TEST",
        "-Werror=format",
        "-D_LIBCPP_HAS_NO_WIDE_CHARACTERS",//Without this we get "couldn't find stdarg.h" error
    };
    
    Linter checker(this, code, type);
    CXUnsavedFile unsavedFile = { "code.cpp", Linter::code().c_str(), (unsigned long)Linter::code().length() };
    
    CXTranslationUnit unit = clang_parseTranslationUnit(
        index,
        "code.cpp",  // Provide a dummy file name
        clang_args, sizeof(clang_args) / sizeof(clang_args[0]),
        &unsavedFile, 1,  // Pass the unsaved file
        CXTranslationUnit_KeepGoing | CXTranslationUnit_DetailedPreprocessingRecord
        //| CXTranslationUnit_Incomplete
    );

    if (unit == nullptr) {
        std::cerr << "Warning: unable to parse translation unit: " << Linter::code() << std::endl;
        std::cerr << "Quitting." << std::endl;
        clang_disposeIndex(index);
        return;
    }
    
    std::string errors = printDiagnostics(unit, false);
    
    if(!errors.empty() && type != CCodeNode::EXTRACT)
    {
        std::cout << "CCodeNode::clang - code evaluation errors:" << std::endl;
        std::cout << errors << std::endl;
    }
    
    struct ClientData {
        CCodeNode* node;
        CodeInspector codeInspector;
    } m_data;
    m_data.node = this;
    m_data.codeInspector = codeInspector;

    CXCursor cursor = clang_getTranslationUnitCursor(unit);
    
    //TODO: Needs extensive testing!!!
    //***************************************************************************
    if(type == CodeType::FUNC_IMPL ||
       type == CodeType::FUNC_CMPL ||
       type == CodeType::FUNC_FIX)
    {
        visitChildren(cursor, [&](CXCursor c, CXCursor parent) {
            bool inspect = checker.filter(c, false);
            if(inspect)
            {
                checker.checkStringLiteral(c);
            }
        });
        
        checker.instrumentStringLiterals(unit);
        
        //Parse again!
        clang_disposeTranslationUnit(unit);
        
        unsavedFile = { "code.cpp", Linter::code().c_str(), (unsigned long)Linter::code().length() };
        unit = clang_parseTranslationUnit(
                                          index,
                                          "code.cpp",  // Provide a dummy file name
                                          clang_args, sizeof(clang_args) / sizeof(clang_args[0]),
                                          &unsavedFile, 1,  // Pass the unsaved file
                                          CXTranslationUnit_KeepGoing | CXTranslationUnit_DetailedPreprocessingRecord
                                          //| CXTranslationUnit_Incomplete
                                          );
        cursor = clang_getTranslationUnitCursor(unit);
    }
    //***************************************************************************
    
    m_codeReview.str("");
    m_codeReview.clear();
    
    //TODO: Enable this for some checks - data definitions, function declaration.
    //It won't work for the first definition of the datatypes. Even without depencencies,
    //the order of definition could be random and we don't have forward declaraton
    if(m_enableDiagnostics)
    {
        if(!errors.empty())
        {
            m_diagnostics = errors;
            if(m_reviewDiagnostics)
            {
                m_codeReview << errors;
            }
        }
    }
    
    visitChildren(cursor, [&](CXCursor c, CXCursor parent) {
        bool inspect = checker.visit(c, parent, m_codeReview);
        if(inspect)
        {
            codeInspector(c, parent);
        }
    });
    
    checker.finishReport(m_codeReview);
    
    //TODO: Move this after the review loop or verify if the function doesn't have definition within the code snippet!!!
    if(type == FUNC_IMPL ||
       type == FUNC_CMPL ||
       type == FUNC_FIX)
    {
        const std::set<std::string>& calls = checker.getCalledFunctions();
        synchronizeFunctionCalls(calls, type);
    }
    
    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
}

void CCodeNode::validateFunctionDeclaration()
{
    CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
    
    if(!m_this || !m_this->m_parent)
        return;
    
    auto parent = (const CCodeNode*)m_this->m_parent->m_data;
    if(!parent)
        return;
    
    std::string testCode;
    
    for(auto inc : CCodeProject::getSTDIncludes())
    {
        testCode += "#include <" + inc + ">\n";
    }
    
    std::string dagPath = parent->getDAGPath("/");
    testCode += proj->declareData(false, dagPath) + "\n\n";
    testCode += proj->defineData(false, dagPath) + "\n\n";
    
    std::set<std::string> calledFunctions = parent->getCalledFunctions();
    
    for(auto func : calledFunctions)
    {
        const CCodeNode* node = nullptr;
        auto it = proj->nodeMap().find(func);
        if(it != proj->nodeMap().end())
        {
            node = (const CCodeNode*)it->second;
        }
        
        //TODO: I'm quite sure we must signal some kind of error if there is no node with the same name.
        if(!node) continue;
        
        std::string dagPath = node->getDAGPath("/");
        testCode += proj->declareData(false, dagPath) + "\n\n";
        testCode += proj->defineData(false, dagPath) + "\n\n";
    }

    testCode += proj->getFunctionStubs(calledFunctions);
    
    testCode += parent->m_implementation.definition;
    testCode += "\n\n";
    
    std::string errors = evaluateCodeForErrors(testCode, true);
    if(!errors.empty())
    {
        std::cout << "ERRORS:" << std::endl << errors << std::endl;
    }
}

void CCodeNode::reflectFunction()
{
    CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
    m_codeReview.str("");
    m_codeReview.clear();
    
    if(m_prototype.declaration.length() < 5)
    {
        m_codeReview << "The provided code snippet:";
        m_codeReview << std::endl << "//***" << m_prototype.declaration << "//***" << std::endl;
        m_codeReview << "Doesn't represent a valid C++ declaration for function '";
        m_codeReview << m_brief.func_name << "'" << std::endl;
        return;
    }
    
    if(!isSimpleFunctionDeclaration(m_prototype.declaration))
    {
        m_codeReview << "The provided code snippet desn't represent simple function declaration:";
        m_codeReview << std::endl << "//***" << m_prototype.declaration << "//***" << std::endl;
        m_codeReview << "Simple function declaration is the one that doesn't have function pointers in its return type and arguments" << std::endl;
        return;
    }
    
    bool needNewSignature = false;
    
    std::string defineStructMembersHintRef = "For how to use enums and structs defined by the applicatoin in C++ function declaration as arguments and return types";
    defineStructMembersHintRef += "refere to the section 'REQUIREMENTS FOR FUNCTION DECLARATIONS'\n";
    
    std::string defineStructMembersHint = defineStructMembersHintRef;
    
    m_prototype.m_signature = parseFunctionSignature(m_prototype.declaration);
    
    std::set<std::string> invalidTypes;
    if(!isValidCppType(m_prototype.m_signature.returnType)) {
        invalidTypes.insert(m_prototype.m_signature.returnType);
    }
    
    for(auto arg : m_prototype.m_signature.argumentTypes) {
        if(!isValidCppType(arg)) {
            invalidTypes.insert(arg);
        }
    }
    
    if(!invalidTypes.empty())
    {
        m_codeReview << "The following types have characters invalid for an C++ type: ";
        bool first = true;
        for (auto invalidType : invalidTypes) {
            m_codeReview << (first ? "" : ", ") << invalidType;
            first = false;
        }
        m_codeReview << std::endl;
        
        needNewSignature = true;
    }
    
    auto constTypes = proj->hasConstantTypes(m_prototype.m_signature);
    if(!constTypes.empty())
    {
        m_codeReview << "The function '" << m_brief.func_name << "' has custom, application-defined arguments or return types ";
        m_codeReview << "that are constant or ::const_iterator. Here are the types:\n";
        for(auto ct : constTypes) {
            m_codeReview << ct << std::endl;
        }
        m_codeReview << " In this software functions must not have constant custom types or iterators for their arguments or return type.";
        m_codeReview << " Make all custom types in the function non-constant and change all iterators from ::const_iterator to ::iterator!";
        //TODO: Consider uncomment this if too many errors are associated with constnt types and iterators!
        //m_codeReview << std::endl
        //m_codeReview << defineStructMembersHint
        m_codeReview << std::endl << std::endl;
        needNewSignature = true;
    }
    
    std::set<std::string> restrictedSTDTypes;
    bool reportInvalidContainers = false;
    if(m_stats.m_containerWithValuesReportsCount == 0)
    {
        auto returnSTDTypes = proj->hasRestrictedStdTypes(m_prototype.m_signature.returnType);
        restrictedSTDTypes.insert(returnSTDTypes.begin(), returnSTDTypes.end());
        
        for(auto arg : m_prototype.m_signature.argumentTypes)
        {
            auto argSTDTypes = proj->hasRestrictedStdTypes(arg);
            restrictedSTDTypes.insert(argSTDTypes.begin(), argSTDTypes.end());
            
            if(proj->isInvalidContainer(arg))
            {
                reportInvalidContainers = true;
                
                m_codeReview << "Argument type: '" << arg << "' seems to be an STD container for values from application-defined structure type.";
                if(proj->isInvalidIterator(arg))
                {
                    m_codeReview << " It is also an STD iterator.";
                }
                m_codeReview << std::endl;
            }
        }
        
        if(reportInvalidContainers)
        {
            m_codeReview << "Note that this software, excepts the external libraries, defines only struct and enum as C++ data types. ";
            m_codeReview << defineStructMembersHint;
            m_codeReview << "Please consider the above requirements when deciding how to define function argument for '";
            m_codeReview << m_brief.func_name << "' ";
            m_codeReview << "Is it efficient to have containers with values instead of shared pointers from data types defined by the application?" << std::endl;
            needNewSignature = true;
            defineStructMembersHint = defineStructMembersHintRef;
        }
        
        m_stats.m_containerWithValuesReportsCount++;
    }
    
    {
        std::set<std::string> typesWithSmartPointersFromSTDtypes;
        std::set<std::string> typesWithMoreSmartPointers;
        
        if(findSharedPointersInType(m_prototype.m_signature.returnType) > 1)
        {
            typesWithMoreSmartPointers.insert(m_prototype.m_signature.returnType);
        }
        
        if(hasSharedPtrToListedStdType(m_prototype.m_signature.returnType, CCodeProject::getStdContainers()))
        {
            typesWithSmartPointersFromSTDtypes.insert(m_prototype.m_signature.returnType);
        }
        
        for(auto arg : m_prototype.m_signature.argumentTypes)
        {
            if(findSharedPointersInType(arg) > 1)
            {
                typesWithMoreSmartPointers.insert(arg);
            }
            
            if(hasSharedPtrToListedStdType(arg, CCodeProject::getStdContainers()))
            {
                typesWithSmartPointersFromSTDtypes.insert(arg);
            }
        }
        
        if(!typesWithSmartPointersFromSTDtypes.empty())
        {
            m_codeReview << "The following types have STD containers that are accessed with std::shared_ptr:\n";
            m_codeReview << getAsCsv(typesWithSmartPointersFromSTDtypes) + "\n";
            m_codeReview << defineStructMembersHint;
            m_codeReview << "Please consider the above requirements when deciding how to define function argument for '";
            m_codeReview << m_brief.func_name << "' ";
            m_codeReview << "STL containers shouldn't be accessed with smart pointers" << std::endl;
            
            needNewSignature = true;
            defineStructMembersHint = defineStructMembersHintRef;
        }
        
        if(!typesWithMoreSmartPointers.empty())
        {
            m_codeReview << "The following types have more than one std::shared_ptr:\n";
            m_codeReview << getAsCsv(typesWithMoreSmartPointers);
            m_codeReview << defineStructMembersHint;
            m_codeReview << "Please consider the above requirements when deciding how to define function argument for '";
            m_codeReview << m_brief.func_name << "' ";
            m_codeReview << "Types shouldn't have more than one std::shared_ptr" << std::endl;
            needNewSignature = true;
            defineStructMembersHint = defineStructMembersHintRef;
        }
    }
    
#ifdef RESTRICT_STL_TYPES
    if(!restrictedSTDTypes.empty())
    {
        listOnMessage(m_codeReview,
            "Function declarations cannot use the following STL types:",
            restrictedSTDTypes,
            "See 'AVAILABLE LIBRARIES' for permitted STL types in function declarations"
        );
        
        needNewSignature = true;
    }
#endif //RESTRICT_STL_TYPES
    
    if(m_prototype.declaration.find("weak_ptr") != std::string::npos ||
       m_prototype.declaration.find("unique_ptr") != std::string::npos)
    {
        m_codeReview << "Don't use std::weak_ptr and std::unique_ptr for function arguments and return types. ";
        m_codeReview << defineStructMembersHint;
        m_codeReview << "\nPlease consider the above requirements when deciding how to define arguments and return type for '";
        m_codeReview << m_brief.func_name << "' " << std::endl;
        
        needNewSignature = true;
        defineStructMembersHint = defineStructMembersHintRef;
    }
    
    if(m_prototype.declaration.find("=") != std::string::npos)
    {
        m_codeReview << "In the function declaration, don't use arguments initialized with default values. ";
        m_codeReview << "You can initialize the arguments later when the function will be defined " << std::endl;
        
        needNewSignature = true;
    }
    
    if(isTemplatedFunction(m_prototype.declaration))
    {
        m_codeReview << "Function templates are not allowed in this software! ";
        m_codeReview << "Note: using templated types (e.g. STL containers) in parameters is fine." << std::endl;
        
        needNewSignature = true;
    }
    
    if(m_prototype.m_signature.functionName.find("::") != std::string::npos)
    {
        m_codeReview << "Don't prefix the function name with a namespace. ";
        m_codeReview << "All functions defined in this project must be in the global namespace." << std::endl;
        
        needNewSignature = true;
    }
    
    if(m_prototype.declaration.find("...") != std::string::npos)
    {
        m_codeReview << "Don't define function with variadic parameters. ";
        m_codeReview << "All function arguments must be explicitly named and typed. " << std::endl;
        m_codeReview << "refere to the section 'REQUIREMENTS FOR FUNCTION DECLARATIONS'\n";
        
        needNewSignature = true;
    }
    
    if(m_prototype.m_signature.functionName != m_brief.func_name)
    {
        std::string message = "Function name in the 'declaration' field: '" + m_prototype.m_signature.functionName;
        message += "' doesn't match already selected name of the function: '" + m_brief.func_name + "'\n";
        message += "Function name in the 'declaration' must be changed to '" + m_brief.func_name + "'\n";
        
        m_codeReview << message << std::endl;
        
        needNewSignature = true;
    }
    
    //If this is the entry point we need to enforce the format of the main function
    if(!m_this || !m_this->m_parent)
    {
        std::set<std::string> appTypes = proj->getAppTypesForFunction(m_prototype.m_signature);
        if(m_prototype.m_signature.functionName != "main" ||
           !appTypes.empty() ||
           m_prototype.m_signature.argumentTypes.size() != 2 ||
           m_prototype.m_signature.argumentTypes[0].substr(0, 3) != "int" ||
           m_prototype.m_signature.argumentTypes[1].substr(0, 4) != "char")
        {
            m_codeReview << "The provided function declaration doesn't match the requirements ";
            m_codeReview << "for the format of the entry point to a POSIX console application." << std::endl;
            m_codeReview << "Current declaration: " << m_prototype.declaration << std::endl;
            m_codeReview << "Required declaration: int main(int argc, char* argv[])" << std::endl;
            
            needNewSignature = true;
        }
    }
    
    std::set<std::string> typesInConflict;
    std::set<std::string> typesNeedSharedPtr;
    std::set<std::string> typesToQualifyEnums;
    std::set<std::string> appTypes = proj->getFullAppTypesForFunction(m_prototype.m_signature);
    
    std::set<std::string> conflictedTypes = proj->getConflictsWithFunctions(appTypes);
    if(!conflictedTypes.empty())
    {
        m_codeReview << "The names of the follwoing data types are in conflict with already defined functions:" << std::endl;
        m_codeReview << getAsCsv(conflictedTypes) << std::endl;
        m_codeReview << "If these data types are newly introduced, consider renaming them!" << std::endl << std::endl;
    }
    
    for(auto type : appTypes)
    {
        std::set<std::string> appDefinedTypes = proj->getAppTypes(type);
        for(auto appType : appDefinedTypes)
        {
            if(isTypePrefixedByNamespace(type, appType))
            {
                m_codeReview << "A type defined by the application is prefixed with a namespace: " << type << std::endl;
                m_codeReview << "Types defined by this application must be in the global namespace." << std::endl;
                
                needNewSignature = true;
            }
            
            checkAppTypeQualification(appType, type, type, typesInConflict, typesNeedSharedPtr, typesToQualifyEnums);
        }
    }
    
    auto funcNS = functionHasCustomNamespaces(m_prototype.declaration);
    if(funcNS.size() > 0)
    {
        m_codeReview << "The declaration of the function '" << m_prototype.m_signature.functionName << "'";
        m_codeReview << " has namespaces that aren't allowed: ";
        bool first = true;
        for (auto const& ns : funcNS) {
            m_codeReview << (first ? "" : ", ") << ns;
            first = false;
        }
        m_codeReview << std::endl;
        m_codeReview << "Refactor the function declaration according to the guidelines for namespaces and STL types in the 'AVAILABLE LIBRARIES' section." << std::endl;
        m_codeReview << "Allowed STD namespaces in this project are: ";
        first = true;
        for (auto const& ns : proj->getLibNamespaces()) {
            m_codeReview << (first ? "" : ", ") << ns;
            first = false;
        }
        m_codeReview << std::endl;
        m_codeReview << "Application-defined functions and data types has to be defined ";
        m_codeReview << "in the global scope rather than within namespaces\n";
        
        needNewSignature = true;
    }
    
    std::set<std::string> appTypesWithNamespace = proj->appTypesHaveNamespace(m_prototype.declaration);
    if(!appTypesWithNamespace.empty())
    {
        listOnMessage(m_codeReview, "The following types defined by the application have namespaces", appTypesWithNamespace,
                      "Application-defined functions and data types should generally be defined in the global scope rather than within namespaces");
        
        needNewSignature = true;
    }
    
    if(!checkSTDTypes())
    {
        needNewSignature = true;
    }
    
    if(!typesInConflict.empty())
    {
        listOnMessage(m_codeReview, "The following types are in conflict with some of the included libraries", typesInConflict,
                      "For each of the listed types consider the following:\n- If it's a custom type defined in this application, consider a different name.\n- If it's from the C++ standard library, it must include the std:: namespace prefix");
        
        needNewSignature = true;
    }
    
    if(!typesNeedSharedPtr.empty())
    {
        listOnMessage(m_codeReview, "The following data types from the function signature contain custom types defined by the application accessed without shared_ptr",
                      typesNeedSharedPtr, defineStructMembersHint);
        
        needNewSignature = true;
        defineStructMembersHint = defineStructMembersHintRef;
    }
    
    if(!typesToQualifyEnums.empty())
    {
        std::string recommendation = "- If it is an application defined C++ struct it has to be accessed as a std::shared_ptr\n";
        recommendation += "- For application-defined C++ enums, use the naming convention specified in the 'NAMING CONVENTION' section.\n";
        recommendation += defineStructMembersHint;
        
        listOnMessage(m_codeReview, "The following types need qulification", typesToQualifyEnums, recommendation);
        
        needNewSignature = true;
        defineStructMembersHint = defineStructMembersHintRef;
    }
    
    if(needNewSignature)
    {
        return;
    }
    
    clang(m_prototype.declaration, CodeType::FUNC_DECL, [&, this](CXCursor c, CXCursor parent) {
        
        if (clang_getCursorKind(c) == CXCursor_FunctionDecl) {
            std::stringstream& review = m_codeReview;
            
            CXString returnType = clang_getTypeSpelling(clang_getCursorResultType(c));
            CXString functionName = clang_getCursorSpelling(c);
            std::string return_type = clang_getCString(returnType);
            std::string func_name = clang_getCString(functionName);
            std::string data_return_type = extractDataType(return_type);
            
            if(m_brief.func_name != func_name)
            {
                review << "The function name from the 'decalaration' field: " << func_name;
                review << " is different from the 'func_name' field: " << m_brief.func_name << std::endl << std::endl;
                return;
            }
            
            //Use parsed signature type. clang returned data_return_type is not reliable
            
            //Things are suspicious. Custom types, defined outside of the translation unit, are recognized as int
            //if(data_return_type == "int" && signature.returnType != "int")
            {
                if(m_prototype.m_signature.returnType.find("std::function<") != std::string::npos)
                {
                    review << "Unexpected return type '" << m_prototype.m_signature.returnType << "' of the function '" << func_name << "'" << std::endl;
                    review << "Remove or replace appropriately. The functions must not return std::function<> types" << std::endl;
                }
            }
            
            bool returnsAppType = proj->isAppType(m_prototype.m_signature.returnType);
            bool returnsVoid = m_prototype.m_signature.returnType == "void";
            bool constArgs = true;
            
            // Print parameters
            unsigned int numArgsInDecl = clang_Cursor_getNumArguments(c);
            
            for (unsigned i = 0; i < numArgsInDecl; i++) {
                CXCursor arg = clang_Cursor_getArgument(c, i);
                
                CXString argType = clang_getTypeSpelling(clang_getCursorType(arg));
                CXString argName = clang_getCursorSpelling(arg);
                std::string name = clang_getCString(argName);
                std::string type =  clang_getCString(argType);
                std::string dataType = extractDataType(type);
                
                constArgs = constArgs && !proj->isMutableType(m_prototype.m_signature.argumentTypes[i]);
                //Use parsed signature type. clang returned dataType is not reliable
                //if(dataType == "int" && signature.argumentTypes[i] != "int")
                {
                    if(m_prototype.m_signature.argumentTypes[i].find("std::function<") != std::string::npos)
                    {
                        review << "Unexpected argument type '" << m_prototype.m_signature.argumentTypes[i] << "' for argument '" << name;
                        review << "' of the function '" << func_name << "'" << std::endl;
                        review << "Remove or replace appropriately. The functions must not accept arguments of type std::function<>" << std::endl;
                    }
                }
                
                clang_disposeString(argType);
                clang_disposeString(argName);
            }
            
            //Very unlikely this function to has effect on the program state
            //if it doesn't have argumets and the return result is not an app defined type
            
            //Let's do a hard check since this could be very crucial for the data flow and won't hurt other things
            //No way something like std::shared_ptr<ASTNode> comp_ast_get_translation_unit(); to work
            
            //if(m_stats.m_noMutableAppDataReportsCount < 5)
            {
                bool showStateAccessMessage = false;
                if(
                   ((returnsVoid || !returnsAppType) && numArgsInDecl==0) ||
                   (returnsVoid && constArgs)
                   )
                {
                    review << "The function '" << func_name << "' will not have effect on the program state via its arguments and return type." << std::endl << std::endl;
                    showStateAccessMessage = true;
                }
                
                if(numArgsInDecl==0 && m_stats.m_noMutableAppDataReportsCount < 5)
                {
                    review << "The function '" << func_name << "' will not have access to the program state via its arguments." << std::endl << std::endl;
                    showStateAccessMessage = true;
                }
                
                if(showStateAccessMessage)
                {
                    std::string message = "In its implementation '" + func_name + "' must not access extern and global variables and data structures! ";
                    message += "To pass data to the function use function arguments, to output data from the function use the return result and the function arguments. ";
                    message += "All data, produced or updated by this function, that need to be accessed by other functions not called directly or indirectly by '" + func_name + "' must be exposed as mutable arguments or return result.";
                    
                    review << message;
                }
                
                m_stats.m_noMutableAppDataReportsCount++;
            }
        
            clang_disposeString(returnType);
            clang_disposeString(functionName);
        }
        return ;
    });
    
    updateExternals();
}

void CCodeNode::reflectData(const std::string& source, std::map<std::string, TypeDefinition>& metadata)
{
    metadata.clear();
    std::map<std::string, std::string> functions;
    extractFromSource(source, true, metadata, functions);
}

bool isValidTypeName(const std::string& name)
{
    // An empty string is not a valid struct name
    if (name.empty()) {
        return false;
    }
    // A valid struct name must start with an underscore or a letter
    if (std::isalpha(name[0], std::locale()) || name[0] == '_') {
    
        // Create a regular expression to match a valid struct name
        std::regex validNameRegex(R"([a-zA-Z_][a-zA-Z0-9_]*)");
    
        // Check if the name matches the regular expression
        return std::regex_match(name, validNameRegex);
    }
    return false;
}

void CCodeNode::extractFromSource(const std::string& source,
                                  bool extractLocalData,
                                  std::map<std::string, TypeDefinition>& dataTypes,
                                  std::map<std::string, std::string>& functions)
{
    clang(source, CodeType::EXTRACT, [&](CXCursor c, CXCursor parent) {

        CXCursorKind kind = clang_getCursorKind(c);
        std::stringstream& review = m_codeReview;
        
        if (kind == CXCursor_StructDecl && clang_isCursorDefinition(c)) {
            
            bool localStruct = isCursorInFunction(c);
            if (!localStruct || extractLocalData) {
                
                std::string structName = getCursorName(c);
                if(isValidTypeName(structName))
                {
                    dataTypes[structName].m_name = structName;
                    dataTypes[structName].m_type = TypeDefinition::STRUCT;
                    dataTypes[structName].m_definition = getCursorSourceCode(c);
                }
            }
            
        }
        else if (kind == CXCursor_EnumDecl && clang_isCursorDefinition(c)) {
            
            bool localStruct = isCursorInFunction(c);
            if (!localStruct || extractLocalData) {
                
                std::string enumName = getCursorName(c);
                if(isValidTypeName(enumName))
                {
                    dataTypes[enumName].m_name = enumName;
                    dataTypes[enumName].m_type = TypeDefinition::ENUM;
                    dataTypes[enumName].m_definition = getCursorSourceCode(c);
                }
            }
        }
        else if (kind == CXCursor_FieldDecl) {
            
            bool localStruct = isCursorInFunction(c);
            if (!localStruct || extractLocalData) {
                
                std::string fieldName = getCursorName(c);
                std::string structName = getCursorName(parent);
                std::string stdCursorSource = getCursorSourceCode(c);
                
                std::string type = extractType(stdCursorSource, fieldName);
                
                dataTypes[structName].m_members[fieldName] = type;
                if(hasCustomNamespaces(type).size() > 0)
                {
                    //TODO: Is this a precise hint, does it work?
                    
                    m_codeReview << "Type '" << type
                    << "' for member '" << fieldName
                    << "' in struct '" << structName
                    << "' has application-defined namespaces.\n"
                    << "Refactor the listed functions or data types "
                    << "to remove them from namespaces. "
                    << "Application-defined functions and data types "
                    << "should generally be defined in the global scope "
                    << "rather than within namespaces\n";
                }
            }
        }
        else if(kind == CXCursor_EnumConstantDecl)
        {
            bool localStruct = isCursorInFunction(c);
            if (!localStruct || extractLocalData) {
                
                std::string constantName = getCursorName(c);
                std::string enumName = getCursorName(parent);
                
                // Get the integer value of the enum constant
                long long constantValue = clang_getEnumConstantDeclValue(c);
                
                dataTypes[enumName].m_members[constantName] = std::to_string(constantValue);
            }
        }
        else if(kind == CXCursor_FunctionDecl && clang_isCursorDefinition(c))
        {
            std::string functionName = getCursorName(c);
            std::string functionSource = getCursorSourceCode(c);
            
            functions[functionName] = functionSource;
        }
    });
    
    //If review is required additional pass must be scheduled with review/reflect*() functions
    m_codeReview.str("");
    m_codeReview.clear();
}

std::set<std::string> CCodeNode::hasDestructiveChanges(const TypeDefinition& newDataDef, std::set<std::string>& referencedNodes)
{
    CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
    std::string existingDataPath;
    
    //auto existingData = proj->findData(newDataDef.m_name, existingDataPath);
    auto existingData = proj->findDataFromSnapshot(newDataDef.m_name, existingDataPath);
    
    std::set<std::string> missingMembers;
    //All data related to the referencedNodes must be visible in the LLM context.
    //If we have update for existing data want to avoid destructive changes like missing members
    if(existingData)
    {
        if(referencedNodes.find(existingDataPath) != referencedNodes.end())
        {
            //dataTypes[enumName].m_members[constantName] = std::to_string(constantValue);
            for(auto member : existingData->m_typeDef.m_members)
            {
                if(newDataDef.m_members.find(member.first) == newDataDef.m_members.end())
                {
                    missingMembers.insert(member.first);
                }
            }
        }
        else
        {
            //NOTE: It is fine to get here when fetching requests from cache
            //since referencedNodes will not have yet reference to the existing data path
            //this scope will update existingDataPath
            
            m_codeReview << "The following data type has been introduced: '" << newDataDef.m_name << "'" << std::endl;
            m_codeReview << "However, data type with the same name already exists in this code base:" << std::endl;
            m_codeReview << "***" << std::endl;
            m_codeReview << proj->declareData(false, existingDataPath);
            m_codeReview << proj->defineData(false, existingDataPath);
            m_codeReview << "***" << std::endl;
            //m_codeReview << "The above definitions are located under the following path: " << existingDataPath << std::endl;
            m_codeReview << "Please review the new and old data definitions for '" << newDataDef.m_name << "' ";
            m_codeReview << "Consider to update if necessary and reuse the already existing data definition ";
            m_codeReview << "without introducing destructive chances since it is already used by other parts of this codebase.";
            m_codeReview << "If reuse is not possible, introduce a new datatype but with a different name!\n\n";
            
            //The data path is already referenced in the context
            referencedNodes.insert(existingDataPath);
        }
    }
    
    return missingMembers;
}

bool CCodeNode::checkSTDTypes()
{
    CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
    std::set<std::string> stdTypes = getFullSTDTypesForFunction(m_prototype.m_signature);
    
    std::set<std::string> allAppTypes;
    for(auto def : stdTypes)
    {
        std::set<std::string> appTypes = proj->getAppTypes(def);
        allAppTypes.insert(appTypes.begin(), appTypes.end());
    }
    
    std::string appTypeDefs;
    for(auto appType : allAppTypes)
    {
        appTypeDefs += "struct ";
        appTypeDefs += appType;
        appTypeDefs += "{};\n";
    }
    
    if(!stdTypes.empty())
    {
        std::string testCode;
        
        for(auto inc : CCodeProject::getSTDIncludes())
        {
            testCode += "#include <" + inc + ">\n";
        }
        
        if(!appTypeDefs.empty())
        {
            testCode += "\n";
            testCode += appTypeDefs;
            testCode += "\n";
        }
        
        int idx = 0;
        testCode += "int main() {\n";
        for(auto def : stdTypes)
        {
            if(!def.empty() && def.back() == '&')
            {
                def.pop_back();
            }
            
            testCode += def + " test";
            testCode += std::to_string(idx) + ";\n";
            
            idx++;
        }
        testCode += "return 0;}\n";
        
        std::string errors = evaluateCodeForErrors(testCode);
        if(!errors.empty())
        {
            m_codeReview << "The clang has reported erros evaluating the following function declaration:" << std::endl;
            m_codeReview << m_prototype.declaration << std::endl;
            m_codeReview << "//***** Errors start *****" << std::endl;
            m_codeReview << errors << std::endl;
            m_codeReview << "//***** Errors end *****" << std::endl;
            m_codeReview << "The reason for the errors could be that some of the data types are prefixed by the std:: namespace";
            m_codeReview << " but actually are custom types defined by the application. ";
            m_codeReview << "Would you verify and if necessary revise the function declaration.";
            m_codeReview << " Custom types must be in the global namespace" << std::endl;
            
            return false;
        }
    }
    
    return true;
}

bool CCodeNode::checkForDependencies(const std::string& source,
                                     std::map<std::string, TypeDefinition>& dataDefinitions,
                                     const std::set<std::string>& referencedNodes)
{
    CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
    
    bool hasDependencyErrors = false;
    std::set<std::string> appDefinedTypes;
    for(const auto& def : dataDefinitions)
    {
        if(def.second.m_type == TypeDefinition::ENUM)
            continue;
        
        for(auto member : def.second.m_members)
        {
            appDefinedTypes.insert(def.second.m_name);
            std::set<std::string> newTypes = proj->getAppTypes(member.second);
            appDefinedTypes.insert(newTypes.begin(), newTypes.end());
        }
    }
    
    std::string appTypeDeclarations;
    for(auto appType : appDefinedTypes)
    {
        bool isEnum = false;
        auto it = dataDefinitions.find(appType);
        if(it != dataDefinitions.end())
        {
            isEnum = it->second.m_type == TypeDefinition::ENUM;
        }
        else
        {
            std::string existingAppType;
            auto existingAppData = proj->findData(appType, existingAppType);
            if(existingAppData)
            {
                isEnum = existingAppData->m_typeDef.m_type == TypeDefinition::ENUM;
            }
            else
            {
                //WE MUST NOT BE HERE!!!
                m_codeReview << "Unknown application defined type: " << appType << std::endl;
            }
        }
        
        if(isEnum)
        {
            appTypeDeclarations += "enum class " + appType + " : uint32_t;\n";
        }
        else
        {
            appTypeDeclarations += "struct " + appType + ";\n";
        }
    }
    
    if(!appTypeDeclarations.empty())
    {
        std::string testCode;
        
        for(auto inc : CCodeProject::getSTDIncludes())
        {
            testCode += "#include <" + inc + ">\n";
        }
        
        testCode += appTypeDeclarations;
        testCode += "\n";
        
        for(const auto& def : dataDefinitions)
        {
            std::string testCodePerDef = testCode;
            
            testCodePerDef += def.second.m_definition + ";\n";
            
            //We must be able to construc object from this data type
            //with only forward declarations of the app-defined types
            
            testCodePerDef += "int main() {\n";
            //for(const auto& def : dataDefinitions)
            {
                testCodePerDef += def.first + " test";
                testCodePerDef += def.first + ";\n";
            }
            testCodePerDef += "return 0;}\n";
            
            std::string errors = evaluateCodeForErrors(testCodePerDef);
            if(!errors.empty())
            {
                m_codeReview << "The clang has reported errors with the provided data type definition:" << std::endl;
                m_codeReview <<  def.second.m_name << std::endl;
                m_codeReview <<  "Here are the errors:" << std::endl;
                m_codeReview << errors << std::endl;
                
                hasDependencyErrors = true;
            }
        }
        
        if(hasDependencyErrors)
        {
            m_codeReview << "Review the above errors if they are caused by missing full definitions or nested data definitions. ";
            m_codeReview << "For how to avoid data and headers cyclic dependencies for structure members from custom application-defined data types, refere to the section 'REQUIREMENTS FOR CUSTOM DATA TYPES'\n";
            //m_codeReview << "must be defined as follows:" << std::endl;
            //m_codeReview << proj->define_struct_members.prompt() << std::endl;
        }
    }
    
    return hasDependencyErrors;
}

void CCodeNode::reviewData(const std::string& source, const std::string& typeName,
                           std::map<std::string, TypeDefinition>& dataDefinitions,
                           std::set<std::string>& referencedNodes)
{
    CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
    
    if(source.length() < 7)
    {
        m_codeReview << "The provided code snippet:";
        m_codeReview << std::endl << "//***" << source << "//***" << std::endl;
        m_codeReview << "Doesn't represent a valid C++ data type definitions" << std::endl;
        return;
    }
    
    reflectData(source, dataDefinitions);
    
    clang(source, CodeType::DATA_DEF, [&](CXCursor c, CXCursor parent) {
        //TODO: Parse data member here
        //return CXChildVisit_Recurse;
    });
    
    std::string defineStructMembersHintRef = "For how to use data types defined by the applicatoin in C++ functions, stucts and enums ";
    defineStructMembersHintRef += "refere to the section 'REQUIREMENTS FOR CUSTOM DATA TYPES'\n";
    
    //It is actually already in the context, just refer to it!
    std::string defineStructMembersHint = defineStructMembersHintRef;
    
    //Must be here, after the clang() call since the call to clang will reset the m_codeReview
    bool hasDependencyErrors = checkForDependencies(source, dataDefinitions, referencedNodes);
    if(hasDependencyErrors) {
        defineStructMembersHint = defineStructMembersHintRef;
    }
    
    bool dataTypeFound = false;
    
    std::set<std::string> appDefinedTypes;
    std::set<std::string> typesWithInvalidCharacters;
    std::set<std::string> defWithDestructiveChanges;
    std::set<std::string> defWithoutDestructiveChanges;
    std::set<std::string> stdTypesWithPointers;
    std::set<std::string> typesWithMorePointers;
    std::set<std::string> constAppTypes;
    std::set<std::string> typesWithSmartPointers;
    std::set<std::string> appTypesWithoutSharedPointers;
    std::set<std::string> typesWithCustomNamespaces;
    std::set<std::string> appTypesInConflict;
    std::set<std::string> typesToQualifyEnums;
    std::set<std::string> restrictedSTDTypes;
    
    for(const auto& def : dataDefinitions)
    {
        auto missingMembers = hasDestructiveChanges(def.second, referencedNodes);
        if(!missingMembers.empty())
        {
            std::string defWithMissingMembers = def.first;
            defWithMissingMembers += " (missing members: ";
            
            uint32_t i=0;
            uint32_t missingMembersCount = (uint32_t)missingMembers.size();
            for(auto member : missingMembers)
            {
                defWithMissingMembers += member;
                
                if(++i >= 5 || i == missingMembersCount)
                    break;
                
                defWithMissingMembers += ", ";
            }
            
            if(missingMembersCount > 5)
                defWithMissingMembers += ", other ...";
            
            defWithMissingMembers += ")\n";
            
            defWithDestructiveChanges.insert(defWithMissingMembers);
        }
        else
        {
            defWithoutDestructiveChanges.insert(def.first);
        }
        
        //No need to enumerate members of enums
        if(def.second.m_type != TypeDefinition::ENUM)
        {
            //Verify all dependend data types are already defined
            for(auto& member : def.second.m_members)
            {
                auto returnSTDTypes = proj->hasRestrictedStdTypes(member.second);
                restrictedSTDTypes.insert(returnSTDTypes.begin(), returnSTDTypes.end());
                
                if(!isValidCppType(member.second))
                {
                    typesWithInvalidCharacters.insert(member.second);
                }
                
                auto customNS = hasCustomNamespaces(member.second);
                if(!customNS.empty())
                {
                    std::string typeWithCustomNS = def.first + "::" + member.second + " " + member.first + ";";
                    typesWithCustomNamespaces.insert(typeWithCustomNS);
                }
                
                std::set<std::string> memberTypes = proj->getAppTypes(member.second);
                if(!memberTypes.empty())
                {
                    if(isConstType(member.second))
                    {
                        std::string constTypeName = member.second + " " + def.second.m_name + "::" + member.first;
                        constAppTypes.insert(constTypeName);
                    }
                    
                    appDefinedTypes.insert(memberTypes.begin(), memberTypes.end());
                    
                    if(findSharedPointersInType(member.second) > 1)
                    {
                        typesWithMorePointers.insert(member.second);
                    }
                    
                    if(hasSharedPtrToListedStdType(member.second, CCodeProject::getStdContainers()))
                    {
                        std::string stdTypeWithPointer = member.second + " ";
                        stdTypeWithPointer += member.first + ";";
                        
                        stdTypesWithPointers.insert(stdTypeWithPointer);
                    }
                    
                    for(auto appTypeInMemeber : memberTypes)
                    {
                        std::string memberDeclSpec = "struct: " + def.first + ", member: " + member.second + " " + member.first + ";";
                        
                        checkAppTypeQualification(appTypeInMemeber, member.second, memberDeclSpec,
                                                  appTypesInConflict, appTypesWithoutSharedPointers,
                                                  typesToQualifyEnums);
                    }
                }
                else
                {
                    if(findSharedPointersInType(member.second) > 0)
                    {
                        std::string stdTypeWithPointer = member.second + " ";
                        stdTypeWithPointer += member.first + ";";
                        
                        stdTypesWithPointers.insert(stdTypeWithPointer);
                    }
                }
                
                if(member.second.find("unique_ptr") != std::string::npos ||
                   member.second.find("weak_ptr") != std::string::npos)
                {
                    std::string typeWithSmartPtr = member.second + " " + def.second.m_name + "::" + member.first;
                    typesWithSmartPointers.insert(typeWithSmartPtr);
                }
            }
        }
        else //if(def.second.m_type == TypeDefinition::ENUM)
        {
            if(!isValidEnumTypeName(def.second.m_name))
            {
                std::string owningPath;
                auto enumDef = proj->findData(def.second.m_name , owningPath);
                if(!enumDef)
                {
                    m_codeReview << "The definition of enum type '" << def.second.m_name;
                    m_codeReview << "' doesn't follow project convention for how to name enums!" << std::endl;
                    m_codeReview << "Refer to the 'NAMING CONVENTION' and 'REQUIREMENTS FOR CUSTOM DATA TYPES' ";
                    m_codeReview << "sections for information how to name C++ enums" << std::endl;
                }
            }
        }
        
        std::string testEnumTypeName = "E" + typeName;
        if(
           def.second.m_name == typeName ||
           (def.second.m_type == TypeDefinition::ENUM && def.second.m_name == testEnumTypeName)
           )
        {
            dataTypeFound = true;
        }
    }
    
    std::set<std::string> missingTypes;
    for(const auto& appType : appDefinedTypes)
    {
        if(dataDefinitions.find(appType) == dataDefinitions.end())
        {
            std::string path;
            if(!proj->findData(appType, path))
            {
                missingTypes.insert(appType);
            }
        }
    }
    
#ifdef RESTRICT_STL_TYPES
    if(!restrictedSTDTypes.empty())
    {
        listOnMessage(m_codeReview,
            "Struct members cannot use the following STL types:",
            restrictedSTDTypes,
            "See 'AVAILABLE LIBRARIES' for permitted STL types in struct members"
        );
    }
#endif //RESTRICT_STL_TYPES
    
    if(!constAppTypes.empty())
    {
        listOnMessage(m_codeReview, "The following data types defined by the application appear to have constant qualifiers",
                      constAppTypes, "Don't declare pointers and references to structs defined by the application as constant. Refactor the struct definitions in a way all members from data types defined by the application are non-constant qualified");
    }
    
    if(!stdTypesWithPointers.empty())
    {
        std::string recommendation = "No need to use std smart pointers for these types, you can use standard C++/STD types directly as a members of a structure or in std data containers.";
        
        listOnMessage(m_codeReview, "The following data types appear as standard C++ or standard library types but have STD smart pointers ",
                      stdTypesWithPointers,
                      recommendation);
    }
    
    if(!typesWithMorePointers.empty() && m_stats.m_containerWithManyPointersReportsCount == 0)
    {
        std::string recommendation = "Please refer to the following instructions for how to use smart pointers for data types defined by the application\n";
        recommendation += defineStructMembersHint;
        defineStructMembersHint = defineStructMembersHintRef;
        
        listOnMessage(m_codeReview, "The following data types appear to have more than one std smart pointer ",
                      typesWithMorePointers,
                      recommendation);
        m_stats.m_containerWithManyPointersReportsCount++;
    }
    
    if(!typesWithSmartPointers.empty())
    {
        std::string recommendation = "Don't use std::weak_ptr and std::unique_ptr. ";
        recommendation += "For structure members from types defined by the application refer to ";
        recommendation += "'Requirements for data types defined by the application' to check when to use std::shared_ptr. ";
        recommendation += "For data types from the Standard Library, consider to use them directly as values as structure members and in STD containers";
        
        listOnMessage(m_codeReview, "The following data types appear to have std::weak_ptr and/or std::unique_ptr ",
                      typesWithSmartPointers, recommendation);
    }
    
    if(!appTypesInConflict.empty())
    {
        listOnMessage(m_codeReview, "The following types are in conflict with some of the included libraries: ", appTypesInConflict,
                      "For each of the listed types consider the following:\n- If it's a custom type defined in this application, consider a different name.\n- If it's from the C++ standard library, it must include the std:: namespace prefix");
        
    }
    
    if(!dataTypeFound)
    {
        m_codeReview << std::endl;
        m_codeReview << "Returned source doesn't provide definition of the " << typeName << " data type";
        m_codeReview << std::endl;
        m_codeReview << "In your response you must include definition of the " << typeName << " data type";
        m_codeReview << std::endl;
    }
    
    if(!defWithDestructiveChanges.empty())
    {
        std::string recommendation = "If you are updating data types merge your changes with the existing definition. ";
        recommendation += "Provide full definiton for modified structs and enums. ";
        recommendation += "In your response, include all new and updated types, not only revisited types with destructive changes. ";
        recommendation += "As a reminder, from your current response, the follwoing data types are new or updated without destructive changes: \n";
        for(auto def : defWithoutDestructiveChanges)
        {
            recommendation += def;
            recommendation += " ";
        }
        recommendation += "\n";
        recommendation += "Always provide full definition for " + typeName;
        
        listOnMessage(m_codeReview, "The following data types have missing members which were presented in original definition, this is a potential destructive change",
                      defWithDestructiveChanges,
                      recommendation);
    }
    
    if(!missingTypes.empty())
    {
        std::string recommendation = "All types that '";
        recommendation += typeName;
        recommendation += "' depends on must be defined! If it is still unclear what should be the members of any of these types, ";
        recommendation += "leaving empty data definition is fine. Provide only the data type definitions, ";
        recommendation += "don't provide forward declarations, they will be added later";
        listOnMessage(m_codeReview, "I couldn't find the following data types defined in the appication", missingTypes, recommendation);
    }
    
    if(!appTypesWithoutSharedPointers.empty())
    {
        std::string recommendation = "Please refer to the 'Requirements for data types defined by the application' for how to use smart pointers for data types defined by the application\n";
        
        listOnMessage(m_codeReview, "The following custom types defined by the application are used without smart pointers", appTypesWithoutSharedPointers, recommendation);
    }
    
    if(!typesWithCustomNamespaces.empty())
    {
        std::string recommendation = "Consider different data type from allowed namespaces. Namespaces allowed in this project are: ";
        bool first = true;
        for (auto const& ns : proj->getLibNamespaces()) {
            recommendation += (first ? "" : ", ");
            recommendation += ns;
            first = false;
        }
        
        listOnMessage(m_codeReview, "The following types have namespaces that aren't allowed. ", typesWithCustomNamespaces, recommendation);
    }
    
    if(!typesWithInvalidCharacters.empty())
    {
        listOnMessage(m_codeReview, "The following C++ types have invalid caracters", typesWithInvalidCharacters,
                      "In types use only characters valid for C++ types");
    }
    
    if(!typesToQualifyEnums.empty())
    {
        std::string recommendation = "- If it is an application defined C++ struct it has to be accessed as a std::shared_ptr\n";
        recommendation += "- For application-defined C++ enums, use the naming convention specified in the 'NAMING CONVENTION' section. When defining enums as struct members, follow the guidelines in the 'REQUIREMENTS FOR CUSTOM DATA TYPES' section\n";
        
        listOnMessage(m_codeReview, "The following types need qulification: ", typesToQualifyEnums, recommendation);
    }
}

void CCodeNode::reviewSignatureChange(const std::string& source, CodeType srcType, bool reportDestructiveChanges)
{
    std::string decl = extractFunctionDeclaration(source);
    ParsedFunction signature = parseFunctionSignature(decl);
    
    std::string declToCmp = removeWhitespace(signature.str());
    std::string oldDeclToCmp = removeWhitespace(m_prototype.m_signature.str());
    
    
    bool theSameFunction = signature.functionName == m_prototype.m_signature.functionName;
    
    if(theSameFunction && declToCmp != oldDeclToCmp)
    {
        if(!evaluateNewDeclaration(decl))
        {
            //There are issues with the new declaration
            return;
        }
        
        //At this point we already have review of the new declartion performed by evaluateNewDeclaration
        //No need to detect destructive changes if not explicitly requested
        if(!reportDestructiveChanges)
        {
            return;
        }
        
        // Check if this function is referenced by any descendant functions of its parent in the call graph
        std::string path = getDAGPath("/");
        std::string parentPath = popFromPath(path, 1);
        
        bool isDestructive = false;
        
        uint32_t refsCount = 0;
        for(auto ref : m_referencedBy)
        {
            std::string refPath = ref->getDAGPath("/");
            
            //if(boost::starts_with(refPath, parentPath) &&
            //   refPath.length() > parentPath.length())//Check everything under the same parent
            {
                const CCodeNode* refNode = (const CCodeNode*)ref;
                //If the reference is already compiled this will introduce destructive changes
                
                if((srcType == CodeType::FUNC_CMPL ||
                    srcType == CodeType::FUNC_FIX) &&
                   refNode->objectExists() && refNode->m_name != m_name)
                {
                    //We have object already compile referencing this function with the old signature
                    //isDestructive = true;
                    refsCount++;
                    break;
                }
                //TODO: Need to add other checks for srcType == CodeType::* if needed
            }
        }
        
        //TODO: Consider some thrade off here. For example, to be desrcuctive only when refsCount > 1
        isDestructive = refsCount > 0;
        
        if(isDestructive)
        {
            if(srcType != CodeType::FUNC_FIX)
            {
                m_codeReview << "Identified change in the function declaration from: " << m_prototype.declaration << std::endl;
                m_codeReview << "to: " << decl << std::endl;
                m_codeReview << "There are other functions already compiled referencing the old declaration. ";
                m_codeReview << "Consider to update the implementation and find a solution without changing the function signature.";
                if(!theSameFunction)
                {
                    //signature.functionName != m_prototype.m_signature.functionName
                    m_codeReview << " Note that only the function '" << m_prototype.m_signature.functionName << "'";
                    m_codeReview << " is expected in the updated code snippet. Addin a new '" << signature.functionName;
                    m_codeReview << "' function might confuse the signature check!";
                }
                m_codeReview << std::endl;
            }
            else //TODO: if(m_references.size() > 1)//In case we debug and fix functions (srcType == CodeType::FUNC_FIX) we can afford to break one that calls this
            {
                m_codeReview << "Identified change in the function declaration from: " << m_prototype.declaration << std::endl;
                m_codeReview << "to: " << decl << std::endl;
                m_codeReview << "There are other functions already compiled referencing the old declaration. ";
                m_codeReview << "Consider to update the implementation and find a solution without changing the function signature.";
                if(!theSameFunction)
                {
                    //signature.functionName != m_prototype.m_signature.functionName
                    m_codeReview << " Note that only the function '" << m_prototype.m_signature.functionName << "'";
                    m_codeReview << " is expected in the updated code snippet. Addin a new '" << signature.functionName;
                    m_codeReview << "' function might confuse the signature check!";
                }
                m_codeReview << std::endl;
                
                //TODO: Somehow this needs to go to the debug notes to feedback the LLM in the Debugger
                std::stringstream ssFeedback;
                
                ssFeedback << "As a result form the last 'fix_function' action, an attempt has been identifide to change the function declaration from: " << m_prototype.declaration << std::endl;
                ssFeedback << "to: " << decl << std::endl;
                ssFeedback << "There are other functions already, compiled referencing the old declaration so it can't be changed. ";
                ssFeedback << std::endl;
                ssFeedback << "If still required for the fix (suggested by 'fix_function' action), instead of modifying the signature of '" + m_brief.func_name + "', ";
                ssFeedback << "consider using the 'fix_function' action on the function(s) that need to call a function with the new signature. ";
                ssFeedback << "In their implementation, introduce a new, not-yet-existing function (let's call it a wrapper) ";
                ssFeedback << "by calling it directly with the appropriate arguments. ";
                ssFeedback << "Then, we can deduce the wrapper's signature from its usage and implement it. ";
                ssFeedback << "Ideally, the wrapper implementation should reuse functionality by calling '" + m_brief.func_name;
                ssFeedback << "' or other existing functions from this software";
                ssFeedback << std::endl;
                
                Debugger::getInstance().feedback(ssFeedback.str());
            }
        }
    }
}

void CCodeNode::reviewImplementation(const std::string& source, CodeType srcType)
{
    if(source.length() < 7)
    {
        m_codeReview << "The provided code snippet:";
        m_codeReview << std::endl << "//***" << source << "//***" << std::endl;
        m_codeReview << "Doesn't represent a valid C++ function implementation" << std::endl;
        return;
    }
    
    clang(source, srcType, [](CXCursor c, CXCursor parent) {
        
        //return CXChildVisit_Recurse;
    });
    
    if(srcType == CodeType::FUNC_CMPL ||
       srcType == CodeType::FUNC_IMPL ||
       srcType == CodeType::FUNC_FIX)
    {
        std::string decl = extractFunctionDeclaration(source);
        ParsedFunction signature = parseFunctionSignature(decl);
        
        if(!signature.isValid())
        {
            m_codeReview << "Expected valid function declaration before the first '{' character in the provided source." << std::endl;
            m_codeReview << "Found: " << decl << std::endl << std::endl;
        }
        
        if(signature.functionName.find("::") != std::string::npos)
        {
            m_codeReview << "Don't prefix the function name with a namespace. ";
            m_codeReview << "All functions defined in this project must be in the global namespace" << std::endl;
        }
        
        //This number might require adjustments
        if(m_stats.m_fewCStatements == 0 && countCStatements(source) < 7)
        {
            m_codeReview << "Function " << m_brief.func_name << " has very few C statements. ";
            m_codeReview << "Ensure this is intended and the function implements the subset of the project features it is responsible for.";
            m_codeReview << std::endl;
            m_stats.m_fewCStatements++;
        }
        
        if(decl.find("=") != std::string::npos)
        {
            m_codeReview << "In the function declaration, don't use arguments initialized with default values. ";
            m_codeReview << std::endl;
        }
        
        if(signature.functionName != m_brief.func_name)
        {
            std::string message = "Function name in the function definition: '" + signature.functionName;
            message += "' doesn't match already selected name for the function: '" + m_brief.func_name + "'\n";
            message += "Function name in the definition must be changed to '" + m_brief.func_name + "'\n";
            
            {
                //signature.functionName != m_prototype.m_signature.functionName
                m_codeReview << " Note that only the function '" << m_prototype.m_signature.functionName << "'";
                m_codeReview << " must be implemented in the updated code snippet.";
                m_codeReview << " More than one function definitions/declarations will confuse the function name check!";
            }
            
            m_codeReview << message << std::endl;
        }
        
        //TODO: Consider to add check for C Statements exceeding a certain limit
    }
    
    //For now destructive changes on the signature will be signaled only during the compilation and debugging
    bool reportDestructiveChanges = srcType == CodeType::FUNC_CMPL || srcType == CodeType::FUNC_FIX;
    reviewSignatureChange(source, srcType, reportDestructiveChanges);
}

}
