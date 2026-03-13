#include <iostream>
#include <vector>
#include <set>
#include <unordered_set>
#include <sstream>
#include <regex>
#include "IncludeBoost.h"

#include "UtilsCodeAnalysis.h"
#include "Utils.h"

bool ParsedFunction::isValid()
{
    // 1. Function name check
    if (!validateFunctionName(functionName)) {
        return false;
    }

    // 2. Return type check
    if (!validateReturnType(returnType)) {
        return false;
    }

    // 3. Each argument check
    for (const auto& argType : argumentTypes) {
        if (!validateArgumentType(argType)) {
            return false;
        }
    }

    // If we get here, we've passed all checks
    return true;
}

std::string ParsedFunction::str()
{
    std::string signature = returnType + functionName;
    
    signature += "(";
    
    for(auto arg : argumentTypes)
    {
        signature += arg;
    }
    
    signature += ");";
    
    return signature;
}

std::string printInclusionStack(CXTranslationUnit TU, CXFile file) {
    struct InclusionVisitor {
        CXFile target;
        std::vector<CXSourceLocation> stack;
        static void visit(CXFile included_file, CXSourceLocation* inclusion_stack, unsigned include_len, CXClientData client_data) {
            auto* self = static_cast<InclusionVisitor*>(client_data);
            if (clang_File_isEqual(included_file, self->target)) {
                self->stack.assign(inclusion_stack, inclusion_stack + include_len);
            }
        }
    } visitor{file};

    clang_getInclusions(TU, &InclusionVisitor::visit, &visitor);

    std::stringstream sout;
    sout << "Inclusion stack:" << std::endl;
    for (auto it = visitor.stack.rbegin(); it != visitor.stack.rend(); ++it) {
        CXFile file;
        unsigned line, column, offset;
        clang_getExpansionLocation(*it, &file, &line, &column, &offset);
        CXString fileName = clang_getFileName(file);
        sout << "  " << clang_getCString(fileName) << ":" << line << ":" << column << std::endl;
        clang_disposeString(fileName);
    }
    
    return sout.str();
}

std::string getLineText(CXTranslationUnit unit, CXFile file, unsigned int line)
{
    CXString fileName = clang_getFileName(file);
        
    std::string stdLineTex;
    // Get file contents
    size_t fileLength;
    const char* fileContents = clang_getFileContents(unit, file, &fileLength);
    if (fileContents) {
        // Find the start of the line
        const char* lineStart = fileContents;
        for (unsigned int i = 1; i < line && *lineStart; ++i) {
            lineStart = strchr(lineStart, '\n');
            if (lineStart) ++lineStart;
        }
        
        // Find the end of the line
        const char* lineEnd = lineStart ? strchr(lineStart, '\n') : nullptr;
        
        if (lineStart && lineEnd) {
            stdLineTex = std::string(lineStart, lineEnd - lineStart);
        }
    }
    
    clang_disposeString(fileName);
    return stdLineTex;
}

std::string printDiagnostics(CXTranslationUnit unit, bool verbose)
{
    std::stringstream sout;
    uint32_t numDiagnostics = clang_getNumDiagnostics(unit);
    uint32_t numErrors = 0;
    for (uint32_t i = 0; i < numDiagnostics; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnostic(unit, i);
        
        // Get diagnostic severity
        CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        const char* severityStr;
        switch (severity) {
            case CXDiagnostic_Ignored: severityStr = "Ignored"; break;
            case CXDiagnostic_Note: severityStr = "Note"; break;
            case CXDiagnostic_Warning: severityStr = "Warning"; break;
            case CXDiagnostic_Error: severityStr = "Error"; break;
            case CXDiagnostic_Fatal: severityStr = "Fatal"; break;
            default: severityStr = "Unknown"; break;
        }
        
        if(!verbose && severity != CXDiagnostic_Error && severity != CXDiagnostic_Fatal)
            continue;
        
        numErrors++;

        // Get diagnostic location
        CXSourceLocation location = clang_getDiagnosticLocation(diagnostic);
        CXFile file;
        unsigned int line, column, offset;
        clang_getExpansionLocation(location, &file, &line, &column, &offset);
        
        if(file)
        {
            CXString fileName = clang_getFileName(file);
            
            // Get diagnostic message
            CXString message = clang_getDiagnosticSpelling(diagnostic);
            
            // Get diagnostic category
            CXString category = clang_getDiagnosticCategoryText(diagnostic);
            
            // Print all the information
            //if(verbose) std::cout << "Diagnostic [" << severityStr << "]:" << std::endl;
            if(verbose) sout << "File: " << clang_getCString(fileName) << std::endl;
            
            if(verbose) 
            {
                sout << printInclusionStack(unit, file);
            }
            
            sout << "Location: Line " << line << ", Column " << column << std::endl;
            std::string lineSource = getLineText(unit, file, line);
            if(!lineSource.empty())
            {
                sout << "Source line: " << lineSource << std::endl;
            }
            
            if(verbose) sout << "Category: " << clang_getCString(category) << std::endl;
            sout << severityStr << ": " << clang_getCString(message) << std::endl;
            
            clang_disposeString(fileName);
            clang_disposeString(message);
            clang_disposeString(category);
        }
        
        if(verbose)
        {
            // Get and print source ranges
            unsigned int numRanges = clang_getDiagnosticNumRanges(diagnostic);
            for (unsigned int j = 0; j < numRanges; ++j) {
                CXSourceRange range = clang_getDiagnosticRange(diagnostic, j);
                CXSourceLocation start = clang_getRangeStart(range);
                CXSourceLocation end = clang_getRangeEnd(range);
                
                unsigned int startLine, startColumn, startOffset;
                unsigned int endLine, endColumn, endOffset;
                CXFile startFile, endFile;
                clang_getExpansionLocation(start, &startFile, &startLine, &startColumn, &startOffset);
                clang_getExpansionLocation(end, &endFile, &endLine, &endColumn, &endOffset);
                
                sout << "  Range " << j + 1 << ": ";
                //sout << "Start(Line " << startLine << ", Col " << startColumn << ") - ";
                //sout << "End(Line " << endLine << ", Col " << endColumn << ")" << std::endl;
                sout << "Start line: " << getLineText(unit, startFile, startLine) << std::endl;
                sout << "End line: " << getLineText(unit, startFile, startLine) << std::endl;
            }
        }
        
        //Do not confuse the LLM with this information for now!
        if(false)//if(verbose)
        {
            // Get and print fix-it hints
            unsigned int numFixIts = clang_getDiagnosticNumFixIts(diagnostic);
            for (unsigned int j = 0; j < numFixIts; ++j) {
                CXSourceRange fixitRange;
                CXString replacement = clang_getDiagnosticFixIt(diagnostic, j, &fixitRange);
                
                CXSourceLocation fixitStart = clang_getRangeStart(fixitRange);
                CXSourceLocation fixitEnd = clang_getRangeEnd(fixitRange);
                
                unsigned int fixitStartLine, fixitStartColumn, fixitStartOffset;
                unsigned int fixitEndLine, fixitEndColumn, fixitEndOffset;
                clang_getExpansionLocation(fixitStart, nullptr, &fixitStartLine, &fixitStartColumn, &fixitStartOffset);
                clang_getExpansionLocation(fixitEnd, nullptr, &fixitEndLine, &fixitEndColumn, &fixitEndOffset);
                
                sout << "  Fix-It " << j + 1 << ": ";
                sout << "Replace range Start(Line " << fixitStartLine << ", Col " << fixitStartColumn << ") - ";
                sout << "End(Line " << fixitEndLine << ", Col " << fixitEndColumn << ") ";
                sout << "with \"" << clang_getCString(replacement) << "\"" << std::endl;
                
                clang_disposeString(replacement);
            }
            
            sout << std::endl;  // Add a blank line between diagnostics
        }
        
        clang_disposeDiagnostic(diagnostic);
    }
    
    return sout.str();
}

std::string printErrors(CXTranslationUnit unit, bool verbose)
{
    std::stringstream sout;
    uint32_t numDiagnostics = clang_getNumDiagnostics(unit);
    uint32_t numErrors = 0;
    for (uint32_t i = 0; i < numDiagnostics; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnostic(unit, i);
        
        CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        
        if(!verbose && severity != CXDiagnostic_Error && severity != CXDiagnostic_Fatal)
            continue;
        
        numErrors++;
        
        unsigned options = CXDiagnostic_DisplaySourceLocation | CXDiagnostic_DisplayColumn | 
                           CXDiagnostic_DisplaySourceRanges | CXDiagnostic_DisplayCategoryName;
        CXString message = clang_formatDiagnostic(diagnostic, options);
        
        sout << clang_getCString(message) << std::endl;
        
        if(verbose)
        {
            // Get diagnostic location
            CXSourceLocation location = clang_getDiagnosticLocation(diagnostic);
            CXFile file;
            unsigned int line, column, offset;
            clang_getExpansionLocation(location, &file, &line, &column, &offset);
            
            sout << printInclusionStack(unit, file);
        }

        clang_disposeString(message);
        clang_disposeDiagnostic(diagnostic);
    }
    
    return sout.str();
}

std::string getCursorFile(CXCursor cursor)
{
    CXSourceLocation location = clang_getCursorLocation(cursor);

    CXFile file;
    clang_getFileLocation(location, &file, nullptr, nullptr, nullptr);
    
    if(!file) return "";

    CXString filename = clang_getFileName(file);
    std::string filePath = clang_getCString(filename);
    clang_disposeString(filename);

    return filePath;
}

std::string getFullLineForCursor(CXCursor cursor,
                                 const std::string& sourceContent,
                                 const std::string& targetFileName) 
{
    CXSourceLocation location = clang_getCursorLocation(cursor);
    CXFile file;
    unsigned int line, column, offset;
    clang_getExpansionLocation(location, &file, &line, &column, &offset);

    CXString fileName = clang_getFileName(file);
    std::string fileNameStr = clang_getCString(fileName);
    clang_disposeString(fileName);

    // Check if the cursor is in the target file
    if (fileNameStr != targetFileName) {
        return ""; // Return empty string if not in the target file
    }

    // Read the line from the provided source content
    std::istringstream sourceStream(sourceContent);
    std::string lineContent;
    for (unsigned int i = 1; i <= line; ++i) {
        if (!std::getline(sourceStream, lineContent)) {
            return "Error: Failed to read line " + std::to_string(line) + " from source content";
        }
    }

    return lineContent;
}

std::vector<std::string> splitDataType(const std::string& dataType)
{
    std::vector<std::string> dataTypeTokens;
    boost::split(dataTypeTokens, dataType, boost::is_any_of(" <>*&,[]()"));
    return dataTypeTokens;
}

const std::unordered_set<std::string>& getCppQualifiers()
{
    static const std::unordered_set<std::string> cppQualifiers = {
        "unsigned", "signed", "const", "static", "volatile"
    };
    
    return cppQualifiers;
}

const std::set<std::string>& getCppBaseTypes()
{
    static const std::set<std::string> baseTypes = {
        "int", "char", "float", "double", "void", "bool", "short",
        "wchar_t", "short", "long", "size_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "int8_t", "int16_t", "int32_t", "int64_t"
    };
    
    return baseTypes;
}

const std::set<std::string>& getStdSupportedIdentifiers()
{
    static const std::set<std::string> stdTypes = {
        "std", "vector", "set", "map", "unordered_set", "unordered_map", "list", "queue", "stack", "iterator"
    };
    
    return stdTypes;
}

std::set<std::string> isSupportedType(const std::string& dataType)
{
    std::vector<std::string> identifiers = splitDataType(dataType);
    
    std::set<std::string> unsupported;
    
    for(auto id : identifiers)
    {
        if(getStdSupportedIdentifiers().find(id) == getStdSupportedIdentifiers().end() &&
           getCppQualifiers().find(id) == getCppQualifiers().end() &&
           getCppBaseTypes().find(id) == getCppBaseTypes().end())
        {
            unsupported.insert(id);
        }
    }
    
    return unsupported;
}

