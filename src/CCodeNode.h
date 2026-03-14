#pragma once

#include <clang-c/Index.h>

#include "Reflection.h"
#include "Function.h"
#include "Node.h"
#include "Test.h"
#include "Utils.h"

namespace hen {

class CCodeProject;
class DataInfo;

using CodeInspector = std::function<void(CXCursor, CXCursor)>;

class TypeDefinition
{
public:
    enum {
        STRUCT,
        ENUM
    } m_type;
    std::string m_name;
    std::string m_definition;
    //first: name, second: type
    std::unordered_map<std::string, std::string> m_members;
    
    std::vector<std::pair<std::string, std::string>> sortMembers() const {
        // Define the lambda that finds the position of a member name in m_definition.
        
        std::string removedComments;
        std::string definition = removeComments(m_definition, removedComments);
        
        auto findMemberPosition = [&](const std::string &memberName) -> size_t {
            std::regex re("\\b" + memberName + "\\b");
            std::smatch match;
            if (std::regex_search(definition, match, re)) {
                return match.position();
            }
            return std::string::npos;
        };

        // Copy the unordered_map into a vector of pairs.
        std::vector<std::pair<std::string, std::string>> sortedMembers(m_members.begin(), m_members.end());

        // Sort the vector based on the position of the member names in m_definition.
        std::sort(sortedMembers.begin(), sortedMembers.end(),
            [&](const auto &a, const auto &b) {
                size_t posA = findMemberPosition(a.first);
                size_t posB = findMemberPosition(b.first);
                return posA < posB;
            }
        );

        return sortedMembers;
    }
};

class CompilationReview
{
public:
    std::string m_initialImplementation;
    std::string m_compilationOutput;
    std::string m_proposedSolution;
    std::string m_revisedImplementation;
};

class NodeStats
{
public:
    
    uint32_t m_unusedFunctionsReportsCount;
    uint32_t m_unusedFunctionsOccurrencesCount;
    uint32_t m_noMutableAppDataReportsCount;
    uint32_t m_containerWithValuesReportsCount;
    uint32_t m_containerWithManyPointersReportsCount;
    uint32_t m_fewCStatements;
    uint32_t m_callingNonExistingFunctionCount = 0;
    
    std::set<std::string> m_unusedFunctions;
    std::set<std::string> m_complexCode;
    std::set<std::string> m_callingNonExistingFunction;
    
    std::string m_initialImplementation;
    std::vector<CompilationReview> m_compileProgress;
    
    void reset()
    {
        m_unusedFunctionsReportsCount = 0;
        m_unusedFunctionsOccurrencesCount = 0;
        m_fewCStatements = 0;
        m_noMutableAppDataReportsCount = 0;
        m_containerWithValuesReportsCount = 0;
        m_containerWithManyPointersReportsCount = 0;
        m_callingNonExistingFunctionCount = 0;
        
        m_unusedFunctions.clear();
        m_complexCode.clear();
        m_callingNonExistingFunction.clear();
    }
    
    NodeStats():
    m_unusedFunctionsReportsCount(0),
    m_noMutableAppDataReportsCount(0),
    m_containerWithValuesReportsCount(0),
    m_containerWithManyPointersReportsCount(0)
    {}
};

class CompileInfo
{
public:
    std::string                 m_sourceFilePath;
    std::string                 m_objFilePath;
    std::vector<std::string>    m_includeDirs;
    std::string                 m_options;
};

class CCodeNode : public Node
{
public:
    enum CodeType {
        FUNC_DECL,
        FUNC_IMPL,
        FUNC_CMPL,
        FUNC_FIX,
        DATA_DEF,
        EXTRACT,
        TEST
    };
    
    enum ReviewLevel
    {
        ReviewLevel_1 = 1,
        ReviewLevel_ALL
    };
    
    enum BuildOptions {
        BUILD_PRINT_TEST = 2,
        BUILD_UNIT_TEST = 4,
        BUILD_EXECUTABLE = 8,
        BUILD_DEBUG = 16,
    };
    
    bool m_defined;
    
    FunctionItem            m_brief;
    Function                m_prototype;
    FunctionDefinition      m_description;
    FunctionList            m_calls;
    LibFunctionList         m_libCalls;
    std::set<std::string>   m_excludeCalls;
    
