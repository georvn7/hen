#include <unordered_set>
#include <sstream>

#include "Project.hpp"
#include "CCodeProject.h"
#include "Client.h"
#include "Utils.h"
#include "UtilsCodeAnalysis.h"
#include "Artifacts.h"
#include "Node.hpp"
#include "Debugger.h"
#include "Distillery.h"
#include "Graph.hpp"

#include "IncludeBoost.h"

namespace stdrave {

	DEFINE_TYPE(CCodeProject)
	DEFINE_FIELD(CCodeProject, coding_style)
    DEFINE_FIELD(CCodeProject, libraries)
	DEFINE_FIELD(CCodeProject, problem_decompose)
        
    DEFINE_FIELD(CCodeProject, list_functions)
    DEFINE_FIELD(CCodeProject, review_list_functions)
    DEFINE_FIELD(CCodeProject, define_function)
    DEFINE_FIELD(CCodeProject, define_data)
    DEFINE_FIELD(CCodeProject, define_data_request)
    DEFINE_FIELD(CCodeProject, define_data_loop)
    DEFINE_FIELD(CCodeProject, implement)
    DEFINE_FIELD(CCodeProject, review_function)
    DEFINE_FIELD(CCodeProject, review_function_self)
    DEFINE_FIELD(CCodeProject, review_source)
    DEFINE_FIELD(CCodeProject, review_source_self)
    DEFINE_FIELD(CCodeProject, review_data)
    DEFINE_FIELD(CCodeProject, review_data_self)
    DEFINE_FIELD(CCodeProject, review_data_request_self)
    DEFINE_FIELD(CCodeProject, source_checklist)
    DEFINE_FIELD(CCodeProject, compare_functions)
    DEFINE_FIELD(CCodeProject, review_compare_functions)
    DEFINE_FIELD(CCodeProject, define_test)
    DEFINE_FIELD(CCodeProject, review_test_self)
    DEFINE_FIELD(CCodeProject, implement_test)
    DEFINE_FIELD(CCodeProject, review_test_source)
    DEFINE_FIELD(CCodeProject, define_test_file)
    DEFINE_FIELD(CCodeProject, review_test_file)
    DEFINE_FIELD(CCodeProject, revise_function)
    DEFINE_FIELD(CCodeProject, review_test_result)
    DEFINE_FIELD(CCodeProject, fix_source_after_test)
    DEFINE_FIELD(CCodeProject, review_compilation_panic)
    DEFINE_FIELD(CCodeProject, review_compilation_luck)
    DEFINE_FIELD(CCodeProject, review_compilation_lib)
    DEFINE_FIELD(CCodeProject, find_in_library)
    DEFINE_FIELD(CCodeProject, refactor_truncated_source)
    DEFINE_FIELD(CCodeProject, review_refactor_brief)
    DEFINE_FIELD(CCodeProject, refactor_original_source)
    DEFINE_FIELD(CCodeProject, review_compilation_slef)
    DEFINE_FIELD(CCodeProject, fix_compilation)
    DEFINE_FIELD(CCodeProject, define_struct_members)
    DEFINE_FIELD(CCodeProject, review_compilation_options)
    DEFINE_FIELD(CCodeProject, review_refactoring_options)
    DEFINE_FIELD(CCodeProject, generate_artifacts)
    DEFINE_FIELD(CCodeProject, abstract_programming)
    DEFINE_FIELD(CCodeProject, generate_readme)
    DEFINE_FIELD(CCodeProject, proj_desc_from_conversation)
    DEFINE_FIELD(CCodeProject, proj_items_from_desc)
    DEFINE_FIELD(CCodeProject, analyze_data_requirements)
    DEFINE_FIELD(CCodeProject, review_data_requirements_self)
    DEFINE_FIELD(CCodeProject, verify)
    DEFINE_FIELD(CCodeProject, review_data_and_source)
    DEFINE_FIELD(CCodeProject, define_function_signatures)
    DEFINE_FIELD(CCodeProject, review_function_description_self)
    DEFINE_FIELD(CCodeProject, describe_function)

    std::unordered_set<std::string> CCodeProject::m_stdIncludes = {
        //C
        "stdio.h",
        "stdlib.h",
        "sys/stat.h",
        "ctype.h",
        "math.h",
        //C++
        "iostream",      // For input/output streams
        "vector",        // For std::vector container
        "string",        // For std::string
        "algorithm",     // For algorithms like std::sort, std::find
        "map",           // For associative containers like std::map
        "memory",        // For smart pointers like std::unique_ptr, std::shared_ptr
        "utility",       // For std::pair, std::move, etc.
        "functional",    // For std::function and related function objects
        "type_traits",   // For type traits like std::is_same
        "stack",         // For std::stack container adapter
        "list",           // For std::list container
        "set",
        "unordered_set",
        "unordered_map",
        "optional",
        "fstream",
        "sstream",
        "regex",
        "cassert",
        "string_view",
        "any",
        "numeric",
        "variant"
    };

    std::set<std::string> CCodeProject::m_libNamespaces = {
        "std"
        //,"filesystem"
    };

    std::set<std::string> CCodeProject::m_cppIentifiers;

    void CCodeProject::inferenceProjDesc(CCodeNode* root)
    {
        std::cout << "Inferencing porject description" << std::endl;
        
        std::ifstream convFile(m_projDir + "/Context.txt");
        if(!convFile.is_open())
        {
            std::cout << "Error: Missing Context.txt file" << std::endl;
            return;
        }
        
        Client::getInstance().setLLM(LLMRole::DIRECTOR);
        
        root->captureContext();
        std::string conversation((std::istreambuf_iterator<char>(convFile)), std::istreambuf_iterator<char>());
        
        std::string message = proj_desc_from_conversation.prompt({
            {"conversation", conversation},
            {"sample_description", m_description.sample_description.prompt()}
                                                        });
        std::string cache = "";
        std::string projectDesc = "review";
        bool truncated = false;
        root->inference(cache, message, projectDesc, &truncated);
        
        std::string unitTests = "\n\nUnit Testing:\n";
        unitTests += "In the project implementation utilize PRINT_TEST(fmt, args...) for detailed unit test outputs, ";
        unitTests += "not available in the final build. This is only for unit tests. ";
        unitTests += "Error handling practices according to the project description ";
        unitTests += "should be implemented regardless of whether PRINT_TEST is active or not.";
        
        projectDesc = "\nPROJECT DESCRIPTION\n\n" + projectDesc + unitTests;
        
        //TODO: Consider test for description size!
        
        root->popContext();
        
        std::ofstream descFile(m_projDir + "/Description.txt");
        descFile << projectDesc << std::endl;
        descFile.close();
        
        std::cout << "Porject description saved to: " << m_projDir << "/Description.txt" << std::endl;
        
        m_description.description = projectDesc;
    }

    void CCodeProject::inferenceProjItems(CCodeNode* root)
    {
        std::cout << "Inferencing porject items" << std::endl;
        
        std::string cache = "../../items.json";
        
        Client::getInstance().setLLM(LLMRole::DIRECTOR);
        
        root->captureContext();
        //TODO: Check these prompts
        std::string message = proj_items_from_desc.prompt({
            {"sample_description", m_description.sample_description.prompt()},
            {"description", m_description.description},
            {"sample_name", m_description.name},
            {"sample_brief", m_description.brief}
        });
        
        root->inference(cache, message, &m_items);
        if(cache == "na")
        {
            m_items.brief += "\nFor full specifications refer to the 'PROJECT DESCRIPTION' section.";
        }
        
        root->popContext();
        
        auto ujson = m_items.to_json().serialize();
        std::string json = utility::conversions::to_utf8string(ujson);
        
        std::ofstream items(m_projDir + "/items.json");
        items << json << std::endl;
        items.close();
        
        std::cout << "Porject items saved to: " << m_projDir << "/items.json" << std::endl;
    }

	Node* CCodeProject::setup(const std::string& projectDir)
	{
        CCodeNode* root = shareNode<CCodeNode>(m_description.func_name, nullptr);
        Project::setup(projectDir, root);
        
        Prompt::clearSearchPaths();
        std::string promptsDirEnv = Client::getInstance().getEnvironmentDir() + "/Prompts";
        Prompt::addSearchPath(promptsDirEnv);
        
        std::string commonHeaderPath = Client::getInstance().getEnvironmentDir() + "/source/common.h";
        m_common_header = getFileContent(commonHeaderPath);
        std::string commonHeaderPathEval = Client::getInstance().getEnvironmentDir() + "/source/common_eval.h";
        m_common_header_eval = getFileContent(commonHeaderPathEval);
        
        //Agent spawn for the chat bot should be on auto by default!
        Client::getInstance().disableUserCommands(true);
        
        //These messages are general for the project, they shouldn't be in any node
        std::string message = coding_style.prompt();
        message += "\n";
        message += libraries.prompt();
        message += "\n";
        pushMessage(message, "user", true);
        
        std::string decompose = problem_decompose.prompt({
            {"entry_point", m_description.entry_point} });
        decompose += "\n";
        pushMessage(decompose, "user", true);
        
        Client::getInstance().enableLog(false);
        
        if(m_description.description.empty())
        {
            inferenceProjDesc(root);
        }
        inferenceProjItems(root);
        
        Client::getInstance().enableLog(true);
        
        Client::getInstance().disableUserCommands(false);
        pushMessage(m_description.description, "user", true);
    
        root->m_brief.func_name = m_description.func_name;
        root->m_brief.brief = m_items.brief;
        m_description.name = m_items.func_name;

        m_focusedAnalyzers.push_back(std::make_shared<Analyzer_StaticCastUnrelatedTypes>());
        m_focusedAnalyzers.push_back(std::make_shared<Analyzer_DefineUnknownType>());
        
        std::stringstream commonIncludes;
        for(auto inc : CCodeProject::getSTDIncludes()) {
            commonIncludes << "#include <" << inc << ">" << std::endl;
        }
        commonIncludes << "#include <exception>" << std::endl;
        commonIncludes << "#include <stdexcept>" << std::endl;
        commonIncludes << "#include <cstdarg>" << std::endl;
        commonIncludes << "#if __has_include(<unistd.h>)" << std::endl;
        commonIncludes << "#include <unistd.h>" << std::endl;
        commonIncludes << "#endif" << std::endl;
        
        std::string cppIdentifiersCache = Client::getInstance().getEnvironmentDir();
        cppIdentifiersCache += "/cpp_identifiers.txt";
        m_cppIentifiers = collectCppIdentifiers(cppIdentifiersCache, commonIncludes.str());
        
        //Reset the build directories each time the agent starts work on a project
        {
            if(boost_fs::exists(m_projDir + "/build"))
            {
                boost_fs::remove_all(m_projDir + "/build");
            }
            
            if(boost_fs::exists(m_projDir + "/build_backup"))
            {
                boost_fs::remove_all(m_projDir + "/build_backup");
            }
            
            if(boost_fs::exists(m_projDir + "/build_instrumented"))
            {
                boost_fs::remove_all(m_projDir + "/build_instrumented");
            }
            
            if(boost_fs::exists(m_projDir + "/cache"))
            {
                boost_fs::remove_all(m_projDir + "/cache");
            }
        }
        
        std::string builCache = "cache/build";
        setBuildCacheDir(builCache);

        return root;
	}

    boost::optional<const DataInfo&> CCodeProject::findData(const std::string& name, std::string& owningPath) const
    {
        auto it = m_objectTypes.find(name);
        if (it == m_objectTypes.end())
        {
            return boost::none;
        }

        owningPath = it->second.m_ownerPath;
        
        return it->second;
    }

    boost::optional<const DataInfo&> CCodeProject::findDataFromSnapshot(const std::string& name, std::string& owningPath)
    {
        auto it = m_objectTypesSnapshot.find(name);
        if (it == m_objectTypesSnapshot.end())
        {
            return boost::none;
        }

        owningPath = it->second.m_ownerPath;
        
        return it->second;
    }

    void CCodeProject::restoreFromSnapshot(const std::string& type)
    {
        std::string owningPath;
        auto archiveData = findDataFromSnapshot(type, owningPath);
        if(archiveData)
        {
            auto it = m_objectTypes.find(type);
            if(it != m_objectTypes.end())
            {
                m_objectTypes[type] = *archiveData;
            }
        }
    }

    void CCodeProject::dataSnapshot()
    {
        m_objectTypesSnapshot.clear();
        m_updatedData.clear();
        
        m_objectTypesSnapshot = m_objectTypes;
    }

    //Format: dataTypeName, set of changed members (empty set means new datatype)
    std::map<std::string, std::set<std::string>> CCodeProject::diffWithDataSnapshot()
    {
        std::map<std::string, std::set<std::string>> diff;
        
        for(auto def : m_objectTypes)
        {
            auto it = m_objectTypesSnapshot.find(def.first);
            if(it != m_objectTypesSnapshot.end())
            {
                for(auto member : def.second.m_typeDef.m_members)
                {
                    auto mIt = it->second.m_typeDef.m_members.find(member.first);
                    if(mIt == it->second.m_typeDef.m_members.end())
                    {
                        //If an mbember from current definition is not in the snapshot - it is new and can be modified
                        diff[def.first].insert(member.first);
                    }
                }
            }
            else
            {
                //New type, not in the snapshot
                diff[def.first] = std::set<std::string>();
            }
        }
        
        return diff;
    }

    std::string CCodeProject::dataChangesFromSnapshot(const std::map<std::string, std::set<std::string>>& diff)
    {
        std::string newTypes;
        std::string modifiedTypes;
        for(auto type : diff)
        {
            if(type.second.empty())
            {
                if(!newTypes.empty()) {
                    newTypes += ", ";
                }
                
                newTypes += type.first;
            }
            else
            {
                modifiedTypes += type.first;
                modifiedTypes += " (Modifiable members: ";
                bool membersEmpty = true;
                for(auto member : type.second)
                {
                    if(!membersEmpty) {
                        modifiedTypes += ", ";
                    }
                    modifiedTypes += member;
                    membersEmpty = false;
                }
                modifiedTypes += ")\n";
            }
        }
        
        std::string modifiable;

        if (!newTypes.empty()) {
            modifiable += "- Modifiable Data Types:\n"
                          "  All struct or enum fields of the following data types can be freely modified:\n"
                          "  " + newTypes + "\n";
        }

        if (!modifiedTypes.empty()) {
            modifiable += "- Restricted Data Types:\n"
                          "  Only the specified struct or enum fields of these data types may be modified:\n"
                          "  " + modifiedTypes + "\n";
        }

        if (modifiedTypes.empty() && newTypes.empty()) {
            modifiable += "- No Alteration of Existing Members:\n"
                          "  It is not permitted to modify any existing struct or enum fields of these data types; "
                          "only new members may be added.\n";
        } else {
            modifiable += "- For All Remaining Data Types:\n"
                          "  It is not permitted to modify existing struct or enum fields; only new members may be added.\n";
        }
        
        return modifiable;
    }

    boost::optional< DataInfo&> CCodeProject::addDataReferences(const std::string& type,
                                                            const std::set<std::string>& references)
    {
        auto it = m_objectTypes.find(type);
        if (it == m_objectTypes.end())
        {
            return boost::none;
        }
        
        it->second.m_references.insert(references.begin(), references.end());
        
        return it->second;
    }

    void CCodeProject::removeDataReference(const std::string& type, const std::string& reference)
    {
        auto it = m_objectTypes.find(type);
        if (it == m_objectTypes.end())
        {
            return;
        }
        
        it->second.m_references.erase(reference);
        
        if(it->second.m_references.empty())
        {
            m_objectTypes.erase(it);
        }
    }

    enum class SortResult {
        Success,
        CircularDependency
    };

    SortResult topologicalSort(const std::unordered_map<std::string, std::set<std::string>>& graph,
                               std::vector<std::string>& result,
                               std::string& error_message)
    {
        std::unordered_map<std::string, bool> visited;
        std::unordered_map<std::string, bool> inStack;
        result.clear();

        std::function<bool(const std::string&)> dfs = [&](const std::string& node) -> bool {
            visited[node] = true;
            inStack[node] = true;

            if (graph.count(node)) {
                for (const auto& neighbor : graph.at(node)) {
                    if (!visited[neighbor]) {
                        if (!dfs(neighbor)) {
                            return false;  // Propagate circular dependency detection
                        }
                    } else if (inStack[neighbor]) {
                        error_message = "Circular dependency detected: " + node + " -> " + neighbor;
                        return false;
                    }
                }
            }

            inStack[node] = false;
            result.push_back(node);
            return true;
        };

        for (const auto& node : graph) {
            if (!visited[node.first]) {
                if (!dfs(node.first)) {
                    return SortResult::CircularDependency;
                }
            }
        }

        return SortResult::Success;
    }