std::set<std::string> getSTDTypes(const std::string& dataType)
{
    std::set<std::string> stdDefinedTypes;
    
    // Regular expression to match string literals
    std::regex stringLiteralRegex(R"("(?:[^"\\]|\\.)*")");
    
    // Remove string literals
    std::string processedDataType = std::regex_replace(dataType, stringLiteralRegex, "");
    
    // Regular expression to match numeric literals (including floating-point)
    std::regex numericRegex(R"(\b-?\d+(\.\d+)?([eE][+-]?\d+)?\b)");
    
    // Remove numeric literals
    processedDataType = std::regex_replace(processedDataType, numericRegex, "");
    
    //std::vector<std::string> dataTypeTokens;
    //boost::split(dataTypeTokens, processedDataType, boost::is_any_of(" <>*&,[]()"));
    
    //std::vector<std::string> dataTypeTokens = splitDataType(processedDataType);
    std::vector<std::string> dataTypeTokens = splitCTypeByNamespace(processedDataType, "::");
    
    std::string prevToken;
    for(const auto& token : dataTypeTokens)
    {
        if(!token.empty() &&
           //token.find("std::") != std::string::npos &&
           prevToken == "std::" &&
           getCppQualifiers().find(token) == getCppQualifiers().end() &&
           getCppBaseTypes().find(token) == getCppBaseTypes().end())
        {
            // Additional check to ensure the token starts with a letter or underscore
            if(std::isalpha(token[0]) || token[0] == '_')
            {
                stdDefinedTypes.insert(token);
            }
        }
        
        if(token == "::")
            prevToken += token;
        else if(!token.empty())
            prevToken = token;
    }
    
    return stdDefinedTypes;
}

std::set<std::string> getFullSTDTypesForFunction(const ParsedFunction& signature)
{
    std::set<std::string> fullSTDDefinedTypes;
    
    if(!getSTDTypes(signature.returnType).empty())
    {
        fullSTDDefinedTypes.insert(signature.returnType);
    }
    
    for(auto arg : signature.argumentTypes)
    {
        std::set<std::string> stdTypes = getSTDTypes(arg);
        if(!stdTypes.empty())
        {
            fullSTDDefinedTypes.insert(arg);
        }
    }
    
    return fullSTDDefinedTypes;
}

std::set<std::string> getSTDTypesForFunction(const ParsedFunction& signature)
{
    std::set<std::string> stdDefinedTypes = getSTDTypes(signature.returnType);
    
    for(auto arg : signature.argumentTypes)
    {
        std::set<std::string> appTypes = getSTDTypes(arg);
        stdDefinedTypes.insert(appTypes.begin(), appTypes.end());
    }
    
    return stdDefinedTypes;
}

std::set<std::string> getSTDFullTypesFromDecl(const std::string& decl)
{
    ParsedFunction signature = parseFunctionSignature(decl);
    
    return getFullSTDTypesForFunction(signature);
}

bool isConstType(const std::string& dataType)
{
    std::vector<std::string> dataTypeTokens = splitDataType(dataType);
    for(auto token : dataTypeTokens)
    {
        if(token == "const" || token == "::const_iterator")
            return true;
    }
    
    return false;
}

std::pair<bool, std::string> isInvalidIterator(
    const std::string& type,
    const std::string& appType,
    const std::unordered_set<std::string>& containerTypes)
{
    // Define a whitespace pattern that includes all possible C++ whitespace
    const std::string ws = "[ \\t\\n\\r]*";

    std::string joinContainerTypes;
    for (const auto& container : containerTypes)
    {
        if(!joinContainerTypes.empty()) joinContainerTypes += "|";
        joinContainerTypes += container;
    }
    
    // Regex for invalid iterator (not wrapped in shared_ptr)
    std::string invalid_pattern = ws + "(const" + ws + ")?" + "std" + ws + "::" + ws + "(" + joinContainerTypes + ")" + ws +
        "<" + ws + appType + ws + ">" + ws + "::" + ws + "(const_?" + ws + ")?" + "iterator" + ws + "(&" + ws + ")?";
    std::regex invalid_iterator_regex(invalid_pattern);

    // Regex for valid iterator (wrapped in shared_ptr)
    std::string valid_pattern = ws + "(const" + ws + ")?" + "std" + ws + "::" + ws + "(" + joinContainerTypes + ")" + ws +
        "<" + ws + "std" + ws + "::" + ws + "shared_ptr" + ws + "<" + ws + appType + ws + ">" + ws + ">" + ws +
        "::" + ws + "(const_?" + ws + ")?" + "iterator" + ws + "(&" + ws + ")?";
    std::regex valid_iterator_regex(valid_pattern);

    bool matchInvalid = std::regex_match(type, invalid_iterator_regex);
    bool matchValid = std::regex_match(type, valid_iterator_regex);

    if (matchInvalid && !matchValid) {
        return {true, appType};  // This is an invalid iterator (not wrapped in shared_ptr)
    }

    return {false, ""};  // This is either a valid iterator or not an iterator at all
}


std::pair<bool, std::string> isInvalidContainer(
    const std::string& type,
    const std::string& appType,
    const std::unordered_set<std::string>& containerTypes)
{
    // Define a whitespace pattern that includes all possible C++ whitespace
    const std::string ws = "[ \\t\\n\\r]*";

    std::string joinContainerTypes;
    for (const auto& container : containerTypes)
    {
        if(!joinContainerTypes.empty()) joinContainerTypes += "|";
        joinContainerTypes += container;
    }
    
    // Regex for invalid iterator (not wrapped in shared_ptr)
    std::string invalid_pattern = ws + "(const" + ws + ")?" + "std" + ws + "::" + ws + "(" + joinContainerTypes + ")" + ws +
        "<" + ws + appType + ws + ">" + ws + "(&" + ws + ")?";
    std::regex invalid_iterator_regex(invalid_pattern);

    // Regex for valid iterator (wrapped in shared_ptr)
    std::string valid_pattern = ws + "(const" + ws + ")?" + "std" + ws + "::" + ws + "(" + joinContainerTypes + ")" + ws +
        "<" + ws + "std" + ws + "::" + ws + "shared_ptr" + ws + "<" + ws + appType + ws + ">" + ws + ">" + ws + "(&" + ws + ")?";
    std::regex valid_iterator_regex(valid_pattern);

    bool matchInvalid = std::regex_match(type, invalid_iterator_regex);
    bool matchValid = std::regex_match(type, valid_iterator_regex);

    if (matchInvalid && !matchValid) {
        return {true, appType};  // This is an invalid iterator (not wrapped in shared_ptr)
    }

    return {false, ""};  // This is either a valid iterator or not an iterator at all
}

std::vector<std::pair<uint32_t, std::string>> findAllOccurrences(const std::string& str, const std::string& regexPattern) {
    std::vector<std::pair<uint32_t, std::string>> matches;
    std::regex regex_pattern(regexPattern);
    
    auto words_begin = std::sregex_iterator(str.begin(), str.end(), regex_pattern);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::smatch match = *i;
        uint32_t position = static_cast<uint32_t>(match.position());
        matches.emplace_back(position, match.str());
    }
    
    return matches;
}

uint32_t findSharedPointersInType(const std::string& dataType)
{
    auto occurences = findAllOccurrences(dataType,
    R"(\b(std::)?(shared_ptr|unique_ptr|weak_ptr)\b)");
    
    return (uint32_t)occurences.size();
}

bool hasSharedPtrToStdNamespace(const std::string& typeStr)
{
    const auto toks = formatCppNamespaces(typeStr);

    for (size_t i = 0; i < toks.size(); ++i)
    {
        if (toks[i] != "shared_ptr")
            continue;

        size_t j = i + 1;

        // Require at least one '|' right after shared_ptr (this corresponds to '<' in normal C++)
        bool sawPipe = false;
        while (j < toks.size() && toks[j] == "|") {
            sawPipe = true;
            ++j;
        }
        if (!sawPipe)
            continue;

        // Now the pointee must begin with std::
        if (j + 1 < toks.size() && toks[j] == "std" && toks[j + 1] == "::")
            return true;
    }

    return false;
}

bool hasSharedPtrToListedStdType(const std::string& typeStr,
                                 const std::unordered_set<std::string>& stlTypesNoStd,
                                 std::string* matchedType /*= nullptr*/)
{
    const auto toks = formatCppNamespaces(typeStr);

    const auto isQualifierTok = [](const std::string& t) -> bool {
        // Keep this small; extend if needed.
        return t == "const" || t == "volatile" || t == "struct" || t == "class" ||
               t == "typename" || t == "signed" || t == "unsigned";
    };

    for (size_t i = 0; i < toks.size(); ++i)
    {
        if (toks[i] != "shared_ptr")
            continue;

        size_t j = i + 1;

        // Require at least one pipe (your '<' becomes '|')
        bool sawPipe = false;
        while (j < toks.size() && toks[j] == "|") { sawPipe = true; ++j; }
        if (!sawPipe) continue;

        // Skip qualifiers like const/volatile after '<'
        while (j < toks.size() && isQualifierTok(toks[j])) ++j;

        // Allow optional leading global scope "::"
        if (j < toks.size() && toks[j] == "::") ++j;

        // Need "std" "::"
        if (j + 1 >= toks.size()) continue;
        if (toks[j] != "std" || toks[j + 1] != "::") continue;

        // Next token is the std type name (e.g. vector, string, optional, etc.)
        const size_t k = j + 2;
        if (k >= toks.size()) continue;

        const auto& typeName = toks[k];
        if (stlTypesNoStd.find(typeName) != stlTypesNoStd.end())
        {
            if (matchedType) *matchedType = typeName;
            return true;
        }
    }

    return false;
}


std::vector<std::string> splitArguments(const std::string& args) {
    std::vector<std::string> result;
    std::string current;
    int nestedLevel = 0;

    for (char ch : args) {
        if (ch == '<') {
            nestedLevel++;
        } else if (ch == '>') {
            nestedLevel--;
        } else if (ch == ',' && nestedLevel == 0) {
            result.push_back(current);
            current.clear();
            continue;
        }
        current += ch;
    }

    if (!current.empty()) {
        result.push_back(current);
    }

    return result;
}

//For now "simple" means no function pointers
bool isSimpleFunctionDeclaration(const std::string& signature) {
    // Remove any leading/trailing whitespace
    auto trimmed = std::regex_replace(signature, std::regex("^\\s+|\\s+$"), "");
    
    // Find the position of the first and last parentheses
    size_t firstParen = trimmed.find('(');
    size_t lastParen = trimmed.rfind(')');
    
    if (firstParen == std::string::npos || lastParen == std::string::npos) {
        return false;  // Not a function declaration if there are no parentheses
    }
    
    // Check return type for function pointer syntax
    std::string returnType = trimmed.substr(0, firstParen);
    if (returnType.find('(') != std::string::npos || returnType.find(')') != std::string::npos) {
        return false;  // Function pointer in return type
    }
    
    // Check arguments for function pointer syntax
    std::string args = trimmed.substr(firstParen + 1, lastParen - firstParen - 1);
    if (args.find('(') != std::string::npos || args.find(')') != std::string::npos) {
        return false;  // Function pointer in arguments
    }
    
    return true;  // If we've passed all checks, it's a simple declaration
}

static std::vector<std::string> splitArgumentsForSignature(const std::string& args) {
    std::vector<std::string> result;
    int bracketLevel = 0;
    std::string currentArg;

    for (size_t i = 0; i < args.size(); ++i) {
        char c = args[i];
        if (c == '<') {
            ++bracketLevel;
        } else if (c == '>') {
            --bracketLevel;
        } else if (c == ',' && bracketLevel == 0) {
            // Found an argument boundary
            if (!currentArg.empty()) {
                result.push_back(currentArg);
                currentArg.clear();
            }
            continue;
        }
        currentArg.push_back(c);
    }
    // push the last argument if it exists
    if (!currentArg.empty()) {
        result.push_back(currentArg);
    }

    return result;
}

// ---------------------------------------------------------
// The parseFunctionSignature that tolerates spaces around "::"
// ---------------------------------------------------------
ParsedFunction parseFunctionSignature(const std::string& declaration) {
    
    std::string signature = normalizeWhitespace(declaration);

    // This pattern splits the signature into:
    //  1) return type (group 1)
    //  2) function name (group 2)
    //  3) argument list (group 3)
    //
    // In (2), we allow multiple segments separated by "::" with optional spaces
    // around them. For example:  A  ::  B  ::  C, etc.
    //
    // Example for functionRegex:
    //   R"((...)\s+([A-Za-z0-9_]+(?:\s*::\s*[A-Za-z0-9_]+)*)\s*\(((...))\))"
    //
    // Explanation:
    //  - ([\w:\s\*&<>,]+) => A broad net for the return type
    //  - \s+ => at least one space
    //  - ([A-Za-z0-9_]+(?:\s*::\s*[A-Za-z0-9_]+)*) => the function name
    //  - \s*\( => optional whitespace, then '('
    //  - ((?:[^()]|\((?:[^()]|\([^()]*\))*\))*) => argument list
    //  - \) => closing parenthesis
    static const std::regex functionRegex(
        R"(([\w:\s\*&<>,]+)\s+([A-Za-z0-9_]+(?:\s*::\s*[A-Za-z0-9_]+)*)\s*\(((?:[^()]|\((?:[^()]|\([^()]*\))*\))*)\))"
    );
    std::smatch match;

    ParsedFunction parsedFunction;

    if (std::regex_search(signature, match, functionRegex)) {
        parsedFunction.returnType  = match[1].str();
        parsedFunction.functionName = match[2].str();

        std::string args = match[3].str();
        std::vector<std::string> arguments = splitArgumentsForSignature(args);

        // For each argument, we again allow multiple segments separated by "::"
        // (with optional spaces around it), plus possible <template> parts,
        // plus optional & or *. We also allow something like "const" in front.
        //
        // Example for argTypeRegex:
        //   R"(((?:const\s+)?[A-Za-z0-9_]+(?:\s*::\s*[A-Za-z0-9_]+)* ... ))"
        //
        // We'll capture group(1) as the argument's "type" portion, ignoring
        // the variable name and any default value after '='.
        static const std::regex argTypeRegex(
            R"(((?:const\s+)?[A-Za-z0-9_]+(?:\s*::\s*[A-Za-z0-9_]+)*(?:\s*<(?:[^<>]|\<(?:[^<>]|<[^<>]*>)*\>)*>)?(?:\s*[&*])?)(?:\s+[\w:]+)?(?:\s*=\s*[^,]*)?)"
        );

        for (const auto& arg : arguments) {
            std::smatch argMatch;
            if (std::regex_search(arg, argMatch, argTypeRegex)) {
                std::string argType = argMatch[1].str();
                parsedFunction.argumentTypes.push_back(argType);
            }
        }
    }

    return parsedFunction;
}


std::string extractFunctionDeclaration(const std::string& input) {
    
    std::string removedComments;
    std::string sourceOnly = hen::removeComments(input, removedComments);
    
    auto bracePos = sourceOnly.find('{');
    std::string declaration = (bracePos == std::string::npos) ? sourceOnly : sourceOnly.substr(0, bracePos);

    // Trim trailing whitespace
    declaration.erase(std::find_if(declaration.rbegin(), declaration.rend(),
                                   [](unsigned char ch) { return !std::isspace(ch); }).base(),
                      declaration.end());
    
    declaration = normalizeWhitespace(declaration);

    // Add a semicolon if it's not already there
    if (!declaration.empty() && declaration.back() != ';') {
        declaration += ';';
    }

    return declaration;
}

std::vector<size_t> findAutoKeyword(const std::string& sourceCode) {
    std::vector<size_t> positions;
    std::regex autoRegex(R"(\bauto\b)");
    
    auto wordsBegin = std::sregex_iterator(sourceCode.begin(), sourceCode.end(), autoRegex);
    auto wordsEnd = std::sregex_iterator();

    for (std::sregex_iterator i = wordsBegin; i != wordsEnd; ++i) {
        std::smatch match = *i;
        positions.push_back(match.position());
    }

    return positions;
}

std::vector<size_t> findFunctionCalls(const std::string& sourceCode, const std::string& functionName) {
    std::vector<size_t> positions;
    
    // Escape special regex characters in the function name
    std::string escapedFunctionName = std::regex_replace(functionName, std::regex(R"([.*+?^${}()|[\]\\])"), R"(\$&)");
    
    // Create a regex pattern to match the function call
    std::string pattern = "\\b" + escapedFunctionName + "\\s*\\(";
    std::regex functionCallRegex(pattern);
    
    auto callsBegin = std::sregex_iterator(sourceCode.begin(), sourceCode.end(), functionCallRegex);
    auto callsEnd = std::sregex_iterator();

    for (std::sregex_iterator i = callsBegin; i != callsEnd; ++i) {
        std::smatch match = *i;
        positions.push_back(match.position());
    }

    return positions;
}

bool isUint32Enum(CXCursor cursor)
{
    if (clang_getCursorKind(cursor) == CXCursor_EnumDecl) {
        // Check if it's an enum class (scoped enum)
        int isScoped = clang_EnumDecl_isScoped(cursor);
        if (isScoped) {
            // Get the underlying type
            CXType enumType = clang_getEnumDeclIntegerType(cursor);
            CXString typeName = clang_getTypeSpelling(enumType);
            
            // Check if the underlying type is int
            if (strcmp(clang_getCString(typeName), "uint32_t") == 0)
            {
                clang_disposeString(typeName);
                return true;
            }
            
            clang_disposeString(typeName);
        }
    }
    
    return false;
}

std::string evaluateCodeForErrors(const std::string& sourceCode, bool print)
{
    CXIndex index = clang_createIndex(0, 0);
    
    std::string sysroot = hen::getSysRoot();
    std::string resourceDir = hen::getClangResourceDir();
    std::string cxxInclude  = hen::getCppInclude();
    std::string cxxIncludeOpt = "-I" + cxxInclude;
    
    const char* clang_args[] = {
        "-x", "c++",
        "-std=c++17",
        "-stdlib=libc++",
        "-DCOMPILE_TEST",
        "-Werror=format",
        "-D_LIBCPP_HAS_NO_WIDE_CHARACTERS",//Without this we get "couldn't find stdarg.h" error
        "-isysroot", sysroot.c_str(),
        "-resource-dir", resourceDir.c_str(), // ← critical for stdarg.h, stdint.h, intrinsics, etc.
        cxxIncludeOpt.c_str(), // ← libc++ headers
        
        // The same include paths seen in the verbose output:
        //"-I/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include/c++/v1",

        // Often you need to add the clang internal includes:
        //"-I/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/16/include",

        // And the system includes:
        //"-I/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include",
        //"-I/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/include"
    };
    
    CXUnsavedFile unsavedFile = { "TestCode.cpp", sourceCode.c_str(), (unsigned long)sourceCode.length() };
    
    CXTranslationUnit unit = clang_parseTranslationUnit(
        index,
        "TestCode.cpp",
        clang_args, sizeof(clang_args) / sizeof(clang_args[0]),
        &unsavedFile, 1,
        CXTranslationUnit_KeepGoing);

    if (unit == nullptr) {
        //WE MUST NOT BE HERE
        clang_disposeIndex(index);
        return "Invalid source! Unable to parse translation unit.";
    }
    
    //std::string errors = printDiagnostics(unit, true);
    std::string errors = printErrors(unit, false);
    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
    
    if(!errors.empty() || print)
    {
        std::cout << "//***** Code for evaluation begin *****" << std::endl;
        std::cout << sourceCode << std::endl;
        std::cout << "//***** Code for evaluation end *****" << std::endl;
        std::cout << "//***** Errors start *****" << std::endl;
        std::cout << errors << std::endl;
        std::cout << "//***** Errors end *****" << std::endl;
    }
    
    return errors;
}

// collectIdentifiersFromType remains the same as before
void collectIdentifiersFromType(CXType type, std::set<std::string> &identifiers) {
    if (type.kind == CXType_Invalid)
        return;

    // Get the canonical type to simplify type aliases
    CXType canonicalType = clang_getCanonicalType(type);

    // Get the type declaration cursor
    CXCursor typeCursor = clang_getTypeDeclaration(canonicalType);
    if (!clang_Cursor_isNull(typeCursor)) {
        CXString typeName = clang_getCursorSpelling(typeCursor);
        if (const char *cstr = clang_getCString(typeName)) {
            std::string identifier(cstr);
            if (!identifier.empty()) {
                identifiers.insert(identifier);
            }
        }
        clang_disposeString(typeName);
    }

    // Handle template arguments
    int numTemplateArgs = clang_Type_getNumTemplateArguments(canonicalType);
    if (numTemplateArgs > 0 && numTemplateArgs != -1) {
        for (int i = 0; i < numTemplateArgs; ++i) {
            CXType argType = clang_Type_getTemplateArgumentAsType(canonicalType, i);
            collectIdentifiersFromType(argType, identifiers);
        }
    }
}

// Forward declaration
void collectIdentifiersFromType(CXType type, std::set<std::string>& identifiers);

// A helper to safely insert a cursor's spelling into the set
static void insertCursorSpelling(CXCursor cursor, std::set<std::string>& identifiers) {
    CXString name = clang_getCursorSpelling(cursor);
    if (const char *cstr = clang_getCString(name)) {
        std::string identifier(cstr);
        if (!identifier.empty()) {
            identifiers.insert(identifier);
        }
    }
    clang_disposeString(name);
}

// We may need a small helper to specifically extract the member name from a CallExpr
// if clang_getCursorReferenced fails. We'll do this by visiting the CallExpr's children.
// We'll make a small lambda for that:
static bool extractMemberNameFromCallExpr(CXCursor callExprCursor, std::string &outName) {
    bool found = false;
    clang_visitChildren(
        callExprCursor,
        [](CXCursor c, CXCursor parent, CXClientData client_data) {
            auto *outName = reinterpret_cast<std::string*>(client_data);
            CXCursorKind ck = clang_getCursorKind(c);
            if (ck == CXCursor_MemberRefExpr || ck == CXCursor_DeclRefExpr) {
                CXString childName = clang_getCursorSpelling(c);
                const char *cn = clang_getCString(childName);
                if (cn && *cn) {
                    *outName = cn;
                    clang_disposeString(childName);
                    return CXChildVisit_Break; // Found the name, no need to continue
                }
                clang_disposeString(childName);
            }
            return CXChildVisit_Recurse;
        },
        &outName
    );
    return !outName.empty();
}


// 1) The helper visitor that traverses the AST nodes (children)
static CXChildVisitResult collectIdentifiersVisitor(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
    auto identifiers = static_cast<std::set<std::string>*>(client_data);
    CXCursorKind kind = clang_getCursorKind(cursor);

    // Check for standard declarations you care about
    if (kind == CXCursor_FunctionDecl ||
        kind == CXCursor_VarDecl       ||
        kind == CXCursor_ParmDecl      ||
        kind == CXCursor_FieldDecl     ||
        kind == CXCursor_TypedefDecl   ||
        kind == CXCursor_EnumConstantDecl)
    {
        CXString spelling = clang_getCursorSpelling(cursor);
        if (const char* cstr = clang_getCString(spelling))
        {
            identifiers->insert(std::string(cstr));
        }
        clang_disposeString(spelling);
    }
    // 2) Also collect macros
    else if (kind == CXCursor_MacroDefinition ||
             kind == CXCursor_MacroExpansion ||
             kind == CXCursor_MacroInstantiation)
    {
        CXString spelling = clang_getCursorSpelling(cursor);
        if (const char* cstr = clang_getCString(spelling))
        {
            identifiers->insert(std::string(cstr));
        }
        clang_disposeString(spelling);
    }
    /*else if (kind == CXCursor_TypeRef)
    {
        CXString spelling = clang_getCursorSpelling(cursor);
        if (const char* cstr = clang_getCString(spelling))
        {
            identifiers->insert(std::string(cstr));
        }
        clang_disposeString(spelling);
    }*/

    // Visit children of this cursor
    clang_visitChildren(cursor, collectIdentifiersVisitor, client_data);

    // Return "Continue" for the current visitor so the parent continues
    return CXChildVisit_Continue;
}

// 3) The public function that sets up the visitor
void collectIdentifiers(CXCursor cursor, std::set<std::string> &identifiers)
{
    clang_visitChildren(cursor, collectIdentifiersVisitor, &identifiers);
}

std::set<std::string> collectCppIdentifiers(const std::string& cacheFilePath, const std::string& prologueCode)
{
    std::string currentVersion = "sdk: " + hen::getSDKVersion() + " clang: " + clangVersion();
    
    std::set<std::string> libraryIdentifiers;
    
    // (A) Check cached version
    {
        std::ifstream inFile(cacheFilePath);
        if (inFile)
        {
            std::string cachedVersion;
            std::getline(inFile, cachedVersion);
            if (cachedVersion == currentVersion)
            {
                std::string line;
                while (std::getline(inFile, line)) {
                    libraryIdentifiers.insert(line);
                }
                
                return libraryIdentifiers;
            }
        }
    }
    
    // (B) Create index & parse translation unit
    CXIndex index = clang_createIndex(0, 0);
    
    std::string sysroot = hen::getSysRoot();
    std::string resourceDir = hen::getClangResourceDir();
    std::string cxxInclude  = hen::getCppInclude();
    std::string cxxIncludeOpt = "-I" + cxxInclude;
    
    const char* clang_args[] = {
        "-x", "c++",
        "-std=c++17",
        "-D_LIBCPP_HAS_NO_WIDE_CHARACTERS",
        //"-fno-builtin",
        "-fno-implicit-modules",        // <─── NEW
        "-fno-implicit-module-maps",    // <─── NEW
        "-isysroot", sysroot.c_str(),
        "-resource-dir", resourceDir.c_str(), // ← critical for stdarg.h, stdint.h, intrinsics, etc.
        cxxIncludeOpt.c_str(), // ← libc++ headers
    };
    
    // Make sure your prologueCode includes headers if you want macros from them:
    // e.g.
    // #include <sys/stat.h>
    // ...
    // This ensures S_ISREG and others are visible

    std::string sourceCode = prologueCode;
    sourceCode += "\n\nint main() { return 0; }\n\n";
    
    CXUnsavedFile unsavedFile;
    unsavedFile.Filename = "TestCode.cpp";
    unsavedFile.Contents = sourceCode.c_str();
    unsavedFile.Length   = (unsigned long)sourceCode.size();
    
    // Include DetailedPreprocessingRecord to get macro definitions
    CXTranslationUnit unit = clang_parseTranslationUnit(
        index,
        "TestCode.cpp",
        clang_args, sizeof(clang_args) / sizeof(clang_args[0]),
        &unsavedFile, 1,
        CXTranslationUnit_KeepGoing | CXTranslationUnit_DetailedPreprocessingRecord
    );

    if (!unit)
    {
        clang_disposeIndex(index);
        std::cerr << "Invalid source! Unable to parse translation unit.\n";
        return libraryIdentifiers;
    }
    
    // (C) Collect identifiers & macros
    CXCursor cursor = clang_getTranslationUnitCursor(unit);
    collectIdentifiers(cursor, libraryIdentifiers);
    
#if 1
    // After collecting from AST, add compiler built-ins that don't appear in the AST
    std::set<std::string> compilerBuiltins = {
        // Variadic function support
        "va_list", "va_start", "va_end", "va_arg", "va_copy",
        "__builtin_va_list", "__builtin_va_start", "__builtin_va_end",
        "__builtin_va_arg", "__builtin_va_copy",
        
        // Common Clang/GCC built-ins that might not show up
        "__builtin_expect", "__builtin_unreachable",
        "__builtin_constant_p", "__builtin_offsetof",
        "__builtin_types_compatible_p",
        "__builtin_choose_expr",
        "__builtin_alloca",
        "__builtin_bswap16", "__builtin_bswap32", "__builtin_bswap64",
        "__builtin_clz", "__builtin_clzl", "__builtin_clzll",
        "__builtin_ctz", "__builtin_ctzl", "__builtin_ctzll",
        "__builtin_popcount", "__builtin_popcountl", "__builtin_popcountll",
        "__builtin_parity", "__builtin_parityl", "__builtin_parityll",
        "__builtin_ffsl", "__builtin_ffsll",
        "__builtin_prefetch",
        "__builtin_huge_val", "__builtin_huge_valf", "__builtin_huge_vall",
        "__builtin_inf", "__builtin_inff", "__builtin_infl",
        "__builtin_nan", "__builtin_nanf", "__builtin_nanl",
        
        // Atomic built-ins
        "__builtin_atomic_load", "__builtin_atomic_store",
        "__builtin_atomic_exchange", "__builtin_atomic_compare_exchange",
        "__builtin_atomic_fetch_add", "__builtin_atomic_fetch_sub",
        "__builtin_atomic_fetch_and", "__builtin_atomic_fetch_or",
        "__builtin_atomic_fetch_xor",
        
        // Synchronization built-ins
        "__builtin_ia32_pause",
        "__sync_fetch_and_add", "__sync_fetch_and_sub",
        "__sync_add_and_fetch", "__sync_sub_and_fetch",
        "__sync_bool_compare_and_swap", "__sync_val_compare_and_swap",
        
        // Object size checking
        "__builtin_object_size",
        "__builtin___memcpy_chk", "__builtin___memmove_chk",
        "__builtin___memset_chk", "__builtin___strcpy_chk",
        "__builtin___strncpy_chk", "__builtin___strcat_chk",
        "__builtin___strncat_chk",
        
        // Return address and frame
        "__builtin_return_address", "__builtin_frame_address",
        
        // Math built-ins (some might be captured, but let's be sure)
        "__builtin_abs", "__builtin_fabs", "__builtin_fabsf", "__builtin_fabsl",
        "__builtin_ceil", "__builtin_ceilf", "__builtin_ceill",
        "__builtin_floor", "__builtin_floorf", "__builtin_floorl",
        "__builtin_sqrt", "__builtin_sqrtf", "__builtin_sqrtl",
        "__builtin_sin", "__builtin_sinf", "__builtin_sinl",
        "__builtin_cos", "__builtin_cosf", "__builtin_cosl",
        "__builtin_exp", "__builtin_expf", "__builtin_expl",
        "__builtin_log", "__builtin_logf", "__builtin_logl",
        "__builtin_pow", "__builtin_powf", "__builtin_powl"
    };
    
    // Insert all built-ins into the main set
    libraryIdentifiers.insert(compilerBuiltins.begin(), compilerBuiltins.end());
#endif
    
    // Add any manual identifiers you desire
    libraryIdentifiers.insert("what");
    
    // (D) Write out the new cache
    {
        std::ofstream outFile(cacheFilePath);
        if (outFile)
        {
            outFile << currentVersion << std::endl;
            for (const auto& str : libraryIdentifiers) {
                outFile << str << std::endl;
            }
        }
    }
    
    // Cleanup
    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
    
    return libraryIdentifiers;
}

bool isTemplated(CXCursor cursor)
{
    CXCursorKind kind = clang_getCursorKind(cursor);

    // Check if the cursor kind is a template declaration or a specialization
    return (kind == CXCursor_ClassTemplate ||
            kind == CXCursor_FunctionTemplate ||
            kind == CXCursor_ClassTemplatePartialSpecialization ||
            kind == CXCursor_TemplateTypeParameter ||
            kind == CXCursor_TemplateTemplateParameter ||
            kind == CXCursor_NonTypeTemplateParameter);
}

bool hasBaseClass(CXCursor cursor)
{
    // Check if the cursor is a class or struct declaration
    CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind != CXCursor_ClassDecl && kind != CXCursor_StructDecl) {
        return false;
    }

    // Function to be called for each child of the cursor
    auto visitChild = [](CXCursor child, CXCursor parent, CXClientData clientData) {
        bool* hasBase = static_cast<bool*>(clientData);
        
        // Check if this child is a base class specifier
        if (clang_getCursorKind(child) == CXCursor_CXXBaseSpecifier) {
            *hasBase = true;
            return CXChildVisit_Break; // Stop iterating once we find a base class
        }
        
        return CXChildVisit_Continue;
    };

    bool hasBase = false;
    clang_visitChildren(cursor, visitChild, &hasBase);

    return hasBase;
}

