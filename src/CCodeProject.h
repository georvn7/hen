#pragma once

#include <utility>
#include <unordered_set>

#include "IncludeBoost.h"

#include "Project.h"
#include "File.h"
#include "CCodeNode.h"

//The goal of this project is to automate development of tasks with complexity 10K-20K lines of source code. This depth should be enough
//Some small LLMs can still decompose and implement but have hard times to decide if a given function is simple enough. Let's stop them on time :)
#define DECOMPOSE_MAX_DEPTH 10
#define DECOMPOSE_TRY_COMPILE_DEPTH 6
#define DECOMPOSE_MAX_BREAKDOWN_HI_DEPTH 4

#define DECOMPOSE_MAX_GRAPH_DEPTH 4
#define DECOMPOSE_MAX_INFO_REQUESTS 3

#define COMPILE_ATTEMPTS_MAX 10
#define COMPILE_ATTEMPTS_OPTIMISTIC 2
#define COMPILE_ATTEMPTS_PANIC 3
#define MAX_COMPILE_ATTEMPTS_HISTORY 3
#define ESCALATE_AFTER_FAILED_REVIEWS 2
#define SEARCH_LIB_AFTER 1
#define PREDICTIVE_BREAKDOWN_AFTER_NODE_COUNT 100
#define BREAKDOWN_SKIP_SELF_REVIEW_BEFORE_NODE_COUNT 60
#define BREAKDOWN_SKIP_SELF_REVIEW_BEFORE_NODE_DEPTH 6
//#define ENABLE_VERBOSE_FUNCTION_DESCRIPTION
//#define ENABLE_CODE_SELF_REVIEW
//#define ENABLE_DATA_SELF_REVIEW

#define MAX_UNKNOWN_TYPES_ATTEMPTS 3

#define VERIFY_ATTEMPTS_MAX 3

#define MAX_TOKENS_IN_CONTENT_CHUNK 4000
//Average token length: Approximately 4 characters per token.
#define CHARACTERS_PER_TOKEN 4

#define MIN_LLM_CONTEXT_SIZE 8192
#define MIN_LLM_OUTPUT_SIZE 800
//#define ESTIMATED_BIG_CONTEXT_TOKENS_THRESHOLD 7200

#define MAX_CHARACTERS_IN_CONTENT_CHUNK (MAX_TOKENS_IN_CONTENT_CHUNK*CHARACTERS_PER_TOKEN)
#define MAX_INFORMATIO_REQUEST_SIZE (3*MAX_CHARACTERS_IN_CONTENT_CHUNK)

#define MAX_TOKENS_IN_COMP_REVIEW 2000
#define MAX_CHARACTERS_IN_COMP_REVIEW (MAX_TOKENS_IN_COMP_REVIEW*CHARACTERS_PER_TOKEN)

#define MAX_TOKENS_IN_SOURCE_SNIPPET 1000
#define MAX_CHARACTERS_IN_SOURCE_SNIPPET (MAX_TOKENS_IN_SOURCE_SNIPPET*CHARACTERS_PER_TOKEN)

#define MAX_LINES_IN_SOURCE_SNIPPET 160
#define MAX_REVIEW_ATTPMPTS 10
#define MAX_REVIEW_HISTORY 3
#define REVIEW_ATTPMPTS_TO_TRIGGER_ANALYSIS 8
#define INVALID_CONTEXT_TAG 0xffffffff

#define ENABLE_INFO_REQUESTS_AFTER_NODE_COUNT 50
#define ENABLE_INFO_REQUESTS_AFTER_NODE_DEPTH 5
#define ENABLE_DECOMPOSE_NOTE_AFTER_NODE_COUNT 70
#define ENABLE_DECOMPOSE_NOTE_AFTER_NODE_DEPTH 7
#define DECOMPOSE_MAX_NODES_COUNT_HINT 100

#define DECOMPOSE_REGEX_MATCH_MAX_CHARACTERS 512