    Code            m_implementation;
    web::json::value m_dataDef;
    DataDefList m_dataAnalysis;
    
    //Hack for code review testing
    std::set<std::string>   m_testFunctions;
    std::string             m_testFunction;
    std::string             m_inRefactoringHint;
    
    UnitTest            m_unitTest;
    
    bool            m_dataDefIsDirty;
    std::string     m_dataDefinitin;
    
    ReviewLevel         m_reviewLevel;
    std::stringstream   m_codeReview;
    mutable NodeStats   m_stats;
    
    std::string         m_diagnostics;
    bool                m_enableDiagnostics;
    bool                m_reviewDiagnostics;
    
    uint32_t            m_compilationStartMessage;
    std::string         m_childNodeNeedsBuild;
    
    friend class CCodeProject;
    
    void reviewDataLoop(std::string& cache, std::string& source, const std::string& typeName,
                        boost::optional<const DataInfo&> existingData,
                        std::map<std::string, TypeDefinition>& dataDefinitions,
                        std::set<std::string>& referencedNodes, bool stopAndWait);
    
    void reasonAboutData(std::string& cache, std::string& source, const std::string& typeName);
    

    std::string inferenceData(const std::string& defineDataMessage,
                              const std::string& typeName,
                              std::map<std::string, TypeDefinition>& dataDefinitions,
                              std::set<std::string>& referencedNodes,
                              std::string& cache);
    
    bool dataRequirementsAnalysis(DataDefList& list, const std::string& typeName);
    bool inferenceDataLoop(std::string& cache, DataDefList& list, const std::string& typeName, std::string& source);
    bool inferenceDataSource(std::string& cache, bool enforceAnalysis, const std::string& message, const std::string& typeName, std::string& source);
    void defineData(const std::string& typeName, const std::string& addContext, bool reviewData, std::set<std::string>& allForUpdate);
    void defineData(bool reviewData);
    void analyzeData();
    
    std::string codeAndDataReview(const std::string& source, std::set<std::string> referencedNodes, bool reviewDataDefinitions, bool updateSource);
    
    std::pair<bool, std::set<std::string>> getUnknownTypes();
    std::set<std::string> getUnknownTypes(const std::string& review);
    void includeUnknownTypes();
    
    bool verify();
    
    void reasonAboutFunction(std::string& cache, const std::string& parentInitialReview);
    void reviewFunction(std::string& cache, const std::string& parentInfo, const std::string& parentInitialReview);
    void pushSummary();
    bool defineFunction();
    bool describeFunction();
    
    void resolveDataDefinitions(const std::string& typeName, const std::string& path,
                                std::set<std::string>& referencedNodes,
                                const std::map<std::string, TypeDefinition>& dataDefinitions,
                                std::map<std::string, TypeDefinition>& resolvedDataDefinitions);
    
    void updateDataDefinitions(const std::string& typeName, 
                               const std::map<std::string, TypeDefinition>& dataDefinitions,
                               bool checkUnknownDataChanged,
                               const std::set<std::string>& referencedNodes,
                               std::set<std::string>& allForUpdate);

    void getDataPaths(std::set<std::string>& dataPaths) const;
    std::string getDataTypes(bool getDetached, std::set<std::string>& referencedNodes) const;
    std::string getDataTypesInScope(std::set<std::string>& referencedNodes) const;
    std::string getDataDeclarationsInScope(std::set<std::string>& referencedNodes) const;
    std::string getDataDeclarations(std::set<std::string>& referencedNodes) const;
    std::string getDataSafeDefinitions() const;
    std::string getContexInfo(bool description, bool implementation, bool dataInScope, std::set<std::string>& referencedNodes) const;
    bool implement(bool speculative);
    
    void reasonAboutCode(std::string& source, CodeType srcType,
                         int tryToRecover = -1,
                         bool enableSelfReview=true,
                         bool enableCache=true);
    
    void codeReview(std::string& source, CodeType srcType,
                    int tryToRecover = -1,
                    bool enableSelfReview=true,
                    bool enableCache=true);
    
    void codeReviewLoop(std::string& source, CodeType srcType, FileName& prompt, bool wasOnAuto, int tryToRecover = -1);
    
    std::string generateIncludes(bool checkIncludes) const;
    std::string generateProjectIncludes() const;
    std::string generateDeclarations(/*const std::string& skip*/) const;
    void getFullVisibility(bool getDetached,
                           std::set<std::string>& owners,
                           std::set<std::string>& structs,
                           std::set<std::string>& enums) const;
    