CXCursor isNestedCallWithinTemplatedCall(CXCursor cursor) {
    // Check if the cursor represents a function call
    if (clang_getCursorKind(cursor) != CXCursor_CallExpr) {
        return clang_getNullCursor();
    }

    // Get the function being called
    CXCursor calledFunc = clang_getCursorReferenced(cursor);
    if (clang_Cursor_isNull(calledFunc)) {
        return clang_getNullCursor();
    }

    // Get the function containing this call
    CXCursor callingFunc = clang_getCursorSemanticParent(cursor);

    // Helper function to check if a function is templated
    auto isTemplated = [](CXCursor funcCursor) {
        CXCursor templateCursor = clang_getSpecializedCursorTemplate(funcCursor);
        return !clang_Cursor_isNull(templateCursor);
    };

    // Check if either the calling function or the called function is templated
    bool callingIsTemplated = isTemplated(callingFunc);
    bool calledIsTemplated = isTemplated(calledFunc);

    if(callingIsTemplated || calledIsTemplated)
    {
        return callingFunc;
    }
}

bool isFunctionCallWithTemplates(CXCursor cursor) {
    CXCursorKind kind = clang_getCursorKind(cursor);
    
    // Debug output
    CXString kindSpelling = clang_getCursorKindSpelling(kind);
    CXString cursorSpelling = clang_getCursorSpelling(cursor);
    //std::cout << "Cursor kind: " << clang_getCString(kindSpelling)
    //          << ", Spelling: " << clang_getCString(cursorSpelling) << std::endl;
    clang_disposeString(kindSpelling);
    clang_disposeString(cursorSpelling);

    // Function to check if a cursor represents a templated function
    auto isTemplatedFunction = [](CXCursor func) {
        CXCursor templateCursor = clang_getSpecializedCursorTemplate(func);
        return !clang_Cursor_isNull(templateCursor);
    };

    // If it's FirstExpr or CallExpr, we need to look at its children
    if (kind == CXCursor_FirstExpr || kind == CXCursor_CallExpr) {
        bool foundTemplateCall = false;
        clang_visitChildren(
            cursor,
            [](CXCursor c, CXCursor parent, CXClientData client_data) {
                CXCursorKind childKind = clang_getCursorKind(c);
                if (childKind == CXCursor_DeclRefExpr || childKind == CXCursor_CallExpr) {
                    CXCursor referencedFunc = clang_getCursorReferenced(c);
                    if (!clang_Cursor_isNull(referencedFunc)) {
                        bool isTemplated = [](CXCursor func) {
                            CXCursor templateCursor = clang_getSpecializedCursorTemplate(func);
                            return !clang_Cursor_isNull(templateCursor);
                        }(referencedFunc);
                        if (isTemplated) {
                            *static_cast<bool*>(client_data) = true;
                            return CXChildVisit_Break;
                        }
                    }
                }
                return CXChildVisit_Recurse;
            },
            &foundTemplateCall
        );
        return foundTemplateCall;
    }

    return false;
}