    bool CCodeProject::requiresFullDefinition(const std::string& path,
                                              const std::string& fullType,
                                              const std::string& testMember,
                                              const std::string& type) const
    {
        std::stringstream declaration;
        std::set<std::string> allTypes = getAppTypes(fullType);
        for(auto t : allTypes)
        {
            if(t == type) 
                continue;
            
            //All other types, except the verified 'type' have stub definition
            declaration << "struct " << t << " {};" << std::endl;
        }
        declaration << "struct TestStruct { " << fullType << " " << testMember << "; };" << std::endl;
        
        
        std::cout << "Checking declaration: " << std::endl << declaration.str();
        std::cout << "For full definition requirement of type '" << type << "'" << std::endl;
        
        std::stringstream code;
        
        for(auto include : CCodeProject::getSTDIncludes())
        {
            code << "#include <" << include << ">" << std::endl;
        }
        
        code << declareData(false, path) << std::endl;
        
        code << declaration.str() << std::endl;
        
        // Get the Clang version
        CXString clangVersion = clang_getClangVersion();
        std::string versionString = clang_getCString(clangVersion);
        // Remember to dispose of the CXString to free memory
        clang_disposeString(clangVersion);
        
        std::string codeStr = code.str();
        
        CXIndex index = clang_createIndex(0, 0);
        
        std::string sysroot = getSysRoot();
        std::string resourceDir = getClangResourceDir();
        std::string cxxInclude  = getCppInclude();
        std::string cxxIncludeOpt = "-I" + cxxInclude;
        
        const char* clang_args[] = {
            "-x", "c++",
            "-std=c++17",
            "-Werror=format",
            "-DCOMPILE_TEST",
            "-D_LIBCPP_HAS_NO_WIDE_CHARACTERS",//Without this we get "couldn't find stdarg.h" error
            "-isysroot", sysroot.c_str(),
            "-resource-dir", resourceDir.c_str(), // ← critical for stdarg.h, stdint.h, intrinsics, etc.
            cxxIncludeOpt.c_str(), // ← libc++ headers
        };
        
        CXUnsavedFile unsavedFile = { "ForwardCheck.cpp", codeStr.c_str(), (unsigned long)codeStr.length() };
        
        CXTranslationUnit unit = clang_parseTranslationUnit(
            index,
            "ForwardCheck.cpp",
            clang_args, sizeof(clang_args) / sizeof(clang_args[0]),
            &unsavedFile, 1,
            CXTranslationUnit_KeepGoing);

        if (unit == nullptr) {
            std::cerr << "Unable to parse translation unit." << std::endl;
            clang_disposeIndex(index);
            return true;
        }
        
        {
            CXTargetInfo targetInfo = clang_getTranslationUnitTargetInfo(unit);
            CXString triple = clang_TargetInfo_getTriple(targetInfo);
            clang_disposeString(triple);
        }
        
        std::string errors = printDiagnostics(unit, false);
        if(!errors.empty())
        {
            clang_disposeTranslationUnit(unit);
            clang_disposeIndex(index);
            return true;
        }

        CXCursor cursor = clang_getTranslationUnitCursor(unit);
        
        struct CountTypeRefs
        {
            int requiresForward = 0;
            int doesntRequireForward = 0;
            int requiresFullDefinition = 0;
            std::string type;
        } typeRefs;
        
        typeRefs.type = "struct " + type;
        
        clang_visitChildren(
        cursor,
        [](CXCursor c, CXCursor parent, CXClientData client_data) {
            CountTypeRefs* localTypeRefs = static_cast<CountTypeRefs*>(client_data);
            if (clang_getCursorKind(c) == CXCursor_TypeRef) {
                CXString spelling = clang_getCursorSpelling(c);
                std::string typeRef = clang_getCString(spelling);
                
                std::string fileName = getCursorFile(c);
                if (fileName == "ForwardCheck.cpp") {
                    if (localTypeRefs->type == typeRef) {
                        CXType cursorType = clang_getCursorType(c);
                        CXType canonicalType = clang_getCanonicalType(cursorType);
                        
                        if (canonicalType.kind == CXType_Elaborated) {
                            canonicalType = clang_Type_getNamedType(canonicalType);
                        }

                        if (canonicalType.kind == CXType_Record) {
                            // Check parent type and smart pointer usage
                            CXCursorKind parentKind = clang_getCursorKind(parent);
                            CXType parentType = clang_getCursorType(parent);

                            if (parentType.kind == CXType_Elaborated) {
                                parentType = clang_Type_getNamedType(parentType);
                            }

                            // Check for smart pointer usage
                            if (parentKind == CXCursor_CallExpr || parentKind == CXCursor_MemberRefExpr) {
                                // Method invocation, full definition required
                                localTypeRefs->requiresFullDefinition += 1;
                                std::cout << "requiresFullDefinition: " << typeRef << std::endl;
                            }
                            else if (parentType.kind != CXType_Pointer && parentType.kind != CXType_LValueReference) {
                                // Check for direct usage without pointer/reference
                                localTypeRefs->requiresForward += 1;
                                std::cout << "requiresForward: " << typeRef << std::endl;
                            }
                            else {
                                // If it's a pointer or reference, forward declaration suffices
                                localTypeRefs->doesntRequireForward += 1;
                                std::cout << "doesntRequireForward: " << typeRef << std::endl;
                            }
                        }
                    }
                }
                clang_disposeString(spelling);
            }
            return CXChildVisit_Recurse;
        },
        &typeRefs);

        clang_disposeTranslationUnit(unit);
        clang_disposeIndex(index);
        
        std::cout << std::endl;
        
        return typeRefs.requiresFullDefinition;
    }

    std::set<std::string> CCodeProject::findDependencies(const std::string& path, const TypeDefinition& typeDef) const
    {
        std::cout << "//***** Begin all dependencies for type: '" << std::endl;
        std::cout << typeDef.m_definition << std::endl;
        
        std::set<std::string> deps;
        std::string structDef = typeDef.m_definition;
        std::string removedComments;
        std::string definition = removeComments(structDef, removedComments);
        
        for(const auto& it : m_objectTypes)
        {
            if(it.second.m_typeDef.m_type == TypeDefinition::ENUM)
                continue;
            
            if(typeDef.m_name == it.first)
                continue;
            
            // Regex pattern to match the type, but not as a pointer or reference
            std::regex pattern("\\b" + it.first + "\\b(?!\\s*[&*])");
            if (std::regex_search(definition, pattern))
            {
                std::cout << "Value of type: '" << it.first << "' found in the definition" << std::endl;
                for(auto member : typeDef.m_members)
                {
                    if (std::regex_search(member.second, pattern))
                    {
                        if(requiresFullDefinition(path, member.second, member.first, it.first))
                        {
                            deps.insert(it.first);
                            break;
                        }
                    }
                }
            }
        }
        
        std::cout << "//***** End all dependencies for type: '";
        std::cout << typeDef.m_name << "' *****" << std::endl;
        
        return deps;
    }

    std::vector<std::string> CCodeProject::orderStructs(const std::string& path) const
    {
        std::vector<std::string> sortedStructs;
        if(!m_reorder)
        {
            for(const auto& it : m_objectTypes)
            {
                if(it.second.m_ownerPath == path &&
                   it.second.m_typeDef.m_type == TypeDefinition::STRUCT)
                {
                    sortedStructs.push_back(it.second.m_typeDef.m_name);
                }
            }
        }
        else
        {
            std::unordered_map<std::string, std::set<std::string>> dependencies;
            
            if(m_objectTypes.size())
            {
                std::cout << "BEGIN COLLECTING all dependencies for types in the path: " << path << std::endl;
                std::cout << "******************************************************************************" << std::endl;
                
                std::cout << "***** Data declaration snippet start *****" << std::endl;
                std::cout << declareData(false, path) << std::endl;
                std::cout << "***** Data declaration snippet end *****" << std::endl;
                // First pass: collect dependencies
                for(const auto& it : m_objectTypes)
                {
                    if(it.second.m_ownerPath == path &&
                       it.second.m_typeDef.m_type == TypeDefinition::STRUCT)
                    {
                        dependencies[it.first] = findDependencies(path, it.second.m_typeDef);
                    }
                }
                std::cout << "******************************************************************************" << std::endl;
                std::cout << "END COLLECTING all dependencies for types in the path: " << path << std::endl;
            }
            
            std::string error_message;
            SortResult result = topologicalSort(dependencies, sortedStructs, error_message);
            
            if (result == SortResult::CircularDependency) {
                // Handle circular dependency
                std::cout << "// ERROR: " << error_message << std::endl;
            }
        }
        
        return sortedStructs;
    }

    std::string CCodeProject::defineReferencedData(const std::string& path) const
    {
        std::stringstream sout;
        std::vector<std::string> sortedStructs =  orderStructs(path);

        // Output enums first (they don't have dependencies)
        for(const auto& it : m_objectTypes)
        {
            if(it.second.m_ownerPath == path &&
               it.second.m_typeDef.m_type == TypeDefinition::ENUM)
            {
                sout << it.second.m_typeDef.m_definition << ";" << std::endl << std::endl;
            }
        }

        // Output sorted structs
        for(const auto& structName : sortedStructs)
        {
            const auto& it = m_objectTypes.find(structName);
            if(it != m_objectTypes.end())
            {
                sout << it->second.m_typeDef.m_definition << ";" << std::endl << std::endl;
            }
        }
        
        return sout.str();
    }

    std::string CCodeProject::defineData(bool getDetached, const std::string& path) const
    {
        std::stringstream sout;
        std::set<std::string> structs;
        std::set<std::string> enums;
        std::set<std::string> owners;
        getVisibleTypes(getDetached, structs, enums, owners, path);
        
        for(auto e : enums)
        {
            std::string owningPath;
            auto dataDef = findData(e, owningPath);
            if(dataDef) //TODO: Actually we do expect the def to exist!!!
            {
                sout << dataDef->m_typeDef.m_definition << ";" << std::endl << std::endl;
            }
        }
        
        for(auto s : structs)
        {
            std::string owningPath;
            auto dataDef = findData(s, owningPath);
            if(dataDef) //TODO: Actually we do expect the def to exist!!!
            {
                sout << dataDef->m_typeDef.m_definition << ";" << std::endl << std::endl;
            }
        }
        
        return sout.str();
    }

    void CCodeProject::getVisibleTypes(bool getDetached,
                                       std::set<std::string>& structs,
                                       std::set<std::string>& enums,
                                       std::set<std::string>& owners,
                                       const std::string& path) const
    {
        for(const auto& it : m_objectTypes)
        {
            if(it.second.m_ownerPath == path ||
               (getDetached && it.second.m_ownerPath == "__DETACHED__"))
            {
                owners.insert(it.second.m_ownerPath);
                
                if(it.second.m_typeDef.m_type == TypeDefinition::STRUCT)
                {
                    //if(structs.find(it.first) == structs.end())
                    {
                        structs.insert(it.first);
                        //Reqursive call until we add all dependent datat types
                        //getVisibleTypes(getDetached, structs, enums, owners, it.second.m_ownerPath);
                    }

                    for(auto member : it.second.m_typeDef.m_members)
                    {
                        auto appTypes = getAppTypes(member.second);
                        for(auto type : appTypes)
                        {
                            auto itDef = m_objectTypes.find(type);
                            if (itDef != m_objectTypes.end())
                            {
                                owners.insert(itDef->second.m_ownerPath);
                                if(itDef->second.m_typeDef.m_type == TypeDefinition::STRUCT)
                                {
                                    //structs.insert(itDef->first);
                                    
                                    if(structs.find(itDef->first) == structs.end())
                                    {
                                        structs.insert(itDef->first);
                                        //Reqursive call until we add all dependent datat types
                                        getVisibleTypes(getDetached, structs, enums, owners, itDef->second.m_ownerPath);
                                    }
                                }
                                else
                                {
                                    enums.insert(itDef->first);
                                }
                            }
                        }
                    }

                }
                else
                {
                    enums.insert(it.first);
                }
            }
        }
        
        for(const auto& it : m_objectTypes)
        {
            if(owners.find(it.second.m_ownerPath) != owners.end())
            {
                if(it.second.m_typeDef.m_type == TypeDefinition::STRUCT)
                {
                    structs.insert(it.first);
                }
                else
                {
                    enums.insert(it.first);
                }
            }
        }
    }

    std::string CCodeProject::declareData(bool getDetached, const std::string& path) const
    {
        std::stringstream sout;
        
        std::set<std::string> structs;
        std::set<std::string> enums;
        std::set<std::string> owners;
        
        getVisibleTypes(getDetached, structs, enums, owners, path);
        
        for(auto e : enums)
        {
            sout << "enum class ";
            sout << e << " : uint32_t;" << std::endl;
        }
        
        for(auto s : structs)
        {
            sout << "struct ";
            sout << s << ";" << std::endl;
        }
        
        return sout.str();
    }

    std::string CCodeProject::defineDataSafe(const std::string& path) const
    {
        std::stringstream sout;
        std::vector<std::string> sortedStructs = orderStructs(path);
        
        // Output enums first (they don't have dependencies)
        for(const auto& it : m_objectTypes)
        {
            if(it.second.m_ownerPath == path &&
               it.second.m_typeDef.m_type == TypeDefinition::ENUM)
            {
                sout << "#if !defined(" << it.first << ")" << std::endl;
                sout << it.second.m_typeDef.m_definition << ";" << std::endl;
                sout << "#endif" << std::endl << std::endl;
            }
        }

        // Output sorted structs
        for(const auto& structName : sortedStructs)
        {
            const auto& it = m_objectTypes.find(structName);
            if(it != m_objectTypes.end())
            {
                sout << "#if !defined(" << it->first << ")" << std::endl;
                sout << it->second.m_typeDef.m_definition << ";" << std::endl;
                sout << "#endif" << std::endl << std::endl;
            }
        }
        
        return sout.str();
    }

    std::string CCodeProject::getDetachedData() const
    {
        std::vector<std::string> structTypes;
        std::vector<std::string> enumDefs;
        std::vector<std::string> structDefs;
        
        std::string definedData;
        
        for(const auto& it : m_objectTypes)
        {
            if(it.second.m_ownerPath == "__DETACHED__")
            {
                if(it.second.m_typeDef.m_type == TypeDefinition::ENUM)
                {
                    enumDefs.push_back(it.second.m_typeDef.m_definition);
                }
                else
                {
                    structTypes.push_back(it.first);
                    structDefs.push_back(it.second.m_typeDef.m_definition);
                }
            }
        }
        
        for(auto def : structTypes)
        {
            definedData += "struct ";
            definedData += def + ";\n";
        }
        
        if(!definedData.empty())
        {
            definedData += "\n";
        }
        
        for(auto def : enumDefs)
        {
            definedData += def;
            definedData += ";\n";
        }
        
        if(!definedData.empty())
        {
            definedData += "\n";
        }
        
        for(auto def : structDefs)
        {
            definedData += def;
            definedData += ";\n";
        }
        
        return definedData;
    }

    void CCodeProject::attachAllDataTo(const std::string& path)
    {
        for(auto& it : m_objectTypes)
        {
            if(it.second.m_ownerPath == "__DETACHED__")
            {
                it.second.m_ownerPath = path;
            }
        }
    }

    void CCodeProject::attachDataToExistingStructs()
    {
        // For each detached data type, search through existing attached structs.
        // If a struct contains members of the same data type,
        // attach the detached data type to that struct.
        for(auto& itDetached : m_objectTypes)
        {
            if(itDetached.second.m_ownerPath == "__DETACHED__")
            {
                bool found = false;
                
                for(auto& itExisting : m_objectTypes)
                {
                    if(itExisting.second.m_ownerPath == "__DETACHED__") {
                        continue;
                    }
                    
                    //Is this possible at all?
                    if(itDetached.first == itExisting.first) {
                        continue;
                    }
                    
                    for(auto member : itExisting.second.m_typeDef.m_members)
                    {
                        if(member.second.find(itDetached.first) != std::string::npos)
                        {
                            itDetached.second.m_ownerPath = itExisting.second.m_ownerPath;
                            found = true;
                            break;
                        }
                    }
                    
                    if(found) {
                        break;
                    }
                }
            }
        }
    }

    const std::set<std::string>&  CCodeProject::updateData(const TypeDefinition& typeDef,
                                    const std::string& param,
                                    const std::string& desc,
                                    std::string& requestingPath)
    {
        std::string owningPath;
        
        if(getCppIdentifiers().find(typeDef.m_name) != getCppIdentifiers().end())
        {
            //WE MUST NOT BE HERE!
            std::cout << "DATA NAME IN CONFLICT WITH LIBRARIES: " << typeDef.m_name << std::endl;
        }

        DataInfo info;
        auto it = m_objectTypes.find(typeDef.m_name);
        if (it != m_objectTypes.end() &&
            it->second.m_ownerPath != "__DETACHED__")
        {
            owningPath = it->second.m_ownerPath;
            info = it->second;
        }
        else {
            owningPath = requestingPath;
        }
        
        info.m_typeDef = typeDef;
        //info.m_description = desc;
        info.m_ownerPath = owningPath;
        
        if(!param.empty())
        {
            info.m_references.insert(param);//The format is path:parameter
        }
        
        m_objectTypes[typeDef.m_name] = info;
        m_updatedData.insert(typeDef.m_name);
        
        requestingPath = owningPath;
        return m_objectTypes[typeDef.m_name].m_references;
    }

    void CCodeProject::generateCommonFiles(const std::string& subdir)
    {
        std::string fileName = getProjDir() + "/" + subdir;
        boost_fs::create_directories(fileName);
        fileName += "/common.h";
        
        std::ofstream header(fileName);
        
        header << "#pragma once" << std::endl << std::endl;
        
        for(auto include : CCodeProject::getSTDIncludes())
        {
            header << "#include <" << include << ">" << std::endl;
        }
        
        header << std::endl << std::endl;
        
        header << m_common_header;
        
        header.close();
    }

    void CCodeProject::generateProjectFiles()
    {
        std::string artifactDir = getProjDir() + "/project";
        std::string sourcesDirectory = getProjDir() + "/project/sources";
        
        boost_fs::create_directory(artifactDir);
        boost_fs::create_directory(sourcesDirectory);
        
        generateCommonFiles("project/sources");
        
        m_dag.depthFirstTraversal(m_dag.m_root, [this](DAGNode<Node*>* node, DAGraph<Node*>& g) {},
            [this](DAGNode<Node*>* node) {
            if(node->m_data)
            {
                CCodeNode* ccNode = (CCodeNode*)node->m_data;
                ccNode->generateProjectSources();
            }
            });
    }