    std::string getNodeBuildSourcePath() const;
    void generateHeader() const;
    void generateSources(bool checkIncludes) const;
    void generateAllSources(bool checkIncludes) const;
    void generateProjectSources() const;
    std::string exec(const std::string& cmd, const std::string& workingDir, const std::string& operation) const;
    std::string compileCommand(const std::string& platform, uint32_t options) const;
    std::shared_ptr<CompileInfo> getCompilationInfo(const std::string& platform, uint32_t options) const;
    std::string buildCompileCommand(std::shared_ptr<CompileInfo> compileInfo) const;
    std::string linkCommand(const std::string& platform, uint32_t options) const;
    
    std::string analyzeTemplatedCalls(const std::set<std::string>& unmachedFunctions) const;
    void pushCompileProgress();
    std::string compileProgressMessage();
    
    enum CompilationReviewType
    {
        OPTIMISTIC,
        NORMAL,
        PANIC,
        COUNT
    };
    
    struct SourceSymbols {
        std::vector<std::string> functions; // qualified + signature where possible
        std::vector<std::string> types;     // qualified
    };

    struct CompilationInfo {
        std::string sourceFile;                 // absolute/relative is fine; must match the TU main file
        std::vector<std::string> clangArgs;     // flags only (NO "clang++", NO source file)
    };
    
    SourceSymbols extractSymbolsFromSource(const CompilationInfo& ci);
    CompilationInfo getCompilationInfoForSymbols(const std::string& platform, uint32_t options) const;
    
    void createAndDecomposeChild(const std::string& child);
    bool compileSource(const std::string& compileCL, std::string& output) const;
    bool updateSource(CodeType type, CCodeNode* parent, const std::string& message, const std::string& compileCL, std::string& output, bool forceRefactoring);
    bool reviewCompilation(CCodeNode* parent, CompilationReviewType approach, const std::string& compileCL, std::string& output);
    std::string analyzeCompilation(const std::string& output, std::set<std::string>& owners,
                                   std::set<std::string>& structs,
                                   std::set<std::string>& enums);
    std::string reviewAppFunctionsUse(LibFunctionList& libFunctions, const std::string& context,
                         const std::string& output, const std::string& api,
                         const std::string& analysis, const std::string& functionsList);
    void inferenceReviewLoop(CodeReview& reviewData, const std::string& context, const std::string& compileCL,
                         const std::string& output, const std::string& api, const std::string& analysis);
    
    std::string getDataDefinitions(std::set<std::string>& structs,
                                   std::set<std::string>& enums,
                                   bool declaration, bool definition, bool reportDiff) const;
    
    std::string getApiForReview(std::set<std::string>& owners,
                                std::set<std::string>& structs,
                                std::set<std::string>& enums,
                                bool getData=true) const;
    
    std::string getDataApi(bool getDetached, bool declaration, bool definition, std::set<std::string>& owners) const;
    
    std::string getTypesInfo(const std::vector<std::shared_ptr<std::string>>& unknownTypes,
                             bool& additionalInfo, const std::set<std::string>& owners);
    std::string getFunctionsInfo(const CodeReview& reviewData, bool &additionalInfo);
    bool refactorFunctions(const CodeReview& reviewData);
    
    std::string getObectFilePath() const;
    std::string getSourceFilePath() const;
    bool objectExists() const;
    bool objectIsValid() const;
    size_t getNodeHash() const;
    size_t getCachedNodeHash() const;
    void saveNodeHash(size_t hash) const;
    void cacheObject() const;
    void restoreCachedObject() const;
    bool compile(int attempts = -1);

    void pushUnitTestDef();
    //void defineUnitTest(const std::string& fullTestPath, const std::string& prevFullTestPath, const std::string& recommendation);
    void defineUnitTest2(const std::string& fullTestPath, const std::string& prevFullTestPath, const std::string& recommendation);
    void generateUnitTestInputFiles();
    bool reviewUnitTest(const std::string& compileCL, const std::string& feedback, std::string& output);
    bool compileUnitTest();
    bool unitTestExists();
    bool unitTestObjectExists();
    std::string validateUnitTestSource();
    bool compileUnitTestSource();
    bool validateUnitTestRegexContract();
    std::string getUnitTestHeaders();
    void generateUnitTestSource();
    bool linkUnitTest(bool enableSanitizer);
    bool rebuildUnitTest(bool enableSanitizer);
    void buildUnitTest(const std::string& fullTestPath, const std::string& prevFullTestPath, const std::string& recommendation);
    