std::string getGenericStubFunction()
{
    std::string genericStub;
    genericStub += "template<typename... Args>\n";
    genericStub += "void stub_function(const char* name, Args&&...) {\n";
    genericStub += "    std::cout << \"Stub called for function: \" << name << std::endl;}\n";
    return genericStub;
}

std::string getStubFunction(const std::string& functionName)
{
    std::string stub;
    stub += "template<typename... Args>\n";
    stub += "void " + functionName + "(Args&&... args) { \n";
    stub += "    stub_function(#func, std::forward<Args>(args)...); }";
    return stub;
}

bool isAssignedFunctionType(CXCursor cursor) 
{
    CXType type = clang_getCursorType(cursor);
    std::string typeSpelling = clang_getCString(clang_getTypeSpelling(type));
    
    if (clang_isExpression(clang_getCursorKind(cursor))) {
        // Check if it's a lambda
        if (clang_getCursorKind(cursor) == CXCursor_LambdaExpr) {
            return true;
        }
        
        // Check if it's std::function
        if (typeSpelling.find("std::function") != std::string::npos) {
            return true;
        }
        
        // Check if it's a function pointer
        CXType pointeeType = clang_getPointeeType(type);
        if (pointeeType.kind != CXType_Invalid) {
            CXType canonicalType = clang_getCanonicalType(pointeeType);
            if (canonicalType.kind == CXType_FunctionProto || canonicalType.kind == CXType_FunctionNoProto) {
                return true;
            }
        }
    }
    
    return false;
}

std::string popFromPath(const std::string& path, uint32_t count)
{
    std::vector<std::string> nodes;
    boost::split(nodes, path, boost::is_any_of("/"));

    if (count >= nodes.size()) {
        return "";
    }

    nodes.erase(nodes.end() - count, nodes.end());
    return boost::join(nodes, "/");
}

/**
 * Analyzes compiler output for "no matching function for call to" errors
 * and extracts the function names.
 *
 * This function scans the provided compiler output for error messages
 * related to unmatched function calls. It extracts the function names
 * enclosed in single quotes from these error messages.
 *
 * @param cmplOutput A string containing the compiler's error output.
 * @return An std::set<std::string> containing unique function names
 *         extracted from the errors. The set may be empty if no
 *         matching errors were found.
 *
 * @note This function uses C++11 regex to parse the error messages.
 */
std::set<std::string> analyzeForUnmatchedFunctions(const std::string& cmplOutput)
{
    std::set<std::string> functionNames;
    std::istringstream stream(cmplOutput);
    std::string line;

    // Regex pattern to match "no matching function for call to" errors
    // and capture function names in single quotes
    std::regex pattern(R"(no matching function for call to '([^']+)')");

    while (std::getline(stream, line)) {
        std::smatch matches;
        if (std::regex_search(line, matches, pattern)) {
            if (matches.size() > 1) {
                functionNames.insert(matches[1].str());
            }
        }
    }

    return functionNames;
}


/**
 * Analyzes compiler output for problematic constructor and conversion errors
 * and extracts the involved types.
 *
 * This function scans the provided compiler output for error messages related to:
 * - No matching constructor for initialization
 * - No matching conversion for functional-style cast
 * It extracts the type names involved in these errors.
 *
 * @param cmplOutput A string containing the compiler's error output.
 * @return An std::set<std::string> containing unique type names
 *         extracted from the errors. The set may be empty if no
 *         matching errors were found.
 *
 * @note This function uses C++11 regex to parse the error messages.
 */
std::set<std::string> analyzeForProblematicTypes(const std::string& cmplOutput)
{
    std::set<std::string> typeNames;
    std::istringstream stream(cmplOutput);
    std::string line;

    // Regex patterns to match the specific error types
    std::regex constructor_pattern(R"(no matching constructor for initialization of '([^']+)')");
    std::regex conversion_pattern(R"(no matching conversion for functional-style cast from '[^']+' to '([^']+)')");

    while (std::getline(stream, line)) {
        std::smatch matches;
        if (std::regex_search(line, matches, constructor_pattern)) {
            if (matches.size() > 1) {
                typeNames.insert(matches[1].str());
            }
        }
        else if (std::regex_search(line, matches, conversion_pattern)) {
            if (matches.size() > 1) {
                typeNames.insert(matches[1].str());
            }
        }
    }

    return typeNames;
}

SyntaxErrorAnalyzer::SyntaxErrorAnalyzer(const std::regex& pattern,
                                         const std::string& prompt,
                                         const std::vector<std::string>& substitutes):
m_pattern(pattern),
m_prompt(prompt),
m_substitutes(substitutes)
{
    
}

std::string SyntaxErrorAnalyzer::analyze(const std::string& code, const std::string& compilerOutput) const
{
    std::string analysis;
    
    // Iterator to traverse the compiler output
    std::sregex_iterator iter(compilerOutput.begin(), compilerOutput.end(), m_pattern);
    std::sregex_iterator end;

    // Iterate over all matches
    while (iter != end)
    {
        std::smatch match = *iter;
        
        std::string hint = onError(code, match);
        analysis += hint;
        analysis += "\n\n";
        
        ++iter;
    }

    return analysis;
}

std::string SyntaxErrorAnalyzer::onError(const std::string& code, const std::smatch& match) const
{
    auto paramsSz = std::min(m_substitutes.size(), match.size());
    std::map<std::string, std::string> params;
    for(int p=0; p<paramsSz; ++p) {
        params[m_substitutes[p]] = match[p];
    }
    
    return hen::buildPrompt(m_prompt, params);
}

bool Analyzer_StaticCastUnrelatedTypes::m_enabled = true;

Analyzer_StaticCastUnrelatedTypes::Analyzer_StaticCastUnrelatedTypes():
SyntaxErrorAnalyzer(
std::regex(R"(error: static_cast from '.*?' \(aka '([^']+)'\) to '.*?' \(aka '([^']+)'\), which are not related by inheritance, is not allowed)"),

"The error is about a static cast from '[[from_type]]' to '[[to_type]]' Note that in this project we define only struct data types without inheritance relation.\nTo solve the error, for binary compatible types, consider std::reinterpret_pointer_cast instead of std::static_pointer_cast when casting std pointers\n",

{"from_type", "to_type"}
)
{
    
}

bool Analyzer_DefineUnknownType::m_enabled = true;
bool Analyzer_DefineUnknownType::m_hintToCreate = false;

Analyzer_DefineUnknownType::Analyzer_DefineUnknownType() :
SyntaxErrorAnalyzer(
    // The regex captures the unknown type name in a single capturing group
    std::regex(R"(error: unknown type name '([^']+)')"),
    
    // Prompt explaining the error and possible resolutions
    "The compiler encountered an unknown type named '[[unknown_type]]'.\n"
    "This typically means the type '[[unknown_type]]' is misspelled or has not been defined before use.\n"
    "- Check the spelling of '[[unknown_type]]'.\n"
    "- If '[[unknown_type]]' is a new type, ensure it is declared before usage.\n",

    // List of substitutes to map regex captures to placeholder names
    {"unknown_type"}
)
{
}

std::string Analyzer_DefineUnknownType::onError(const std::string& code, const std::smatch& match) const
{
    std::string base = SyntaxErrorAnalyzer::onError(code, match);
    
    std::string unknownType = match[0];
    
    if(m_hintToCreate)
    {
        if(code.find(unknownType) == std::string::npos)
        {
            base += "Couldn't find type '" + unknownType + "' in the compiled source. ";
            base += "Consider providing definition for this data type or revising the source!";
        }
    }
    
    return base;
}

std::set<std::string> findPatternsInComments(const std::string& code,
                                                   const std::vector<std::regex>& patterns) 
{
    std::set<std::string> matches;
    std::regex comment_regex(R"(//.*?$|/\*[\s\S]*?\*/|("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'))",
                             std::regex::multiline);
    
    std::sregex_iterator comment_it(code.begin(), code.end(), comment_regex);
    std::sregex_iterator end;

    while (comment_it != end) {
        std::string match = comment_it->str();
        
        // Only process if it's a comment, not a string literal
        if (match[0] == '/' || match[1] == '/') {
            for (const auto& pattern : patterns) {
                std::sregex_iterator pattern_it(match.begin(), match.end(), pattern);
                
                while (pattern_it != end) {
                    matches.insert(pattern_it->str());
                    ++pattern_it;
                }
            }
        }
        
        ++comment_it;
    }

    return matches;
}

void analyzeFunctionForEffect(CXCursor cursor, std::set<std::string>& noEffectFunctions)
{
    CXCursor callee = clang_getCursorReferenced(cursor);
    std::string functionName = getClangString(clang_getCursorSpelling(callee));
    bool hasNoEffect = true;

    // Check if function has arguments
    int numArgs = clang_Cursor_getNumArguments(cursor);
    if (numArgs > 0) {
        for (int i = 0; i < numArgs; ++i) {
            CXCursor arg = clang_Cursor_getArgument(cursor, i);
            CXType argType = clang_getCursorType(arg);
            
            // Check for pointer types
            if (argType.kind == CXType_Pointer) {
                CXType pointeeType = clang_getPointeeType(argType);
                
                // If it's a pointer to non-const, assume it might be modified
                if (!clang_isConstQualifiedType(pointeeType)) {
                    hasNoEffect = false;
                    break;
                }
            }
            // For non-pointer types, keep the previous check
            else if (!clang_isConstQualifiedType(argType)) {
                hasNoEffect = false;
                break;
            }
        }
    }

    // Check if return value is used
    CXCursor parent = clang_getCursorSemanticParent(cursor);
    if (clang_getCursorKind(parent) != CXCursor_CompoundStmt) {
        hasNoEffect = false;
    }

    if (hasNoEffect) {
        noEffectFunctions.insert(functionName);
    }
}

bool appFunctionHasInitializerList(CXCursor c)
{
    int numArgs = clang_Cursor_getNumArguments(c);
    for (int i = 0; i < numArgs; ++i) {
        CXCursor arg = clang_Cursor_getArgument(c, i);
        CXCursorKind argKind = clang_getCursorKind(arg);
        if (argKind == CXCursor_InitListExpr) {
            // We found a call argument that is an initializer list
            //CXString spelling = clang_getCursorSpelling(cursor);
            //printf("Function call '%s' has an initializer list argument.\n", clang_getCString(spelling));
            //clang_disposeString(spelling);
            return true;
        }
    }
    
    return false;
}