#define BRIEF_MAX_CHARACTERS 384
#define BRIEF_MAX_CHARACTERS_NOTE 300
#define DESCRIPTION_MAX_CHARACTERS 2048
#define DESCRIPTION_MAX_CHARACTERS_NOTE 2000

#define RESTRICT_STL_TYPES

#define UNIT_TEST_FUNCTIONS_DEPTH 4

namespace hen {

    class DataInfo
    {
    public:
        std::string             m_ownerPath;
        std::set<std::string>   m_references;
        TypeDefinition          m_typeDef;
    };

	class CCodeProject : public Reflection<CCodeProject>, public Project
	{
    protected:
        
        std::map<std::string, DataInfo> m_objectTypes;
        
        std::map<std::string, DataInfo> m_objectTypesSnapshot;
        std::set<std::string>           m_updatedData;
        
        static std::unordered_set<std::string> m_stdIncludes;
        static std::set<std::string> m_cppIentifiers;
        
        static std::set<std::string> m_libNamespaces;
        
        std::vector<std::shared_ptr<SyntaxErrorAnalyzer>> m_focusedAnalyzers;
        
        Context     m_decomposeContext;
        FunctionItem m_items;
        
        std::string m_buildCacheDir;
        std::string m_plan;
        bool m_runDebugTests;
        bool m_runTrainingDataSynthesis;
        
        void clear();
        
	public:
        
        std::map<std::string, std::string> m_tempGraph;
        uint32_t m_refactoringDepth;
        std::set<std::string> m_buildingNow;
        
        std::string m_common_header;
        std::string m_common_header_eval;
        
        CCodeProject():
        Project(),
        m_refactoringDepth(0),
        m_runDebugTests(true),
        m_runTrainingDataSynthesis(false)
        {
            
        }
        
    public:
		DECLARE_DERIVED_TYPE(CCodeProject, Project, "Description")
		DECLARE_FIELD(FileName, coding_style, "Description")
		DECLARE_FIELD(FileName, libraries, "Description")
		DECLARE_FIELD(FileName, problem_decompose, "Description")
		
		DECLARE_FIELD(FileName, list_functions, "Description")
        DECLARE_FIELD(FileName, review_list_functions, "Description")
		DECLARE_FIELD(FileName, define_function, "Description")
		DECLARE_FIELD(FileName, define_data, "Description")
        DECLARE_FIELD(FileName, define_data_request, "Description")
        DECLARE_FIELD(FileName, define_data_loop, "Description")
        DECLARE_FIELD(FileName, implement, "Description")
        DECLARE_FIELD(FileName, review_function, "Description")
        DECLARE_FIELD(FileName, review_function_self, "Description")
        DECLARE_FIELD(FileName, review_source, "Description")
        DECLARE_FIELD(FileName, review_source_self, "Description")
        DECLARE_FIELD(FileName, review_data, "Description")
        DECLARE_FIELD(FileName, review_data_self, "Description")
        DECLARE_FIELD(FileName, review_data_request_self, "Description")
        DECLARE_FIELD(FileName, source_checklist, "Description")
        DECLARE_FIELD(FileName, compare_functions, "Description")
        DECLARE_FIELD(FileName, review_compare_functions, "Description")
        DECLARE_FIELD(FileName, define_test, "Description")
        DECLARE_FIELD(FileName, review_test_self, "Description")
        DECLARE_FIELD(FileName, implement_test, "Description")
        DECLARE_FIELD(FileName, review_test_source, "Description")
        DECLARE_FIELD(FileName, define_test_file, "Description")
        DECLARE_FIELD(FileName, review_test_file, "Description")
        DECLARE_FIELD(FileName, revise_function, "Description")
        DECLARE_FIELD(FileName, review_test_result, "Description")
        DECLARE_FIELD(FileName, fix_source_after_test, "Description")
        DECLARE_FIELD(FileName, review_compilation_panic, "Description")
        DECLARE_FIELD(FileName, review_compilation_luck, "Description")
        DECLARE_FIELD(FileName, review_compilation_lib, "Description")
        DECLARE_FIELD(FileName, find_in_library, "Description")
        DECLARE_FIELD(FileName, refactor_truncated_source, "Description")
        DECLARE_FIELD(FileName, review_refactor_brief, "Description")
        DECLARE_FIELD(FileName, refactor_original_source, "Description")
        DECLARE_FIELD(FileName, review_compilation_slef, "Description")
        DECLARE_FIELD(FileName, fix_compilation, "Description")
        DECLARE_FIELD(FileName, define_struct_members, "Description")
        DECLARE_FIELD(FileName, review_compilation_options, "Description")
        DECLARE_FIELD(FileName, review_refactoring_options, "Description")
        DECLARE_FIELD(FileName, generate_artifacts, "Description")
        DECLARE_FIELD(FileName, abstract_programming, "Description")
        DECLARE_FIELD(FileName, generate_readme, "Description")
        DECLARE_FIELD(FileName, proj_desc_from_conversation, "Description")
        DECLARE_FIELD(FileName, proj_items_from_desc, "Description")
        DECLARE_FIELD(FileName, analyze_data_requirements, "Description")
        DECLARE_FIELD(FileName, review_data_requirements_self, "Description")
        DECLARE_FIELD(FileName, verify, "Description")
        DECLARE_FIELD(FileName, review_data_and_source, "Description")
        DECLARE_FIELD(FileName, define_function_signatures, "Description")
        DECLARE_FIELD(FileName, review_function_description_self, "Description")
        DECLARE_FIELD(FileName, describe_function, "Description")