    void implementUnitTest();
    void inferenceUnitTestDef(const std::string& message);
    bool reviewTestResult(const std::string& testCL, const std::string& output);
    void deleteUnitTest();
    void storeUnitTestContent();
    void summarizeUnitTestDesc();
    
    void clang(const std::string& code, CodeType type, CodeInspector codeInspector);
    void validateFunctionDeclaration();
    void reflectFunction();
    void reflectData(const std::string& source, std::map<std::string, TypeDefinition>& metadata);
    void extractFromSource(const std::string& source, bool extractLocalData,
                           std::map<std::string, TypeDefinition>& dataTypes,
                           std::map<std::string, std::string>& functions);
    
    std::set<std::string> hasDestructiveChanges(const TypeDefinition& newDataDef, std::set<std::string>& referencedNodes);
    bool checkForDependencies(const std::string& source,
                              std::map<std::string, TypeDefinition>& dataDefinitions,
                              const std::set<std::string>& referencedNodes);
    
    bool checkSTDTypes();
    
    void reviewData(const std::string& source,
                    const std::string& typeName,
                    std::map<std::string, TypeDefinition>& dataDefinitions,
                    std::set<std::string>& referencedNodes);
    bool evaluateNewDeclaration(const std::string& decl);
    void reviewSignatureChange(const std::string& source, CodeType srcType, bool reportDestructiveChanges);
    void reviewImplementation(const std::string& source, CodeType srcType);
    std::string summarizeCalls(bool brief, bool path, bool decl) const;
    void synchronizeFunctionCalls(const std::set<std::string>& calledFunctions, CodeType type);
    bool purgeUnusedNodes(bool deleteNode);
    void updateExternals();
    bool hasCachedData();
    bool updateDeclaration();
    bool isDefined();
    bool loadOrder();
    bool searchLibrary(const std::set<std::string>& exclude);
    void reviewLibFunctions(const std::string& functionList);
    bool isFromLibrary(const std::string& functionName) const;
    const std::shared_ptr<FunctionItem> callsFunction(const std::string& functionName) const;
    bool breakdown(bool addLibCalls, bool skipReviews, bool skipReasoning);
    bool resolveName(const CCodeNode* existingFuncNode, FunctionItem& brief);
    std::set<std::string> getCalledFunctions() const;
    std::string refactorTruncatedSource(std::string& source, CodeType srcType);
    
    bool mergeLibCalls();
    bool updateCallsUsage(bool createNodes, bool deleteUnusedNodes);
    void getRefactorExcludeCalls(std::set<std::string>& exclude);
    
    bool tryToImplement();
    bool checkAppTypeQualification(const std::string& appType,
                                   const std::string& type,
                                   const std::string& decl,
                                   std::set<std::string>& inConflict,
                                   std::set<std::string>& needsSharedPtr,
                                   std::set<std::string>& qualifyEnums) const;
    
    std::string callsConflictsWithData(const FunctionList& calls, bool listDataTypes);
    
    bool hasPathToMain();
    bool calledInTheSource(const std::string& otherFunction);
    
public:
    CCodeNode():Node(),
    m_defined(false),
    m_enableDiagnostics(false),
    m_reviewDiagnostics(false),
    m_compilationStartMessage(INVALID_HANDLE_ID),
    m_reviewLevel(CCodeNode::ReviewLevel_ALL)
    {}
    
    void decompose() override;
    void integrate() override;
    void save() override;
    void load() override;
    void build() override;
    void preBuild() override;
    void test() override;
    void onDelete() override;
    std::string summarize(bool brief) const override;
    void defineAndBuild();
    
    bool doesItCall(std::stack<const CCodeNode*>& path, const CCodeNode* callee) const;
    CCodeNode* getParent();
    std::string printCallsInfo();
    std::string generateReport();
    bool improveUnitTest();
    bool unitTestIsBroken(const std::string& prevTestTrajectory, const std::string& fullTestDesc, const std::string& testExecLog);
};

}