    std::pair<std::string, std::vector<std::string>>
    CCodeProject::generateTestScript(const std::string& testJsonPath)
    {
        //------------------------------------------------------------------
        // 0.  Load the TestDef from test.json
        //------------------------------------------------------------------
        TestDef test;
        {
            std::ifstream f(testJsonPath + "/test.json");
            const std::string jsonStr{ std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
            test.from_json(web::json::value::parse(utility::conversions::to_string_t(jsonStr)));
        }

        //------------------------------------------------------------------
        // 1.  Gather input/output file sets
        //------------------------------------------------------------------
        std::set<std::string> inputSet, outputSet;
        auto collect = [&](const TestStep& step)
        {
            for (auto f : step.input_files)  inputSet.insert(*f);
            for (auto f : step.output_files) outputSet.insert(*f);
        };
        
        collect(test.pretest);
        
        for (auto f : test.test.input_files)  inputSet.insert(*f);
        for (auto f : test.test.output_files) outputSet.insert(*f);
        
        collect(test.posttest);

        //------------------------------------------------------------------
        // 2.  Compute read-only files = input − output
        //------------------------------------------------------------------
        std::vector<std::string> readonly;
        std::set_difference(inputSet.begin(), inputSet.end(),
                            outputSet.begin(), outputSet.end(),
                            std::back_inserter(readonly));

        //------------------------------------------------------------------
        // 3. Render the shell script
        //------------------------------------------------------------------
        std::ostringstream sh;
        sh << "#!/usr/bin/env bash\n"
           //<< "set -euo pipefail\n\n"
           << "# ------------------------------------------------------------\n"
           << "# Test: " << test.name << '\n';

        if (!test.description.empty()) {
            sh << "# Description:\n";
            std::istringstream d(test.description);
            std::string line;
            while (std::getline(d, line)) sh << "#   " << line << '\n';
        }
        sh << "# ------------------------------------------------------------\n\n";

        auto appendCommand = [&](const std::string& tag, const std::string& command)
        {
            bool debug = false;
            bool finalResult = false;
            std::string strippedCmd = command;
            std::string expectedResult;
            std::string stdoutRegex;
            
            parsePrefixFlags(command, debug, finalResult, expectedResult, stdoutRegex, strippedCmd);
            
            if (tag == "TEST")
            {
                strippedCmd = removeFirstWord(strippedCmd, "main");
            }
            
            if(finalResult)
            {
                strippedCmd += " && printf 'RESULT %d\n' 0 || printf 'RESULT %d\n' \"$?\"";
            }
            
            //copies everything except the exact pattern \"
            std::string fixed = std::regex_replace(strippedCmd, std::regex(R"(\\")"), "\"");
            sh << fixed << '\n';
            
            if (finalResult && !expectedResult.empty())
            {
                sh << "echo \"EXPECTED_RESULT=\\\"" << expectedResult << "\\\"\"\n";
            }
            
            //TODO: Do the actual test and print whether it matches
            if(finalResult && !stdoutRegex.empty())
            {
                sh << "echo \"EXPECTED_STDOUT_MATCH=\\\"" << stdoutRegex << "\\\"\"\n";
            }
        };
        // -----------------------------------------------------------------
        // 3a.  SELF-EXTRACTION  (works with the awk bundled on macOS)
        // -----------------------------------------------------------------
        sh << "CPP_FILE=\"${CPP_FILE:-$(dirname \"$0\")/" << m_description.name << "." << PRODUCT_FILENAME_EX << ".cpp}\"\n"
           << "awk '/^#ifdef[ \\t]*__EMBEDDED_TESTS/ {inside=1; next}\n"
              "     inside && /^#endif/              {inside=0}\n"
              "     inside && /\\/\\/\\[\\[file:/ {\n"
              "         fname=$0;\n"
              "         sub(/^.*\\[\\[file:[ \\t]*/, \"\", fname);\n"
              "         sub(/\\]\\].*$/,            \"\", fname);\n"
              "         gsub(/[ \\t]/, \"\", fname);\n"
              "         if (fname==\"test.sh\") { getline; while (getline && $0!~/^```/); next }\n"
              "         getline;                                  # ← only ONE getline now\n"
              "         while (getline && $0!~/^```/) print > fname;\n"
              "         close(fname);\n"
              "     }' \"$CPP_FILE\"\n\n";

        // -----------------------------------------------------------------
        // 3b.  Run PRETEST / TEST / POSTTEST sections
        // -----------------------------------------------------------------
        auto appendBlock = [&](const std::string& tag, const TestStep& step)
        {
            sh << "echo \"--- " << tag << " ---\"\n";
            if (tag == "TEST") sh << "./" << m_description.name << ' ';
            for (const auto& cmd : step.commands)
            {
                if (!cmd->empty())
                {
#if 0
                    bool debug = false;
                    bool finalResult = false;
                    std::string strippedCmd = *cmd;
                    std::string expectedResult;
                    std::string stdoutRegex;
                    parsePrefixFlags(*cmd, debug, finalResult, expectedResult, stdoutRegex, strippedCmd);
                    
                    if(finalResult)
                    {
                        strippedCmd += " && printf 'RESULT %d\n' 0 || printf 'RESULT %d\n' \"$?\"";
                    }
                    
                    //copies everything except the exact pattern \"
                    std::string fixed = std::regex_replace(strippedCmd, std::regex(R"(\\")"), "\"");
                    sh << fixed << '\n';
                    
                    if (finalResult && !expectedResult.empty())
                    {
                        sh << "echo \"EXPECTED_RESULT=\\\"" << expectedResult << "\\\"\"\n";
                    }
                    
                    //TODO: Do the actual test and print whether it matches
                    if(finalResult && !stdoutRegex.empty())
                    {
                        sh << "echo \"EXPECTED_STDOUT_MATCH=\\\"" << stdoutRegex << "\\\"\"\n";
                    }
#else
                    appendCommand(tag, *cmd);
#endif
                }
            }
            sh << '\n';
        };

        appendBlock("PRETEST",  test.pretest);
        //appendBlock("TEST",     test.test);
        appendCommand("TEST", test.test.command);
        appendBlock("POSTTEST", test.posttest);

        return { sh.str(), std::move(readonly) };
    }

    std::string CCodeProject::generateEmbeddedTest()
    {
        std::string embeddedTest;
        
        std::string testDir = getProjDir() + "/tests/default/public";
        
        if(!boost_fs::exists(testDir))
        {
            return std::string();
        }
            
        auto scriptAndFiles = CCodeProject::generateTestScript(testDir);
        
        if(scriptAndFiles.first.empty())
            return embeddedTest;
        
        embeddedTest += "#ifdef __EMBEDDED_TESTS //Don't define __EMBEDDED_TESTS! The only purpose of this macro is to hide the tests from compilation and to provide a hint for test files extractiоn\n";
        embeddedTest += "//[[file: test.sh]]\n";
        embeddedTest += "```sh\n";
        embeddedTest += scriptAndFiles.first;
        embeddedTest += "\n```\n";
        
        for(auto file : scriptAndFiles.second)
        {
            std::string filePath = testDir + "/" + file;
            boost_fs::path fsFilePath = filePath;
            if(!boost_fs::exists(fsFilePath)) {
                continue;
            }
            
            std::string ext;
            if (fsFilePath.has_extension())          // guard against “no-extension” files
            {
                ext = fsFilePath.extension().string();   // e.g. ".txt"
                if (!ext.empty() && ext.front() == '.')
                    ext.erase(0, 1);                    // -> "txt"
            }
            
            if(ext.empty()) {
                //Default to .txt but it is concerning
                ext = "txt";
            }
            
            std::ifstream ifFile(filePath);
            std::string fileContent((std::istreambuf_iterator<char>(ifFile)), std::istreambuf_iterator<char>());
            
            if(fileContent.empty()) {
                continue;
            }
            
            embeddedTest += "//[[file: " + file + "]]\n";
            embeddedTest += "```" + ext + "\n";
            embeddedTest += fileContent;
            embeddedTest += "\n```\n";
        }
        
        embeddedTest += "#endif //__EMBEDDED_TESTS\n\n";
        
        return embeddedTest;
    }

    void CCodeProject::generateSingleSourceFile()
    {
        std::string fileName = m_description.name + "." + PRODUCT_FILENAME_EX + ".cpp";
        std::string filePath = getProjDir() + "/" + fileName;
        
        boost_fs::remove(filePath);
        
        std::ofstream cpp(filePath);
        
        //cpp << "#pragma once" << std::endl;
        std::string copyright = Peer::getHeader();
        printAsComment(copyright, cpp);
        cpp << std::endl;
        
        std::string disclamer = Peer::getDisclamer();
        printAsComment(disclamer, cpp);
        cpp << std::endl;
        
        std::string description = Peer::getProductDescription();
        printAsComment(description, cpp);
        cpp << std::endl;
        
        cpp << "//The following project description and test cases are the ONLY inputs provided to the " << MERCH_PRODUCT_NAME;
        cpp << ". Everything eles is autonomuosly generated!" << std::endl;
        
        printAsComment(m_description.description, cpp);
        cpp << std::endl;
        
        std::string embeddedTestSection = generateEmbeddedTest();
        cpp << embeddedTestSection << std::endl;
        
        std::string commandRelease = "clang++ -std=c++17 -O3 -o " + m_description.name + " " + fileName;
        std::string commandDebug = "clang++ -std=c++17 -Werror=format -g -O0 -DCOMPILE_TEST -o " + m_description.name + " " + fileName;
        
        cpp << "//To compile the project use the following command line" << std::endl;
        cpp << "//Debug: " << commandDebug << std::endl;
        cpp << "//Release: " << commandRelease << std::endl << std::endl;
        
        cpp << R"(// To test the project run: cpp=)"
            << m_description.name << "." << PRODUCT_FILENAME_EX << ".cpp; "
            << R"(awk '/\/\/\[\[file:[ \t]*test\.sh[ \t]*\]\]/{getline;getline;while(getline&&$0!~/^```/){print}}' "$cpp" > test.sh && chmod +x test.sh && ./test.sh
        )";
        
        cpp << std::endl;
        
        std::string asciGraph = printGraph({}, -1, true);
        cpp << "/*" << std::endl;
        cpp << asciGraph;
        cpp << "*/" << std::endl << std::endl;
        
        for(auto include : CCodeProject::getSTDIncludes())
        {
            cpp << "#include <" << include << ">" << std::endl;
        }
        cpp << std::endl << std::endl;
        cpp << m_common_header << std::endl;
        
        cpp << listAllEnumDefinitions();
        cpp << listAllStructDeclarations();
        cpp << std::endl;
        cpp << listAllStructDefinitions();
        cpp << std::endl;
        
        std::string functions = listAllFunctions({}, -1, true, true, true, {});
        cpp << functions << std::endl << std::endl;
        
        std::string source = listAllFunctionsSource();
        cpp << source << std::endl;
    }

    std::string CCodeProject::printGraph(const std::string& root, int maxDepth, bool desc)
    {
        if(!m_dag.m_root || !m_dag.m_root->m_data)
        {
            return std::string();
        }
        
        // Build the complete graph by traversing everything
        GraphPrinter printer;
        
        // First pass: traverse the entire DAG and collect ALL relationships
        
        printer.addNode(m_dag.m_root->m_data->getName(), "");
        
        m_dag.depthFirstTraversal(m_dag.m_root->m_data->m_this,
        [this, &printer](DAGNode<Node*>* node, DAGraph<Node*>& g)
        {
            auto _ccnode = (const CCodeNode*)node->m_data;
            for(auto func : _ccnode->m_calls.items)
            {
                const CCodeNode* ccNode = nullptr;
                auto it = nodeMap().find(func->func_name);
                if(it != nodeMap().end())
                {
                    ccNode = (const CCodeNode*)it->second;
                }
                
                if(!ccNode)
                    continue;
                
                //handleNode(ccNode, depth+1);
                printer.addNode(ccNode->getName(), _ccnode->getName());
            }
        });
        
        std::string asciiGraph;
        if(desc)
        {
            asciiGraph += "Printing ASCII application call graph\n";
            if(!root.empty())
            {
                asciiGraph += "Call graph root function: " + root + "\n";
            }
            
            if(maxDepth > 0)
            {
                asciiGraph += "Maximum depth for the call graph limited to: " + std::to_string(maxDepth) + "\n";
            }
            
            asciiGraph += "Notation:\n";
            asciiGraph += "(cycle)  - Cyclic dependency detected. The function is called directly or indirectly by itself\n";
            asciiGraph += "...      - The function called and already expanded by other functions previously printed\n";
            asciiGraph += "+        - The call graph under this function is collapsed due to print max depth limit\n\n";
            
            asciiGraph += "Note that some of the called functions (sub-nodes) under a given function might not be actually called in its implementation!\n\n";
        }
        
        // Print from specific node or all roots
        if (!root.empty()) {
            CCodeNode* ccRootFound = getNodeByName(root);
            if(ccRootFound) {
                asciiGraph += printer.print(ccRootFound->getName(), maxDepth);
            } else {
                asciiGraph += "Error: Function '" + root + "' not found\n";
            }
        } else {
            // Print all roots
            asciiGraph += printer.print(m_dag.m_root->m_data->getName(), maxDepth);
        }
        
        asciiGraph += "\n";
        
        return asciiGraph;
    }

    std::string CCodeProject::printNewNodes(const std::string& root, const std::set<std::string>& prevSnapshot)
    {
        if(!m_dag.m_root || !m_dag.m_root->m_data)
        {
            return std::string();
        }
        
        std::set<std::string> newSnapshot = getNodeNames();
        
        std::set<std::string> newNodes;
        // Find new entries (newSnapshot - prevSnapshot)
        std::set_difference(newSnapshot.begin(), newSnapshot.end(),
                            prevSnapshot.begin(), prevSnapshot.end(),
                            std::inserter(newNodes, newNodes.begin()));
        
        if(!newNodes.empty())
        {
            GraphPrinter  printer;
            
            //Expricitly add the root
            printer.addNode(m_dag.m_root->m_data->getName(), "");
            
            m_dag.depthFirstTraversal(m_dag.m_root,
            [this, &printer](DAGNode<Node*>* node, DAGraph<Node*>& g)
            {
                for(auto ref : node->m_data->m_referencedBy)
                {
                    printer.addNode(node->m_data->getName(), ref->getName());
                }
            });
            
            std::string asciiGraph = printer.print(root, newNodes, "New");
            
            return asciiGraph;
        }
        
        return std::string();
    }

    void CCodeProject::setupBuild()
    {
        saveReferences();
        saveDataDefinitions();
        
        for(auto obj : m_objectTypes)
        {
            //This is a bad thing but not critical
            if(obj.second.m_ownerPath == "__DETACHED__") {
                std::cout << "__DETACHED__ object is still in the project: " << obj.first << std::endl;
            }
        }
        
        //Clean the leftover detached objects at this point
        for (auto it = m_objectTypes.begin(); it != m_objectTypes.end(); ) {
            if (it->second.m_ownerPath == "__DETACHED__") {
                it = m_objectTypes.erase(it);
            } else {
                ++it;
            }
        }
        
        std::string asciiGraph = printGraph({}, -1, true);
        
        std::stringstream report;
        report << "\n\nCRAFTING...\n\n";
        report << "The problem has been fully decomposed!" << std::endl << std::endl;
        report << asciiGraph << std::endl;
        report << "Proceeding to the integration phase. Please, do expect bumpy ride..." << std::endl << std::endl;
        
        std::cout << std::endl << "The problem has been fully decomposed!" << std::endl << std::endl;
        std::cout << asciiGraph << std::endl;
        std::cout << "Proceeding to the integration phase..." << std::endl << std::endl;
        
        Client::getInstance().agentToServer(report.str());
        
        generateCommonFiles("build");
        
        compileCommonHeader(CCodeNode::BUILD_PRINT_TEST | CCodeNode::BUILD_DEBUG);
    }

    bool CCodeProject::buildBinary(bool enableSanitizer)
    {
        Client& client = Client::getInstance();
        std::string buildDir = getProjDir() + "/build";
        std::string platform = getPlatform() + "_test";
        std::string binDir = buildDir + "/" + platform;
        
        //TODO: Use project name for the name of the executable
        std::string executable = binDir + "/";
        executable += m_dag.m_root->m_data->getName();
        
        boost_fs::remove(executable);
        std::string command = "clang++ -v -std=c++17 -arch arm64 -o " + executable;
        
        if(enableSanitizer)
        {
            command += " -fsanitize=address,undefined";
        }
        
        for (const auto& file : boost_fs::directory_iterator(binDir))
        {
            if (file.path().extension() == ".o") {
                command += " " + file.path().string();
            }
        }
        
        stdrave::exec(command, binDir, "Link", false);
        
        return boost_fs::exists(executable);
    }

    bool CCodeProject::buildUnitTest(const std::string& function, bool enableSanitizer)
    {
        CCodeNode* ccNode = getNodeByName(function);
        
        std::string buildSourcePath = ccNode->getNodeBuildSourcePath();
        std::string buildDir = getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testExec = nodeDir + "/test/main";
        
        boost_fs::remove(testExec + ".o");
        boost_fs::remove(testExec);
        
        return ccNode->rebuildUnitTest(true);
    }