        Node* setup(const std::string& projectDir) override;
        void inferenceProjDesc(CCodeNode* root);
        void inferenceProjItems(CCodeNode* root);
        void inferenceProjPlan();
        const std::string& whatsThePlan() const {return m_plan;}
        
        boost::optional<const DataInfo&> findData(const std::string& type, std::string& owningPath) const;
        boost::optional<const DataInfo&> findDataFromSnapshot(const std::string& type, std::string& owningPath);
        void restoreFromSnapshot(const std::string& type);
        void dataSnapshot();
        
        //Format: dataTypeName, set of changed members (empty set means new datatype)
        std::map<std::string, std::set<std::string>> diffWithDataSnapshot();
        std::string dataChangesFromSnapshot(const std::map<std::string, std::set<std::string>>& diff);
        void getVisibleTypes(bool getDetached, std::set<std::string>& structs, std::set<std::string>& enums,
                             std::set<std::string>& owners, const std::string& path) const;
        std::string declareData(bool getDetached, const std::string& path) const;
        bool requiresFullDefinition(const std::string& path, const std::string& fullType, const std::string& testMember, const std::string& type) const;
        std::set<std::string> findDependencies(const std::string& path, const TypeDefinition& typeDef) const;
        std::vector<std::string> orderStructs(const std::string& path) const;
        
        std::string defineReferencedData(const std::string& path) const;
        std::string defineData(bool getDetached, const std::string& path) const;
        std::string defineDataSafe(const std::string& path) const;
        std::string getDetachedData() const;
        void attachAllDataTo(const std::string& path);
        void attachDataToExistingStructs();
        
        const std::set<std::string>& updateData(const TypeDefinition& typeDef,
                                  const std::string& param,
                                  const std::string& desc,
                                  std::string& requestingPath);
        boost::optional< DataInfo&> addDataReferences(const std::string& type,
                                  const std::set<std::string>& references);
        void removeDataReference(const std::string& type, const std::string& reference);
        std::string getFunctionInfo(const std::string& name) const;
        std::string getFunctionDescription(const std::string& name) const;
        std::string getFunctionDeclaration(const std::string& name) const;
        std::string getFunctionImplementation(const std::string& name) const;
        std::string getFunctionDetailedInfo(const std::string& name) const;
        