std::string getClangString(CXString s)
{
    const char* p = clang_getCString(s);   // may be nullptr
    std::string out = p ? p : "";          // guard against nullptr
    clang_disposeString(s);                // always safe to call once
    return out;
}

// Example function to get the variable name
std::string getCursorName(CXCursor cursor)
{
    CXString name = clang_getCursorSpelling(cursor);
    return getClangString(name);
}

std::string getCursorKind(CXCursor cursor)
{
    CXString kind = clang_getCursorKindSpelling(clang_getCursorKind(cursor));
    return getClangString(kind);
}

std::string getCursorType(CXCursor cursor)
{
    CXType type = clang_getCursorType(cursor);
    CXString typeSpelling = clang_getTypeSpelling(type);
    return getClangString(typeSpelling);
}

std::string getCursorLocation(CXCursor cursor)
{
    CXSourceRange cursorRange = clang_getCursorExtent(cursor);
    
    if (clang_Range_isNull(cursorRange))
    {
        return "Invalid range";
    }
    
    // Get start and end locations
    CXSourceLocation start = clang_getRangeStart(cursorRange);
    CXSourceLocation end = clang_getRangeEnd(cursorRange);
    
    // Get file, line, column for each location
    CXFile file1, file2;
    unsigned line1, line2;
    unsigned column1, column2;
    unsigned offset1, offset2;
    
    clang_getSpellingLocation(start, &file1, &line1, &column1, &offset1);
    clang_getSpellingLocation(end, &file2, &line2, &column2, &offset2);
    
    if(file1 == nullptr || file2 == nullptr)
    {
        return "WARNING: Invalid file location";
    }
        
    std::string filename1 = getClangString(clang_getFileName(file1));
    std::string filename2 = getClangString(clang_getFileName(file2));
    
    // Compare files
    if (!clang_File_isEqual(file1, file2)) {
        return "WARNING: Different locations: file1: " + filename1 + " file2: " + filename2;
    }
    
    std::stringstream sstr;
    
    sstr << filename1 << " " << line1 << ":" << column1 << "->" << line2 << ":" << column2 << " = " << (offset2-offset1);
    
    return sstr.str();
}

//Extract function name from strings like: parse_cmd_args(argc, argv, args)
std::string extractFunctionName(const std::string& callExpr) {
    std::regex functionNameRegex(R"((\w+)\s*\()");
    std::smatch match;
    
    if (std::regex_search(callExpr, match, functionNameRegex)) {
        if (match.size() > 1) {
            return match[1].str();
        }
    }
    
    return ""; // Return empty string if no match found
}

//Identify and report all occurrences where 'typeName' is not in a shared_ptr
std::vector<std::string> findNonSharedPtrUsages(const std::string& typeString, const std::string& typeName) {
    std::vector<std::string> substrings;

    // Build a regex pattern to match 'typeName' as a whole word
    std::string patternStr = "\\b" + typeName + "\\b";
    std::regex typeRegex(patternStr);

    // Iterator to find all occurrences of 'typeName' as a whole word
    auto wordsBegin = std::sregex_iterator(typeString.begin(), typeString.end(), typeRegex);
    auto wordsEnd = std::sregex_iterator();

    size_t prevEnd = 0; // End position of the previous finding or start of the string

    for (auto it = wordsBegin; it != wordsEnd; ++it) {
        std::smatch match = *it;
        size_t pos = match.position();

        // Check if 'typeName' is inside a 'shared_ptr' with optional namespaces
        // Regex pattern to match optional namespaces before 'shared_ptr'
        std::regex sharedPtrRegex("(\\b\\w+::)*\\bshared_ptr\\s*<\\s*$");

        // Extract the code before 'typeName', up to a reasonable length to avoid overrun
        size_t maxLookBehind = 100;
        size_t lookBehindStart = pos >= maxLookBehind ? pos - maxLookBehind : 0;
        std::string beforeType = typeString.substr(lookBehindStart, pos - lookBehindStart);

        // Remove any trailing whitespace from 'beforeType'
        beforeType = std::regex_replace(beforeType, std::regex("\\s+$"), "");

        // Check if 'beforeType' ends with the shared_ptr pattern
        if (std::regex_search(beforeType, sharedPtrRegex)) {
            // 'Type' is within a shared_ptr, skip it
            continue;
        }

        // 'Type' is not within a shared_ptr, so we collect it
        size_t end = pos + match.length();
        // Include any trailing whitespace or pointer/reference symbols
        while (end < typeString.length() &&
               (std::isspace(static_cast<unsigned char>(typeString[end])) ||
                typeString[end] == '*' ||
                typeString[end] == '&')) {
            end++;
        }

        // Extract the substring from 'prevEnd' to 'end' of current finding
        std::string substring = typeString.substr(prevEnd, end - prevEnd);
        substrings.push_back(substring);

        // Update 'prevEnd' to the end of the current finding
        prevEnd = end;
    }

    // Do not add the remaining text after the last finding

    return substrings;
}


// Helper function to escape regex special characters
std::string escapeRegex(const std::string& str) {
    static const std::regex regex_special(R"([\-\\\[\]{}()*+?.,\^$|#\s])");
    return std::regex_replace(str, regex_special, R"(\\$&)");
}

// Function to check if 'typeStr' contains 'MyType' prefixed by any namespace with optional whitespaces
bool isTypePrefixedByNamespace(const std::string& typeStr, const std::string& myType) {
    // Escape the 'myType' to handle any regex special characters
    std::string escapedMyType = escapeRegex(myType);
    
    // Construct the regex pattern:
    // - [A-Za-z_][A-Za-z0-9_]* matches any valid C++ identifier (namespace)
    // - \s* allows for any whitespace (including none) between namespace and ::
    // - :: is the scope resolution operator
    // - \s* allows for any whitespace between :: and MyType
    // - \b ensures that MyType is a whole word (not a prefix of another identifier)
    std::string pattern = R"([A-Za-z_][A-Za-z0-9_]*\s*::\s*)" + escapedMyType + R"(\b)";
    
    // Create a regex object. Use ECMAScript syntax which is the default.
    std::regex regexPattern;
    try {
        regexPattern = std::regex(pattern);
    } catch (const std::regex_error& e) {
        std::cerr << "Regex error: " << e.what() << " Code: " << e.code() << "\n";
        return false;
    }
    
    // Search for the pattern in 'typeStr'
    return std::regex_search(typeStr, regexPattern);
}

std::string findSubstringBefore(const std::string& input, const std::string& delimiter) {
    size_t pos = input.find(delimiter);
    if (pos != std::string::npos) {
        return input.substr(0, pos);
    }
    return input;
}

std::string findSubstringAfter(const std::string& input, const std::string& delimiter) {
    size_t pos = input.find(delimiter);
    if (pos != std::string::npos) {
        return input.substr(pos + delimiter.length());
    }
    return "";
}

std::string replaceCTypeSpecialChars(const std::string& input) {
    std::string output = input;
    //std::string chars_to_replace = " <>*&,[]()";
    std::string chars_to_replace = "<>*&,[]()";
    
    for (char c : chars_to_replace) {
        std::replace(output.begin(), output.end(), c, '|');
    }
    
    return output;
}

std::vector<std::string> splitKeepDelimiters(const std::string& input, const std::vector<std::string>& delimiters) {
    std::vector<std::string> tokens;
    std::string current;
    size_t pos = 0;
    
    while (pos < input.length()) {
        bool foundDelimiter = false;
        
        // Check each delimiter
        for (const auto& delim : delimiters) {
            if (input.substr(pos, delim.length()) == delim) {
                // Add accumulated token if any
                if (!current.empty()) {
                    tokens.push_back(current);
                    current.clear();
                }
                // Add delimiter as token
                tokens.push_back(delim);
                pos += delim.length();
                foundDelimiter = true;
                break;
            }
        }
        
        if (!foundDelimiter) {
            current += input[pos];
            pos++;
        }
    }
    
    // Add remaining token if any
    if (!current.empty()) {
        tokens.push_back(current);
    }
    
    return tokens;
}

std::vector<std::string> splitOnWhitespace(const std::string& input) {
    std::vector<std::string> tokens;
    boost::split(tokens, input, boost::is_any_of(" \t\n\r\f\v"));
    return tokens;
}

std::vector<std::string> formatCppNamespaces(const std::string& identifier)
{
    std::string noCTypeChars = replaceCTypeSpecialChars(identifier);
    std::vector<std::string> noWhiteSpaces = splitOnWhitespace(noCTypeChars);
    std::vector<std::string> tokensNS;
    for(auto token : noWhiteSpaces)
    {
        auto splitToken = splitKeepDelimiters(token, {"|", "::"});
        tokensNS.insert(tokensNS.end(), splitToken.begin(), splitToken.end());
    }
    
    return tokensNS;
}

std::set<std::string> parseNamespaces(const std::string& identifier)
{
    std::set<std::string> namespaces;
    
    std::vector<std::string> tokensNS = formatCppNamespaces(identifier);
    
    std::string prevToken;
    for(auto token : tokensNS)
    {
        if(token == "::" && !prevToken.empty() && prevToken != "|")
        {
            namespaces.insert(prevToken);
        }
        
        prevToken = token;
    }
    
    return namespaces;
}

std::set<std::string> hasCustomNamespaces(const std::string& identifier)
{
    std::set<std::string> customNamespaces;
    //Allowed namespaces
    static std::set<std::string> g_cppNamespaces = {
        "std",
    };
    
    std::set<std::string> namespacesInID = parseNamespaces(identifier);
    
    for (const auto& ns : namespacesInID) {
        
        if(ns.empty()) continue;
        
        if(g_cppNamespaces.find(ns) == g_cppNamespaces.end()) {
            //return true;
            customNamespaces.insert(ns);
        }
    }
    
    return customNamespaces;
}

std::set<std::string> functionHasCustomNamespaces(const std::string& declOrDef)
{
    std::string declaration = extractFunctionDeclaration(declOrDef);
    
    ParsedFunction signature = parseFunctionSignature(declaration);

    std::set<std::string> customNS;
    //if(hasCustomNamespaces(signature.returnType))
    //    return true;
    auto returnNS = hasCustomNamespaces(signature.returnType);
    customNS.insert(returnNS.begin(), returnNS.end());
    
    //if(hasCustomNamespaces(signature.functionName))
    //    return true;
    auto funcNS = hasCustomNamespaces(signature.functionName);
    customNS.insert(funcNS.begin(), funcNS.end());
    
    std::set<std::string> argNamespaces;
    for(auto arg : signature.argumentTypes) {
        //if(hasCustomNamespaces(arg))
        //    return true;
        auto argNS = hasCustomNamespaces(arg);
        customNS.insert(argNS.begin(), argNS.end());
    }
    
    return customNS;
}

std::vector<std::string> splitByToken(const std::string& input, const std::string& delimiter) {
    std::vector<std::string> tokens;
    
    // Escape special regex characters in the delimiter
    std::string escaped_delimiter = std::regex_replace(delimiter, std::regex("[.^$|()\\[\\]{}*+?\\\\]"), "\\$&");
    
    // Construct the regex pattern
    std::string pattern = "(" + escaped_delimiter + ")|([^" + escaped_delimiter + "]+)";
    std::regex regex_pattern(pattern);
    
    auto words_begin = std::sregex_iterator(input.begin(), input.end(), regex_pattern);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::string token = i->str();
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }

    return tokens;
}

std::vector<std::string> splitCTypeByNamespace(const std::string& input, const std::string& delimiter)
{
    std::vector<std::string> tokens;
    
    std::vector<std::string> dataTypeTokens;
    boost::split(dataTypeTokens, input, boost::is_any_of(" <>*&,[]()"));
    
    for(auto dataTypeToken : dataTypeTokens)
    {
        std::vector<std::string> nsTokens = splitByToken(dataTypeToken, delimiter);
        tokens.insert(tokens.end(), nsTokens.begin(), nsTokens.end());
    }
    
    return tokens;
}

//**************************

// The cleanClangOutput function is designed to process and filter the output from the Clang compiler,
// focusing on extracting and presenting only the most relevant error messages and code snippets related
// to the user's project. Its primary goal is to aid in debugging by providing a concise and clear summary
// of compilation issues without overwhelming the user with unnecessary or extraneous information.
//
// Key Behaviors and Features:
//
// - **File Path Simplification**: The function strips full file paths down to just filenames, removing
//   any directory structures or sensitive information such as user names. This helps protect privacy
//   and focuses attention on the files of interest.
//
// - **Exclusion of Include Stacks**: It omits include directives (e.g., "In file included from...")
//   and include stacks that do not contain direct error information. This reduces clutter and
//   conserves context space, ensuring that only pertinent information is presented.
//
// - **Project Identifier Filtering**: The function accepts a set of project-related identifiers
//   (such as function names, class names, and variable names). It includes warnings, notes, and
//   code snippets only if they contain any of these identifiers, ensuring that the output is
//   directly relevant to the user's code.
//
// - **Error Message Inclusion**: All error messages are included in the output by default, as they
//   are critical for identifying and fixing issues.
//
// - **Code Snippet Inclusion**: It captures and includes code snippets that follow error messages,
//   provided they contain project identifiers or match specific patterns (like line numbers or
//   caret markers). This gives context to the errors, helping the user understand the source of
//   the problem.
//
// - **Output Formatting**: The function improves readability by separating individual errors with
//   empty lines. This visual separation helps the user distinguish between different errors and
//   their associated notes or code snippets.
//
// - **Whitespace Handling**: It trims leading and trailing whitespace from lines to ensure accurate
//   pattern matching using regular expressions.
//
// - **Regular Expression Utilization**: The function employs regular expressions to identify and
//   process different parts of the compiler output, such as error messages, code snippets, and
//   error counts.
//
// - **Context Optimization**: By filtering out irrelevant information and focusing on essential
//   errors and notes, the function keeps the output concise. This is particularly useful when
//   working with tools or environments that have limited space or context windows, such as
//   language models or integrated development environments.
//
// Overall, cleanClangOutput streamlines the compiler output to present a clear, focused, and
// privacy-conscious summary of compilation errors and relevant code snippets, directly aiding
// the user in debugging and enhancing productivity.


#include <string>
#include <vector>
#include <regex>
#include <set>
#include <sstream>
#include <algorithm>
#include <cctype>