    std::multimap<uint32_t, std::string, std::greater<uint32_t>>
    CCodeProject::generateUnitTests(const std::string& finalTestPath, const std::string& prevFinalTestPath, std::string& fullTestRecommendation)
    {
        Client& client = Client::getInstance();
        
        std::multimap<uint32_t, std::string, std::greater<uint32_t>> sortedFunctions;
        std::vector<std::string> topFunctions = listAllFunctions("", UNIT_TEST_FUNCTIONS_DEPTH, {});
        
        std::string subSystems;
        for(auto func : topFunctions)
        {
            if(func == "main")
            {
                continue;
            }
            
            CCodeNode* ccNode = (CCodeNode*)getNodeByName(func);
            if(!ccNode) continue;
            
            std::vector<std::string> children = listAllFunctions(func, -1, {});
            
            //We try to generating tests only for functions that seem to be sub-systems.
            //Unlikely a given function to be a sub-system if it has few children
            //TODO: Tweak this value if needed
            if(children.size() < 4) continue;
            
            uint32_t depth = ccNode->getDepth();
            sortedFunctions.insert({depth, func});
            if(!subSystems.empty())
            {
                subSystems += ", ";
            }
            subSystems += func;
        }
        
        //std::string recommendation;
        TestDef finalTest;
        Distillery::getInstance().clear();
        if(Distillery::getInstance().loadTrajectory(this, finalTestPath, 0 , -1) &&
           finalTest.load(finalTestPath + "/test.json"))
        {
            std::string trajectory = Distillery::getInstance().printTrajectory();
            
            std::string systemData = getHighLevelAppInfo("main", UNIT_TEST_FUNCTIONS_DEPTH, UNIT_TEST_FUNCTIONS_DEPTH);
            
            std::string fullTest = finalTest.getDescription(finalTestPath);
            
            std::string testFrameworkMan = getFileContent(client.getEnvironmentDir() + "/Prompts/TestFramework.txt");
            
            systemData += "\n\nSome of the above functions are designated as subsystems:\n\n";
            systemData += subSystems + "\n\n";
            systemData += "For each of these sub-system functions we are going to implement an unit test (by wrapping it in a main.cpp file). ";
            systemData += "The unit tests will be implemented based on our bespoke test framework (see TEST FRAMEWORK MANUAL)\n\n";
            
            Prompt testRecommendation("TestRecommendation.txt", {
                {"trajectory", trajectory},
                {"test_framework_manual", testFrameworkMan},
                {"full_test", fullTest},
                {"system_data", systemData}});
            
            
            Cache cache;
            bool truncated = false;
            std::string reco = "review";
            
            //TODO: push/pop context
            captureContext(std::string());
            
            inference(cache, testRecommendation.str(), reco, &truncated);
            
            popContext();
            //TODO: Consider validation loop: too long/short, self-review, etc
            
            fullTestRecommendation = "ANALYSIS AND RECOMMENDATION FROM THE LAST DEBUGGING SESSION OF THE FULL APPLICATION TEST\n\n";
            fullTestRecommendation += reco + "\n\n";
        }
        
        std::multimap<uint32_t, std::string, std::greater<uint32_t>> testedFunctions;
        for(auto func : sortedFunctions)
        {
            CCodeNode* ccNode = (CCodeNode*)getNodeByName(func.second);
            if(!ccNode) continue;
            
            ccNode->deleteUnitTest(); //Ensure we first delete the old one
            
            ccNode->buildUnitTest(finalTestPath, prevFinalTestPath, fullTestRecommendation);
            
            if(ccNode->unitTestExists())
            {
                testedFunctions.insert({func.first, func.second});
            }
        }
        
        return testedFunctions;
    }

    uint32_t CCodeProject::archiveTest(const std::string& testPath, std::string& trajectoryDir)
    {
        TestDef unitTestDef;
        unitTestDef.load(testPath + "/test.json");
        std::string testDebugDir = getProjDir() + "/debug/" + unitTestDef.name;
        trajectoryDir = testDebugDir + "/trajectory";
        if(!boost_fs::exists(trajectoryDir))
        {
            return 1;
        }
        
        uint32_t archIndex = (uint32_t)nextIndex(testDebugDir, "archive");
        std::string archiveDir = testDebugDir + "/archive" + std::to_string(archIndex);
        
        boost::system::error_code ec;
        boost_fs::copy(trajectoryDir, archiveDir,
                       boost_fs::copy_options::recursive |
                       boost_fs::copy_options::overwrite_existing, ec);
        
        boost_fs::remove_all(trajectoryDir);
        
        //Logs
        //For now just delete logs
        std::string logsDebugDir = getProjDir() + "/logs/debug/" + unitTestDef.name;
        boost_fs::remove_all(logsDebugDir);
        
        return archIndex + 1;//Old archives plus the current trajectory
    }

    void CCodeProject::debugTests()
    {
        std::string testDirecotry = getProjDir() + "/tests";
        
        web::json::value jsonConfig;
        loadJson(jsonConfig, testDirecotry + "/config.json");
        TestConfig config;
        config.from_json(jsonConfig);
        
        std::string regressionTestPath;
        
        bool started = false;
        
        //Ramp up in the problem space
        for(auto test : config.ramp)
        {
            std::string filePath = testDirecotry + "/" + *test;
            if(!boost_fs::exists(filePath))
            {
                continue;
            }
            
            if(config.current == *test)
            {
                started = true;
            }
            
            if(!started)
            {
                regressionTestPath = filePath;
                continue;
            }
            
            config.current = *test;
            saveJson(config.to_json(), testDirecotry + "/config.json");
            
            std::string publicTestPath = filePath + "/public";
            if(boost_fs::exists(publicTestPath))
            {
                std::string privateTestPath = filePath + "/private";
                if(!boost_fs::exists(privateTestPath))
                {
                    privateTestPath.clear();
                }
                
                std::string prevPublicTestPath;
                if(!regressionTestPath.empty())
                {
                    prevPublicTestPath = regressionTestPath + "/public";
                }
                std::string fullTestRecommendation;
                
                for(int i=0; i<2; ++i)
                {
#if 1 //this is basically ramp up the solution space via unit tests
                    
                    //Here we need to build/update unit tests and initially compile and link them
                    std::multimap<uint32_t, std::string, std::greater<uint32_t>> unitTests;
                    if (!config.current_unit_test.empty() && config.ramp_unit_tests.size() > 0)
                    {
                        generateDataHeader();
                        // TODO: handle starting from specific unit test if needed
                        for(auto test : config.ramp_unit_tests)
                        {
                            std::vector<std::string> functionAndDepth;
                            boost::split(functionAndDepth, *test, boost::is_any_of(":"));
                            
                            CCodeNode* ccNode = (CCodeNode*)getNodeByName(functionAndDepth[0]);
                            if(!ccNode) continue;
                            
                            //Ensure all required files are in the test directory
                            ccNode->storeUnitTestContent();
                            buildUnitTest(functionAndDepth[0], true);
                            
                            auto testPair = std::make_pair((uint32_t)std::atoi(functionAndDepth[1].c_str()), functionAndDepth[0]);
                            unitTests.insert(testPair);
                        }
                    }
                    else
                    {
                        unitTests = generateUnitTests(publicTestPath, prevPublicTestPath, fullTestRecommendation);
                        
                        std::string commitMessage = config.current + "_unit_tests";
                        std::string afterUnitTestsCommit = commit(commitMessage);
                        
                        //Setup and record the config with generated unit tests
                        std::string utKey;
                        for(auto test : unitTests)
                        {
                            std::string testKey = test.second + ":" + std::to_string(test.first);
                            
                            //pickup the first as starting point
                            if(utKey.empty())
                            {
                                utKey = testKey;
                            }
                            
                            config.ramp_unit_tests.push_back(std::make_shared<std::string>(testKey));
                        }
                        
                        config.current_unit_test = utKey;
                        saveJson(config.to_json(), testDirecotry + "/config.json");
                    }
                    
                    bool utStarted = false;
                    for(auto test : unitTests)
                    {
                        std::string utKey = test.second + ":" + std::to_string(test.first);
                        
                        std::string unitTestPath = getProjDir() + "/build/source/" + test.second + "/test";
                        
                        if(utKey == config.current_unit_test)
                        {
                            utStarted = true;
                        }
                        
                        if(!utStarted)
                        {
                            continue;
                        }
                        
                        //Archive the previous trajectory
                        std::string trajectoryDir;
                        uint32_t utAttempt = archiveTest(unitTestPath, trajectoryDir);
                        
                        std::string branchName = "before_" + config.current + "_" + std::to_string(utAttempt);
                        
                        branchName += "_" + test.second + "_" + std::to_string(utAttempt);
                        std::string beforeTheUTest = createBranchFromCurrent(getProjDir() + "/dag", branchName);
                        
                        config.current_unit_test = utKey;
                        saveJson(config.to_json(), testDirecotry + "/config.json");
                        
                        bool uintTestPass = false;
                        bool hasBeenReset = false;
                        int j=0;
                        while(j<3)
                        {
                            CCodeNode* ccNode = (CCodeNode*)getNodeByName(test.second);
                            if(!ccNode->unitTestExists())
                            {
                                std::cout << "Unit test doesn't exist: " << ccNode->getName() << std::endl;
                                break;
                            }
                            
                            ccNode->storeUnitTestContent();
                            
                            auto dbgResult = Debugger::getInstance().debug(this,
                                                                         100,
                                                                         test.second,
                                                                         unitTestPath,
                                                                         std::string(),
                                                                         std::string(),
                                                                         Client::getInstance().getDebugPort());
                            
                            uintTestPass = dbgResult.first;
                            
                            if(uintTestPass)
                            {
                                //TODO: Consider learning after each unit test
                                //Distillery::getInstance().distillTrajectory(this, unitTestPath, 0 , -1);
                                break;
                            }
                            else
                            {
                                //Check if the unit test needs improvements
                                Distillery::getInstance().clear();
                                Distillery::getInstance().loadTrajectory(this, unitTestPath, 0 , -1);
                                std::string trajectory = Distillery::getInstance().printTrajectory();
                                
                                TestDef fullTest;
                                fullTest.load(publicTestPath + "/test.json");
                                
                                std::string fullTestDesc = fullTest.getDescription(publicTestPath);
                                
                                captureContext("");
                                
                                bool isBroken = ccNode->unitTestIsBroken(trajectory, fullTestDesc, dbgResult.second);
                                bool stopTest = false;
                                if(isBroken && !hasBeenReset && j < 2)
                                {
                                    //Destructive hard reset
                                    std::string revertToCommit = resetBranchToBranchedFromCommit(getProjDir() + "/dag", branchName);
                                    assert(revertToCommit == beforeTheUTest);
                                    j=0;
                                    hasBeenReset = true;
                                    
                                    //Get pointer to the new node after reload
                                    ccNode = getNodeByName(test.second);
                                    ccNode->improveUnitTest();
                                }
                                else
                                {
                                    j++;
                                    stopTest = isBroken;
                                }
                                
                                popContext();
                                
                                if(stopTest) break;
                            }
                        }
                        
                        if(!uintTestPass)
                        {
                            //Inform, but continue with other unit tests
                            std::cout << "Couldn't pass the unit test: " << test.second << std::endl;
                        }
                    }
#endif
                    
                    //We start with different unit tests each run. So it can't be continuation of the previous trajectory!
                    std::string trajectoryDirFullTest;
                    archiveTest(publicTestPath, trajectoryDirFullTest);
                    
                    auto dbgResult = Debugger::getInstance().debug(this, -1,
                                                                  "main",
                                                                  publicTestPath,
                                                                  privateTestPath,
                                                                  regressionTestPath,
                                                                  Client::getInstance().getDebugPort());
                    
                    bool testPass = dbgResult.first;
                    if(testPass)
                    {
                        //Distillery::getInstance().distillTrajectory(this, publicTestPath, 0 , -1);
                        //TODO: Learn
                        
                        break;
                    }
                    else
                    {
                        //Delete unit tests so we can recreate them in the next attempt but with
                        //analysis and recommendations from the recently failed trajectory
                        config.current_unit_test.clear();
                        config.ramp_unit_tests.clear();
                        saveJson(config.to_json(), testDirecotry + "/config.json");
                    }
                }
                
                //Delete unit tests from the config
                config.current_unit_test.clear();
                config.ramp_unit_tests.clear();
                saveJson(config.to_json(), testDirecotry + "/config.json");
                
                regressionTestPath = filePath;
            }
        }
        
        //Let's start with the first test again
        config.current = *config.ramp.at(0);
        saveJson(config.to_json(), testDirecotry + "/config.json");
    }

    void CCodeProject::finalizeBuild()
    {
        Client& client = Client::getInstance();
        std::string buildDir = getProjDir() + "/build";
        std::string platform = getPlatform() + "_test";
        std::string binDir = buildDir + "/" + platform;
        
        buildBinary(true);
        
        saveDataDefinitions();
        
        Client::getInstance().agentToServer("\n\nDEBUGGING...\n\n");
        
        debugTests();
        
        generateSingleSourceFile();
        
        {
            Client::getInstance().agentToServer("\n\nDELIVERING...\n\n");
            
            generateProjectFiles();
            
            std::string sourcesDirectory = getProjDir() + "/project/sources";
            
            client.setLLM(LLMRole::DIRECTOR);
            
            std::string functionsList;
            for(const auto& node : m_nodeMap)
            {
                auto ccNode = (const CCodeNode*)node.second;
                
                if(!functionsList.empty())
                {
                    functionsList += "\n";
                }
                
                functionsList += ccNode->m_prototype.brief + "\n";
                functionsList += ccNode->m_prototype.declaration + "\n";
                
                if(ccNode->getDepth() <= 3)
                {
                    functionsList += "Stack: " + ccNode->getDAGPath(">") + "\n";
                    functionsList += ccNode->m_prototype.description+ "\n";
                }
            }
            
            std::string generateArtifactsMsg = generate_artifacts.prompt({
                                                                         {"functions_list", functionsList}
                                                                         });
            ProjectArtifact artifact;
            std::string cache;
            
            client.enableLog(false);
            
            //I really don't like this
            CCodeNode* root = (CCodeNode*)m_dag.m_root->m_data;
            root->captureContext();
            root->inference<ProjectArtifact>(cache, generateArtifactsMsg, &artifact);
            root->popContext();
            
            std::map<std::string, std::vector<std::string>> groups;
            for(auto group : artifact.file_groups)
            {
                for(auto file : group->files)
                {
                    groups[group->group_name].push_back(*file);
                }
            }
            
            cache.clear();
            
            std::string readme = "⚠️ IMPORTANT DISCLAIMER!\n\n";
            readme += Peer::getDisclamer();
            readme += "\n";
            readme += m_description.description;
            
            client.enableLog(true);
            
            generateCMakeFile(m_description.name, sourcesDirectory, groups, readme);
        }
    }

    void CCodeProject::compileCommonHeader(uint32_t options) const
    {
        Client& client = Client::getInstance();
        //-------------------------------------------------------------------
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::string buildDir = getProjDir() + "/build";
        std::string pchFile = buildDir + "/common.pch";
        
        //Delete the old precompiled file if exists
        if(boost_fs::exists(pchFile))
        {
            boost_fs::remove(pchFile);
        }
        
        std::string cmdLine = "clang++ -v -std=c++17 -arch arm64 -Werror=format -fno-diagnostics-show-note-include-stack ";
        
        if(options & CCodeNode::BUILD_DEBUG) {
            cmdLine += "-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O0 -fno-inline-functions -fno-optimize-sibling-calls ";
        }
        
        std::string include = "-I";
        include += buildDir;
        include += " ";
        cmdLine += include;
        
        if(options & CCodeNode::BUILD_PRINT_TEST) {
            cmdLine += "-DCOMPILE_TEST ";
        }
        
        cmdLine += "-x c++-header common.h -o common.pch";
        //-------------------------------------------------------------------
        
        //Recompile the updated sources
        std::string output = exec(cmdLine, buildDir, "CompileCommonHeader", false);
        std::cout << "Generating precompiled header file:\n\n";
        std::cout << output << "\n\n";
    }

    std::string CCodeProject::getFunctionInfo(const std::string& name) const
    {
        std::string functionInfo;
        auto it = m_nodeMap.find(name);
        if(it != m_nodeMap.end())
        {
            auto ccNode = (const CCodeNode*)it->second;
            
            functionInfo += "\n";
            if(!ccNode->m_prototype.declaration.empty())
            {
                functionInfo += ccNode->m_prototype.brief + "\n";
                functionInfo += ccNode->m_prototype.declaration;
            }
            else
            {
                functionInfo += ccNode->m_brief.brief;
            }
            functionInfo += "\n";
        }
        
        return functionInfo;
    }

    std::string CCodeProject::getFunctionDescription(const std::string& name) const
    {
        std::string functionInfo;
        auto it = m_nodeMap.find(name);
        if(it != m_nodeMap.end())
        {
            auto ccNode = (const CCodeNode*)it->second;
            if(ccNode)
            {
                return ccNode->m_prototype.description;
            }
        }
        
        return "";
    }

    std::string CCodeProject::getFunctionDeclaration(const std::string& name) const
    {
        std::string functionInfo;
        auto it = m_nodeMap.find(name);
        if(it != m_nodeMap.end())
        {
            auto ccNode = (const CCodeNode*)it->second;
            if(ccNode)
            {
                return ccNode->m_prototype.declaration;
            }
        }
        
        return "";
    }

    std::string CCodeProject::getFunctionImplementation(const std::string& name) const
    {
        std::string functionInfo;
        auto it = m_nodeMap.find(name);
        if(it != m_nodeMap.end())
        {
            auto ccNode = (const CCodeNode*)it->second;
            if(ccNode)
            {
                return ccNode->m_implementation.m_source;
            }
        }
        
        return "";
    }

    std::string CCodeProject::getFunctionDetailedInfo(const std::string& name) const
    {
        std::string functionInfo;
        auto it = m_nodeMap.find(name);
        if(it != m_nodeMap.end())
        {
            auto ccNode = (const CCodeNode*)it->second;
            if(ccNode)
            {
                if(!ccNode->m_prototype.description.empty())
                {
                    functionInfo += ccNode->m_prototype.description + "\n\n";
                }
                
                if(!ccNode->m_implementation.m_source.empty())
                {
                    functionInfo += ccNode->m_implementation.m_source + "\n\n";
                }
                
                return functionInfo;
            }
        }
        
        return "";
    }

