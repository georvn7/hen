#pragma once

#include <clang-c/Index.h>
#include <regex>
#include <unordered_set>

struct ParsedFunction {
    std::string returnType;
    std::string functionName;
    std::vector<std::string> argumentTypes;
    
    bool isValid();
    std::string str();
};

std::string printDiagnostics(CXTranslationUnit unit, bool verbose);
std::string printErrors(CXTranslationUnit unit, bool verbose); //clang style
std::string getCursorFile(CXCursor cursor);
std::string getFullLineForCursor(CXCursor cursor, const std::string& sourceContent, const std::string& targetFileName);
bool isUint32Enum(CXCursor cursor);

std::set<std::string> getSTDTypes(const std::string& dataType);
std::set<std::string> getSTDTypesFromDecl(const std::string& decl);
std::set<std::string> getSTDFullTypesFromDecl(const std::string& decl);

bool isConstType(const std::string& dataType);
bool isContainerWithAppValues(const std::string& dataType);
bool isIteratorWithAppValues(const std::string& dataType);
std::vector<std::pair<uint32_t, std::string>> findAllOccurrences(const std::string& str, const std::string& regexPattern);
uint32_t findSharedPointersInType(const std::string& dataType);
bool hasSharedPtrToStdNamespace(const std::string& typeStr);
bool hasSharedPtrToListedStdType(const std::string& typeStr,
                                 const std::unordered_set<std::string>& stlTypesNoStd,
                                 std::string* matchedType = nullptr);

std::set<std::string> getFullSTDTypesForFunction(const ParsedFunction& signature);
    
std::vector<std::string> splitArguments(const std::string& args);
bool isSimpleFunctionDeclaration(const std::string& signature);
ParsedFunction parseFunctionSignature(const std::string& signature);
std::string extractFunctionDeclaration(const std::string& input);
std::vector<size_t> findAutoKeyword(const std::string& sourceCode);
std::vector<size_t> findFunctionCalls(const std::string& sourceCode, const std::string& functionName);
std::string evaluateCodeForErrors(const std::string& sourceCode, bool print=false);
std::set<std::string> collectCppIdentifiers(const std::string& cacheFilePath, const std::string& prologueCode);
bool isTemplated(CXCursor cursor);
CXCursor isNestedCallWithinTemplatedCall(CXCursor cursor);
bool isFunctionCallWithTemplates(CXCursor cursor);
bool hasBaseClass(CXCursor cursor);
std::string getGenericStubFunction();
std::string getStubFunction(const std::string& functionName);
std::string getLineText(CXTranslationUnit unit, CXFile file, unsigned int line);
bool isAssignedFunctionType(CXCursor cursor);
std::string popFromPath(const std::string& path, uint32_t count);

std::set<std::string> analyzeForUnmatchedFunctions(const std::string& cmplOutput);
std::set<std::string> analyzeForProblematicTypes(const std::string& cmplOutput);

std::set<std::string> findPatternsInComments(const std::string& code, const std::vector<std::regex>& patterns);

inline bool isExternalVariable(CXCursor cursor) {
    CX_StorageClass storageClass = clang_Cursor_getStorageClass(cursor);
    return storageClass == CX_SC_Extern;
}

void analyzeFunctionForEffect(CXCursor cursor, std::set<std::string>& noEffectFunctions);
std::string getClangString(CXString s);
std::string getCursorName(CXCursor cursor);
std::string getCursorKind(CXCursor cursor);
std::string getCursorType(CXCursor cursor);

std::string getSourceForRange(CXTranslationUnit tu, CXSourceRange range);
std::string getCursorSource(CXCursor cursor);
std::string getClangString(CXString name);
std::string getCursorLocation(CXCursor cursor);
std::string extractFunctionName(const std::string& callExpr);

bool isTypePrefixedByNamespace(const std::string& typeStr, const std::string& myType);
std::vector<std::string> findNonSharedPtrUsages(const std::string& typeString, const std::string& typeName);