// Function to strip the file path down to just the filename.
// This helps to remove any directory structure or sensitive information.
std::string strip_project_dir(const std::string& file_path) {
    size_t last_sep = file_path.find_last_of("/\\");
    return (last_sep != std::string::npos) ? file_path.substr(last_sep + 1) : file_path;
}

// Function to check if a given message contains any of the project-related identifiers.
// This helps in deciding whether to include certain messages or code snippets.
bool contains_project_identifier(const std::string& message, const std::set<std::string>& project_identifiers) {
    for (const auto& identifier : project_identifiers) {
        if (message.find(identifier) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Safer trim: no UB on empty/all-whitespace strings.
static inline std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;

    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;

    return s.substr(b, e - b);
}

// process_line(...) that captures clang snippets robustly.
// - Does NOT require indentation.
// - Captures "N | ..." source line plus subsequent "| ..." caret/fix-it lines.
// - Bounded capture so it stays compact.
// - Keeps your identifier filtering for warnings/notes and extra lines.

static void process_line(const std::vector<std::string>& lines, size_t& index,
                         const std::set<std::string>& project_identifiers, std::ostream& output,
                         bool& is_first_error, bool onlyErrors)
{
    // Diagnostic line: file:line:col: (fatal )?(error|warning|note|remark): message
    static const std::regex message_regex(
        R"(^(.*?):(\d+):(\d+):\s*(fatal error|error|warning|note|remark):\s*(.*)$)");

    // Clang pretty-printed snippet lines:
    //   "  42 | foo(bar)"
    static const std::regex code_line_regex(
        R"(^\s*\d+\s*\|.*$)");

    // Caret / range / fix-it lines:
    //   "     |     ^~~"
    //   "     |     something"
    static const std::regex pipe_line_regex(
        R"(^\s*\|\s*.*$)");

    static const std::regex error_count_regex(
        R"(^(\d+)\s+(error|errors|warning|warnings) generated\.)");

    const std::string line = trim(lines[index]);
    std::smatch match;

    if (std::regex_match(line, match, message_regex)) {
        const std::string file_path    = match[1];
        const std::string line_number  = match[2];
        const std::string column_number= match[3];
        const std::string message_type = match[4];
        const std::string message      = match[5];

        const std::string file_name = strip_project_dir(file_path);

        bool include_line = false;

        if (message_type == "error" || message_type == "fatal error") {
            include_line = true;
        } else if (!onlyErrors && (message_type == "warning" || message_type == "note" || message_type == "remark")) {
            if (contains_project_identifier(message, project_identifiers)) {
                include_line = true;
            }
        }

        if (!include_line) return;

        if ((message_type == "error" || message_type == "fatal error") && !is_first_error) {
            output << "\n";
        }
        if (message_type == "error" || message_type == "fatal error") {
            is_first_error = false;
        }

        output << file_name << ":" << line_number << ":" << column_number
               << ": " << message_type << ": " << message << "\n";

        // Capture following clang snippet lines (bounded).
        size_t i = index + 1;
        int budget = 12;                 // hard cap: keep output compact
        bool sawCodeLine = false;

        while (i < lines.size() && budget-- > 0) {
            const std::string& raw = lines[i];
            const std::string t = trim(raw);

            if (t.empty()) { i++; continue; }

            // Stop if we hit another diagnostic or the final summary line.
            if (std::regex_match(t, message_regex) || std::regex_match(t, error_count_regex)) {
                break;
            }

            const bool isCodeLine = std::regex_match(raw, code_line_regex);
            const bool isPipeLine = std::regex_match(raw, pipe_line_regex);

            if (isCodeLine) {
                output << raw << "\n";
                sawCodeLine = true;
                i++;
                continue;
            }

            // After we see "N | ...", include subsequent "| ..." lines (caret/ranges/fix-its)
            if (sawCodeLine && isPipeLine) {
                output << raw << "\n";
                i++;
                continue;
            }

            // Keep your original behavior: include extra relevant lines if they contain identifiers.
            if (contains_project_identifier(raw, project_identifiers)) {
                output << raw << "\n";
                i++;
                continue;
            }

            // If we already started a snippet and now we're off it, stop early.
            if (sawCodeLine) break;

            // Otherwise skip stray unrelated lines.
            i++;
        }

        index = i - 1;
        return;
    }

    // Final "X errors generated." line handling (keep mostly as you had it).
    if (std::regex_match(line, match, error_count_regex)) {
        if (onlyErrors) {
            const std::string type = match[2];
            if (type == "error" || type == "errors") {
                output << line << "\n";
            }
        } else {
            output << line << "\n";
        }
    }
}


// The main function that cleans the Clang compiler output.
// It filters the output to include only relevant error messages and code snippets.
// It removes unnecessary paths and includes, and focuses on project-related identifiers.
// The 'onlyErrors' flag controls whether to include only errors or also include warnings and notes.
std::string cleanClangOutput(const std::string& clangOutput, const std::string& projectDirectory,
                             const std::set<std::string>& projectIdentifiers, bool onlyErrors) {
    // Split the Clang output into individual lines for processing.
    std::vector<std::string> lines;
    std::istringstream iss(clangOutput);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }

    // Prepare an output stream to collect the filtered output.
    std::ostringstream oss;
    // Variable to track whether the first error has been processed.
    bool is_first_error = true;
    // Iterate over each line in the Clang output.
    for (size_t i = 0; i < lines.size(); ++i) {
        // Process each line and write relevant information to the output stream.
        process_line(lines, i, projectIdentifiers, oss, is_first_error, onlyErrors);
    }

    // Return the collected filtered output as a string.
    return oss.str();
}

std::string clangVersion()
{
    // Get the Clang version
    CXString clangVersion = clang_getClangVersion();

    std::string versionString = clang_getCString(clangVersion);

    // Remember to dispose of the CXString to free memory
    clang_disposeString(clangVersion);

    return versionString;
}

std::string getAbstractCode(const std::string& cppCode)
{
    std::string abstractCode = std::regex_replace(cppCode, std::regex("std::"), "");
    
    // Match 'shared_ptr<type>' or 'std::shared_ptr<type>' and strip it
    std::regex pattern("(std::)?shared_ptr<([^>]+)>");
    abstractCode = std::regex_replace(abstractCode, pattern, "$2");
    
    // Remove 'const' keyword (being careful with word boundaries)
    abstractCode = std::regex_replace(abstractCode, std::regex("\\bconst\\b\\s*"), "");
    
    // Remove pointer (*) and reference (&) symbols
    abstractCode = std::regex_replace(abstractCode, std::regex("[*&]+"), "");
    
    // Replace both ::iterator and ::const_iterator with ::itr
    abstractCode = std::regex_replace(abstractCode, std::regex("::(const_)?iterator\\b"), "::itr");
    
    // Replace vector with vec (with word boundaries)
    abstractCode = std::regex_replace(abstractCode, std::regex("\\bvector\\b"), "vec");
    
    // Replace unordered_map with map and unordered_set with set
    abstractCode = std::regex_replace(abstractCode, std::regex("\\bunordered_map\\b"), "map");
    abstractCode = std::regex_replace(abstractCode, std::regex("\\bunordered_set\\b"), "set");
    
    return abstractCode;
}


bool isValidCppType(const std::string& type) {
    if (type.empty()) {
        return false;
    }

    for (size_t i = 0; i < type.length(); ++i) {
        char c = type[i];

        // Skip whitespace
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }

        // Check for valid identifier characters
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            continue;
        }

        // Check for valid symbols in C++ types
        switch (c) {
            case ':': // Namespace separator '::'
                if (i + 1 < type.length() && type[i + 1] == ':') {
                    ++i; // Skip the next ':'
                } else {
                    return false; // Single ':' is invalid in type names
                }
                break;
            case '*': // Pointer
            case '&': // Reference
            case '<': // Template opening
            case '>': // Template closing
            case ',': // Template parameter separator
            case '[': // Array type opening
            case ']': // Array type closing
            case '(': // Function pointer opening
            case ')': // Function pointer closing
                // These are valid symbols in C++ types
                break;
            case '.': // Could be part of an ellipsis '...'
                if (i + 2 < type.length() && type[i + 1] == '.' && type[i + 2] == '.') {
                    i += 2; // Skip next two '.'
                } else {
                    return false; // Single '.' is invalid in type names
                }
                break;
            default:
                // Any other character is invalid
                return false;
        }
    }

    // All characters are valid
    return true;
}

bool isValidEnumTypeName(const std::string& typeName)
{
    // Check minimum length (at least 2 characters)
    if (typeName.length() < 2)
    {
        return false;
    }

    // Check first letter is 'E'
    if (typeName[0] != 'E')
    {
        return false;
    }

    // Check second letter is uppercase
    if (!std::isupper(typeName[1]))
    {
        return false;
    }

    return true;
}

bool isAllLowercase(const std::string& ident) {
    // Returns true if ident consists only of lowercase letters and/or underscores.
    // Adjust if your identifiers can include digits, etc.
    if (ident.empty()) return false;
    for (unsigned char c : ident) {
        if (!(std::islower(c) || c == '_')) {
            return false;
        }
    }
    return true;
}

bool isAllUppercase(const std::string& ident) {
    // Returns true if ident consists only of uppercase letters and/or underscores.
    // Adjust if your identifiers can include digits, etc.
    if (ident.empty()) return false;
    for (unsigned char c : ident) {
        if (!(std::isupper(c) || c == '_')) {
            return false;
        }
    }
    return true;
}

std::set<std::string> extractUnknownTypes(const std::string& diagnostics) {
    std::set<std::string> unknownTypes;

    // 1) Regex patterns to capture relevant lines
    //    a) "Location: Line X, Column Y"
    //    b) "Source line: Some code snippet"
    //    c) "Error/Fatal: unknown type name 'XYZ'" or "Error/Fatal: use of undeclared identifier 'XYZ'"
    //    d) "Error/Fatal: no member named 'SomeMember' in 'SomeType'"
    //    e) "type 'XYZ' is undeclared" or "type XYZ is not declared anywhere visible"
    std::regex locationRegex(R"(Location:\s*Line\s+(\d+),\s*Column\s+(\d+))");
    std::regex sourceLineRegex(R"(Source line:\s*(.*))");
    std::regex errorRegex(R"((?:Error|Fatal):\s*(?:unknown type name|use of undeclared identifier)\s*'([^']+)')");
    std::regex noMemberRegex(R"((?:Error|Fatal):\s*no member named\s*'([^']+)'\s*in\s*'([^']+)')");
    std::regex undeclaredTypeRegex(R"(type\s+'?([A-Za-z_][A-Za-z0-9_:]*)'?\s+is\s+(?:undeclared|not declared anywhere visible))");

    // 2) Track the current location and source line
    int currentLine = -1;
    int currentCol = -1;
    std::string currentSourceLine;

    // 3) Read the diagnostics line by line
    std::istringstream iss(diagnostics);
    std::string line;
    while (std::getline(iss, line)) {
        std::smatch match;

        // a) Check if this line is "Location: Line X, Column Y"
        if (std::regex_search(line, match, locationRegex)) {
            currentLine = std::stoi(match[1].str());
            currentCol  = std::stoi(match[2].str());
            continue;
        }

        // b) Check if this line is "Source line: <some code>"
        if (std::regex_search(line, match, sourceLineRegex)) {
            currentSourceLine = match[1].str();
            continue;
        }

        // c) Check if this line reports an unknown type or undeclared identifier
        if (std::regex_search(line, match, errorRegex) && match.size() > 1) {
            std::string ident = match[1].str();

            bool isUnknownTypeError = (line.find("unknown type name") != std::string::npos);
            if (isUnknownTypeError) {
                // Definitely an unknown type
                unknownTypes.insert(ident);
            } else {
                // "use of undeclared identifier 'X'"
                //
                // 1) If all lowercase or all uppercase → skip adding as an unknown type
                //    (it's presumably a variable, function name, or macro)
                if (isAllLowercase(ident) || isAllUppercase(ident)) {
                    // Skip
                } else {
                    // 2) Check if ident is used as a function argument (inside `(...)`)
                    //    We'll do a naive single-line approach with a regex:
                    //    \([^)]*\bIDENT\b[^)]*\)
                    
                    // Escape the identifier for safe use in a regex
                    std::string escapedIdent = std::regex_replace(
                        ident,
                        std::regex(R"([-[\]{}()*+?.,\^$|#\s])"),
                        R"(\$&)"
                    );

                    std::regex functionArgRegex("\\([^)]*\\b" + escapedIdent + "\\b[^)]*\\)");
                    bool foundInParentheses = std::regex_search(currentSourceLine, functionArgRegex);

                    // 3) Check if it's used as a member-like usage: .X, ->X, X., X->
                    std::regex dotBeforeRegex("\\.\\s*" + escapedIdent);
                    std::regex arrowBeforeRegex("->\\s*" + escapedIdent);
                    std::regex dotAfterRegex(escapedIdent + "\\s*\\.");
                    std::regex arrowAfterRegex(escapedIdent + "\\s*->");

                    bool foundMemberUsage =
                        std::regex_search(currentSourceLine, dotBeforeRegex) ||
                        std::regex_search(currentSourceLine, arrowBeforeRegex) ||
                        std::regex_search(currentSourceLine, dotAfterRegex) ||
                        std::regex_search(currentSourceLine, arrowAfterRegex);

                    // If none of the usage patterns apply, treat as an unknown type
                    if (!foundMemberUsage && !foundInParentheses) {
                        unknownTypes.insert(ident);
                    }
                }
            }

            // Reset for the next block
            currentLine = -1;
            currentCol  = -1;
            currentSourceLine.clear();
            continue;
        }

        // d) Check if this line reports: "Error/Fatal: no member named 'tokens' in 'MacroDefinition'"
        if (std::regex_search(line, match, noMemberRegex) && match.size() > 2) {
            // The second capture group is the type in question, e.g. "MacroDefinition"
            std::string ident = match[2].str();
            
            //Add to unknownTypes only if this is an application defined type
            //TODO: Needs better check
            if(ident.find("std::") == std::string::npos)
            {
                // If you want to apply the same filters/heuristics as above, you can do so.
                // For simplicity, we'll just insert it as an unknown type:
                unknownTypes.insert(ident);
            }

            // Reset for the next block
            currentLine = -1;
            currentCol  = -1;
            currentSourceLine.clear();
        }

        // e) Check if this line reports: "type 'DiagnosticsConfig' is undeclared"
        //    or "type DiagnosticsConfig is not declared anywhere visible"
        if (std::regex_search(line, match, undeclaredTypeRegex) && match.size() > 1) {
            std::string ident = match[1].str();
            if (!isAllLowercase(ident) && !isAllUppercase(ident)) {
                unknownTypes.insert(ident);
            }

            currentLine = -1;
            currentCol  = -1;
            currentSourceLine.clear();
        }
    }

    return unknownTypes;
}