    std::vector<std::string> CCodeProject::listAllFunctions(const std::string& root, int maxDepth,
                                          const std::set<std::string>& exclude)
    {
        std::vector<std::string> functions;
        
        CCodeNode* ccNode = (CCodeNode*)m_dag.m_root->m_data;
        if(!root.empty())
        {
            CCodeNode* ccNodeFound = getNodeByName(root);
            if(ccNodeFound)
            {
                ccNode = ccNodeFound;
            }
        }
        
        if(maxDepth <= 0)
        {
            maxDepth = -1;
        }
        
        Graph graphBuilder;
        
        //add root
        graphBuilder.addNode(ccNode->getName(), "");
        
        m_dag.depthFirstTraversal(ccNode->m_this,
        [&, this](DAGNode<Node*>* node, DAGraph<Node*>& g)
        {

            auto _ccnode = (const CCodeNode*)node->m_data;
            
            for(auto func : _ccnode->m_calls.items)
            {
                const CCodeNode* childNode = nullptr;
                auto it = nodeMap().find(func->func_name);
                if(it != nodeMap().end())
                {
                    childNode = (const CCodeNode*)it->second;
                }
                
                if(!childNode)
                    continue;
                
                graphBuilder.addNode(childNode->getName(), _ccnode->getName());
            }
        });
        
        std::set<std::string> listed;
        
        graphBuilder.traverse(ccNode->getName(),
                [&](const std::string& node, int depth) {
        
                    if (exclude.find(node) != exclude.end())
                        return;                                       // skip excluded

                    if(listed.find(node) == listed.end())
                    {
                        functions.push_back(node);
                        
                        listed.insert(node);
                    }
                },
                maxDepth
            );
        
        return functions;
    }

    std::string CCodeProject::listAllFunctions(const std::string& root, int maxDepth,
                                               bool decl, bool brief, bool briefInComment,
                                               const std::set<std::string>& exclude)
    {
        std::string functions;
    
        std::vector<std::string> nodeList = listAllFunctions(root, maxDepth, exclude);
        for(auto node : nodeList)
        {
            const CCodeNode* n = nullptr;
            auto it = nodeMap().find(node);
            if(it != nodeMap().end())
            {
                n = (const CCodeNode*)it->second;
            }
            
            if (!functions.empty())
                functions += "\n\n";
            
            if (brief && !n->m_brief.brief.empty()) {
                std::stringstream ssBrief;
                printAsComment(n->m_brief.brief, ssBrief);
                std::string briefStr = briefInComment ? (ssBrief.str() + '\n') : (n->m_brief.brief + '\n');
                if(briefStr.length() > BRIEF_MAX_CHARACTERS)
                {
                    briefStr = briefStr.substr(0, BRIEF_MAX_CHARACTERS_NOTE);
                    briefStr += " [[...truncated]]\n";
                }
                functions += briefStr;
            }
            
            functions += (decl && !n->m_prototype.declaration.empty())
            ?  n->m_prototype.declaration
            :  n->m_brief.func_name;
        }
        
        if(!functions.empty())
        {
            functions += "\n";
        }
        
        return functions;
    }

    std::string CCodeProject::listAllFunctionsSource()
    {
        std::string functions;
        
        if(!m_dag.m_root || !m_dag.m_root->m_data)
        {
            return std::string();
        }

        std::vector<std::string> nodeList = listAllFunctions(m_dag.m_root->m_data->getName(), -1, {});
        for(auto node : nodeList)
        {
            const CCodeNode* ccNode = nullptr;
            auto it = nodeMap().find(node);
            if(it != nodeMap().end())
            {
                ccNode = (const CCodeNode*)it->second;
            }
            
            if(!functions.empty())
            {
                functions += "\n\n";
            }
            
            if(!ccNode->m_prototype.description.empty())
            {
                //Description in comment
                std::stringstream ssDesc;
                printAsComment(ccNode->m_prototype.description, ssDesc);
                functions += ssDesc.str();
            }
            
            if(!ccNode->m_implementation.m_source.empty())
            {
                functions += ccNode->m_implementation.m_source;
            }
        }
        
        return functions;
    }

    std::string CCodeProject::listAllDataTypes() const
    {
        std::string list;
        for(auto obj : m_objectTypes)
        {
            if(obj.second.m_typeDef.m_type == TypeDefinition::STRUCT)
            {
                list += "struct " + obj.first + ";\n";
            }
            else
            {
                list += "enum class " + obj.first + " : uint32_t;\n";
            }
        }
        
        return list;
    }

    std::string CCodeProject::listAllDataTypeNames() const
    {
        std::string list;
        for(auto obj : m_objectTypes)
        {
            list += obj.first + "\n";
        }
        
        return list;
    }

    std::string CCodeProject::listAllEnumDefinitions() const
    {
        std::string list;
        for(auto obj : m_objectTypes)
        {
            if(obj.second.m_typeDef.m_type == TypeDefinition::ENUM)
            {
                list += obj.second.m_typeDef.m_definition;
                list += ";\n\n";
            }
        }
        
        return list;
    }

    std::string CCodeProject::listAllStructDeclarations() const
    {
        std::string list;
        for(auto obj : m_objectTypes)
        {
            if(obj.second.m_typeDef.m_type == TypeDefinition::STRUCT)
            {
                list += "struct " + obj.first + ";\n";
            }
        }
        
        return list;
    }

    std::string CCodeProject::listAllStructDefinitions() const
    {
        std::string list;
        for(auto obj : m_objectTypes)
        {
            if(obj.second.m_typeDef.m_type == TypeDefinition::STRUCT)
            {
                list += obj.second.m_typeDef.m_definition;
                list += ";\n\n";
            }
        }
        
        return list;
    }