        std::vector<std::string> listAllFunctions(const std::string& root, int maxDepth,
                                                  const std::set<std::string>& exclude);
        
        std::string listAllFunctions(const std::string& root, int maxDepth,
                                     bool decl, bool brief, bool briefInComment,
                                     const std::set<std::string>& exclude);
        std::string listAllFunctionsSource();
        std::string listAllDataTypes() const;
        std::string listAllDataTypeNames() const;
        
        std::string listAllEnumDefinitions() const;
        std::string listAllStructDeclarations() const;
        std::string listAllStructDefinitions() const;
        
        void setupBuild() override;
        int executeCommand(const std::string& command, const std::string& cli, const boost_opt::variables_map& args) override;
        void finalizeBuild() override;
        bool archiveTest(const std::string& testPath, std::string& trajectoryDir, uint32_t& nextAttempt, std::string& failureReason);
        bool archiveBrokenTest(const std::string& testPath, std::string& failureReason);
        void debugTests();
        void synthetizeTrainingData();
        std::multimap<uint32_t, std::string, std::greater<uint32_t>>
        generateUnitTests(const std::string& finalTestPath, const std::string& prevFinalTestPath, std::string& fullTestRecommendation);
        bool buildBinary(bool enableSanitizer);
        bool buildUnitTest(const std::string& function, bool enableSanitizer);
        void generateCommonFiles(const std::string& subdir);
        void generateProjectFiles();
        void generateSingleSourceFile();
        
        std::pair<std::string, std::vector<std::string>> generateTestScript(const std::string& testJsonPath);//(const TestDef& test);
        std::string generateEmbeddedTest();
        std::string getFunctionStubs(const std::set<std::string>& appFunctions) const;
        
        static const std::unordered_set<std::string>& getSTDIncludes() {return m_stdIncludes;}
        static const std::set<std::string>& getCppTypes() {return ::getCppBaseTypes();}
        
        static const std::set<std::string>& getLibNamespaces() {return m_libNamespaces;}
        static std::set<std::string>& getCppIdentifiers() {return m_cppIentifiers;}
        
        std::set<std::string> filterIdentifiers(std::set<std::string>& identifiers,
                               std::set<std::string>& functions,
                               std::set<std::string>& structs,
                               std::set<std::string>& enums);
        void load() override;
        void reload() override;
        
        void saveDataDefinitions();
        std::map<std::string, std::string> loadDataDefinitions();
        
        bool isADependency(std::stack<const CCodeNode*>& path,
                                  const std::string& dependency,
                                  const std::string& dependent);
       
        bool appTypeHasNamespace(const std::string& identifier, std::set<std::string>& namespaces) const;
        std::set<std::string> appTypesHaveNamespace(const std::string& declOrDef) const;
        std::set<std::string> getAllIdentifiers();
        
        const std::vector<std::shared_ptr<SyntaxErrorAnalyzer>>& getFocusedAnalyzers() const { return m_focusedAnalyzers; }
        std::set<std::string> getAppTypes(const std::string& dataType) const;
        std::set<std::string> getAppTypesForFunction(const ParsedFunction& signature) const;
        std::set<std::string> getFullAppTypesForFunction(const ParsedFunction& signature) const;
        std::set<std::string> getAppTypesFromDecl(const std::string& decl) const;
        std::set<std::string> analyzeForAppIdentifiers(const std::string& cmplOutput) const;
        bool isCppOrStdType(const std::string& dataType) const;
        bool isAppType(const std::string& dataType) const;
        bool isMutableType(const std::string& dataType) const;
        bool isInvalidIterator(const std::string& dataType) const;
        bool isInvalidContainer(const std::string& dataType) const;
        bool hasEffect(const std::string& functionDecl) const;
        std::set<std::string> hasConstantTypes(const ParsedFunction& signature) const;
        std::set<std::string> hasConstantTypes(const std::string& functionDecl) const;
        static const std::unordered_set<std::string> getStdContainers();
        static const std::unordered_set<std::string> getStdUtilityTypes();
        std::set<std::string> hasRestrictedStdTypes(const std::string& dataType);
        