/**
 * \brief Returns a filtered version of the diagnostics string, removing any
 * diagnostic block that contains the specific error:
 *   "use of overloaded operator '=' is ambiguous (with operand types
 *    'std::string' (aka 'basic_string<char>') and 'AnyReturn')"
 *
 * A "block" is defined as:
 *   Location: ...
 *   Source line: ...
 *   Error: ...
 * followed by the next "Location:" line or end of input.
 */
std::string filterAnyReturnAmbiguityErrors(const std::string& diagnostics)
{
    // The substring we want to filter out:
    static const std::string kUnwantedError =
        "use of overloaded operator '=' is ambiguous (with operand types 'std::string' (aka 'basic_string<char>') and 'AnyReturn')";

    std::istringstream iss(diagnostics);
    std::string line;

    // We'll collect the final output here
    std::ostringstream filteredOutput;

    // We store the lines of the "current" diagnostic block in this vector
    std::vector<std::string> currentBlock;
    // Whether we skip the current block entirely
    bool skipBlock = false;

    // Helper lambda to flush the current block into the filtered output if not skipped
    auto flushCurrentBlock = [&](bool skip) {
        if (!skip) {
            for (const auto& l : currentBlock) {
                filteredOutput << l << "\n";
            }
        }
        currentBlock.clear();
    };

    while (std::getline(iss, line)) {
        // If a line starts a new "Location:" block,
        // then we decide what to do with the previous block.
        if (line.rfind("Location:", 0) == 0) {
            // Flush the old block
            flushCurrentBlock(skipBlock);

            // Start a new block
            skipBlock = false;
            currentBlock.push_back(line);
        }
        else {
            // We are within a block. Check if this line matches the error pattern:
            //   "Error: use of overloaded operator '=' is ambiguous..."
            if (!skipBlock &&
                line.find("Error:") != std::string::npos &&
                line.find(kUnwantedError) != std::string::npos)
            {
                // Mark this block to be skipped
                skipBlock = true;
            }
            // Accumulate line in the current block
            currentBlock.push_back(line);
        }
    }

    // End of file. Flush any remaining lines.
    flushCurrentBlock(skipBlock);

    return filteredOutput.str();
}

/**
 * \brief Returns a filtered version of the diagnostics string, removing any
 * diagnostic block that contains an error mentioning "AnyReturn".
 *
 * A "block" is defined as:
 *   Location: ...
 *   Source line: ...
 *   Error: ...
 * followed by the next "Location:" line or end of input.
 */
std::string filterAnyReturnErrors(const std::string& diagnostics)
{
    // We'll search for "Error:" and the substring "AnyReturn" in the same line.
    static const std::string kFilterSubstring = "AnyReturn";

    std::istringstream iss(diagnostics);
    std::string line;

    // We'll collect the final output here.
    std::ostringstream filteredOutput;

    // We store lines of the "current" diagnostic block here.
    std::vector<std::string> currentBlock;
    // Whether we skip the current block entirely.
    bool skipBlock = false;

    // Helper lambda: flush the current block to the output if not skipping.
    auto flushCurrentBlock = [&](bool skip) {
        if (!skip) {
            for (const auto& l : currentBlock) {
                filteredOutput << l << "\n";
            }
        }
        currentBlock.clear();
    };

    while (std::getline(iss, line)) {
        // If this line starts a new "Location:" block:
        if (line.rfind("Location:", 0) == 0) {
            // Flush the previous block.
            flushCurrentBlock(skipBlock);

            // Start a new block, reset skip flag.
            skipBlock = false;
            currentBlock.push_back(line);
        } else {
            // We're inside a block.
            // Check if this line is an error mentioning AnyReturn.
            if (!skipBlock &&
                line.find("Error:") != std::string::npos &&
                line.find(kFilterSubstring) != std::string::npos)
            {
                // Mark this block to be skipped entirely.
                skipBlock = true;
            }
            currentBlock.push_back(line);
        }
    }

    // End of file: flush any remaining lines from the last block.
    flushCurrentBlock(skipBlock);

    return filteredOutput.str();
}

/**
 * Transforms all C++ string literals in the given code snippet into empty ones.
 *
 * For example:
 *    "Hello"      -> ""
 *    L"Hello"     -> L""
 *    u8"Hi"       -> u8""
 *    R"(abc)"     -> R"()"
 *    R"delim(abc)delim" -> R"delim()delim"
 *
 * The function preserves the rest of the code.  It does *not* remove the string
 * tokens themselves—only their content.
 *
 * NOTE: This is a best-effort approach. Handling 100% of all corner cases from
 *       the C++ standard (especially pathological raw-string forms or macros)
 *       might require a fully compliant lexer.
 */
std::string emptyAllStringLiterals(const std::string& code)
{
    // We'll track a simple state machine:
    //   Normal    -> Not inside a string
    //   InString  -> Inside an ordinary string literal
    //   InRawString -> Inside a raw string literal
    //
    // For normal strings, we detect a prefix like L" or u8" or a plain quote ".
    // Then we copy that prefix (including the quote) to the result, skip all
    // contents until the matching unescaped quote, and then copy the closing quote.
    //
    // For raw strings, we detect something like R"delim( ... )delim". We'll parse
    // out the prefix including R"delim(, copy it, skip the content between ( and ),
    // and finally copy the )delim" part. This yields an empty raw string literal
    // (e.g. R"delim()delim").

    enum class State {
        Normal,
        InString,
        InRawString
    };

    State state = State::Normal;
    std::string result;
    result.reserve(code.size());  // optional optimization

    // For raw strings, we store the delimiter so we know when we've reached the end.
    // Example: if we see R"foo( ... )foo", delimiter_ = "foo".
    std::string rawDelimiter;

    // Keep track of the last character (for checking escapes in normal strings).
    char lastChar = '\0';

    // A helper lambda to check if code at position `pos` starts with `prefix`.
    auto startsWith = [&](const std::string& prefix, size_t pos) {
        if (pos + prefix.size() > code.size()) return false;
        return code.compare(pos, prefix.size(), prefix) == 0;
    };

    // Recognized prefixes for raw string starts: R", LR", u8R", uR", UR"
    // We'll match from longest to shortest to catch "u8R" first, etc.
    static const std::string rawPrefixes[] = {
        "u8R\"",
        "uR\"",
        "UR\"",
        "LR\"",
        "R\""
    };

    // Recognized prefixes for ordinary string starts: "u8\"", "u\"", "U\"", "L\"", "\""
    static const std::string normalPrefixes[] = {
        "u8\"",
        "u\"",
        "U\"",
        "L\"",
        "\""
    };

    // Where we store the raw-prefix text while detecting it (to copy into result).
    // Example: if we detect `u8R"xyz(`, we'll accumulate `u8R"xyz(` into rawPrefixBuffer.
    std::string rawPrefixBuffer;

    for (size_t i = 0; i < code.size(); ++i) {
        char c = code[i];

        switch (state) {
        case State::Normal:
        {
            bool foundRaw = false;
            bool foundNormal = false;

            // 1) Check if we're at the start of a raw string literal
            for (auto &rp : rawPrefixes) {
                if (startsWith(rp, i)) {
                    // Example: if rp == "u8R\"", we've matched e.g. `u8R"`
                    // Next, we need to parse the custom delimiter (if any) up to '('
                    // We'll copy everything from i until we hit the '(' into 'rawPrefixBuffer'
                    // and also into 'result'.

                    // We'll find the '(' that must appear after the prefix R" ... .
                    // Move i forward to include that prefix in result (since we keep it).
                    // For instance, if rp == "u8R\"", rp.size() = 4.
                    // We copy those 4 chars (u8R") into the result, but not the entire raw-literal yet
                    // because we also need to capture the delimiter up to '('.
                    rawPrefixBuffer.clear();
                    size_t prefixStart = i;
                    i += rp.size(); // skip past e.g. "u8R"

                    // Now we expect to see optional custom delimiter characters up until '('.
                    // For example:  R"delimiter( ... )delimiter"
                    // We'll gather them in rawDelimiter.

                    // (We do a sanity check that code[i] should be '(' if the snippet is valid.)
                    // But it might be something like R"(some text)". Then the delimiter is empty.
                    // So let's gather everything up to the first '('.
                    // i now points at the first character *after* "u8R\"", which is either '(' or
                    // more delimiter characters (like R"abc( ... )abc).

                    // We'll push rp itself to result first, e.g. "u8R"
                    result.append(rp);

                    // Now accumulate any delimiter chars and the '(' in both rawDelimiter and result.
                    // The delimiter is everything from the first character after `u8R"` up to (but not including) '('.
                    // For instance in:  R"delim(  -> delimiter = "delim"
                    // Then we also push '(' to the result, but not into rawDelimiter.
                    // So the final raw-literal prefix in result might look like: R"delim(

                    rawDelimiter.clear();
                    // Keep reading until we see '(' or until we run out of code.
                    while (i < code.size() && code[i] != '(') {
                        rawDelimiter.push_back(code[i]);
                        result.push_back(code[i]);
                        ++i;
                    }
                    // If i < code.size() and code[i] == '(', then add '(' to result
                    if (i < code.size() && code[i] == '(') {
                        result.push_back('(');
                    }
                    // Now we've appended everything up to and including '(' to result.
                    // rawDelimiter contains the custom delimiter text.

                    // We'll transition to InRawString. The content is everything until
                    // we see )delimiter".
                    state = State::InRawString;
                    foundRaw = true;
                    break;
                }
            }

            if (foundRaw) {
                // We found a raw string prefix and switched to InRawString,
                // so skip the rest of the logic for normal prefixes.
                break;
            }

            // 2) Check if we're at the start of a normal string literal
            for (auto &np : normalPrefixes) {
                if (startsWith(np, i)) {
                    // For example, np == "u8\"". We'll append that entire prefix (including the ").
                    result.append(np);
                    // Advance i to skip that prefix (minus 1 because the for-loop will do ++i).
                    i += np.size() - 1;
                    // Switch to InString
                    state = State::InString;
                    foundNormal = true;
                    break;
                }
            }

            if (!foundRaw && !foundNormal) {
                // Just a normal character; copy it to result.
                result.push_back(c);
            }

            break;
        }

        case State::InString:
        {
            // We are inside an ordinary string. We want to skip all characters
            // until we find the *unescaped* closing quote. We do NOT copy the
            // characters in between (that empties the string content).
            // But when we do find the closing quote, we output it to result
            // so that the string literal becomes something like "" or u8"".

            if (c == '"' && lastChar != '\\') {
                // This is the real end of the string. Output it.
                result.push_back('"');
                // Go back to normal.
                state = State::Normal;
            }
            // Otherwise, we skip the character (do not add to result).
            break;
        }

        case State::InRawString:
        {
            // We are inside a raw string. We have a known `rawDelimiter`, e.g. "abc"
            // so we look for `)abc"` to end the literal. Everything in between is skipped
            // so the raw literal ends up empty inside: R"abc()abc".
            if (c == ')') {
                // Potentially the end of the raw string. Check if the following chars match
                // rawDelimiter + "\"".
                const size_t delimSize = rawDelimiter.size();

                // The pattern we want is: ) <delimiter> "
                // i.e. code[i+1..i+delimSize] == rawDelimiter, code[i+1+delimSize] == '"'
                // Let's do a safe check.
                size_t afterParenPos = i + 1;
                if (afterParenPos + delimSize < code.size()) {
                    // Check if code[afterParenPos..afterParenPos+delimSize-1] == rawDelimiter
                    if (code.compare(afterParenPos, delimSize, rawDelimiter) == 0) {
                        size_t quotePos = afterParenPos + delimSize;
                        if (quotePos < code.size() && code[quotePos] == '"') {
                            // Yes, we've found the exact end sequence: )delim"
                            // We now output that entire end sequence to `result`.
                            // That means: `)`, then the delimiter, then `"`.

                            // Output the right parenthesis
                            result.push_back(')');

                            // Output the delimiter
                            result.append(rawDelimiter);

                            // Output the final quote
                            result.push_back('"');

                            // Move `i` so that we've consumed )delim" fully.
                            i = quotePos;  // the for-loop will then increment i.
                            // Return to normal state.
                            state = State::Normal;
                        }
                    }
                }
            }
            // We skip everything else while in the raw string (do not add it to result).
            break;
        }
        } // end switch

        lastChar = c;
    }

    return result;
}

bool isCursorInFunction(CXCursor cursor)
{
    int i=0;//We need some limit for the itereations. Sometimes libclang enters closed loop
    while (true && i++ < 10) {
        CXCursorKind kind = clang_getCursorKind(cursor);
        // If we reached top-level (translation unit), stop -> it's not inside a function
        if (clang_isTranslationUnit(kind)) {
            return false;
        }
        // If we find a FunctionDecl or CXXMethod, we know we are inside a function/method
        if (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod) {
            return true;
        }
        // Move up one parent
        cursor = clang_getCursorSemanticParent(cursor);
    }
    return false;
}

// Replace all whitespace (including \n, \t, etc.) with a single space.
std::string normalizeWhitespace(const std::string& str) {
    std::string result;
    bool lastWasSpace = false;
    for (char c : str) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!lastWasSpace) {
                // insert one space for the whole run of whitespace
                result.push_back(' ');
                lastWasSpace = true;
            }
        } else {
            result.push_back(c);
            lastWasSpace = false;
        }
    }
    return result;
}


// ---------------------------------------------------------------------------
// 1) Check that a string only contains certain allowed characters and patterns
// ---------------------------------------------------------------------------
bool isAlphaNumericWithColonsUnderscore(const std::string& str) {
    // We allow:
    //   - Alphanumeric [A-Za-z0-9]
    //   - Underscore `_`
    //   - Double-colon `::` (with optional spaces around it)
    //
    // This function is quite restrictive!
    // If you want to allow more C++-like identifiers (e.g. tildes for destructors,
    // operator+, etc.), you'll have to expand these rules.
    //
    // For simplicity, we’ll validate the string as a sequence of tokens separated
    // by "::" with optional spaces around them, and each token must be `[A-Za-z0-9_]+`.
    
    static const std::regex validPattern(
        R"(^[A-Za-z0-9_]+(?:\s*::\s*[A-Za-z0-9_]+)*$)"
    );
    return std::regex_match(str, validPattern);
}