    std::string CCodeProject::getFunctionStubs(const std::set<std::string>& appFunctions) const
    {
        std::stringstream functionStubs;
        functionStubs << getGenericStubFunction() << std::endl;
        for(auto func : appFunctions)
        {
            bool hasDeclaration = false;
            auto it = nodeMap().find(func);
            if(it != nodeMap().end())
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
                functionStubs << getStubFunction(func) << std::endl;
            }
        }
        return functionStubs.str();
    }

    std::set<std::string> CCodeProject::filterIdentifiers(std::set<std::string>& identifiers,
                           std::set<std::string>& functions,
                           std::set<std::string>& structs,
                           std::set<std::string>& enums)
    {
        std::set<std::string> owners;
        std::set<std::string> filteredIDs;
        functions.clear();
        structs.clear();
        enums.clear();
        
        for(auto id : identifiers)
        {
            std::string owningPath;
            auto dataDef = findData(id, owningPath);
            if(dataDef)
            {
                owners.insert(owningPath);
                if(dataDef->m_typeDef.m_type == TypeDefinition::STRUCT)
                {
                    structs.insert(dataDef->m_typeDef.m_name);
                }
                else
                {
                    enums.insert(dataDef->m_typeDef.m_name);
                }
                
                filteredIDs.insert(id);
            }
            else //must be a function
            {
                auto nodeIt = nodeMap().find(id);
                if(nodeIt != nodeMap().end())
                {
                    functions.insert(id);
                    filteredIDs.insert(id);
                    auto idPath = nodeIt->second->getDAGPath("/");
                    owners.insert(idPath);
                }
            }
        }
        
        identifiers = filteredIDs;
        return owners;
    }

    void CCodeProject::load()
    {
        if(m_cache)
        {
            m_tempGraph = loadDataDefinitions();
            Project::load();
            loadReferences();
            m_tempGraph.clear();
        }
    }

    void CCodeProject::clear()
    {
        Project::clear();
        
        m_objectTypes.clear();
        m_objectTypesSnapshot.clear();
        m_updatedData.clear();
        m_tempGraph.clear();
        m_refactoringDepth = 0;
        m_buildingNow.clear();
    }

    void CCodeProject::reload()
    {
        clear();

        CCodeNode* root = shareNode<CCodeNode>(m_description.func_name, nullptr);

        // root->m_this already created by shareNode(); ensure root is that
        m_dag.m_root = root->m_this;

        load();
    }

    void CCodeProject::saveDataDefinitions()
    {
        auto jsonDataDefinitions = json::value::object();
        
        //Save the graph for checks during loading
        auto graph = json::value::object();
        for(auto node : nodeMap())
        {
            auto ccNode = (const CCodeNode*)node.second;
            if(!ccNode) continue;
            
            auto nameU = conversions::to_string_t(node.first);
            auto pathU = conversions::to_string_t(ccNode->getDAGPath("/"));
            graph[nameU] = json::value::string(pathU);
        }
        jsonDataDefinitions.as_object()[U("__GRAPH__")] = graph;
        
        
        if(!m_objectTypes.empty())
        {
            for(const auto& obj : m_objectTypes)
            {
                auto definitionValue = json::value::string(conversions::to_string_t(obj.second.m_typeDef.m_definition));
                auto ownerValue = json::value::string(conversions::to_string_t(obj.second.m_ownerPath));
                
                auto dataDefinitionsObj = json::value::object();
                dataDefinitionsObj.as_object()[U("definition")] = definitionValue;
                dataDefinitionsObj.as_object()[U("owner")] = ownerValue;
                
                auto type = obj.second.m_typeDef.m_type == TypeDefinition::STRUCT ? "struct" : "enum";
                auto typeStr = json::value::string(conversions::to_string_t(type));
                dataDefinitionsObj.as_object()[U("type")] = typeStr;
                
                auto mambers = json::value::object();
                for(auto member : obj.second.m_typeDef.m_members)
                {
                    auto memberNameStr = conversions::to_string_t(member.first);
                    auto memberTypeVal = json::value::string(conversions::to_string_t(member.second));
                    mambers[memberNameStr] = memberTypeVal;
                    
                }
                dataDefinitionsObj.as_object()[U("members")] = mambers;
                
                auto refArray = json::value::array();
                for(auto ref : obj.second.m_references)
                {
                    auto size = refArray.size();
                    refArray[size] = json::value::string(conversions::to_string_t(ref));
                }
                dataDefinitionsObj.as_object()[U("references")] = refArray;
                
                jsonDataDefinitions.as_object()[conversions::to_string_t(obj.first)] = dataDefinitionsObj;
            }
        }
            
        std::string path = m_projDir + "/dag";
        try {
            boost_fs::create_directories(path);
        }
        catch (const boost_fs::filesystem_error& e) {
            std::cout << "Can't save data definitions! Unable to create directory: " << path << std::endl;
            return;
        }
        
        path += "/DataDefinitons.json";
        
        std::ofstream fileJson(path);
        if (!fileJson.is_open()) {
            std::cout << "Can't save data definitions! Unable to create file " << path << std::endl;
            return false;
        }

        std::string strJson = conversions::to_utf8string(jsonDataDefinitions.serialize());
        fileJson << strJson << std::endl;
        fileJson.close();
    }

    std::map<std::string, std::string> CCodeProject::loadDataDefinitions()
    {
        std::map<std::string, std::string> graph;
        std::string path = m_projDir + "/dag/DataDefinitons.json";
        std::ifstream file(path);
        if(!file.good())
        {
            return graph;
        }
        std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        json::value jsonDataDefinitions = json::value::parse(conversions::to_string_t(str));
        
        if(jsonDataDefinitions.has_field(U("__GRAPH__")))
        {
            auto& graphObj = jsonDataDefinitions[U("__GRAPH__")].as_object();
            for(auto node : graphObj)
            {
                auto nodeStr = conversions::to_utf8string(node.first);
                auto pathStr = conversions::to_utf8string(node.second.as_string());
                graph[nodeStr] = pathStr;
            }
        }
        
        for(auto data : jsonDataDefinitions.as_object())
        {
            auto typeStr = conversions::to_utf8string(data.first);
            
            if(typeStr == "__GRAPH__") {
                continue;
            }
            
            m_objectTypes[typeStr].m_typeDef.m_name = typeStr;
            
            auto defStr = conversions::to_utf8string(data.second.as_object()[U("definition")].as_string());
            m_objectTypes[typeStr].m_typeDef.m_definition = defStr;
            
            auto dataTypeStr = conversions::to_utf8string(data.second.as_object()[U("type")].as_string());
            m_objectTypes[typeStr].m_typeDef.m_type = dataTypeStr == "struct" ? TypeDefinition::STRUCT : TypeDefinition::ENUM;
            
            for(auto member : data.second.as_object()[U("members")].as_object())
            {
                auto memberNameStr = conversions::to_utf8string(member.first);
                auto memberTypeStr = conversions::to_utf8string(member.second.as_string());
                m_objectTypes[typeStr].m_typeDef.m_members[memberNameStr] = memberTypeStr;
            }
            
            auto ownerStr = data.second.as_object()[U("owner")].as_string();
            m_objectTypes[typeStr].m_ownerPath = ownerStr;
            for(auto ref : data.second.as_object()[U("references")].as_array())
            {
                auto refStr = conversions::to_utf8string(ref.as_string());
                m_objectTypes[typeStr].m_references.insert(refStr);
            }
        }
        
        return graph;
    }

    bool stdrave::CCodeProject::isADependency(std::stack<const CCodeNode*>& path,
                                           const std::string& dependency,
                                           const std::string& dependent)
    {
        // Start each query clean to ensure the returned path is only for this query.
        while (!path.empty()) path.pop();

        auto itDependency = m_nodeMap.find(dependency);
        if (itDependency == m_nodeMap.end()) return false;

        auto itDependent = m_nodeMap.find(dependent);
        if (itDependent == m_nodeMap.end()) return false;

        const CCodeNode* from = static_cast<const CCodeNode*>(itDependency->second);
        const CCodeNode* to   = static_cast<const CCodeNode*>(itDependent->second);
        if (!from || !to) return false;

        return from->doesItCall(path, to);
    }

    bool CCodeProject::appTypeHasNamespace(const std::string& identifier, std::set<std::string>& namespaces) const
    {
        bool appTypeWithNamespace = false;
        
        std::vector<std::string> tokens = splitCTypeByNamespace(identifier, "::");
        
        std::string prevToken;
        for(auto token : tokens)
        {
            std::string owningPath;
            auto dataType = findData(token, owningPath);
            if(dataType && prevToken == "::")
            {
                namespaces.insert(token);
                appTypeWithNamespace = true;
            }
            
            prevToken = token;
        }
        
        return appTypeWithNamespace;
    }

    std::set<std::string> CCodeProject::appTypesHaveNamespace(const std::string& declOrDef) const
    {
        std::string declaration = extractFunctionDeclaration(declOrDef);
        
        ParsedFunction signature = parseFunctionSignature(declaration);
        
        std::set<std::string> namespaces;
        appTypeHasNamespace(signature.returnType, namespaces);
        
        for(const auto& arg : signature.argumentTypes)
        {
            appTypeHasNamespace(arg, namespaces);
        }
        
        return namespaces;
    }

    std::set<std::string> CCodeProject::getAllIdentifiers()
    {
        std::set<std::string> identifiers;
        
        for(const auto& node : m_nodeMap)
        {
            auto ccNode = (const CCodeNode*)node.second;
            
            if(!ccNode)
                continue;
            
            if(!ccNode->m_brief.func_name.empty())
            {
                identifiers.insert(ccNode->m_brief.func_name);
            }
            else
            {
                identifiers.insert(ccNode->m_brief.func_name);
            }
        }
        
        for(const auto& it : m_objectTypes)
        {
            identifiers.insert(it.second.m_typeDef.m_name);
            
            for(auto member : it.second.m_typeDef.m_members) {
                identifiers.insert(member.first);
            }
        }
        
        return identifiers;
    }

    std::set<std::string> CCodeProject::getAppTypes(const std::string& dataType) const
    {
        std::set<std::string> appDefinedTypes;
        
        // Regular expression to match string literals
        std::regex stringLiteralRegex(R"("(?:[^"\\]|\\.)*")");
        
        // Remove string literals
        std::string processedDataType = std::regex_replace(dataType, stringLiteralRegex, "");
        
        // Regular expression to match numeric literals (including floating-point)
        std::regex numericRegex(R"(\b-?\d+(\.\d+)?([eE][+-]?\d+)?\b)");
        
        // Remove numeric literals
        processedDataType = std::regex_replace(processedDataType, numericRegex, "");
        
        std::vector<std::string> dataTypeTokens = splitDataType(processedDataType);
        
        std::vector<std::string> tokensNS = formatCppNamespaces(dataType);
        
        std::string prevToken;
        //for(const auto& token : dataTypeTokens)
        for(const auto& token : tokensNS)
        {
            bool isIdentifier = getCppIdentifiers().find(token) != getCppIdentifiers().end();
            bool isQualifier = getCppQualifiers().find(token) != getCppQualifiers().end();
            if(!token.empty() &&
               
               !endsWith(prevToken, "std::") && token != "::" && token != "std" &&
               
               !isQualifier && !isIdentifier &&
               getCppBaseTypes().find(token) == getCppBaseTypes().end())
            {
                // Additional check to ensure the token starts with a letter or underscore
                if(std::isalpha(token[0]) || token[0] == '_')
                {
                    appDefinedTypes.insert(token);
                }
            }
            
            if(!isQualifier) //skip qualifiers
            {
                if(token == "|") {
                    prevToken.clear();
                }
                else {
                    prevToken += token;
                }
            }
        }
        
        return appDefinedTypes;
    }

    std::set<std::string> CCodeProject::getAppTypesForFunction(const ParsedFunction& signature) const
    {
        std::set<std::string> appDefinedTypes = getAppTypes(signature.returnType);
        
        for(auto arg : signature.argumentTypes)
        {
            std::set<std::string> appTypes = getAppTypes(arg);
            appDefinedTypes.insert(appTypes.begin(), appTypes.end());
        }
        
        return appDefinedTypes;
    }

    std::set<std::string> CCodeProject::getFullAppTypesForFunction(const ParsedFunction& signature) const
    {
        std::set<std::string> fullAppDefinedTypes;
        
        if(!getAppTypes(signature.returnType).empty())
        {
            fullAppDefinedTypes.insert(signature.returnType);
        }
        
        for(auto arg : signature.argumentTypes)
        {
            std::set<std::string> appTypes = getAppTypes(arg);
            if(!appTypes.empty())
            {
                fullAppDefinedTypes.insert(arg);
            }
        }
        
        return fullAppDefinedTypes;
    }

    std::set<std::string> CCodeProject::getAppTypesFromDecl(const std::string& decl) const
    {
        ParsedFunction signature = parseFunctionSignature(decl);
        
        return getAppTypesForFunction(signature);
    }

    /**
     * Analyzes compiler output for specific error types and extracts identifiers.
     *
     * This function scans the provided compiler output for certain error messages
     * related to undeclared identifiers, incomplete types, missing members,
     * unmatched functions, constructors, or conversions. It extracts the relevant
     * identifiers or type names enclosed in single quotes from these error messages.
     *
     * The function looks for the following error types:
     * - use of undeclared identifier
     * - field has incomplete type
     * - no member named
     * - no matching function for call to
     * - no matching constructor for initialization of
     * - no matching conversion for functional-style cast from/to
     * - invalid operands to binary expression
     *
     * @param cmplOutput A string containing the compiler's error output.
     * @return An std::set<std::string> containing unique identifiers extracted from the errors.
     *         The set may be empty if no matching errors were found.
     *
     * @note This function uses C++11 regex to parse the error messages. It's designed
     *       to be flexible and can handle variations in error message formatting,
     *       as long as the identifiers are enclosed in single quotes.
     */
    std::set<std::string> CCodeProject::analyzeForAppIdentifiers(const std::string& cmplOutput) const
    {
        std::set<std::string> identifiers;
        std::istringstream stream(cmplOutput);
        std::string line;

        // Existing regex patterns
        std::regex general_pattern(
            "(?:"
            "use of undeclared identifier|"
            "field has incomplete type|"
            "no matching function for call to|"
            "no matching constructor for initialization of|"
            "no matching conversion for functional-style cast from|"
            "to) '([^']+)'"
        );

        std::regex member_pattern("no member named '[^']+' in '([^']+)'");

        // New pattern for "invalid operands to binary expression" errors
        std::regex binary_expression_pattern("invalid operands to binary expression \\('([^']+)' and '([^']+)'\\)");

        while (std::getline(stream, line)) {
            std::smatch matches;
            if (std::regex_search(line, matches, binary_expression_pattern)) {
                // For "invalid operands to binary expression" errors, capture both types
                if (matches.size() > 2) {
                    std::set<std::string> appIdentifiers1 = getAppTypes(matches[1].str());
                    std::set<std::string> appIdentifiers2 = getAppTypes(matches[2].str());
                    identifiers.insert(appIdentifiers1.begin(), appIdentifiers1.end());
                    identifiers.insert(appIdentifiers2.begin(), appIdentifiers2.end());
                }
            }
            else if (std::regex_search(line, matches, member_pattern)) {
                // Existing logic for "no member named" errors
                if (matches.size() > 1) {
                    std::set<std::string> appIdentifiers = getAppTypes(matches[1].str());
                    identifiers.insert(appIdentifiers.begin(), appIdentifiers.end());
                }
            }
            else if (std::regex_search(line, matches, general_pattern)) {
                // Existing logic for other error types
                if (matches.size() > 1) {
                    std::set<std::string> appIdentifiers = getAppTypes(matches[1].str());
                    identifiers.insert(appIdentifiers.begin(), appIdentifiers.end());
                }
            }
        }

        return identifiers;
    }

    bool CCodeProject::isCppOrStdType(const std::string& dataType) const
    {
        return getAppTypes(dataType).empty();
    }

    bool CCodeProject::isAppType(const std::string& dataType) const
    {
        return !getAppTypes(dataType).empty();
    }

    bool CCodeProject::isMutableType(const std::string& dataType) const
    {
        std::vector<std::string> dataTypeTokens = splitDataType(dataType);
        
        if(!dataTypeTokens.size())
            return false;
        
        if(dataTypeTokens.size()==1 && dataTypeTokens[0] == "void")
            return false;
        
        bool hasAppTypes = getAppTypes(dataType).size() != 0;
        
        if(!hasAppTypes && dataTypeTokens[0] == "const")
            return false;
        
        //No pointers or references
        if(!hasAppTypes && dataType.find("*") == std::string::npos &&
           dataType.find("&") == std::string::npos)
            return false;
        
        return true;
    }

    bool CCodeProject::isInvalidIterator(const std::string& dataType) const
    {
        auto appTypes = getAppTypes(dataType);
        if(appTypes.empty()) {
            return false;
        }
        
        for(auto appType : appTypes)
        {
            auto result = ::isInvalidIterator(dataType, appType, getStdContainers());
            if(result.first)
            {
                return true;
            }
        }
        
        return false;
    }

    bool CCodeProject::isInvalidContainer(const std::string& dataType) const
    {
        auto appTypes = getAppTypes(dataType);
        if(appTypes.empty()) {
            return false;
        }
        
        for(auto appType : appTypes)
        {
            auto result = ::isInvalidContainer(dataType, appType, getStdContainers());
            if(result.first)
            {
                return true;
            }
        }
        
        return false;
    }

    bool CCodeProject::hasEffect(const std::string& functionDecl) const
    {
        ParsedFunction signature = parseFunctionSignature(functionDecl);
        
        bool isConst = signature.returnType == "void";
        if(!isConst)
        {
            return true;
        }
        
        for(auto arg : signature.argumentTypes)
        {
            isConst = !isMutableType(arg);
            if(!isConst)
            {
                return true;
            }
        }
        
        return false;
    }

    std::set<std::string> CCodeProject::hasConstantTypes(const ParsedFunction& signature) const
    {
        std::set<std::string> constantTypes;
        if(isAppType(signature.returnType) && isConstType(signature.returnType))
        {
            constantTypes.insert(signature.returnType);
        }
        
        for(auto arg : signature.argumentTypes)
        {
            if(isAppType(arg) && isConstType(arg))
            {
                constantTypes.insert(arg);
            }
        }
        
        return constantTypes;
    }

    std::set<std::string> CCodeProject::hasConstantTypes(const std::string& functionDecl) const
    {
        ParsedFunction signature = parseFunctionSignature(functionDecl);
        
        return hasConstantTypes(signature);
    }

    const std::unordered_set<std::string> CCodeProject::getStdContainers()
    {
        static const std::unordered_set<std::string> stdContainers = {
            "vector",
            "map",
            "stack",
            "list",
            "set",
            "queue"
        };
        
        return stdContainers;
    }

    const std::unordered_set<std::string> CCodeProject::getStdUtilityTypes()
    {
        static const std::unordered_set<std::string> stdUtilityTypes = {
            "string",
            "shared_ptr"
        };
        
        return stdUtilityTypes;
    }

    std::set<std::string> CCodeProject::hasRestrictedStdTypes(const std::string& dataType)
    {
        std::set<std::string> restrictedTypes;
        std::set<std::string> stdTypes = getSTDTypes(dataType);
        
        for(auto type : stdTypes)
        {
            if(getStdContainers().find(type) == getStdContainers().end() &&
               getStdUtilityTypes().find(type) == getStdUtilityTypes().end()) {
                restrictedTypes.insert(type);
            }
        }
        
        return restrictedTypes;
    }

    std::map<std::string, DataInfo> CCodeProject::getDataShapshot() const
    {
        return m_objectTypes;
    }

    void CCodeProject::restoreDataSnapshot(const std::map<std::string, DataInfo>& snapshot)
    {
        m_objectTypes.clear();
        m_objectTypes = snapshot;
    }

    std::shared_ptr<CompileInfo> CCodeProject::getCompilationInfo(const std::string& functionName, const std::string& platform, uint32_t options) const
    {
        auto it = m_nodeMap.find(functionName);
        if(it == m_nodeMap.end()) {
            return nullptr;
        }
        
        auto compileInfo = std::make_shared<CompileInfo>();
        auto ccNode = (const CCodeNode*)it->second;
        if(!ccNode) {
            return nullptr;
        }
        
        return ccNode->getCompilationInfo(platform, options);
    }

    bool CCodeProject::updateSource(const std::string& node, CCodeNode::CodeType type, const std::string& message, std::string& output, bool enableRefactoring)
    {
        auto it = m_nodeMap.find(node);
        if(it == m_nodeMap.end())
        {
            return false;
        }
        
        CCodeNode* ccNode = (CCodeNode*)it->second;
        if(!ccNode)
        {
            return false;
        }
        
        CCodeNode* parent = nullptr;
        if(ccNode->m_this && ccNode->m_this->m_parent)
        {
            parent = (CCodeNode*)ccNode->m_this->m_parent;
        }
        
        std::string platform = getPlatform() + "_test";
        std::string compileCL = ccNode->compileCommand(platform, CCodeNode::BUILD_PRINT_TEST | CCodeNode::BUILD_DEBUG);
        
        //Delete the old object file to ensure it is actually recompiled
        std::string objFile = ccNode->getObectFilePath();
        boost_fs::remove(objFile);
        
        bool needsRefactoring = false;
        
        uint32_t oldCompilationStartMessage = ccNode->m_compilationStartMessage;
        if(enableRefactoring)
        {
            size_t linesCount = countLines(ccNode->m_implementation.m_source);
            needsRefactoring = linesCount > MAX_LINES_IN_SOURCE_SNIPPET;
            if(needsRefactoring)
            {
                //Don't let CCodeNode::refactorTruncatedSource to store/restore the context
                ccNode->m_compilationStartMessage = INVALID_HANDLE_ID;
            }
        }
        bool result = ccNode->updateSource(type, parent, message, compileCL, output, needsRefactoring);
        ccNode->m_compilationStartMessage = oldCompilationStartMessage;
        
        //Another chance with decomposeAndBuild - verify, decompose, build
        if(!result && decomposeAndBuild(ccNode))
        {
            //Regenerate source files
            ccNode->generateAllSources(true);
            
            std::string buildSourcePath = ccNode->getNodeBuildSourcePath();
            std::string buildDir = getProjDir() + "/build";
            std::string nodeDir = buildDir + "/" + buildSourcePath;
            std::string testDir = nodeDir + "/test";
            
            //Recompile the updated sources
            output = ccNode->exec(compileCL, testDir, "Compile");
            
            result = ccNode->objectExists();
        }
        
        //Save the node on success!
        if(result)
        {
            ccNode->save();
            
            //TODO: Switch to decompose context here and decompose sub nodes
        }
        
        return result;
    }

    Context* CCodeProject::switchToDecomposeContext(const std::string& nodePath)
    {
        if(m_activeContext == &m_decomposeContext)
        {
            return m_activeContext;
        }
        
        m_decomposeContext.reset();
     
        std::string envDir = Client::getInstance().getEnvironmentDir();
        
        //These messages are general for the project, so they shouldn't be in any node
        m_decomposeContext.add(m_description.role.prompt(), "system");
        
        //These messages are general for the project, they shouldn't be in any node
        std::string style = coding_style.prompt();
        style += "\n";
        style += libraries.prompt();
        style += "\n";
        m_decomposeContext.add(style, "user");
        
        std::string decompose = problem_decompose.prompt({
            {"entry_point", m_description.entry_point} });
        decompose += "\n";
        m_decomposeContext.add(decompose, "user");
        
        m_decomposeContext.add(m_description.description, "user");
        
        auto nodes = getNodesForPath(nodePath);
        for(auto node : nodes)
        {
            std::string nodeSummary = node->summarize(true);
            m_decomposeContext.add(nodeSummary, "user");
        }
    
        return setActiveContext(&m_decomposeContext);
    }

    bool CCodeProject::decomposeAndBuild(CCodeNode* ccNode)
    {
        //Wherever we are, switching to decompose context for this node should be the right thing to do
        Context* prevCtx = switchToDecomposeContext(ccNode->getDAGPath("/"));
        
        ccNode->updateCallsUsage(true, false);
        dataSnapshot();
        
        bool verified = ccNode->verify();
        if(!verified)
        {
            setActiveContext(prevCtx);
            return false;
        }
        else
        {
            ccNode->save();
            ccNode->generateAllSources(true);
        }
        
        captureContext("");
        buildHierarchy(ccNode->m_this);
        popContext();
        
        setActiveContext(prevCtx);
        
        return buildBinary(true);
    }

    bool CCodeProject::compile()
    {
        //Wherever we are, switching to decompose context for this node should be the right thing to do
        Context* prevCtx = switchToDecomposeContext(m_dag.m_root->m_data->getDAGPath("/"));
        
        captureContext("");
        buildHierarchy(m_dag.m_root);
        popContext();
        
        setActiveContext(prevCtx);
        
        return buildBinary(true);
    }

    std::set<std::string> CCodeProject::getDirectsInCallGraph(const std::string& node)
    {
        std::set<std::string> directs;
        
        auto it = m_nodeMap.find(node);
        if(it != m_nodeMap.end())
        {
            CCodeNode* ccNode = (CCodeNode*)it->second;
            if(!ccNode)
            {
                return directs;
            }
            
            CCodeNode* parent = nullptr;
            if(ccNode->m_this && ccNode->m_this->m_parent)
            {
                parent = (CCodeNode*)ccNode->m_this->m_parent;
                directs.insert(parent->m_brief.func_name);
            }
            
            for(auto fun : ccNode->m_calls.items)
            {
                auto funIt = m_nodeMap.find(fun->func_name);
                if(funIt != m_nodeMap.end())
                {
                    directs.insert(fun->func_name);
                }
            }
        }
        
        return directs;
    }

    std::string CCodeProject::getBuildSourcePath() const
    {
        std::string buildSourcePath;
        buildSourcePath += "source";
        return buildSourcePath;
    }

    std::string CCodeProject::listIncludes(std::set<std::string> includes, bool checkIncludes) const
    {
        std::stringstream sout;
        
        for(auto owner : includes)
        {
            std::string nodeName = getLastToken(owner, '/');
            std::string include = getBuildSourcePath() + "/" + nodeName + "/" + nodeName;
            include += ".h";
            if(checkIncludes)
            {
                std::string includeFilePath = getProjDir();
                includeFilePath += "/build/";
                includeFilePath += include;
                if(!boost_fs::exists(includeFilePath))
                {
                    continue;
                }
            }
            
            sout << "#include \"" << include << "\"" << std::endl;
        }
        
        return sout.str();
    }

    std::string CCodeProject::generateStructPrinter(const TypeDefinition& typeDef)
    {
        std::string printer;
        printer += "inline void printValue(std::ostream& os, const ";
        printer += typeDef.m_name;
        printer += "& val, std::size_t depth, const PrintConfig& cfg) {\n";
        
        printer += "   if (depth > cfg.maxDepth) { os << \"" + typeDef.m_name + " {...}\"; return; }\n\n";
        printer += "   std::string indent(depth*2, ' ');\n";
        std::vector<std::pair<std::string, std::string>> members = typeDef.sortMembers();
        
        printer += "   os << std::endl << indent;\n";
        printer += "   os << \"" + typeDef.m_name + " {\";\n";
        uint32_t memberId = 1;
        std::string memberIdStr;
        for(auto& member : members)
        {
            memberIdStr = std::to_string(memberId);
            //Close the previouse statement
            if(member != members.front())
            {
                printer += "   os << \", \";\n";
            }
            
            printer += "   os << std::endl << indent;\n";
            
            bool isEnum = false;
            std::string owningPath;
            auto memberData = findData(member.second, owningPath);
            if(memberData)
            {
                isEnum = memberData->m_typeDef.m_type == TypeDefinition::ENUM;
            }
            
            if(isEnum)
            {
                printer += "   os << \"" + member.first + "=\";\n   printValue(os, static_cast<" + member.second + ">(val." + member.first + "), depth + 1, cfg);\n";
            }
            else
            {
                printer += "   os << \"" + member.first + "=\";\n   printValue(os, val." + member.first + ", depth + 1, cfg);\n";
            }
            
            if(members.size() > memberId)
            {
                printer += "   if(cfg.maxMembers == " + memberIdStr + ") {os << \",\" << std::endl << indent << \"...\" << std::endl << indent << \"}\"; return;}";
            }
            
            memberId++;
        }
        
        printer += "   os << std::endl << indent;\n";
        printer += "   os << \"}\";";
        
        printer += "\n}\n\n";
        
        return printer;
    }

    std::string CCodeProject::generateEnumPrinter(const TypeDefinition& typeDef)
    {
        std::string printer;
        printer += "inline void printValue(std::ostream& os, ";
        printer += typeDef.m_name;
        printer += " val, std::size_t depth, const PrintConfig& cfg) {\n";
        
        //---------------------------------------------------------------------
        std::vector<std::pair<std::string, std::string>> members = typeDef.sortMembers();
        printer += "   switch(val) {\n";
        for(auto& member : members)
        {
            printer += "        case " + typeDef.m_name + "::" + member.first + ":";
            printer += " os << \"" + typeDef.m_name + "::" + member.first + "\"; break;\n";
        }
        printer += "        default : os << \"Unknown " + typeDef.m_name + "\"; break;\n";
        printer += "   }\n";
        printer += "}\n\n";
        
        return printer;
    }

    std::string CCodeProject::generateStructPrinterDecl(const TypeDefinition& typeDef)
    {
        std::string printer;
        printer += "void printValue(std::ostream& os, const ";
        printer += typeDef.m_name;
        printer += "& val, std::size_t depth, const PrintConfig& cfg);";
        return printer;
    }

    std::string CCodeProject::generateEnumPrinterDecl(const TypeDefinition& typeDef)
    {
        std::string printer;
        //printer += "inline void print" + typeDef.m_name + "(std::ostream& os, ";
        printer += "void printValue(std::ostream& os, ";
        printer += typeDef.m_name;
        printer += " val, std::size_t depth, const PrintConfig& cfg);\n";
        return printer;
    }

    std::string CCodeProject::generateDataPrinters()
    {
        std::string printers;
        
        std::string declarations;
        std::string forwardDeclarations;
        std::set<std::string> includes;
        
        //1 Data printers for all custom data types
        
        for(auto& obj : m_objectTypes)
        {
            includes.insert(obj.second.m_ownerPath);
            
            if(obj.second.m_typeDef.m_type == TypeDefinition::STRUCT)
            {
                declarations += "struct " + obj.second.m_typeDef.m_name + ";\n";
                
                forwardDeclarations += generateStructPrinterDecl(obj.second.m_typeDef);
                printers += generateStructPrinter(obj.second.m_typeDef);
            }
            else
            {
                declarations += "enum class " + obj.second.m_typeDef.m_name + " : uint32_t;\n";
                forwardDeclarations += generateEnumPrinterDecl(obj.second.m_typeDef);
                printers += generateEnumPrinter(obj.second.m_typeDef);
            }
            
        }
        
        std::string includesList = listIncludes(includes, false);

        std::string path = getProjDir() + "/build_instrumented";
        boost_fs::create_directories(path);
        std::ofstream sourceFile(path + "/data_printers.h");
        
        sourceFile << "#pragma once" << std::endl << std::endl;
        sourceFile << includesList << std::endl;
        sourceFile << declarations << std::endl;
        sourceFile << forwardDeclarations << std::endl;
        sourceFile << printers;
        sourceFile.close();
        
        return printers;
    }

    void CCodeProject::generateDataHeader()
    {
        std::string declarations;
        std::set<std::string> includes;
        
        //1 Data printers for all custom data types
        for(auto& obj : m_objectTypes)
        {
            includes.insert(obj.second.m_ownerPath);
            
            if(obj.second.m_typeDef.m_type == TypeDefinition::STRUCT)
            {
                declarations += "struct " + obj.second.m_typeDef.m_name + ";\n";
            }
            else
            {
                declarations += "enum class " + obj.second.m_typeDef.m_name + " : uint32_t;\n";
            }
            
        }
        
        std::string includesList = listIncludes(includes, false);

        std::string path = getProjDir() + "/build";
        boost_fs::create_directories(path);
        std::ofstream sourceFile(path + "/data_defs.h");
        
        sourceFile << "#pragma once" << std::endl << std::endl;
        sourceFile << declarations << std::endl;
        sourceFile << includesList << std::endl;
        sourceFile.close();
    }

    void CCodeProject::generateSources()
    {
        m_dag.depthFirstTraversal(m_dag.m_root, [this](DAGNode<Node*>* node, DAGraph<Node*>& g) {},
            [this](DAGNode<Node*>* node) {
            if(node->m_data)
            {
                if(m_integrate) node->m_data->preBuild();
            }
            });
    }

    CCodeNode* CCodeProject::getNodeByName(const std::string& nodeName) const
    {
        auto it = m_nodeMap.find(nodeName);
        if(it != m_nodeMap.end())
        {
            return (CCodeNode*)it->second;
        }
        
        return nullptr;
    }

    void CCodeProject::buildHierarchy(DAGNode<Node*>* root)
    {
        m_dag.depthFirstTraversal(root, [this](DAGNode<Node*>* node, DAGraph<Node*>& g) {
            if(node->m_data)
            {
                node->m_data->captureContext();
                node->m_data->decompose();
                node->m_data->popContext();
                pushMessage(node->m_data->summarize(true), "user", true);
            }
            },
            [this](DAGNode<Node*>* node) {
            if(node->m_data)
            {
                if(m_integrate) node->m_data->integrate();
            }
            });
        
        for(auto obj : m_objectTypes)
        {
            //This is a bad but not critical
            if(obj.second.m_ownerPath == "__DETACHED__") {
                std::cout << "__DETACHED__ object is still in the project: " << obj.first << std::endl;
            }
        }
        
        //Clean the leftover detached objects at this point
        for (auto it = m_objectTypes.begin(); it != m_objectTypes.end(); ) {
            if (it->second.m_ownerPath == "__DETACHED__") {
                it = m_objectTypes.erase(it);
            } else {
                ++it;
            }
        }
        
        //Currently we need to prebuid from the root. This is not ideal but will simplify the things for the code gen
        // since during the above decomposition of the CCodeNode, some of the data structures that are not in the herarchy might be also updated.
        //We don't keep track which exactly nodes have updated data structures to prebuild only them, for now prebuild everything
        m_dag.depthFirstTraversal(m_dag.m_root, [this](DAGNode<Node*>* node, DAGraph<Node*>& g) {},
            [this](DAGNode<Node*>* node) {
            if(node->m_data)
            {
                if(m_integrate) node->m_data->preBuild();
            }
            });
        
        m_dag.depthFirstTraversal(root, [this](DAGNode<Node*>* node, DAGraph<Node*>& g) {
                
            },
            [this](DAGNode<Node*>* node) {
            if(node->m_data)
            {
                node->m_data->captureContext();
                if(m_integrate) node->m_data->build();
                node->m_data->popContext();
            }
            });
    }

    std::string CCodeProject::provideInfo(const InfoRequest& request)
    {
        std::string info;
        std::string hitTheLimitMsg = "Unable to provide all of the requested information due to size limit!\n";
        
        #define CHECK_INFORMATIO_REQUEST_SIZE if(info.length() > MAX_INFORMATIO_REQUEST_SIZE){ info += "\n"; info += hitTheLimitMsg + "\n"; return info; }
        
        for(auto fun : request.functions)
        {
            std::string functionInfo;
            auto it = nodeMap().find(*fun);
            if(it != nodeMap().end())
            {
                auto ccNode = (const CCodeNode*)it->second;
                if(!ccNode)
                {
                    info += "\n\nFunction '" + *fun + "' not defined in the project.\n\n";
                    continue;
                }
                
                std::string functionInfo;
                if(!ccNode->m_prototype.description.empty())
                {
                    functionInfo += ccNode->m_prototype.description + "\n\n";
                }
                else if(!ccNode->m_brief.brief.empty())
                {
                    functionInfo += ccNode->m_brief.brief + "\n\n";
                }
                
                CHECK_INFORMATIO_REQUEST_SIZE
                
                if(!ccNode->m_implementation.m_source.empty())
                {
                    functionInfo += ccNode->m_implementation.m_source + "\n\n";
                }
                
                if(functionInfo.empty())
                {
                    info += "\n\nInformation for the function '" + *fun + "' not available.\n\n";
                }
                else
                {
                    info += "\n\nInformation for function: " + *fun + "\n\n";
                    info += functionInfo;
                }
                
                CHECK_INFORMATIO_REQUEST_SIZE
            }
            else
            {
                info += "\n\nFunction '" + *fun + "' not defined in the project.\n\n";
                continue;
            }
        }
        
        for(auto data : request.data_types)
        {
            std::string owningPath;
            auto dataDef = findData(*data, owningPath);
            if(!dataDef)
            {
                info += "\n\nData type '" + *data + "' is not defined in the project.\n\n";
                continue;
            }
            
            info += dataDef->m_typeDef.m_definition + ";\n\n";
            
            CHECK_INFORMATIO_REQUEST_SIZE
        }
        
        for(auto fun : request.graph_and_brief)
        {
            auto it = nodeMap().find(*fun);
            if(it != nodeMap().end())
            {
                info += printGraph(*fun, DECOMPOSE_MAX_GRAPH_DEPTH, true);
                info += listAllFunctions(*fun, DECOMPOSE_MAX_GRAPH_DEPTH, true, true, false, {});
            }
            else
            {
                info += "\n\nInformation for subgraph of the function '" + *fun + "' is not available. The function is not defined in the project\n\n";
            }
            
            CHECK_INFORMATIO_REQUEST_SIZE
        }
        
        for(auto pattern : request.match_regex)
        {
            std::string error;
            std::regex regexPattern;
            if(!tryMakeRegex(*pattern, regexPattern, std::regex_constants::ECMAScript, &error))
            {
                info += *pattern + " is not a valid regex pattern\n";
            }
            else
            {
                std::string searchResult = searchSource(regexPattern);
                
                //Trim to LOG_SECTION_SIZE
                if(info.length() > DECOMPOSE_REGEX_MATCH_MAX_CHARACTERS)
                {
                    info += searchResult.substr(0, DECOMPOSE_REGEX_MATCH_MAX_CHARACTERS);
                    info += "\nThe search result has been trimmed due to size limitations\n";
                }
                else if(searchResult.empty())
                {
                    info += "\nNo matching strings found in the source for patter: " + *pattern + "\n";
                }
            }
            
            CHECK_INFORMATIO_REQUEST_SIZE
        }
        
        return info;
    }

    void CCodeProject::setBuildCacheDir(const std::string& cacheDir)
    {
        m_buildCacheDir = m_projDir + "/" + cacheDir;
        if(!boost_fs::exists(m_buildCacheDir))
        {
            boost_fs::create_directories(m_buildCacheDir);
        }
    }

    size_t CCodeProject::getCachedNodeHash(const std::string& nodeName)
    {
        // Find the node by name
        CCodeNode* ccNode = getNodeByName(nodeName);
        if (!ccNode)
            return 0;

        // Build the cache directory path
        boost_fs::path cacheDir = m_buildCacheDir;
        cacheDir /= ccNode->getName();

        // Path to the hash file
        boost_fs::path hashFile = cacheDir / "hash.txt";

        // Check that the directory exists and is a directory
        if (!boost_fs::exists(cacheDir) || !boost_fs::is_directory(cacheDir))
            return 0;

        // Ensure exactly one file exists in the directory
        size_t fileCount = 0;
        for (auto& entry : boost_fs::directory_iterator(cacheDir)) {
            if (entry.is_regular_file()) {
                ++fileCount;
            }
        }
        if (fileCount != 1)
            return 0;

        // Verify it's the expected file
        if (!boost_fs::exists(hashFile))
            return 0;

        // Read the contents of hash.txt
        std::ifstream in(hashFile.string());
        if (!in.is_open())
            return 0;

        std::string content;
        if (!std::getline(in, content))
            return 0;

        // Convert to size_t and return
        try {
            return static_cast<size_t>(std::stoull(content));
        }
        catch (const std::invalid_argument&) {
            // Content was not a valid number
            return 0;
        }
        catch (const std::out_of_range&) {
            // Number out of range for size_t
            return 0;
        }
    }

    // Grab from the start of the line containing the match, up to the later of
    // (the end of the regex match) or (the end of that line).
    static std::vector<std::string>
    extractMatchingSnippets(const std::string& src, const std::regex& pattern)
    {
        std::vector<std::string> out;
        for (auto it = std::sregex_iterator(src.begin(), src.end(), pattern),
                  end = std::sregex_iterator();
             it != end; ++it)
        {
            const auto& m = *it;

            const size_t match_begin = static_cast<size_t>(m.position(0));
            const size_t match_end   = match_begin + static_cast<size_t>(m.length(0));

            // Start of the line containing match_begin
            size_t line_start = src.rfind('\n', match_begin);
            line_start = (line_start == std::string::npos) ? 0 : line_start + 1;

            // First newline after the match begins (i.e., end of that line)
            size_t eol = src.find('\n', match_begin);
            size_t line_end = (eol == std::string::npos) ? src.size() : (eol);

            // We want to include the whole match, even if it crosses the newline.
            // So choose the later endpoint.
            size_t snippet_end = std::max(match_end, line_end);

            out.emplace_back(src.substr(line_start, snippet_end - line_start));
        }
        return out;
    }

    struct MatchSnippets
    {
        std::vector<std::string> lines; // matching lines returned
        std::size_t totalMatches = 0;   // all matching lines found
    };

    static MatchSnippets extractMatchingLinesLimited(const std::string& src,
                                                    const std::regex& pattern,
                                                    std::size_t maxMatchesPerScope,     // 0 => unlimited for this scope
                                                    std::size_t* remainingTotal)        // null => no global limit
    {
        MatchSnippets r;
        const bool unlimitedScope = (maxMatchesPerScope == 0);

        std::istringstream iss(src);
        std::string line;
        while (std::getline(iss, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // If we have a global budget and it's exhausted, stop scanning this src.
            if (remainingTotal && *remainingTotal == 0)
                break;

            if (std::regex_search(line, pattern))
            {
                ++r.totalMatches;

                const bool canStoreByScope = unlimitedScope || (r.lines.size() < maxMatchesPerScope);
                const bool canStoreByTotal = (!remainingTotal || *remainingTotal > 0);

                if (canStoreByScope && canStoreByTotal)
                {
                    line = trimLeadingWhitespace(line);
                    r.lines.push_back(line);

                    if (remainingTotal) --(*remainingTotal);
                }
            }
        }
        return r;
    }

    static void appendGroupedBlock(std::string& out,
                                   const std::string& header,
                                   bool& headerPrinted,
                                   const std::string& labelLine,          // e.g. "(in function foo):"
                                   const MatchSnippets& m,
                                   std::size_t indentSpaces = 4)
    {
        if (m.lines.empty()) return;

        if (!headerPrinted)
        {
            out += header;
            headerPrinted = true;
        }

        out += labelLine;
        out += "\n";

        const std::string indent(indentSpaces, ' ');
        for (const auto& ln : m.lines)
        {
            out += indent;
            out += ln;
            out += "\n";
        }

        if (m.totalMatches > m.lines.size())
        {
            out += indent;
            out += "Other remaining matches: ";
            out += std::to_string(m.totalMatches - m.lines.size());
            out += "\n";
        }

        out += "\n";
    }

    std::string CCodeProject::searchSource(const std::regex& pattern,
                                           std::size_t maxMatchesPerFunction,
                                           std::size_t maxMatchesPerType,
                                           std::size_t maxTotalMatchedLines) const
    {
        std::string result;

        std::size_t remaining = maxTotalMatchedLines;          // only used if maxTotalMatchedLines != 0
        std::size_t* remainingPtr = (maxTotalMatchedLines == 0) ? nullptr : &remaining;

        // --- Data types ---
        bool dataFound = false;
        for (const auto& type : m_objectTypes)
        {
            if (remainingPtr && *remainingPtr == 0) break;

            const std::string dataType =
                (type.second.m_typeDef.m_type == TypeDefinition::STRUCT) ? "struct" : "enum";

            const std::string& src = type.second.m_typeDef.m_definition;

            auto m = extractMatchingLinesLimited(src, pattern, maxMatchesPerType, remainingPtr);

            appendGroupedBlock(
                result,
                "\n\nThe search pattern was found in the following data types:\n\n",
                dataFound,
                "(in " + dataType + " " + type.first + "):",
                m, 0
            );
        }

        // --- Functions ---
        bool functionsFound = false;
        for (const auto& node : m_nodeMap)
        {
            if (remainingPtr && *remainingPtr == 0) break;

            const auto* ccNode = static_cast<const CCodeNode*>(node.second);
            const std::string& src = ccNode->m_implementation.m_source;

            auto m = extractMatchingLinesLimited(src, pattern, maxMatchesPerFunction, remainingPtr);

            appendGroupedBlock(
                result,
                "The search pattern was found in the following functions:\n\n",
                functionsFound,
                "(in function " + node.first + "):",
                m, 0
            );
        }

        return result;
    }

    std::set<std::string> CCodeProject::getConflictsWithData(const std::set<std::string>& calledFunctions)
    {
        std::set<std::string> dataConflicts;
        
        for(const auto& call : calledFunctions)
        {
            if(nodeMap().find(call) != nodeMap().end()) continue;
            
            std::string owningPath;
            auto definedData = findData(call, owningPath);
            if(definedData)
            {
                dataConflicts.insert(call);
            }
        }
        
        return dataConflicts;
    }

    std::set<std::string> CCodeProject::getConflictsWithFunctions(const std::set<std::string>& dataDefinitions)
    {
        std::set<std::string> functionConflicts;
        
        for(const auto& definedData : dataDefinitions)
        {
            std::string owningPath;
            auto existingData = findData(definedData, owningPath);
            if(existingData) continue;
            
            auto it = nodeMap().find(definedData);
            if(it != nodeMap().end())
            {
                functionConflicts.insert(definedData);
            }
        }
        
        return functionConflicts;
    }

    /**
     * Commit only selected source-like files recursively in `folder`.
     * - Initializes a repo in `folder` if needed.
     * - Stages and commits only *.json + *.cpp changes (adds/modifies/deletes).
     * - Returns the new commit hash, or "" if there was nothing to commit and repo had no HEAD.
     */
    std::string CCodeProject::commit(const std::string& folder, const std::string& commitMessage)
    {
        const boost_fs::path repo = boost_fs::absolute(folder);

        if (!boost_fs::exists(repo) || !boost_fs::is_directory(repo)) {
            throw std::runtime_error("commit(): folder does not exist or is not a directory: " + repo.string());
        }

        const std::string repoQ = shQuote(repo.string());

        // 1) Initialize repo if needed
        if (!boost_fs::exists(repo / ".git")) {
            (void)exec("git -C " + repoQ + " init",
                       repo.string(), "gitInit", /*deleteOutput*/true);
        }

        // Helper: run git command that may fail (e.g. HEAD doesn't exist yet) and return "" instead of throwing.
        auto tryGit = [&](const std::string& cmd, const char* tag) -> std::string {
            try {
                return exec(cmd, repo.string(), tag, /*deleteOutput*/true);
            } catch (...) {
                return std::string();
            }
        };

        // Record current HEAD (may be empty if repo has no commits yet)
        const std::string prevHash = trim(tryGit(
            "git -C " + repoQ + " rev-parse --verify HEAD",
            "gitRevParseHeadPrev"
        ));

        // Pathspecs for the ONLY file types we want to stage/commit.
        // IMPORTANT: keep each spec separately quoted, and place after `--`.
        const std::string includeSpec =
            " ':(glob)**/*.json'"
            " ':(glob)**/*.cpp'";

        // 2) Stage selected changes recursively (adds/modifies/deletes)
        (void)exec("git -C " + repoQ + " add -A --" + includeSpec,
                   repo.string(), "gitAddJsonCpp", /*deleteOutput*/true);

        // 3) Check if there is anything staged among the selected file types
        const std::string staged = exec(
            "git -C " + repoQ + " diff --cached --name-only --" + includeSpec,
            repo.string(), "gitDiffCachedJsonCpp", /*deleteOutput*/true
        );

        if (trim(staged).empty()) {
            // Nothing to commit -> return previous commit (HEAD), if it exists.
            return prevHash; // may be ""
        }

        // 4) Write commit message to a temporary file
        const boost_fs::path msgPath = repo / ".git" / "ai_commit_message.txt";
        {
            std::ofstream mf(msgPath.string(), std::ios::binary);
            if (!mf) {
                throw std::runtime_error("commit(): unable to create commit message file: " + msgPath.string());
            }
            mf << commitMessage << std::endl;
        }
        const std::string msgQ = shQuote(msgPath.string());

        // 5) Commit only selected changes.
        const std::string commitCmd =
            "git -C " + repoQ +
            " -c user.name='AI Agent' -c user.email='agent@noreply.local' "
            "commit -F " + msgQ + " --only --" + includeSpec;

        (void)exec(commitCmd, repo.string(), "gitCommitJsonCpp", /*deleteOutput*/true);

        // 6) Get the resulting commit hash
        const std::string hash = trim(exec(
            "git -C " + repoQ + " rev-parse HEAD",
            repo.string(), "gitRevParseHead", /*deleteOutput*/true
        ));

        // 7) Cleanup (best-effort)
        boost_fs::remove(msgPath);

        return hash;
    }

    /**
     * Revert working tree + index to the last commit (HEAD) unconditionally.
     * - Aborts in-progress operations best-effort (merge/rebase/cherry-pick/revert).
     * - Resets tracked files to HEAD (discard local changes + staging).
     * - Removes untracked files/dirs (git clean -fd).
     * - Returns HEAD hash after revert, or "" if repo has no commits yet.
     *
     * NOTE: git clean -fd does NOT remove ignored files. If you want that too, use -fdx.
     */
    std::string CCodeProject::revert(const std::string& folder)
    {
        const boost_fs::path repo = boost_fs::absolute(folder);

        if (!boost_fs::exists(repo) || !boost_fs::is_directory(repo)) {
            throw std::runtime_error("revert(): folder does not exist or is not a directory: " + repo.string());
        }

        const std::string repoQ = shQuote(repo.string());

        if (!boost_fs::exists(repo / ".git")) {
            throw std::runtime_error("revert(): not a git repo (missing .git): " + repo.string());
        }

        auto tryGit = [&](const std::string& cmd, const char* tag) -> std::string {
            try { return exec(cmd, repo.string(), tag, /*deleteOutput*/true); }
            catch (...) { return std::string(); }
        };

        // Determine if HEAD exists (repo might have no commits yet)
        const std::string headBefore = trim(tryGit(
            "git -C " + repoQ + " rev-parse --verify HEAD",
            "gitRevParseHeadBeforeRevert"
        ));

        if (headBefore.empty()) {
            // No commits yet -> nothing meaningful to reset to.
            // Best-effort: clear staging by removing index (avoids "staged" state).
            boost_fs::remove(repo / ".git" / "index");
            return "";
        }

        // Best-effort: abort any in-progress operations (ignore failures)
        (void)tryGit("git -C " + repoQ + " merge --abort",       "gitMergeAbort");
        (void)tryGit("git -C " + repoQ + " rebase --abort",      "gitRebaseAbort");
        (void)tryGit("git -C " + repoQ + " cherry-pick --abort", "gitCherryPickAbort");
        (void)tryGit("git -C " + repoQ + " revert --abort",      "gitRevertAbort");

        // 1) Discard tracked changes + unstage everything
        (void)exec("git -C " + repoQ + " reset --hard HEAD",
                   repo.string(), "gitResetHardHead", /*deleteOutput*/true);

        // 2) Remove untracked files/dirs (keeps ignored files)
        (void)exec("git -C " + repoQ + " clean -fd",
                   repo.string(), "gitCleanFd", /*deleteOutput*/true);

        // Return the resulting HEAD
        return trim(exec("git -C " + repoQ + " rev-parse HEAD",
                         repo.string(), "gitRevParseHeadAfterRevert", /*deleteOutput*/true));
    }

    /**
     * Return the last `maxCommits` commits that touched `filePath`, including:
     * - full commit message (subject + body)
     * - patch/diff for THAT file only
     *
     * Notes:
     * - Uses `git log -p -- <file>` so the patch is restricted to the file.
     * - If `followRenames` is true, history follows renames (git --follow).
     * - Returns "" if repo doesn't exist or the file has no history.
     */
    std::string CCodeProject::getGitHistory(const std::string& folder,
                                                             const std::string& filePath,
                                                             std::size_t maxCommits,
                                                             bool followRenames /*=true*/)
    {
        const boost_fs::path repo = boost_fs::absolute(folder);

        if (!boost_fs::exists(repo) || !boost_fs::is_directory(repo)) {
            throw std::runtime_error("getGitHistory(): folder does not exist or is not a directory: " + repo.string());
        }

        if (!boost_fs::exists(repo / ".git")) {
            return std::string(); // not a git repo
        }

        const std::string repoQ = shQuote(repo.string());

        // Make a repo-relative path for git (best-effort).
        boost_fs::path p(filePath);
        boost_fs::path absFile = p.is_absolute() ? boost_fs::absolute(p) : boost_fs::absolute(repo / p);

        boost_fs::path rel = p;
        try {
            rel = boost_fs::relative(absFile, repo);
        } catch (...) {
            // If relative() fails, fall back to the original input.
            rel = p;
        }

        // Git prefers forward slashes; generic_string() does that.
        const std::string relS  = rel.generic_string();
        const std::string fileQ = shQuote(relS);

        const std::string nPart      = (maxCommits > 0) ? (" -n " + std::to_string(maxCommits)) : "";
        const std::string followPart = followRenames ? " --follow" : "";

        // Pretty header + full commit message (%B) + patch (-p), restricted to the pathspec after `--`.
        // `core.quotepath=false` keeps paths readable (no octal escapes).
        const std::string cmd =
            "git -C " + repoQ +
            " -c core.quotepath=false"
            " log" + nPart + followPart +
            " --no-color --date=iso-strict"
            " --pretty=format:'===%ncommit %H%nDate: %ad%n%n%B%n'"
            " -p -- " + fileQ;

        std::string out;
        try {
            out = exec(cmd, repo.string(), "gitLogFileWithPatch", /*deleteOutput*/true);
        } catch (...) {
            return std::string();
        }

        // If the file has no history, git log returns empty.
        if (trim(out).empty()) return std::string();
        return out;
    }

    /**
     * Revert working tree + index to a specific commit (hard reset) unconditionally.
     * - Aborts in-progress operations best-effort (merge/rebase/cherry-pick/revert).
     * - Verifies the target resolves to a commit object.
     * - Resets tracked files + index to that commit (discard local changes + staging).
     * - Removes untracked files/dirs (git clean -fd).
     * - Returns resulting HEAD hash, or "" if repo has no commits yet.
     *
     * NOTE: This MOVES HEAD (and the current branch pointer if you're on a branch).
     *       If you want to keep HEAD and only restore files, use git restore --source instead.
     */
    std::string CCodeProject::revertToCommit(const std::string& folder,
                                            const std::string& commitish)
    {
        int32_t request_id = Client::getInstance().getRequestId();
        
        const boost_fs::path repo = boost_fs::absolute(folder);

        if (!boost_fs::exists(repo) || !boost_fs::is_directory(repo)) {
            throw std::runtime_error("revertToCommit(): folder does not exist or is not a directory: " + repo.string());
        }

        const std::string repoQ = shQuote(repo.string());

        if (!boost_fs::exists(repo / ".git")) {
            throw std::runtime_error("revertToCommit(): not a git repo (missing .git): " + repo.string());
        }

        auto tryGit = [&](const std::string& cmd, const char* tag) -> std::string {
            try { return exec(cmd, repo.string(), tag, /*deleteOutput*/true); }
            catch (...) { return std::string(); }
        };

        // Determine if HEAD exists (repo might have no commits yet)
        const std::string headBefore = trim(tryGit(
            "git -C " + repoQ + " rev-parse --verify HEAD",
            "gitRevParseHeadBeforeRevertToCommit"
        ));

        if (headBefore.empty()) {
            // No commits yet -> nothing meaningful to reset.
            boost_fs::remove(repo / ".git" / "index");
            return "";
        }

        if (trim(commitish).empty()) {
            throw std::runtime_error("revertToCommit(): empty commitish");
        }

        // Best-effort: abort any in-progress operations (ignore failures)
        (void)tryGit("git -C " + repoQ + " merge --abort",       "gitMergeAbort");
        (void)tryGit("git -C " + repoQ + " rebase --abort",      "gitRebaseAbort");
        (void)tryGit("git -C " + repoQ + " cherry-pick --abort", "gitCherryPickAbort");
        (void)tryGit("git -C " + repoQ + " revert --abort",      "gitRevertAbort");

        // Resolve + verify target is a commit object
        const std::string targetSpecQ = shQuote(commitish + "^{commit}");
        const std::string targetHash = trim(tryGit(
            "git -C " + repoQ + " rev-parse --verify " + targetSpecQ,
            "gitRevParseVerifyTargetCommit"
        ));

        if (targetHash.empty()) {
            throw std::runtime_error("revertToCommit(): target does not resolve to a commit: " + commitish);
        }

        const std::string targetHashQ = shQuote(targetHash);

        // 1) Move HEAD (and current branch pointer if on a branch) to the target commit
        (void)exec("git -C " + repoQ + " reset --hard " + targetHashQ,
                   repo.string(), "gitResetHardTarget", /*deleteOutput*/true);

        // 2) Remove untracked files/dirs (keeps ignored files)
        (void)exec("git -C " + repoQ + " clean -fd",
                   repo.string(), "gitCleanFdAfterResetTarget", /*deleteOutput*/true);

        // Return resulting HEAD
        return trim(exec("git -C " + repoQ + " rev-parse HEAD",
                         repo.string(), "gitRevParseHeadAfterRevertToCommit", /*deleteOutput*/true));
        
        reload();
        Client::getInstance().setRequestId(request_id);
        saveStats();
    }

    std::string CCodeProject::currentCommit(const std::string& folder)
    {
        const boost_fs::path repo = boost_fs::absolute(folder);

        if (!boost_fs::exists(repo) || !boost_fs::is_directory(repo)) {
            throw std::runtime_error("currentCommit(): folder does not exist or is not a directory: " + repo.string());
        }

        if (!boost_fs::exists(repo / ".git")) {
            throw std::runtime_error("currentCommit(): not a git repo (missing .git): " + repo.string());
        }

        const std::string repoQ = shQuote(repo.string());

        auto tryGit = [&](const std::string& cmd, const char* tag) -> std::string {
            try { return exec(cmd, repo.string(), tag, /*deleteOutput*/true); }
            catch (...) { return {}; }
        };

        // HEAD may not exist in a repo with no commits.
        return trim(tryGit("git -C " + repoQ + " rev-parse --verify HEAD",
                           "gitRevParseHeadCurrent"));
    }

    std::string CCodeProject::currentCommit()
    {
        std::string dagDirectory = m_projDir + "/dag";
        return currentCommit(dagDirectory);
    }

    std::string CCodeProject::revertToCommit(const std::string& commitish)
    {
        std::string dagDirectory = m_projDir + "/dag";
        std::string result = revertToCommit(dagDirectory, commitish);
        return result;
    }

    std::string CCodeProject::commit(const std::string& commitMessage)
    {
        std::string dagDirectory = m_projDir + "/dag";
        return commit(dagDirectory, commitMessage);
    }

    std::string CCodeProject::revert()
    {
        std::string dagDirectory = m_projDir + "/dag";
        std::string result = revert(dagDirectory);
        
        reload();
        return result;
    }

    static std::string firstLine(const std::string& s)
    {
        std::istringstream iss(s);
        std::string line;
        if (std::getline(iss, line)) return trim(line);
        return {};
    }

    std::string CCodeProject::createBranchFromCurrent(const std::string& folder,
                                                      const std::string& branchName)
    {
        const boost_fs::path repo = boost_fs::absolute(folder);
        if (!boost_fs::exists(repo) || !boost_fs::is_directory(repo))
            throw std::runtime_error("createBranchFromCurrent(): invalid folder: " + repo.string());

        const std::string repoQ = shQuote(repo.string());
        const std::string brQ   = shQuote(branchName);

        auto tryGit = [&](const std::string& cmd, const char* tag) -> std::string {
            try { return trim(exec(cmd, repo.string(), tag, /*deleteOutput*/true)); }
            catch (...) { return {}; }
        };

        // Record where we currently are ("" if unborn repo)
        const std::string base = tryGit("git -C " + repoQ + " rev-parse --verify HEAD 2>/dev/null",
                                        "gitRevParseHeadBase");

        // Validate branch name
        (void)exec("git -C " + repoQ + " check-ref-format --branch " + brQ,
                   repo.string(), "gitCheckRefFormat", true);

        // Create + switch
        (void)exec("git -C " + repoQ + " switch -c " + brQ,
                   repo.string(), "gitSwitchCreateBranch", true);

        // Persist branch point (only if we had a commit)
        if (!base.empty()) {
            const std::string key = "branch." + branchName + ".branchPoint";
            (void)exec("git -C " + repoQ + " config --local " + shQuote(key) + " " + shQuote(base),
                       repo.string(), "gitStoreBranchPoint", true);
        }

        return base;
    }


    // Resets an existing branch back to the commit it branched from (DESTRUCTIVE).
    // Strategy:
    //   1) If branch has an upstream: use merge-base --fork-point (best), fallback merge-base
    //   2) Else: fallback to the oldest entry in the branch reflog (often the creation point)
    // Notes:
    //   - Requires a clean working tree (by default) to avoid clobbering local changes.
    //   - If no upstream and reflog is expired/truncated, inference can fail.

    static std::string lastNonEmptyLine(const std::string& s)
    {
        std::istringstream iss(s);
        std::string line, last;
        while (std::getline(iss, line)) {
            line = trim(line);
            if (!line.empty()) last = line;
        }
        return last;
    }

    static std::string firstNonEmptyLine(const std::string& s)
    {
        std::istringstream iss(s);
        std::string line;
        while (std::getline(iss, line)) {
            line = trim(line);
            if (!line.empty()) return line;
        }
        return {};
    }

    std::string CCodeProject::inferBranchPointCommit(const std::string& folder,
                                                     const std::string& branchName)
    {
        const boost_fs::path repo = boost_fs::absolute(folder);
        const std::string repoQ = shQuote(repo.string());

        auto tryGit = [&](const std::string& cmd, const char* tag) -> std::string {
            try { return trim(exec(cmd, repo.string(), tag, /*deleteOutput*/true)); }
            catch (...) { return {}; }
        };
        auto isCommit = [&](const std::string& h) -> bool {
            if (h.empty()) return false;
            return !tryGit("git -C " + repoQ + " rev-parse --verify " + shQuote(h + "^{commit}") + " 2>/dev/null",
                           "gitVerifyCommit").empty();
        };

        // 1) Recorded branch point (reliable, no upstream needed)
        {
            const std::string key = "branch." + branchName + ".branchPoint";
            const std::string recorded = tryGit("git -C " + repoQ + " config --get " + shQuote(key) + " 2>/dev/null",
                                                "gitGetRecordedBranchPoint");
            if (isCommit(recorded)) return recorded;
        }

        // 2) Fallback: oldest reflog entry (best-effort)
        const std::string reflog = tryGit(
            "git -C " + repoQ + " reflog show --format=%H " + shQuote("refs/heads/" + branchName) + " 2>/dev/null",
            "gitReflogShow"
        );
        const std::string candidate = lastNonEmptyLine(reflog);
        if (isCommit(candidate)) return candidate;

        return {};
    }

    std::string CCodeProject::resetBranchToBranchedFromCommit(const std::string& folder,
                                                              const std::string& branchName)
    {
        uint32_t request_id = Client::getInstance().getRequestId();
        
        const boost_fs::path repo = boost_fs::absolute(folder);
        if (!boost_fs::exists(repo) || !boost_fs::is_directory(repo)) {
            std::cout << "resetBranchToBranchedFromCommit(): invalid folder: " << repo.string() << std::endl;
            return std::string();
        }

        const std::string repoQ = shQuote(repo.string());
        const std::string brQ   = shQuote(branchName);

        // Are we already on that branch?
        const std::string cur = trim(exec(
            "git -C " + repoQ + " rev-parse --abbrev-ref HEAD",
            repo.string(), "gitCurrentBranch", true
        ));
        if (cur != branchName) {
            std::cout << "Not on branch '" << branchName << "'. (No-switch mode requires it.)" << std::endl;
            return std::string();
        }

        const std::string base = inferBranchPointCommit(folder, branchName);
        if (base.empty()) {
            std::cout << "Could not infer branched-from commit for '" << branchName << "'." << std::endl;
            return std::string();
        }

        // Discard everything (tracked changes)
        (void)exec("git -C " + repoQ + " reset --hard " + shQuote(base),
                   repo.string(), "gitResetHardToBase", true);

        // Optional: also remove untracked files/dirs
        (void)exec("git -C " + repoQ + " clean -fd",
                   repo.string(), "gitCleanUntracked", true);
        
        reload();
        Client::getInstance().setRequestId(request_id);
        saveStats();

        return base;
    }


    std::string CCodeProject::getHighLevelAppInfo(const std::string& functionName,
                                              int functionsMaxDepth, int callGraphMaxDepth)
    {
        std::string info;
        
        if(callGraphMaxDepth)
        {
            info += printGraph(functionName, callGraphMaxDepth, true);
        }
        
        if(functionsMaxDepth)
        {
            if(functionsMaxDepth < 0) {
                info += "List of all functions defined by this application:\n\n";
            }
            else {
                info += "List of all functions defined by this application from the call graph with maximum depth of up to ";
                info += std::to_string(functionsMaxDepth) + ":\n\n";
            }
            
            info += listAllFunctions(functionName, functionsMaxDepth, true, true, false, {});
        }
        
        return info;
    }

}