        CCodeNode* getRoot() {return (CCodeNode*)m_dag.m_root->m_data;}
        
        std::map<std::string, DataInfo> getDataShapshot() const;
        void restoreDataSnapshot(const std::map<std::string, DataInfo>& snapshot);
        const std::string& getProjectName() const { return m_description.name; }
        const std::string& getProjectDescription() const { return m_description.description; }
        const std::string& getProjectBrief() const { return m_items.brief; }
        std::string printGraph(const std::string& root, int maxDepth, bool desc);
        std::string printNewNodes(const std::string& root, const std::set<std::string>& prevSnapshot);
        
        void compileCommonHeader(uint32_t options) const;
        std::shared_ptr<CompileInfo> getCompilationInfo(const std::string& functionName, const std::string& platform, uint32_t options) const;
        bool updateSource(const std::string& node, CCodeNode::CodeType type, const std::string& message, std::string& output, bool enableRefactoring);
        
        Context* switchToDecomposeContext(const std::string& nodePath);
        bool decomposeAndBuild(CCodeNode* ccNode);
        bool compile();
        std::set<std::string> getDirectsInCallGraph(const std::string& node);
        
        std::string getBuildSourcePath() const;
        std::string listIncludes(std::set<std::string> includes, bool checkIncludes) const;
        std::string generateStructPrinter(const TypeDefinition& typeDef);
        std::string generateEnumPrinter(const TypeDefinition& typeDef);
        
        std::string generateStructPrinterDecl(const TypeDefinition& typeDef);
        std::string generateEnumPrinterDecl(const TypeDefinition& typeDef);
        
        std::string generateDataPrinters();
        void generateDataHeader();
        
        void generateSources();
        CCodeNode* getNodeByName(const std::string& nodeName) const;
        void buildHierarchy(DAGNode<Node*>* root) final;
        std::string provideInfo(const InfoRequest& request);
        std::string provideInfoLoop(const std::string& message, uint32_t infoRequestsMax);
        void setBuildCacheDir(const std::string& cacheDir);
        const std::string& getBuildCacheDir() const {return m_buildCacheDir;}
        size_t getCachedNodeHash(const std::string& nodeName);
        std::string searchSource(const std::regex& pattern,
                                 std::size_t maxMatchesPerFunction=3,
                                 std::size_t maxMatchesPerType=3,
                                 std::size_t maxTotalMatchedLines=100) const;
        
        std::set<std::string> getConflictsWithData(const std::set<std::string>& calledFunctions);
        std::set<std::string> getConflictsWithFunctions(const std::set<std::string>& dataDefinitions);
        
        std::string commit(const std::string& folder, const std::string& commitMessage);
        std::string commit(const std::string& commitMessage);
        std::string revert(const std::string& folder);
        std::string revert();
        
        std::string getGitHistory(const std::string& folder,
                                                   const std::string& filePath,
                                                   std::size_t maxCommits,
                                                   bool followRenames = true);
        
        std::string revertToCommit(const std::string& folder, const std::string& commitish);
        std::string revertToCommit(const std::string& commitish);
        
        std::string currentCommit(const std::string& folder);
        std::string currentCommit();
        
        std::string createBranchFromCurrent(const std::string& folder,
                                            const std::string& branchName);
        
        std::string inferBranchPointCommit(const std::string& folder,
                                           const std::string& branchName);
        
        std::string resetBranchToBranchedFromCommit(const std::string& folder,
                                                    const std::string& branchName);
        
        std::string getHighLevelAppInfo(const std::string& functionName, int functionsMaxDepth, int callGraphMaxDepth);
	};

}