// ---------------------------------------------------------------------------
// 2) Check balanced angle brackets (e.g. for templates like std::vector<int>).
//    This won't fully parse C++ templates, but it ensures we don't have unbalanced
//    `<` or `>` characters.
// ---------------------------------------------------------------------------
bool hasBalancedAngleBrackets(const std::string& s) {
    int balance = 0;
    for (char c : s) {
        if (c == '<') {
            ++balance;
        } else if (c == '>') {
            --balance;
            if (balance < 0) {
                return false; // a '>' came before a matching '<'
            }
        }
    }
    return (balance == 0);
}

// ---------------------------------------------------------------------------
// 3) Check for suspicious punctuation or characters that generally shouldn't
//    appear in C++ type declarations. (You can customize this.)
// ---------------------------------------------------------------------------
bool hasForbiddenCharacters(const std::string& s) {
    // Suppose we don't allow these characters in a type or function name:
    //   '!', '?', '@', '$', '%', '^', '~', '`', '|', or any control chars
    // (Again, adjust to your preferences.)
    static const std::string forbidden = "!?@$%^~`|;";
    
    for (char c : s) {
        if (std::iscntrl(static_cast<unsigned char>(c))) {
            return true; // control char
        }
        if (forbidden.find(c) != std::string::npos) {
            return true; // found a forbidden symbol
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 4) Validate the function name. For example:
//    - Must not be empty
//    - Must match our "allowed pattern" (e.g. only alnum, underscores, optional "::")
//    - Must not contain forbidden characters
// ---------------------------------------------------------------------------
bool validateFunctionName(const std::string& functionName) {
    if (functionName.empty()) {
        return false;
    }
    if (!isAlphaNumericWithColonsUnderscore(functionName)) {
        return false;
    }
    if (hasForbiddenCharacters(functionName)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 5) Validate the return type. For example:
//    - Must not be empty
//    - Balanced angle brackets
//    - No forbidden characters
//    - Possibly check for suspicious keywords (like "forbidden_keyword")
// ---------------------------------------------------------------------------
bool validateReturnType(const std::string& returnType) {
    if (returnType.empty()) {
        return false;
    }
    if (!hasBalancedAngleBrackets(returnType)) {
        return false;
    }
    if (hasForbiddenCharacters(returnType)) {
        return false;
    }
    
    // Optionally, check if we want to allow certain specifiers. E.g.,
    // we might allow these words: "const", "volatile", "unsigned", "long", "short",
    // or check them in context. This can get complicated quickly, so
    // below is just a simple check that the string doesn't contain a "forbidden_keyword".
    /*if (returnType.find("forbidden_keyword") != std::string::npos) {
        return false;
    }*/
    
    return true;
}

// ---------------------------------------------------------------------------
// 6) Validate an argument type. Similar checks as return type, but we also
//    might allow references and pointers.
// ---------------------------------------------------------------------------
bool validateArgumentType(const std::string& argType) {
    if (argType.empty()) {
        return false;
    }
    if (!hasBalancedAngleBrackets(argType)) {
        return false;
    }
    if (hasForbiddenCharacters(argType)) {
        return false;
    }
    // We could do more thorough scanning to ensure `&` and `*` appear in logical places,
    // but that quickly leads us toward writing a mini C++ parser. For a "basic" check,
    // let’s just ensure the entire string doesn’t violate the fundamental rules.

    // Possibly also check if argType has a "varName" portion or default value,
    // but remember, your original parsing likely separated that out or only
    // captured the type portion. Adjust if necessary.

    return true;
}

std::string getSourceForRange(CXTranslationUnit tu, CXSourceRange range)
{
    CXSourceLocation start = clang_getRangeStart(range);
    CXSourceLocation end   = clang_getRangeEnd(range);

    // 2) Extract file, line, and column info for start & end.
    CXFile fileStart, fileEnd;
    unsigned startLine, startCol, endLine, endCol;
    clang_getSpellingLocation(start, &fileStart, &startLine, &startCol, nullptr);
    clang_getSpellingLocation(end,   &fileEnd, &endLine,   &endCol,   nullptr);
    
    if(!fileStart || !fileEnd)
    {
        return {};
    }

    // 3) Convert CXFile to string (the path to the file).
    std::string fileNameStart = getClangString(clang_getFileName(fileStart));
    if (fileNameStart.empty())
    {
        return {}; // Cursor might be from a macro expansion or invalid location.
    }
    
    std::string fileNameEnd = getClangString(clang_getFileName(fileEnd));
    if (fileNameEnd.empty())
    {
        return {}; // Cursor might be from a macro expansion or invalid location.
    }
    
    if(fileNameStart != fileNameEnd)
    {
        return {}; // Cursor might be from a macro expansion or invalid location.
    }

    // 4) Get the file content through libclang API instead of reading from disk
    size_t length = 0; // Changed from unsigned int to size_t
    const char* fileContent = clang_getFileContents(tu, fileStart, &length);
    
    if (!fileContent || length == 0)
    {
        std::cerr << "Failed to access source file content: " << fileNameStart << std::endl;
        return {};
    }

    // 5) Process the file content line by line
    std::string result;
    std::vector<std::string> lines;
    std::string currentLine;
    
    // Split the content into lines
    for (size_t i = 0; i < length; ++i) // Changed to size_t to match length type
    {
        if (fileContent[i] == '\n')
        {
            lines.push_back(currentLine);
            currentLine.clear();
        }
        else
        {
            currentLine.push_back(fileContent[i]);
        }
    }
    
    // Add the last line if it doesn't end with a newline
    if (!currentLine.empty())
    {
        lines.push_back(currentLine);
    }

    // Process each line in the range
    for (unsigned currentLine = startLine; currentLine <= endLine; ++currentLine)
    {
        if (currentLine > lines.size())
        {
            break; // Beyond the file's content
        }

        // 0-based line index
        const std::string& lineStr = lines[currentLine - 1];
        unsigned lineLength = static_cast<unsigned>(lineStr.size());
        unsigned startIndex = 0;
        unsigned endIndex = lineLength;

        // If this is the start line, clip the front
        if (currentLine == startLine)
        {
            startIndex = (startCol <= lineLength) ? startCol - 1 : lineLength;
        }

        // If this is the end line, clip the back
        if (currentLine == endLine)
        {
            endIndex = (endCol <= lineLength) ? endCol - 1 : lineLength;
        }

        if (startIndex < lineLength && startIndex < endIndex)
        {
            result.append(lineStr.substr(startIndex, endIndex - startIndex));
        }

        // Add newline if not the last line
        if (currentLine < endLine)
        {
            result.push_back('\n');
        }
    }

    return result;
}

// Returns the *exact text* in the file that corresponds to the cursor's source range.
// Works with both saved and unsaved files.
std::string getCursorSource(CXCursor cursor)
{
    // Get the translation unit from the cursor
    CXTranslationUnit TU = clang_Cursor_getTranslationUnit(cursor);
    if (!TU)
    {
        return {};
    }

    // 1) Get the cursor's extent (start & end locations).
    CXSourceRange range = clang_getCursorExtent(cursor);
    return getSourceForRange(TU, range);
}

// Returns the matched valid C++ identifier if the input string matches the pattern "<identifier> *[]"
// Otherwise, returns an empty string.
std::string checkArrayOfPointers(const std::string& typeStr)
{
    // The regex breakdown:
    // ^\\s*                     : Optional leading whitespace.
    // ([a-zA-Z_][a-zA-Z0-9_]*)   : Capture group for a valid C++ identifier.
    // \\s*                      : Optional whitespace.
    // \\*                      : A pointer symbol '*'.
    // \\s*                      : Optional whitespace.
    // \\[\\s*\\]               : Square brackets "[]" with optional whitespace inside.
    // \\s*$                    : Optional trailing whitespace.
    std::regex pattern("^\\s*([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\*\\s*\\[\\s*\\]\\s*$");
    std::smatch match;
    
    if (std::regex_match(typeStr, match, pattern)) {
        // match[1] contains the captured identifier.
        return match[1];
    }
    return "";
}

// Returns:
//   >0 : the 1-based column of the first non-whitespace character on that line
//    0 : the line exists but is all whitespace (or empty)
//   -1 : file couldn't be opened or line number is out of range
int getFirstColumn(const std::string& filePath, int line) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        return -1;  // couldn't open file
    }

    std::string text;
    int currentLine = 1;
    while (std::getline(file, text)) {
        if (currentLine == line) {
            // scan for first non-whitespace
            for (size_t i = 0; i < text.size(); ++i) {
                // cast to unsigned char to safely handle chars > 127
                if (!std::isspace(static_cast<unsigned char>(text[i]))) {
                    return static_cast<int>(i) + 1;
                }
            }
            return 0;  // line exists but no non-whitespace chars
        }
        ++currentLine;
    }

    return -1;  // reached EOF before hitting requested line
}

// Returns:
//   >= 0 : 0-based offset of the first non-whitespace character on that line
//    -1  : atLine < 1, line > total lines, or line has no non-whitespace chars
int getFirstCharacterOffset(const std::string& text, int atLine) {
    if (atLine < 1)
        return -1;

    // Find the start of the target line
    size_t pos = 0;
    for (int line = 1; line < atLine; ++line) {
        pos = text.find('\n', pos);
        if (pos == std::string::npos)
            return -1;    // fewer than atLine lines
        ++pos;            // move just past the newline
    }

    // Scan from line-start for first non-space, stopping at end-of-line
    size_t i = pos;
    while (i < text.size() && text[i] != '\n' &&
           std::isspace(static_cast<unsigned char>(text[i]))) {
        ++i;
    }

    // If we hit end-of-text or end-of-line without finding a char, fail
    if (i >= text.size() || text[i] == '\n')
        return -1;

    return static_cast<int>(i);
}

bool isTemplatedFunction(const std::string& decl) {
    size_t n = decl.size();

    // 1) Find "template"
    auto kw = decl.find("template");
    if (kw == std::string::npos) return false;
    size_t i = kw + /*"template".length()=*/8;

    // 2) Skip whitespace and expect '<'
    while (i < n && std::isspace(static_cast<unsigned char>(decl[i]))) ++i;
    if (i >= n || decl[i] != '<') return false;

    // 3) Balance '<' / '>' to skip template-parameter list
    int depth = 1;
    ++i;
    while (i < n && depth > 0) {
        if      (decl[i] == '<') ++depth;
        else if (decl[i] == '>') --depth;
        ++i;
    }
    if (depth != 0) return false;

    // 4) Skip whitespace to the part before the '('
    while (i < n && std::isspace(static_cast<unsigned char>(decl[i]))) ++i;

    // 5) Find the first '('
    auto p = decl.find('(', i);
    if (p == std::string::npos) return false;

    // 6) Ensure there's an identifier (function name) immediately before '('
    size_t j = p;
    // skip any spaces just before '('
    while (j > i && std::isspace(static_cast<unsigned char>(decl[j-1]))) --j;
    if (j == i) return false;

    // the character before j should be alnum or '_'
    char c = decl[j-1];
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
        return true;

    return false;
}

std::string extractBrief(std::string& source) {
    auto is_space = [](char c) { return c == ' ' || c == '\t'; };

    std::string brief;
    std::size_t pos = 0;

    while (pos < source.size()) {
        std::size_t line_start = pos;
        std::size_t nl = source.find('\n', pos);
        std::size_t line_end = (nl == std::string::npos) ? source.size() : nl;

        std::size_t i = line_start;
        while (i < line_end && is_space(source[i])) ++i;

        // Look for "///"
        if (i + 3 <= line_end && source.compare(i, 3, "///") == 0) {
            i += 3;
            while (i < line_end && is_space(source[i])) ++i;

            static const char kBriefTok[] = "@brief";
            static const std::size_t kBriefLen = sizeof(kBriefTok) - 1;

            // Look for "@brief"
            if (i + kBriefLen <= line_end && source.compare(i, kBriefLen, kBriefTok) == 0) {
                i += kBriefLen;

                // Skip separators like spaces, ':' or '-'
                while (i < line_end && (is_space(source[i]) || source[i] == ':' || source[i] == '-')) ++i;

                // Extract first line text
                std::size_t desc_start = i;
                std::size_t desc_end = line_end;

                if (desc_end > desc_start && source[desc_end - 1] == '\r') --desc_end;
                while (desc_end > desc_start && is_space(source[desc_end - 1])) --desc_end;

                brief = source.substr(desc_start, desc_end - desc_start);

                // We'll erase from the start of the @brief line through any subsequent //-lines we consume.
                std::size_t erase_end = (nl == std::string::npos) ? source.size() : (nl + 1);

                // Consume subsequent contiguous lines that start with '//' (including '///')
                std::size_t next_pos = erase_end;
                while (next_pos < source.size()) {
                    std::size_t next_nl = source.find('\n', next_pos);
                    std::size_t next_end = (next_nl == std::string::npos) ? source.size() : next_nl;

                    std::size_t j = next_pos;
                    while (j < next_end && is_space(source[j])) ++j;

                    // Count leading slashes
                    std::size_t j_slashes = j;
                    std::size_t slash_count = 0;
                    while (j_slashes < next_end && source[j_slashes] == '/') {
                        ++j_slashes;
                        ++slash_count;
                    }

                    // Stop if the line doesn't begin with at least two slashes
                    if (slash_count < 2) break;

                    // Skip the slashes and following spaces
                    j = j_slashes;
                    while (j < next_end && is_space(source[j])) ++j;

                    // Extract the text portion of this comment line
                    std::size_t tstart = j;
                    std::size_t tend = next_end;
                    if (tend > tstart && source[tend - 1] == '\r') --tend;
                    while (tend > tstart && is_space(source[tend - 1])) --tend;

                    if (!brief.empty()) brief.push_back('\n');
                    if (tend > tstart) {
                        brief.append(source, tstart, tend - tstart);
                    }
                    // Extend the erase range to include this line
                    erase_end = (next_nl == std::string::npos) ? source.size() : (next_nl + 1);
                    next_pos = erase_end;
                }

                // Remove the consumed lines from the source
                source.erase(line_start, erase_end - line_start);

                return brief;
            }
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    return brief;
}