std::set<std::string> hasCustomNamespaces(const std::string& identifier);
std::set<std::string> functionHasCustomNamespaces(const std::string& declOrDef);
std::vector<std::string> formatCppNamespaces(const std::string& identifier);

std::string findSubstringBefore(const std::string& input, const std::string& delimiter);
std::string findSubstringAfter(const std::string& input, const std::string& delimiter);

std::vector<std::string> splitByToken(const std::string& input, const std::string& delimiter);
std::vector<std::string> splitCTypeByNamespace(const std::string& input, const std::string& delimiter);
std::string replaceCTypeSpecialChars(const std::string& input);

std::string cleanClangOutput(const std::string& clangOutput, const std::string& projectDirectory,
                             const std::set<std::string>& projectIdentifiers, bool onlyErrors);

std::string clangVersion();
std::string getAbstractCode(const std::string& cppCode);
bool isValidCppType(const std::string& type);
std::vector<std::string> splitDataType(const std::string& dataType);
std::set<std::string> isSupportedType(const std::string& dataType);
std::pair<bool, std::string> isInvalidIterator(
    const std::string& type,
    const std::string& appType,
                                               const std::unordered_set<std::string>& containerTypes);
std::pair<bool, std::string> isInvalidContainer(
    const std::string& type,
    const std::string& appType,
                                                const std::unordered_set<std::string>& containerTypes);
const std::set<std::string>& getCppBaseTypes();
const std::unordered_set<std::string>& getCppQualifiers();
bool isValidEnumTypeName(const std::string& typeName);

bool appFunctionHasInitializerList(CXCursor c);

bool isAllLowercase(const std::string& ident);
bool isAllUppercase(const std::string& ident);
std::set<std::string> extractUnknownTypes(const std::string& diagnostics);
std::string filterAnyReturnAmbiguityErrors(const std::string& diagnostics);
std::string filterAnyReturnErrors(const std::string& diagnostics);
std::string emptyAllStringLiterals(const std::string& code);
bool isCursorInFunction(CXCursor cursor);

std::string normalizeWhitespace(const std::string& str);

//For C++ functions signature analysis
bool isAlphaNumericWithColonsUnderscore(const std::string& str);
bool hasBalancedAngleBrackets(const std::string& s);
bool hasForbiddenCharacters(const std::string& s);
bool validateFunctionName(const std::string& functionName);
bool validateReturnType(const std::string& returnType);
bool validateArgumentType(const std::string& argType);

std::string checkArrayOfPointers(const std::string& typeStr);

int getFirstColumn(const std::string& filePath, int line);
int getFirstCharacterOffset(const std::string& text, int atLine);
// Returns the declaration string (prototype) for a function cursor (decl or def).

bool isTemplatedFunction(const std::string& decl);

//Extracts Doxygen single line brief function description from source
std::string extractBrief(std::string& source);

class SyntaxErrorAnalyzer
{
protected:
    std::regex m_pattern;
    std::string m_prompt;
    std::vector<std::string> m_substitutes;
    virtual std::string onError(const std::string& code, const std::smatch& match) const;
    
public:
    SyntaxErrorAnalyzer(const std::regex& pattern,
                        const std::string& prompt,
                        const std::vector<std::string>& substitutes);
    
    virtual std::string analyze(const std::string& code, const std::string& compilerOutput) const;
};

class Analyzer_StaticCastUnrelatedTypes : public SyntaxErrorAnalyzer
{
public:
    static bool m_enabled;
    Analyzer_StaticCastUnrelatedTypes();
};


class Analyzer_DefineUnknownType : public SyntaxErrorAnalyzer
{
public:
    static bool m_enabled;
    static bool m_hintToCreate;
    std::string onError(const std::string& code, const std::smatch& match) const;
    
    Analyzer_DefineUnknownType();
};
