#include "CCodeNode.h"
#include "Node.hpp"
#include "Project.h"
#include "Utils.h"
#include "CCodeProject.h"
#include "Client.h"
#include "Graph.hpp"
#include "UtilsCodeAnalysis.h"
#include "Debugger.h"

using namespace web;
using namespace utility;

namespace hen {

    void CCodeNode::reviewDataLoop(std::string& cache, std::string& source, const std::string& typeName,
                                   boost::optional<const DataInfo&> existingData,
                                   std::map<std::string, TypeDefinition>& dataDefinitions,
                                   std::set<std::string>& referencedNodes, bool stopAndWait)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        client.selectLLM(InferenceIntent::DATA);
        
        std::string reviseMessage;
        bool loop = true;
        int escalateThreshold = 0;
        
        int attempts = 0;
        
        uint32_t startReviewTag = captureContext();
        
        std::string srcMessage = "```cpp\n";
        srcMessage += source;
        srcMessage += "```\n";
        pushMessage(srcMessage, "assistant");
        
        while(loop && attempts < MAX_REVIEW_ATTPMPTS)
        {
            reviewData(source, typeName, dataDefinitions, referencedNodes);
            
            if(m_codeReview.str().empty())
                break;
            
            if(escalateThreshold >= ESCALATE_AFTER_FAILED_REVIEWS)
            {
                client.escalateLLM();
                escalateThreshold = 0;
            }
            
            if(attempts > MAX_REVIEW_HISTORY)
            {
                //Erase the first review request and response
                proj->eraseFromContext(startReviewTag, 2);
            }
            
            //Clear the cache. We must get the data definition right!
            cache.clear();
            
            if(stopAndWait) client.stop();
            
            reviseMessage = proj->review_data.prompt({
                {"struct", typeName},
                {"review", m_codeReview.str()}});
            
            source = "cpp";
            
            bool enforceAnalysis = attempts >= REVIEW_ATTPMPTS_TO_TRIGGER_ANALYSIS;
            loop = inferenceDataSource(cache, enforceAnalysis, reviseMessage, typeName, source);

            if(existingData && startsWithIgnoreCase(source, "ACTUAL"))
            {
                m_codeReview.str("");
                m_codeReview.clear();
                
                break;
            }
            else if(startsWithIgnoreCase(source, "SKIP"))
            {
                m_codeReview.str("");
                m_codeReview.clear();
                
                break;
            }
            
            srcMessage = "```cpp\n";
            srcMessage += source;
            srcMessage += "```\n";
            
            escalateThreshold++;
            attempts++;
        }
        
        popContext();
    }

    bool CCodeNode::inferenceDataSource(std::string& cache, bool enforceAnalysis, const std::string& message, const std::string& typeName, std::string& source)
    {
        bool truncated = enforceAnalysis;
        bool loop = true;
        
        uint32_t beforeMessage = captureContext();
        if(!truncated)
        {
            loop = inference(cache, message, source, &truncated);
        }
        
        if(truncated)
        {
            cache = "na";
            uint32_t archiveId = storeContext(beforeMessage);
            pushMessage(message, "user");
            pushMessage("Source code truncated due to length. Analyze which structs and enums need updates, then modify them individually.", "assistant");
            
            DataDefList list;
            loop = loop && dataRequirementsAnalysis(list, typeName);
            loop = loop && inferenceDataLoop(cache, list, typeName, source);
            restoreContext(archiveId);
        }
        popContext();
        
        pushMessage(message, "user");
        std::string sourceMsg = "```cpp\n";
        sourceMsg += source;
        sourceMsg += "\n```";
        pushMessage(sourceMsg, "assistant");
        
        return loop;
    }

    void CCodeNode::reasonAboutData(std::string& cache, std::string& source, const std::string& typeName)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        client.selectLLM(InferenceIntent::REASON_DATA);
        
        std::string reviewMessage = proj->review_data_self.prompt({
            {"struct", typeName} });

        bool selfReviewPass = inference(cache, reviewMessage, true);

        bool reviewPass = m_codeReview.str().empty();
        std::string reviseMessage;
        
        if(!selfReviewPass)
        {
            reviseMessage += "Would you please fix this and update the data types definitions in the source?";
        }
        
        if(!reviewPass)
        {
            //Clear the cache. We must get the data definition right!
            if(selfReviewPass)
            {
                reviseMessage.clear();
            }
            cache.clear();
            
            reviseMessage += " Here are a few other things that may also require attention, but aren't necessarily a problem: \n";
            reviseMessage += m_codeReview.str();
        }
        
        if(!selfReviewPass || !reviewPass)
        {
            client.selectLLM(InferenceIntent::DATA);
            
            reviseMessage += "\nProvide answer in markdown syntax. In your response prefix the C++ source code with ```cpp and postfix the code with ```";
            
            source = "cpp";
            inferenceDataSource(cache, false, reviseMessage, typeName, source);
        }
    }

    bool CCodeNode::dataRequirementsAnalysis(DataDefList& list, const std::string& typeName)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        captureContext();
        
        LLMRole savedLLM = client.getLLM();
        if(savedLLM < client.getLLMIntent(InferenceIntent::REASON_DATA))
        {
            client.selectLLM(InferenceIntent::REASON_DATA);
        }
        
        std::string analysisMsg = proj->define_data_request.prompt({
            {"struct", typeName},
        });
        
        std::string cache = "";
        bool loop = inference(cache, analysisMsg, &list);
        
        std::string reviewAnalysisMsg = proj->review_data_request_self.prompt({
            {"struct", typeName},
        });
        
        bool selfReviewPass = inference(cache, reviewAnalysisMsg, true);

        if(!selfReviewPass)
        {
            loop = loop && inference(cache, "Plase improve the analysis based on this feedback!", &list);
        }
        
        client.setLLM(savedLLM);
        
        popContext();
        
        return loop;
    }

    bool CCodeNode::inferenceDataLoop(std::string& cache, DataDefList& list, const std::string& typeName, std::string& source)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        bool loop = true;
        
        std::string analysisJson = utility::conversions::to_utf8string(list.to_json().serialize());
        analysisJson = formatJson(analysisJson, std::string("  "));
        
        if(list.items.empty())
        {
            source = "ACTUAL";
            return loop;
        }
        
        std::string context = "Undating one by one definitions for the following data types: ";
        bool first = true;
        for(auto def : list.items)
        {
            if(!first) {
                context += ", ";
            }
            
            context += def->type + " " + def->type_name;
            first = false;
        }
        
        context += "\n\n";
        
        first = true;
        std::string assembledSource;
        for(auto def : list.items)
        {
            captureContext();
            
            std::string analysisRequest = "Analyze reqiured data type definitions and modifications related to '";
            analysisRequest += typeName;
            analysisRequest += "' based on the information provided in our discussion";
            pushMessage(analysisRequest, "user");
            pushMessage(analysisJson, "assistant");
            
            std::string defSource = "cpp";
            bool truncated = false;
            
            std::string defineDataMessage = proj->define_data_loop.prompt({
                {"struct", def->type_name},
                {"context", context}
            });
            
            loop = loop && inference(cache, defineDataMessage, defSource, &truncated);
            if(truncated)
            {
                //Now this is really bad!
                std::cout << "ERROR: Truncated data definition" << std::endl;
            }
            
            std::map<std::string, TypeDefinition> dataDefinitions;
            reflectData(defSource, dataDefinitions);
            
            auto it = dataDefinitions.find(def->type_name);
            if(it != dataDefinitions.end())
            {
                defSource = it->second.m_definition;
            }
            
            if(!assembledSource.empty())
            {
                assembledSource += "\n\n";
            }
            assembledSource += defSource;
            
            if(first) {
                context += "Currently updated data types:\n\n";
            }
            context += defSource;
            context += "\n\n";
            
            popContext();
            
            first = false;
        }
        
        source = assembledSource;
        return loop;
    }

    std::string CCodeNode::inferenceData(const std::string& defineDataMessage,
                                         const std::string& typeName,
                                         std::map<std::string, TypeDefinition>& dataDefinitions,
                                         std::set<std::string>& referencedNodes,
                                         std::string& cache)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::string source = "cpp";
        std::string owningPath;
        auto existingData = proj->findData(typeName, owningPath);
        
        //If we have cache - file data_def.json in the node dir,
        //that means this node already has updated the data with it's requirements
        //Since all data edits currently in this framework are non-destructive existing data in the project is actual.
        //If at some point destructive data edits are introduced, this early-out won't work
        std::string cachedData = getNodeDirectory();
        cachedData += "/data_def.json";
        if(existingData && boost_fs::exists(cachedData))
        {
            web::json::value json;
            loadJson(json, cachedData);
            if(json.has_field(U( utility::conversions::to_string_t(typeName) )))
            {
                source = "ACTUAL";
                return source;
            }
        }
        
        client.selectLLM(InferenceIntent::DATA);
        
        inferenceDataSource(cache, false, defineDataMessage, typeName, source);
        
        if(existingData && startsWithIgnoreCase(source, "ACTUAL"))
        {
            return source;
        }
        else if(startsWithIgnoreCase(source, "SKIP"))
        {
            return source;
        }
        
        reviewData(source, typeName, dataDefinitions, referencedNodes);
        
        bool dataDefNeedsReview = !proj->m_cache || cache == "na";
        bool wasOnAuto = client.run();
        if(dataDefNeedsReview)
        {
#ifdef ENABLE_DATA_SELF_REVIEW
            captureContext();
            reasonAboutData(cache, source, typeName);
            popContext();
#endif
            //During the review loop, we don't need the initial date definition provided by the firs inference:
            //inferenceDataSource(cache, false, defineDataMessage, typeName, source);
            popMessages(1);
            
            reviewDataLoop(cache, source, typeName, existingData, dataDefinitions, referencedNodes, !wasOnAuto);
        }
        
        if(!wasOnAuto) client.stop();
        
        return source;
    }

    void CCodeNode::updateDataDefinitions(const std::string& typeName, 
                                          const std::map<std::string, TypeDefinition>& dataDefinitions,
                                          bool checkUnknownDataChanged,
                                          const std::set<std::string>& referencedNodes,
                                          std::set<std::string>& allForUpdate)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string path = getDAGPath("/");
        
        std::string existingDataPath = path;
        auto existingData = proj->findData(typeName, existingDataPath);
        
        std::set<std::string> externals;
        for(const auto& def : dataDefinitions)
        {
            //TODO: Description is not correct here. We need description of the data type, not a given parameter.
            //This need to be additional request for data description
            //const std::string& desc = param.type().description;
        
            //If we have a new data type definition (condition: !existingDef && def.second.m_name != typeName)
            //requested as update to existing data (condition: existingData)
            //This means we need to put the new data definition in the same path as the existing data (updatedPath = existingDataPath)
            std::string updatedPath = path;
            auto existingDef = proj->findData(def.second.m_name, updatedPath);
            if(existingData && !existingDef && def.second.m_name != typeName)
            {
                updatedPath = existingDataPath;
            }
            else if(checkUnknownDataChanged && existingDef &&
                    referencedNodes.find(updatedPath) == referencedNodes.end())
            {
                //We must not be here :( resolveDataDefinitions should guarantee it
                std::cout << "ERROR **********************************************" << std::endl;
                std::cout << "defineData for '" << typeName << "' predefined '" << def.second.m_name;
                std::cout << "' without reviewing the old definition!" << std::endl;
                std::cout << "New definition:" << std::endl << def.second.m_definition << std::endl;
                std::cout << "Old definition:" << std::endl << existingDef->m_typeDef.m_definition << std::endl;
                std::cout << "ERROR **********************************************" << std::endl;
            }
        
            std::string reference;
            //Only the function arguments reference the data
            if(typeName == def.second.m_name)
            {
                reference = path;
            }
            
            //TODO: handle detached data definitions.
            //Detached data is a data type used in the scope of node/function being defined
            //but is still not associated with a function declaration
            
            std::set<std::string> dataTypes = proj->getAppTypesForFunction(m_prototype.m_signature);
            
            if(
               !existingData &&
               !existingDef && //This is a newly introduced data type
               updatedPath == path && //Not defined on other place - Do we need this check after the above 2?
               dataTypes.find(typeName) == dataTypes.end()//defined datat type is not used in the function signature
               )
            {
                updatedPath = "__DETACHED__";
            }
            else if (//If we have a detached data type used in the current signature - attach it to this node
                     existingDef &&
                     updatedPath == "__DETACHED__" &&
                     dataTypes.find(typeName) != dataTypes.end()
                     )
            {
                updatedPath = path;
            }
            
            const auto& refs = proj->updateData(def.second, reference, typeName, updatedPath);
            
            if(updatedPath != path && updatedPath != "__DETACHED__")
            {
                externals.insert(updatedPath);
            }
            
            for(const auto& ref : refs) {
                std::vector<std::string> pathAndParam;
                boost::split(pathAndParam, ref, boost::is_any_of(":"));
                if(pathAndParam[0] != path) //skip this node since we just defined the data after the implementation
                {
                    allForUpdate.insert(pathAndParam[0]);
                }
            }
        }
        
        //Add paths where data structures are defined to externals
        //This is needed to generate header files
        //
        //Due to limitation in the json reflection, this is a workaround to handle the m_implementation.externals vector as a set
        for(const auto& ref : externals)
        {
            addToSet(m_implementation.externals, ref);
        }
    }

    void CCodeNode::defineData(const std::string& typeName, const std::string& addContext, bool reviewData, std::set<std::string>& allForUpdate)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::set<std::string> referencedNodes;
        
        captureContext();
        
        m_stats.reset();
        
        std::string path = getDAGPath("/");
        
        std::string context = addContext;
        std::string parentInfo;
        std::string parentPath;
        if(m_this && m_this->m_parent && m_this->m_parent->m_data)
        {
            CCodeNode* parent = (CCodeNode*)m_this->m_parent->m_data;
            
            context += parent->getContexInfo(false, true, true, referencedNodes);
            
            parentInfo = "Notice this data will be used in the implementation of the function: '";
            parentInfo += parent->m_prototype.declaration;
            parentInfo += "'\n to call: '";
            parentInfo += m_prototype.declaration;
            parentInfo += "'\nTake a look to understand more about the required data fields";
            parentInfo += "\n";
            
            parentPath = parent->getDAGPath("/");
        }
        
        //We need to hint for already existing data but only if it is not defined the parent
        //We will print the full parent context, including the data
        std::string existingDataPath = path;
        auto existingData = proj->findData(typeName, existingDataPath);
        
        //Early out if we don't want to revise existing data
        if(existingData && !reviewData)
        {
            std::set<std::string> references = {{path}};
            proj->addDataReferences(existingData->m_typeDef.m_name, references);
            if(existingDataPath != "__DETACHED__")
            {
                addToSet(m_implementation.externals, existingDataPath);
            }
            
            //This will be used for cache.
            string_t sourceU = utility::conversions::to_string_t(existingData->m_typeDef.m_definition);
            m_dataDef[U(typeName)] = json::value(sourceU);
            
            popContext();
            return;
        }
        
        std::string existingDataInfo;
        if(existingData && existingDataPath != parentPath)
        {
            existingDataInfo = "Notice that data with the same name '";
            existingDataInfo += typeName;
            existingDataInfo += "', is already defined at the following path: \n";
            existingDataInfo += existingDataPath;
            if(referencedNodes.find(existingDataPath) == referencedNodes.end())
            {
                existingDataInfo += "\nHere is the existing data definition for reference\n";
                existingDataInfo += "//*****\n";
                existingDataInfo += proj->declareData(false, existingDataPath);
                existingDataInfo += proj->defineData(false, existingDataPath);
                existingDataInfo += "//*****\n";
            }
            existingDataInfo += "\nYou are welcome to review and modify existing data definitions if needed. ";
            existingDataInfo += "Try to be in a non-destructive way for the rest of the software. ";
            existingDataInfo += "Don't modify the name of existing data types. ";
            existingDataInfo += "Try to reuse existing struct/enum members and if this is not possible then introduce new members. ";
            existingDataInfo += "Read the member names and inline comments to decide on how compatible is each member. ";
            existingDataInfo += "\n";
            //TODO: Should I add the implementation of the function that uses/defines the existing data
            referencedNodes.insert(existingDataPath);
        }
        
        //TODO: NEEDS TESTING: Add __DETACHED__ data definitions to the context?
        {
            std::string detachedData = proj->getDetachedData();
            if(!detachedData.empty() &&
               referencedNodes.find("__DETACHED__") == referencedNodes.end())
            {
                referencedNodes.insert("__DETACHED__");
                context += proj->getDetachedData();
            }
        }
        context += m_implementation.m_source;
        
        std::string defineDataMessage = proj->define_data.prompt({
            {"struct", typeName},
            {"context", context},
            {"function", m_prototype.declaration},
            {"parent", parentInfo},
            {"existing", existingDataInfo},
            {"struct_members", proj->define_struct_members.prompt()}});
        
        std::string cache = "data_def.json:";
        cache += typeName;
        defineDataMessage += "Please, provide only the requested data definitions in your response!\n";
        
        std::map<std::string, TypeDefinition> dataDefinitions;

        std::string source = inferenceData(defineDataMessage, typeName, dataDefinitions, referencedNodes, cache);
        
        if(existingData && startsWithIgnoreCase(source, "ACTUAL"))
        {
            source = existingData->m_typeDef.m_definition;
            if(existingDataPath != "__DETACHED__")
            {
                addToSet(m_implementation.externals, existingDataPath);
            }
        }
        else if(startsWithIgnoreCase(source, "SKIP"))
        {
            std::cout << "Skip data definition for: " << typeName << std::endl;
        }
        else
        {
            updateDataDefinitions(typeName, dataDefinitions, true, referencedNodes, allForUpdate);
        }
        
        //This will be used for cache.
        string_t sourceU = utility::conversions::to_string_t(source);
        m_dataDef[U(typeName)] = json::value(sourceU);
        
        popContext();
    }

    void CCodeNode::defineData(bool reviewData)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::string path = getDAGPath("/");
        proj->attachAllDataTo(path);
        
        std::set<std::string> allForUpdate;
        
        std::set<std::string> dataTypes;
        
        //*****
        bool fullyDefinedNode = true;
        for(auto func : m_calls.items)
        {
            const CCodeNode* node = nullptr;
            auto it = proj->nodeMap().find(func->func_name);
            if(it == proj->nodeMap().end())
            {
                fullyDefinedNode = false;
                break;
            }
            
            node = (const CCodeNode*)it->second;
            if(!node) continue;
            //For whatever reason this node is undefined
            if(node->m_prototype.declaration.empty())
            {
                fullyDefinedNode = false;
                break;
            }
        }
        //*****
        
        std::pair<bool, std::set<std::string>> fromEvaluation = getUnknownTypes();

        // Safety net: if the local review misses a type introduced directly in the
        // signature, seed data-definition work only for app-defined signature types
        // that do not yet exist in the project.
        if(!fullyDefinedNode)
        {
            std::set<std::string> signatureTypes = proj->getAppTypesForFunction(m_prototype.m_signature);
            for(const auto& type : signatureTypes)
            {
                std::string owningPath;
                if(!proj->findData(type, owningPath))
                {
                    dataTypes.insert(type);
                }
            }
        }
        
        m_dataDef = web::json::value();
        
        dataTypes.insert(fromEvaluation.second.begin(), fromEvaluation.second.end());
        
        proj->dataSnapshot();
        
        for(const auto& type : dataTypes)
        {
            std::string addContext;
            defineData(type, addContext, reviewData, allForUpdate);
        }
    }

    void CCodeNode::analyzeData()
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        captureContext();
        
        std::set<std::string> referencedNodes;
        
        std::string context;
        std::string parentInfo;
        std::string parentPath;
        if(m_this && m_this->m_parent && m_this->m_parent->m_data)
        {
            CCodeNode* parent = (CCodeNode*)m_this->m_parent->m_data;
            
            context += parent->getContexInfo(false, true, true, referencedNodes);
            
            parentInfo = "Notice the function '";
            parentInfo += m_brief.func_name;
            parentInfo += "' and data types referenced in its declaration will be used in the implementation of the function: '";
            parentInfo += parent->m_prototype.declaration;
            
            parentInfo += "'\n to call: '";
            parentInfo += m_prototype.declaration;
            parentInfo += "'\nTake a look to understand more about the required data fields";
            parentInfo += "\n";
            
            parentPath = parent->getDAGPath("/");
        }
        
        if(!m_implementation.m_source.empty())
        {
            context += m_implementation.m_source;
            context += "\n\n";
        }
        
        std::string analysis;
        std::string existingTypes;
        std::string newTypes;
        std::set<std::string> newTypesSet;
        std::set<std::string> dataTypes = proj->getAppTypesForFunction(m_prototype.m_signature);
        for(const auto& type : dataTypes)
        {
            std::string owningPath;
            if(proj->findData(type, owningPath))
            {
                if(!existingTypes.empty()) {
                    existingTypes += ", ";
                }
                existingTypes += type;
            }
            else
            {
                if(!newTypes.empty()) {
                    newTypes += ", ";
                }
                newTypes += type;
                newTypesSet.insert(type);
            }
        }
        
        if(!existingTypes.empty() || !newTypes.empty())
        {
            analysis += "\nLooking at the declartion of '";
            analysis += m_brief.func_name + "' \n";
            if(!existingTypes.empty())
            {
                analysis += "The following data types are already defined: ";
                analysis += existingTypes;
                analysis += "\n";
            }
            
            if(!newTypes.empty())
            {
                analysis += "The following data types are new and has to be defined: ";
                analysis += newTypes;
                analysis += "\n";
            }
            
            analysis += "\n";
        }
        
        client.selectLLM(InferenceIntent::REASON_DATA);
        
        std::string analysisMsg = proj->analyze_data_requirements.prompt({
            {"context", context},
            {"function", m_brief.func_name},
            {"parent", parentInfo},
            {"quick_analysis", m_brief.func_name}
        });
        
        std::string cache = "";
        bool loop = inference(cache, analysisMsg, &m_dataAnalysis);
        
        captureContext();
        
        //TODO: Introduce fix loop if any of the newTypes used in the function is not defined
        uint32_t attempts = 0;
        do
        {
            std::string missingNewTypes;
            for(auto type : newTypesSet)
            {
                bool typeFound = false;
                for(auto item : m_dataAnalysis.items)
                {
                    if(item->type_name == type)
                    {
                        typeFound = true;
                        break;
                    }
                }
                
                if(!typeFound)
                {
                    if(!missingNewTypes.empty()) {
                        missingNewTypes += ", ";
                    }
                    missingNewTypes += type;
                }
            }
            
            std::string newDataNotInSource;
            for(auto item : m_dataAnalysis.items)
            {
                std::string owningPath;
                if(!proj->findData(item->type_name, owningPath))
                {
                    if(context.find(item->type_name) == std::string::npos)
                    {
                        if(!newDataNotInSource.empty()) {
                            newDataNotInSource += ", ";
                        }
                        newDataNotInSource += item->type_name;
                    }
                }
            }
            
            if(missingNewTypes.empty() && newDataNotInSource.empty()) {
                break;
            }
            
            std::string missingTypesMsg;
            
            if(!missingNewTypes.empty())
            {
                missingTypesMsg += "The following new data types used in the declartion of '";
                missingTypesMsg += m_prototype.declaration + "' are missing: " + missingNewTypes + "\n";
                missingTypesMsg += "All of the listed types need to be presented in the analysis!\n";
            }
            
            if(!newDataNotInSource.empty())
            {
                missingTypesMsg += "The following new data types can't be found";
                missingTypesMsg += " in the source code related to '" + m_brief.func_name + "' function : " + newDataNotInSource + "\n";
                missingTypesMsg += "Is this intended, are these data type definitions required?\n";
            }
            
            missingTypesMsg += "In the response include all data types that are subject of your analysis: ";
            missingTypesMsg += "new data types and data types that need modification!\n";
            
            captureContext();
            bool loop = inference(cache, missingTypesMsg, &m_dataAnalysis);
            popContext();
        }
        while(attempts++ < MAX_REVIEW_ATTPMPTS);
        
        popContext();
        popMessages(1);
        
        auto uJson = m_dataAnalysis.to_json().serialize();
        std::string json = utility::conversions::to_utf8string(uJson);
        json = formatJson(json, "  ");
        pushMessage(json, "assistant");
        
        std::string reviewAnalysisMsg = proj->review_data_requirements_self.prompt({
            {"function", m_brief.func_name}
        });
        
        bool selfReviewPass = inference(cache, reviewAnalysisMsg, true);
        
        if(!selfReviewPass)
        {
            std::string improveMessage = "Plase improve the analysis based on this feedback! ";
            improveMessage += "In the response include all data types that are subject of your analysis: ";
            improveMessage += "new data types and data types that need modification!\n";
            loop = loop && inference(cache, improveMessage, &m_dataAnalysis);
        }
        
        popContext();
        
        proj->dataSnapshot();
        
        std::string analysisJson = utility::conversions::to_utf8string(m_dataAnalysis.to_json().serialize());
        analysisJson = formatJson(analysisJson, std::string("  "));
        
        std::set<std::string> allForUpdate;
        for(const auto& item : m_dataAnalysis.items)
        {
            std::string addContext = "Analysis for new data types and update on existing data types ";
            addContext += "required for the implementation of the function: ";
            addContext += getName() + "\n";
            addContext += analysisJson + "\n";
            defineData(item->type_name, addContext, true, allForUpdate);
        }
        
        return loop;
    }

    std::set<std::string> CCodeNode::getUnknownTypes(const std::string& review)
    {
        std::set<std::string> typesFromReview = extractUnknownTypes(review);
        
        std::set<std::string> newTypes;
        for(auto type : typesFromReview)
        {
            std::string owningPath;
            //All unknown types will be added to the list, inclding existing tyeps
            if(// !proj->findData(type, owningPath) &&
               type != "AnyReturn" && type != "ReturnValueHelper" && //Exclude function stub helpers
               m_stats.m_callingNonExistingFunction.find(type) == m_stats.m_callingNonExistingFunction.end())
            {
                newTypes.insert(type);
            }
        }
        
        return newTypes;
    }

    std::pair<bool, std::set<std::string>> CCodeNode::getUnknownTypes()
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        m_enableDiagnostics = true;
        m_reviewDiagnostics = true;
        
        std::string source = m_implementation.m_source;
        reviewImplementation(source, CodeType::FUNC_IMPL);
        
        m_enableDiagnostics = false;
        m_reviewDiagnostics = false;
        
        std::string review = m_codeReview.str();
        
        bool hasErrors = !review.empty();
        
        std::cout << "Evaluating '" << m_brief.func_name << "' for unknown data types..." << std::endl;
        std::cout << "******* Evaluation Start *******" << std::endl;
        std::cout << review;
        std::cout << "******* Evaluation End *******" << std::endl;
        
        std::set<std::string> newTypes = getUnknownTypes(review);
        
        return std::make_pair(hasErrors, newTypes);
    }

    void CCodeNode::includeUnknownTypes()
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        auto unknownTypes = getUnknownTypes(m_codeReview.str());
        //int iteration = 0;
        //while(!unknownTypes.empty() && iteration < MAX_UNKNOWN_TYPES_ATTEMPTS)
        {
            for(auto type : unknownTypes)
            {
                std::string owningPath;
                auto typeDef = proj->findData(type, owningPath);
                if(typeDef)
                {
                    if(typeDef->m_ownerPath != "__DETACHED__")
                    {
                        addToSet(m_implementation.dependencies, typeDef->m_ownerPath);
                    }
                }
            }
        }
    }

    std::string CCodeNode::codeAndDataReview(const std::string& source, std::set<std::string> referencedNodes, bool reviewDataDefinitions, bool updateSource)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        //Extract the changed function and datat types (if any changed data types)
        std::map<std::string, TypeDefinition> dataTypes;
        std::map<std::string, std::string> functions;
        extractFromSource(source, false, dataTypes, functions);
        
        if(dataTypes.empty() && functions.empty())
        {
            std::string message = "No code or data definitions found in the provided source\n";
            std::cout << message;
            return message;
        }
        
        if(!functions.empty() && functions.find(m_brief.func_name) == functions.end())
        {
            std::string message = "Couldn't find function '" + m_brief.func_name + "' in the provided source\n";
            std::cout << message;
            return message;
        }
        
        //Perform reviews with relaxed checks - for example checks for auto keyword or consts
        //As these are very common for trivial fail of reviews.
        m_reviewLevel = CCodeNode::ReviewLevel_1;
        
        std::string path = getDAGPath("/");
        
        std::string dataReviewIssues;
        
        bool hasIssues = false;
        if(reviewDataDefinitions)
        {
            for(auto def : dataTypes) {
                
                std::map<std::string, TypeDefinition> unusedDataTypes;
                reviewData(def.second.m_definition, def.second.m_name, unusedDataTypes, referencedNodes);
                dataReviewIssues += m_codeReview.str();
                
                //We need to update the data so that the code review with reviewImplementation will be on code+data
                std::string owningPath = path;
                std::string desc = def.first;
                proj->updateData(def.second, path, desc, owningPath);
            }
        }
        
        m_enableDiagnostics = true;
        m_reviewDiagnostics = true;
        
        Analyzer_DefineUnknownType::m_enabled = true;
        Analyzer_DefineUnknownType::m_hintToCreate = true;
        
        std::string sourceToReview = functions[m_brief.func_name];
        reviewImplementation(sourceToReview, CodeType::FUNC_IMPL);
        
        Analyzer_DefineUnknownType::m_hintToCreate = false;
        
        m_enableDiagnostics = false;
        m_reviewDiagnostics = false;
        m_reviewLevel = CCodeNode::ReviewLevel_ALL;
        
        std::string review = dataReviewIssues;
        std::string filteredErrors = filterAnyReturnErrors(m_codeReview.str());
        review += filteredErrors;
        
        {
            std::set<std::string> owners;
            std::set<std::string> structs;
            std::set<std::string> enums;
            
            std::string analysis = analyzeCompilation(m_codeReview.str(), owners, structs, enums);
            
            if(!analysis.empty()) {
                review += "Here is my quick analysis based on the above errors: \n";
                review += analysis;
            }
        }
        
        if(updateSource)
        {
            m_implementation.m_source = sourceToReview;
        }
        
        return review;
    }

    bool CCodeNode::verify()
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        bool wasOnAuto = client.run();
        
        auto dataSnapshot = proj->getDataShapshot();
        std::string savedSource = m_implementation.m_source;
        Function savedDefinition = m_prototype;
        
        client.selectLLM(InferenceIntent::REASON_DATA);
        
        std::string source = m_implementation.m_source;
        int attempts = 0;
        
        std::set<std::string> referencedNodes;
        std::string initial_errors = codeAndDataReview(source, referencedNodes, true, true);
        std::string errors = initial_errors;
        bool firstReview = true;
        
        captureContext();
        
        while(attempts < VERIFY_ATTEMPTS_MAX)
        {
            if(errors.empty())
                break;
            
            if(attempts > 0) //Try more powerful LLM
            {
                if(client.escalateLLM())
                {
                    //Reset state
                    proj->restoreDataSnapshot(dataSnapshot);
                    m_implementation.m_source = savedSource;
                    m_prototype = savedDefinition;
                    firstReview = true;
                    
                    popContext();
                    captureContext();
                }
            }
            
            //Reset the source to the initial state
            source = m_implementation.m_source;
            
            std::set<std::string> owners;
            referencedNodes.clear();
            std::string code = getDataApi(true, true, true, referencedNodes);
            code += "\n";
            code += source;
            code += "\n";
            
            const auto& diff = proj->diffWithDataSnapshot();
            std::string modifiableData = proj->dataChangesFromSnapshot(diff);
            
            std::string code_checklist = proj->source_checklist.prompt({{"function", m_brief.func_name}});
            
            std::string data_checklist = proj->define_struct_members.prompt();
            
            std::string call_api = "None";
            if(m_calls.items.size())
            {
                call_api = summarizeCalls(true, false, true);
            }
            
            std::string reviewMsg;
            if(firstReview)
            {
                reviewMsg = proj->verify.prompt({
                    {"function", m_brief.func_name},
                    {"errors", initial_errors},
                    {"api", call_api},
                    {"code", code},
                    {"modifiable", modifiableData},
                    {"source_checklist", code_checklist},
                    {"data_requirements", data_checklist}
                });
            }
            else
            {
                reviewMsg = proj->review_data_and_source.prompt({
                    {"function", m_brief.func_name},
                    {"review", errors}
                });
            }
            
            std::string source = "cpp";
            std::string cache; //no cache - if it doesn't compile we have to fix it!
            bool truncated = false;
            
            inference(cache, reviewMsg, source, &truncated);
            
            errors = codeAndDataReview(source, referencedNodes, true, true);
        
            firstReview = false;
            attempts++;
        }
        
        popContext();
        
        if(!errors.empty())
        {
            std::cout << "Final node verification finished with errors:" << std::endl;
            std::cout << "*********Errors Start*********" << std::endl;
            std::cout << errors;
            std::cout << "*********Errors End*********" << std::endl;
            
            //TODO: Restore data and code
            proj->restoreDataSnapshot(dataSnapshot);
            m_implementation.m_source = savedSource;
            m_prototype = savedDefinition;
            
            if(!wasOnAuto) client.stop();
            return false;
        }
        else
        {
            proj->attachDataToExistingStructs();
            
            //Detect changes in the function signature and update description if necessary
            if(updateDeclaration())
            {
                updateExternals();
            }
        }
        
        if(!wasOnAuto) client.stop();
        return true;
    }

    void CCodeNode::reasonAboutFunction(std::string& cache, const std::string& parentInitialReview)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::string parentReviewErrors;
        CCodeNode* parent = nullptr;
        if(//initialReviewPass &&
           m_this &&
           m_this->m_parent &&
           m_this->m_parent->m_data)
        {
            parent = (CCodeNode*)m_this->m_parent->m_data;
            
            std::string source = parent->m_implementation.m_source;
            std::set<std::string> referencedNodes;
            
            parent->m_stats.reset(); //Ensure the codeAndDataReview starts from clear stats. The same is expected from parentInitialReview
            parentReviewErrors = parent->codeAndDataReview(source, referencedNodes, false, false);
        }
        
        bool parentReviewPass = !parentInitialReview.empty() || parentReviewErrors.empty() || parentReviewErrors == parentInitialReview;
        
        std::string funcDefinitionReview = proj->review_function_self.prompt({
            {"function", m_brief.func_name}});
        
        client.selectLLM(InferenceIntent::REASON_DEFINE);
        
        captureContext();
        
        bool selfReviewPass = true;
#ifdef ENABLE_CODE_SELF_REVIEW
        selfReviewPass = inference(cache, funcDefinitionReview, true);
#endif
        
        bool reviewPass = m_codeReview.str().empty();
        std::string reviseMessage;
        if(!selfReviewPass)
        {
            reviseMessage += "Would you please fix this in the function declaration?";
        }
        if(!reviewPass || !parentReviewPass)
        {
            reviseMessage += " And a few other things that also might require attention: \n";
            if(!reviewPass)
            {
                reviseMessage += m_codeReview.str();
            }
            if(!parentReviewPass)
            {
                reviseMessage += "I've tried to compile the function '" + parent->m_brief.func_name;
                reviseMessage += "' but the clang compiler has reported some errors that might be releveant to the new declaration of '";
                reviseMessage += m_brief.func_name + "':\n";
                reviseMessage += "******* Error report start *******\n";
                reviseMessage += parentReviewErrors;
                reviseMessage += "\n******* Error report end *******\n";
                reviseMessage += "Note that this might not be a paroblem, ";
                reviseMessage += "as we plan to revisit the implementation of '" + parent->m_brief.func_name + "' later. ";
                reviseMessage += "I'm mentioning it in case there's an oversight.\n";
            }
        }
        
        if(!reviseMessage.empty())
        {
            inference(cache, reviseMessage, &m_prototype);
        }
        
        popContext();
        popMessages(1);//Remove initial response. In case of subsequent reviews required information will be provided in the prompt
        
        return reviseMessage;
    }

    void CCodeNode::reviewFunction(std::string& cache, const std::string& parentInfo, const std::string& parentInitialReview)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
       
        reasonAboutFunction(cache, parentInitialReview);
        
        bool wasOnAuto = client.run();
        
        client.selectLLM(InferenceIntent::DEFINE);
        
        bool loop = true;
        int escalateThreshold = 0;
        int attempts = 0;
    
        uint32_t startReviewTag = captureContext();
        
        while(loop && attempts < MAX_REVIEW_ATTPMPTS)
        {
            ensureEndsWith(m_prototype.declaration, ';');
            reflectFunction();
            
            if(m_codeReview.str().empty())
                break;
            
            if(!wasOnAuto) client.stop();
            
            if(escalateThreshold >= ESCALATE_AFTER_FAILED_REVIEWS)
            {
                client.escalateLLM();
                escalateThreshold = 0;
            }
            
            if(attempts > MAX_REVIEW_HISTORY)
            {
                //Erase the first review request and response
                proj->eraseFromContext(startReviewTag, 2);
            }
            
            std::string reviseMessage = proj->review_function.prompt({
                {"function", m_brief.func_name},
                {"declaration", m_prototype.declaration},
                {"description", m_prototype.description},
                {"parent", parentInfo},
                {"review", m_codeReview.str()}});
            
            loop = inference(cache, reviseMessage, &m_prototype);
            
            escalateThreshold++;
            
            attempts++;
        }
        
        popContext();
        
        ensureEndsWith(m_prototype.declaration, ';');
        reflectFunction();
        
        if(!wasOnAuto) client.stop();
    }

    void CCodeNode::pushSummary()
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::stringstream summary;
        summary << "Function: " << std::endl;
        summary << m_prototype.declaration << std::endl;
        summary << "Description: " << std::endl;
        summary << m_prototype.description << std::endl;
        summary << "Definition path: " << getDAGPath("/") << std::endl;
        
        proj->pushMessage(summary.str(), "user", true);
    }

    bool CCodeNode::defineFunction()
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        captureContext();
        
        std::string parent_info;
        
        m_stats.reset();
        
        CCodeNode* parent = nullptr;
        if(m_this && m_this->m_parent && m_this->m_parent->m_data)
        {
            parent = (CCodeNode*)m_this->m_parent->m_data;
            parent_info = "Notice this function will be called in the implementation of the function: ";
            parent_info += parent->m_brief.func_name;
            parent_info += "\n";
            std::set<std::string> referencedNodes;
            parent_info += parent->getContexInfo(false, true, true, referencedNodes);
            parent_info += "\n";
        }
        
        std::string parentInitialReview;
        if(parent)
        {
            std::string source = parent->m_implementation.m_source;
            std::set<std::string> referencedNodes;
            
            parent->m_stats.reset(); //Ensure the codeAndDataReview starts from clear stats to get proper initial review.
                                     //We must do the same later when we compare againts this parentInitialReview !!!
            parentInitialReview = parent->codeAndDataReview(source, referencedNodes, false, false);
        }
        
        client.selectLLM(InferenceIntent::DEFINE);
        
        std::string funcDefinitionMessage = proj->define_function.prompt({
            {"requirements", proj->define_function_signatures.prompt() },
            {"function", m_brief.func_name},
            {"brief", m_brief.brief},
            {"parent", parent_info} });
        
        std::string cache = "prototype.json";
        inference(cache, funcDefinitionMessage, &m_prototype);
        m_prototype.description = m_brief.brief;
        ensureEndsWith(m_prototype.declaration, ';');
        reflectFunction();
        
        //If this hasn't been read from a cache for fully defined node, we need to reveiw it!
        if(!proj->m_cache || cache == "na")
        {
            reviewFunction(cache, parent_info, parentInitialReview);
        }
        
        m_description.brief = m_prototype.brief;
        m_description.description = m_brief.brief;
        
        m_brief.brief = m_prototype.brief;
        
        popContext();
        pushSummary();
        
        //Validation
        if(m_prototype.declaration.size() < 6) return false;
        if(!m_prototype.description.size())
        {
            if(m_description.description.size())
            {
                //In case of loading we might read from the cache invalid description
                //but to have a valid one cached in m_description.description
                m_prototype.description = m_description.description;
            }
            else
            {
                return false;
            }
        }
        if(!m_brief.func_name.size())
        {
            return false;
        }
        
        return true;
    }

    bool CCodeNode::describeFunction()
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        m_stats.reset();
#ifdef ENABLE_VERBOSE_FUNCTION_DESCRIPTION
        captureContext();
        
        std::string parent_info;
        
        if(m_this && m_this->m_parent && m_this->m_parent->m_data)
        {
            CCodeNode* parent = (CCodeNode*)m_this->m_parent->m_data;
            parent_info = "Notice this function will be called in the implementation of the function: ";
            parent_info += parent->m_brief.func_name;
            parent_info += "\n";
            std::set<std::string> referencedNodes;
            parent_info += parent->getContexInfo(false, true, true, referencedNodes);
            parent_info += "\n";
        }
        
        if(!m_inRefactoringHint.empty())
        {
            //In case of refactoring, do it with the smartest model
            Client::getInstance().setLLM(LLMRole::DIRECTOR);
            
            //Next implement() function call will clear the refactorin hint
            parent_info += m_inRefactoringHint;
        }
        
        std::string api = summarizeCalls(true, false, true);
        
        client.selectLLM(InferenceIntent::DEFINE);
        
        std::string funcDefinitionMessage = proj->describe_function.prompt({
            {"abstract", proj->abstract_programming.prompt()},
            {"function", m_brief.func_name},
            //{"brief", m_prototype.description},
            {"parent", parent_info},
            {"api", api}
        });
        
        funcDefinitionMessage += "\n\nConsider detailed 'description' to 3 paragraphs or fewer, less than 2000 characters.\n";
        funcDefinitionMessage += "Consider 'brief' field to 3 sentences or fewer, less than 300 characters.";
        
        std::string cache = "description.json";
        inference(cache, funcDefinitionMessage, &m_description);
        
        //If this hasn't bean red from a cache for fully defined node, we need to reveiw it!
        if(!proj->m_cache || cache == "na")
        {
            std::string funcDefinitionReview = proj->review_function_description_self.prompt({
                {"function", m_brief.func_name}});
            
            client.selectLLM(InferenceIntent::REASON_DEFINE);
            
            captureContext();
            
            bool selfReviewPass = inference(cache, funcDefinitionReview, true);
            
            bool reviewPass = m_codeReview.str().empty();
            std::string reviseMessage;
            if(!selfReviewPass)
            {
                std::string checkLength;
                
                //Don't miss the opportunity to comment the length
                if(m_description.brief.length() > BRIEF_MAX_CHARACTERS)
                {
                    checkLength += " consider 'brief' description to 3 sentences or fewer, less than ";
                    checkLength += std::to_string(BRIEF_MAX_CHARACTERS_NOTE) + " characters.\n";
                }
                
                if(m_description.description.length() > 2*DESCRIPTION_MAX_CHARACTERS)
                {
                    checkLength += " consider detailed 'description' to 3 paragraphs or fewer, less than ";
                    checkLength += std::to_string(DESCRIPTION_MAX_CHARACTERS_NOTE) + " characters.\n";
                }
                
                if(!checkLength.empty())
                {
                    reviseMessage += "\n\nNote: The description is longer than usual, but this is not necessarily a problem. However,";
                    reviseMessage += checkLength;
                    reviseMessage += "\n\n";
                }
                
                reviseMessage += "Would you please fix all this in the function description?";
                inference(cache, reviseMessage, &m_description);
            }
            
            popContext();
        }
        
        popContext();
#else //ENABLE_VERBOSE_FUNCTION_DESCRIPTION
        
        //Already done in defineFunction();
        //m_description.description = m_brief.brief;
        //m_description.brief = m_prototype.brief;
#endif //ENABLE_VERBOSE_FUNCTION_DESCRIPTION
        
        m_prototype.brief = m_description.brief;
        m_brief.brief = m_description.brief;
        
        m_prototype.description = m_description.description;
        
        //Validation
        if(m_prototype.declaration.size() < 6) return false;
        if(!m_prototype.description.size()) return false;
        if(!m_brief.func_name.size()) return false;
        
        return true;
    }
    
    // Pressure ladder:
    // 1. Probe whether app-defined calls are needed.
    // 2. Add library context and do a fast accept for shallow/small graphs.
    // 3. Escalate to self-review and revision only when conflicts/size pressure appear.
    bool CCodeNode::breakdown(bool addLibCalls, bool probeOnly, bool skipSelfReview)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        m_calls.items.clear();
        
        std::string libraryFunctions;
        if(addLibCalls && !m_libCalls.items.empty())
        {
            //Patch the prompt with lib calls
            libraryFunctions = "\n\n";
            
            libraryFunctions += "Here is a list with functions that are already defined in this application ";
            libraryFunctions += "and you can consider to use them in the implementation of '";
            libraryFunctions += m_brief.func_name + "' :\n";
            libraryFunctions += "//*****\n";
            for(auto libFn : m_libCalls.items)
            {
                libraryFunctions += proj->getFunctionInfo(libFn->func_name);
            }
            libraryFunctions += "//*****\n";
            
            libraryFunctions += "List in your response selected from the application-defined functions from above. ";
            libraryFunctions += "Then, if required to implement the full functinality needed for the implementation of '";
            libraryFunctions += m_brief.func_name + "' you can add to the list more functions.\n\n";
            
        }
        
        std::string graph;
        graph += proj->printGraph("", DECOMPOSE_MAX_GRAPH_DEPTH, true);
        graph += proj->listAllFunctions("", DECOMPOSE_MAX_GRAPH_DEPTH, true, true, false, {});
        
        std::string list_functions = "\n";
        list_functions += proj->list_functions.prompt({
            {"plan", proj->whatsThePlan()},
            {"abstract", proj->abstract_programming.prompt()},
            {"caller", m_brief.func_name},
            {"library", libraryFunctions},
            {"graph", graph}
        });
        
        uint32_t depth = getDepth();
        InferenceIntent intent = depth <= DECOMPOSE_MAX_BREAKDOWN_HI_DEPTH ? InferenceIntent::BREAKDOWN_HI : InferenceIntent::BREAKDOWN_LO;
        client.selectLLM(intent);
        
        std::string cache = "calls.json";
        
        client.selectLLM(InferenceIntent::REASON_BREAKDOWN);
        
        uint32_t infoRequestsCount = 1;
        InfoRequest infoRequest;
        
        std::string responseInfo;
        size_t nodesCount = proj->nodeMap().size();
        bool infoRequestsByNodeCount = nodesCount > ENABLE_INFO_REQUESTS_AFTER_NODE_COUNT;
        bool infoRequestsByDepth = depth > ENABLE_INFO_REQUESTS_AFTER_NODE_DEPTH;
        bool enableInfoRequests = infoRequestsByNodeCount || infoRequestsByDepth;
        
        if(!probeOnly && enableInfoRequests)
        {
            //Let's give the model a chance to build contextural information

            std::string maxInfoRequestsStr = std::to_string(DECOMPOSE_MAX_INFO_REQUESTS);
            std::string maxDecomposeNodesStr = std::to_string(DECOMPOSE_MAX_NODES_COUNT_HINT);
            std::string infoRequestNodeCountStr = std::to_string(ENABLE_INFO_REQUESTS_AFTER_NODE_COUNT);
            std::string infoRequestDepthStr = std::to_string(ENABLE_INFO_REQUESTS_AFTER_NODE_DEPTH);

            list_functions += "\n\nCurrently decomposing function " + std::to_string(nodesCount) + "/" + maxDecomposeNodesStr;
            list_functions += " (" + maxDecomposeNodesStr + " is a guideline for maximum scope, not a hard limit or goal we have to reach)";
            list_functions += "\nCurrent call depth: " + std::to_string(depth);
            list_functions += "\nInformation requests are enabled after node count > " + infoRequestNodeCountStr;
            list_functions += " or call depth > " + infoRequestDepthStr + ".";

            if(infoRequestsByNodeCount && infoRequestsByDepth)
            {
                list_functions += "\nReason: both project size and call depth are above the thresholds.";
            }
            else if(infoRequestsByNodeCount)
            {
                list_functions += "\nReason: project size is above the threshold.";
            }
            else if(infoRequestsByDepth)
            {
                list_functions += "\nReason: call depth is above the threshold.";
            }
            
            list_functions += "\nPrefer balancing the architecture as: application -> sub-system -> component -> module -> function ";

            list_functions += "\n\nBefore proceeding to list the functions, let me know if you need additional information ";
            list_functions += "about the architecture and implementation of this application ";
            list_functions += "for better assessing how '" + m_brief.func_name + "' should be decomposed. ";
            list_functions += "You can have up to " + maxInfoRequestsStr;
            list_functions += " information requests. If you don't need more information in order to decide leave all fields empty.\n\n";
            list_functions += "Note, you must not list the called functions yet. ";
            list_functions += "This is only a chance to receive additional information before listing the functions. ";
            list_functions += "Then I will ask you to list the called functions.\n";

            bool noteByNodeCount = nodesCount >= ENABLE_DECOMPOSE_NOTE_AFTER_NODE_COUNT;
            bool noteByDepth = depth >= ENABLE_DECOMPOSE_NOTE_AFTER_NODE_DEPTH;

            if(noteByNodeCount || noteByDepth)
            {
                list_functions += "\n\nScope guidance for decomposition:";

                if(noteByNodeCount)
                {
                    std::string hintReason = nodesCount < DECOMPOSE_MAX_NODES_COUNT_HINT ? "approaching" : "exceeding";
                    list_functions += "\n- We are " + hintReason + " the recommended maximum for the number of functions.";
                }

                if(noteByDepth)
                {
                    list_functions += "\n- Current call depth is " + std::to_string(depth) +
                                    ", which is at or above the recommended depth of " +
                                    std::to_string(ENABLE_DECOMPOSE_NOTE_AFTER_NODE_DEPTH) + ".";
                }

                list_functions += "\nConsider using information requests to reuse existing architecture and functionality and avoid introducing extra wrapper/helper layers that increase scope.\n\n";
            }
            
            responseInfo = proj->provideInfoLoop(list_functions, DECOMPOSE_MAX_INFO_REQUESTS);
        }
        else
        {
            responseInfo = list_functions;
        }
        
        responseInfo += "Now please go ahead and list all functions under '" + m_brief.func_name + "' as previously discussed!";
        
        inference(cache, responseInfo, &m_calls);
        
        bool listDataOnConflict = true;
        std::string conflictMessage = callsConflictsWithData(m_calls, listDataOnConflict);
        if(!conflictMessage.empty()) {
            listDataOnConflict = false;
        }
        
        std::string excludeCalls;
        if(!m_excludeCalls.empty())
        {
            for(auto call : m_excludeCalls)
            {
                if(!excludeCalls.empty()) {
                    excludeCalls += ", ";
                }
                else {
                    excludeCalls += "The following functions must not be in the list: ";
                }
                excludeCalls += call;
            }
            
            excludeCalls += "\n";
            
            std::string excludeListedCalls;
            for(auto call : m_calls.items)
            {
                auto exIt = m_excludeCalls.find(call->func_name);
                if(exIt != m_excludeCalls.end())
                {
                    if(!excludeListedCalls.empty()) {
                        excludeListedCalls += ", ";
                    }
                    else {
                        excludeListedCalls += "The following calls seem are already listed, would you confirm and if so excluded them from the list: ";
                    }
                    
                    excludeListedCalls += call->func_name;
                }
            }
            
            if(!excludeListedCalls.empty())
            {
                excludeCalls += excludeListedCalls;
                excludeCalls += "\n";
            }
        }
        
        std::string tooManyFunctions;
        if(m_calls.items.size() > 8)
        {
            tooManyFunctions += "There are ";
            tooManyFunctions += std::to_string(m_calls.items.size());
            tooManyFunctions += " functions listed. These a more than usual. The expection is to have 3-6 functions listed. ";
            tooManyFunctions += " Consider to group the requred functionality for '" + getName();
            tooManyFunctions += "' in 3 to 6 functoins and then we will further decompose those functions.\n";
        }
        
        if(probeOnly)
        {
            if(m_calls.motivation.empty())
            {
                std::cout << "Empty motivation for node!" << std::endl;
            }
            
            return !m_calls.items.empty();
        }
        
        bool wasOnAuto = client.run();
        
        std::string reviewListFunctions = proj->review_list_functions.prompt({
            {"caller", m_brief.func_name},
            {"exclude", excludeCalls}
        });
        
        bool selfReviewPass = true;
        if(!skipSelfReview)
        {
            selfReviewPass = inference(cache, reviewListFunctions, true);
        }
        
        if(!wasOnAuto) client.stop();
        
        if(!selfReviewPass || !tooManyFunctions.empty() || !conflictMessage.empty())
        {
            client.selectLLM(InferenceIntent::REASON_BREAKDOWN);
            
            std::set<std::string> callsBefore;
            for(auto call : m_calls.items) {
                callsBefore.insert(call->func_name);
            }
            
            std::string reviewMessage = tooManyFunctions;
            
            if(!conflictMessage.empty()) {
                reviewMessage += conflictMessage;
            }
            
            reviewMessage += "Consider fixing this and revise your answer without asking further questions!";
            
            inference(cache, reviewMessage, &m_calls);
            
            conflictMessage = callsConflictsWithData(m_calls, listDataOnConflict);
            if(!conflictMessage.empty()) {
                listDataOnConflict = false;
            }
            
            std::set<std::string> callsAfter;
            for(auto call : m_calls.items) {
                callsAfter.insert(call->func_name);
            }
            
            auto diff = getSetDifferences(callsBefore, callsAfter);
            
            if(tooManyFunctions.empty() && //It is expected in case of 'too many functions' the list will be revised - so don't complain
               (!diff.first.empty() || !diff.second.empty()))
            {
                std::stringstream message;
                if(!diff.second.empty())
                {
                    message << "The following function calls are no longer in the list:" << std::endl;
                    std::string callsList;
                    for(auto call : diff.second) {
                        if(!callsList.empty()) callsList += ", ";
                        callsList += call;
                    }
                    message << callsList << std::endl;
                }
                
                if(!diff.first.empty())
                {
                    message << "The following new function calls have been added to the list:" << std::endl;
                    std::string callsList;
                    for(auto call : diff.first) {
                        if(!callsList.empty()) callsList += ", ";
                        callsList += call;
                    }
                    message << callsList << std::endl;
                }
                
                message << "If this was intended you can return the same list. If this was not intended you can revise your answer. Whatever you decide, ensure this function can be fully implemnted without TODOs and placeholders!" << std::endl;
                
                if(m_calls.items.size() > 8)
                {
                    tooManyFunctions.clear();
                    
                    tooManyFunctions += "\n\nCurrently there are ";
                    tooManyFunctions += std::to_string(m_calls.items.size());
                    tooManyFunctions += " functions listed. These a more than usual. The expection is to have 3-6 functions listed. ";
                    tooManyFunctions += " Consider to group the requred functionality for '" + getName();
                    tooManyFunctions += "' in 3 to 6 functoins and then we will further decompose those functions.\n";
                    message << tooManyFunctions;
                }
                
                if(!conflictMessage.empty()) {
                    message << conflictMessage;
                }
                
                inference(cache, message.str(), &m_calls);
            }
        }
        
        if(m_calls.motivation.empty())
        {
            std::cout << "Empty motivation for node!" << std::endl;
        }
        
        //Ensure excluded calls aren't listed
        m_calls.items.erase(
            std::remove_if(m_calls.items.begin(), m_calls.items.end(),
                [this](const std::shared_ptr<FunctionItem>& item) {
                    return m_excludeCalls.find(item->func_name) != m_excludeCalls.end();
                }), m_calls.items.end());
        
        //bool readFromCache = cache != "na";
        //We don't want to search the library when reading from cache!
        //return !m_calls.items.empty() && !readFromCache;
        return !m_calls.items.empty();
    }

    bool CCodeNode::resolveName(const CCodeNode* existingFuncNode, FunctionItem& brief)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        bool isShared = false;
        
        captureContext();
        
        while(existingFuncNode && compareCaseInsensitive(brief.func_name, existingFuncNode->m_brief.func_name))
        {
            //We have a conflict with the name of an existing function
            //Let's ask the LLM to compare both functions and decide if they are similar and can be reused
            //Or to adjust the name and description of the current function
            //auto existingFuncNode = (const CCodeNode*)it->second;
            std::string existingCallstack = existingFuncNode->getDAGPath(">");
            std::string currentCallstack = getDAGPath(">") + ">" + brief.func_name;
            
            std::string existing_implementation;
            if(existingFuncNode->m_implementation.m_source.size() >= 7)
            {
                existing_implementation = "Implementation for the existing function: \n";
                existing_implementation += existingFuncNode->m_implementation.m_source;
                existing_implementation += "\nNotice, modification on the existing function to satisfy the requirements of the function being defined is possible but the existing functionality must be preserved\n";
            }
            
            std::set<std::string> emptyEx;
            std::string existingFunctions = proj->listAllFunctions({}, -1, false, false, false, emptyEx);
            
            std::string compareFunctions = proj->compare_functions.prompt({
                {"function", brief.func_name},
                {"current_callstack", currentCallstack},
                {"current_brief", brief.brief},
                {"existing_callstack", existingCallstack},
                {"existing_description", existingFuncNode->m_brief.brief},
                {"existing_implementation", existing_implementation},
                {"existing_functions", existingFunctions}
            });
            
            client.selectLLM(InferenceIntent::RESOLVE);
            
            std::string cache = "calls.json:items[func_name=" + brief.func_name;
            cache += "]";
            inference(cache, compareFunctions, &brief);
            if(!compareCaseInsensitive(brief.func_name, existingFuncNode->m_brief.func_name))
            {
                isShared = true;
                
                bool wasOnAuto = client.run();
                std::string reviewCompareFunctions = proj->review_compare_functions.prompt({
                    {"old_name", existingFuncNode->m_brief.func_name},
                    {"new_name", brief.func_name},
                });
                
                client.selectLLM(InferenceIntent::REASON_RESOLVE);

                bool selfReviewPass = inference(cache, reviewCompareFunctions, true);

                if(!wasOnAuto) client.stop();
                
                if(!selfReviewPass)
                {
                    client.selectLLM(InferenceIntent::RESOLVE);
                    inference(cache, "Do your best to fix all this and revise the answer without asking further questions!", &brief);
                }
                
                auto it = proj->nodeMap().find(brief.func_name);
                if(it != proj->nodeMap().end())
                {
                    //After all, it turns out the LLM returns the original name,
                    //so I guess wants to keep the original function
                    if(brief.func_name == existingFuncNode->m_brief.func_name)
                    {
                        existingFuncNode = nullptr;
                    }
                    else
                    {
                        //WE MUST NOT BE HERE!!!
                        existingFuncNode = (const CCodeNode*)it->second;
                    }
                }
            }
            else
            {
                //break the look
                existingFuncNode = nullptr;
            }
        }
        
        popContext();
        
        return isShared;
    }

    bool CCodeNode::tryToImplement()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        bool implemented = true;
        
        std::set<std::string> dataTypes = proj->getAppTypesForFunction(m_prototype.m_signature);
        //All application types must be already defined
        for(auto type : dataTypes)
        {
            std::string owningPath;
            if(!proj->findData(type, owningPath))
            {
                implemented = false;
                break;
            }
        }
        
        if(implemented)
        {
            m_enableDiagnostics = true;
            m_reviewDiagnostics = true;
            bool cached = implement(true);
            m_enableDiagnostics = false;
            m_reviewDiagnostics = false;
            if(m_diagnostics.empty() && m_codeReview.str().empty())
            {
                std::cout << "Speculative implementation succeeded!" << std::endl;
            }
            else
            {
                implemented = false;
                std::cout << "Speculative implementation failed!" << std::endl;
                std::cout << "Diagnostics start **********************************" << std::endl;
                std::cout << m_diagnostics;
                std::cout << "Code review: " << std::endl << m_codeReview.str();
                std::cout << "Diagnostics end **********************************" << std::endl;
            }
            m_diagnostics.clear();
            m_codeReview.str("");
            m_codeReview.clear();
            
            implemented = implemented && !cached;
        }
        
        return implemented;
    }

    void  CCodeNode::decompose()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        //Do not traverse further referenced functions
        if(m_defined)
        {
            //Let's ensure all child nodes for the calls are referenced
            for(auto call : m_calls.items) {
                proj->shareNode<CCodeNode>(call->func_name, this);
            }
            
            return;
        }
        
        bool used = true;
        //TODO: This needs a little bit more testing to enable it!
        /*used = hasPathToMain();
        if(!used)
        {
            std::cout << "Node: '" << getName() << "' is not called by the program entry point" << std::endl;
            hasPathToMain();//To investigate;
            return;
        }*/
        
        Client::getInstance().agentToServer("\n\nTHINKING...\n\n");
        
        m_excludeCalls.clear();
        if(proj->m_refactoringDepth > 0)
        {
            //getRefactorExcludeCalls(m_excludeCalls);
        }
        
        Client::getInstance().setContextLogDir(proj->getProjDir() + "/logs/nodes/" + m_brief.func_name);
        
        std::string decomposeMessage = "Currently decompose function '" + m_brief.func_name + "' with call stack: ";
        decomposeMessage += getDAGPath(">") + "\n\n";
        
        CCodeNode* parent = getParent();
        if(parent)
        {
            decomposeMessage += parent->printCallsInfo();
            
            CCodeNode* grandParent = parent->getParent();
            if(grandParent)
            {
                decomposeMessage += grandParent->printCallsInfo();
            }
        }
        
        pushMessage(decomposeMessage, "user");
        
        captureContext();
        
        setName(m_brief.func_name);
        if(!defineFunction())
        {
            //Something went terribly wrong!!!
            std::cout << "IVALID NODE AT: " << getDAGPath("/") << std::endl;
            return;
        }
        
        //For now we don't rely on the LLM to decide if the function is simple enough
        //Obviously, if the LLM couldn't decompose the current function further that means it is already 'simple'
        //However we also have hard limit DECOMPOSE_MAX_DEPTH since some LLMs will never stop offering decomposition
        
        uint32_t depth = getDepth();
        bool noFurtherDecompose = depth >= DECOMPOSE_MAX_DEPTH;
        
        noFurtherDecompose = noFurtherDecompose || proj->m_refactoringDepth > 0 || !used;
        
        //Try speculative implementation
        bool implemented = false;
        if(!noFurtherDecompose && depth >= DECOMPOSE_TRY_COMPILE_DEPTH)
        {
            implemented = tryToImplement();
        }
        
        uint32_t functionsCount = (uint32_t)proj->nodeMap().size();
        if(!implemented && !noFurtherDecompose)
        {
            captureContext();
        
            bool hasCachedOrder = loadOrder();
            
            if(noFurtherDecompose || //In case when noFurtherDecompose is true we still want this function to be able to call functions from the library
               functionsCount < PREDICTIVE_BREAKDOWN_AFTER_NODE_COUNT || //Avoid next breakdown condition. Inferencing braekdwon is more expensive than lib search
               breakdown(false, true, true)) //From the first breakdown attempt it seems this function calls app-defined functions
            {
                //We don't want history of the previous breakdown call
                popContext();
                captureContext();
                
                if(proj->nodeMap().size() > SEARCH_LIB_AFTER)//Let's accumulate functions first
                {
                    searchLibrary(m_excludeCalls);
                }
                
                //The first breakdown above is without reasoning, so let's do a new one.
                //Commenting out the check for listed lib calls to do proper breakdown
                //if(!m_libCalls.items.empty())
                {
                    //This time including found library functions for consideration.
                    if(!noFurtherDecompose)
                    {
                        //Under certain node depth and count we shouldn't worry too much
                        //and rely on the project plan to hint the breakdown
                        
                        bool skipSelfReview =
                        depth < BREAKDOWN_SKIP_SELF_REVIEW_BEFORE_NODE_DEPTH &&
                        functionsCount < BREAKDOWN_SKIP_SELF_REVIEW_BEFORE_NODE_COUNT;
                        
                        breakdown(true, false, skipSelfReview);
                    }
                }
            }
            
            if(hasCachedOrder)
            {
                m_calls.applyOrder();
            }
            else
            {
                m_calls.updateOrder();
            }
            
            for (auto& func : m_calls.items)
            {
                FunctionItem brief = *func;
                if(!brief.func_name.size())
                {
                    //This is not good!!
                    std::cout << "INVALID CALL FOR NODE: " << getDAGPath("/") << std::endl;
                    //Probably need to 'return' and to let repair this branch on the next run?!?!
                    continue;
                }
                
                if(brief.func_name == m_brief.func_name)
                {
                    //Should we resolve this conflict?
                    continue;
                }
                
                auto it = proj->nodeMap().find(brief.func_name);
                
                bool isShared = false;
                //No need to resolve conflicts for functions from library.
                //They were already selected to be called in the implementation of the current function
                if(!isFromLibrary(brief.func_name) && it != proj->nodeMap().end())
                {
                    //We have a conflict with the name of an existing function
                    //Let's ask the LLM to compare both functions and decide if they are similar and can be reused
                    //Or to adjust the name and description of the current function
                    auto existingFuncNode = (const CCodeNode*)it->second;
                    
                    std::string existingPath = existingFuncNode->getDAGPath("/");
                    std::string callPath = getDAGPath("/") + "/" + brief.func_name;
                    
                    //Loading the node or reading the list functions request from cache
                    //and then creating the nodes might signal false positive need to resolve conflicts
                    //Checkin the path will help to avoid that
                    if(existingPath != callPath)
                    {
                        isShared = resolveName(existingFuncNode, brief);
                        
                        if(brief.func_name.empty() || brief.brief.empty())
                        {
                            //This is not good!!
                            std::cout << "INVALID CALL RESOLVE FOR NODE: " << getDAGPath("/") << std::endl;
                            continue;
                        }
                    }
                }
                
                *func = brief;
            }
            
            popContext();
        }
        
        describeFunction();
        popContext();
        
        pushSummary();
        
        if(!implemented)
        {
            implement(false);
        }
        
        updateCallsUsage(true, true);
        
        bool reviseData = proj->m_refactoringDepth < 1;
        
        defineData(reviseData);
        verify();
        
        if(m_calls.items.size()==0)
        {
            std::cout << "No further decomposition required for function: " << m_brief.func_name << std::endl;
            std::cout << "Path: " << getDAGPath(">") << std::endl;
        }
        
        m_defined = isDefined();
        
        if(m_defined && proj->m_save)
        {
            save();
        }
        
        if(m_defined)
        {
            std::string commitMessage = "\nDefine function: " + m_brief.func_name + "\n\n";
            commitMessage += m_prototype.description;
            commitMessage += "\n";
            proj->commit(commitMessage);
            
            //std::cout << "***************REPORT START***************" << std::endl;
            std::string report = generateReport();
            Client::getInstance().agentToServer(report);
            //std::cout << report;
            //std::cout << "***************REPORT END***************" << std::endl;
        }
    }

    void CCodeNode::getDataPaths(std::set<std::string>& dataPaths) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::set<std::string> dataTypes = proj->getAppTypesForFunction(m_prototype.m_signature);
        for(auto obj: dataTypes)
        {
            std::string path;
            proj->findData(obj, path);
            dataPaths.insert(path);
        }
    }

    std::string CCodeNode::getDataTypes(bool getDetached, std::set<std::string>& referencedNodes) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::stringstream sout;
        
        std::set<std::string> allDataDefs;
        getDataPaths(allDataDefs);
        
        for(const auto& def : allDataDefs) {
            if(referencedNodes.find(def) == referencedNodes.end())
            {
                sout << proj->declareData(getDetached, def);
            }
        }
        
        for(const auto& def : allDataDefs) {
            if(referencedNodes.find(def) == referencedNodes.end())
            {
                sout << proj->defineData(getDetached, def);
            }
        }
        
        referencedNodes.insert(allDataDefs.begin(), allDataDefs.end());
        
        return sout.str();
    }

    std::string CCodeNode::getDataTypesInScope(std::set<std::string>& referencedNodes) const
    {
        std::string dataTypes;

        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        for(const auto& call : m_calls.items)
        {
            auto it = proj->nodeMap().find(call->func_name);
            if(it != proj->nodeMap().end())
            {
                auto childCCNode = (CCodeNode*)it->second;
                if(childCCNode->m_defined)
                {
                    dataTypes += childCCNode->getDataTypes(false, referencedNodes);
                }
            }
        }
        
        //This is in test - getDataTypes aready uses getVisibleTypes for both declarations and definitions
        //In this case, dataInScope would only mean to add the detached data types to the context
        //This is necessary I've seen a situation (gpt4o_haiku35_gpt4o-mini) where defineFunction will introduce
        //new ASTNode struct instead of using existing AstNode with visible declaration
        std::string detachedData = proj->getDetachedData();
        if(!detachedData.empty() &&
           referencedNodes.find("__DETACHED__") == referencedNodes.end())
        {
            referencedNodes.insert("__DETACHED__");
            dataTypes += detachedData;
        }
        
        
        return dataTypes;
    }

    std::string CCodeNode::getDataDeclarationsInScope(std::set<std::string>& referencedNodes) const
    {
        std::string dataDecals;
        for(auto* child : m_this->m_children)
        {
            //This is not cool for sure!
            if(!child->m_data) continue;
            
            auto childCCNode = (CCodeNode*)child->m_data;
            
            if(childCCNode->m_defined)
            {
                dataDecals += childCCNode->getDataDeclarations(referencedNodes);
            }
        }
        
        return dataDecals;
    }

    std::string CCodeNode::getDataDeclarations(std::set<std::string>& referencedNodes) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::stringstream sout;
        
        std::set<std::string> allDataDefs;
        getDataPaths(allDataDefs);
        
        for(const auto& def : allDataDefs) {
            if(referencedNodes.find(def) == referencedNodes.end())
            {
                sout << proj->declareData(false, def);
            }
        }
        
        referencedNodes.insert(allDataDefs.begin(), allDataDefs.end());
        
        return sout.str();
    }

    std::string CCodeNode::getDataSafeDefinitions() const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::stringstream sout;
        
        std::set<std::string> allDataDefs;
        getDataPaths(allDataDefs);
        
        for(const auto& def : allDataDefs) {
            sout << proj->defineDataSafe(def);
        }
        
        return sout.str();
    }

    std::string CCodeNode::getContexInfo(bool description, 
                                         bool implementation,
                                         bool dataInScope,
                                         std::set<std::string>& referencedNodes) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::stringstream sout;
        
        if(description)
        {
            sout << "Description of function: ";
            sout << m_prototype.declaration << std::endl << std::endl;
            sout << m_prototype.description << std::endl << std::endl;
            sout << "Data and function definition:" << std::endl;
        }
    
        sout << getDataTypes(false, referencedNodes);
        if(dataInScope)
        {
            sout << "//Data in scope start" << std::endl;
            sout << getDataTypesInScope(referencedNodes);
            sout << "//Data in scope end" << std::endl;
        }
        
        if(implementation) {
            sout << m_implementation.m_source << std::endl;
        }
        else {
            sout << m_prototype.declaration << std::endl;
        }
        
        return sout.str();
    }

    std::string CCodeNode::compileCommand(const std::string& platform, uint32_t options) const
    {
        //TODO: Rewrite this function to use getCompilationInfo and buildCompileCommand
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string binDir = buildDir + "/" + platform;
        
        std::string include = "-I";
        include += buildDir;
        include += " -I";
        include += buildDir + "/" + buildSourcePath;
        include += " ";
        
        std::string cmdLine = "clang++ -v -std=c++17 -arch arm64 -Werror=format -fno-diagnostics-show-note-include-stack -c ";
        
        if(options & BUILD_DEBUG) {
            cmdLine += "-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O0 -fno-inline-functions -fno-optimize-sibling-calls ";
        }
        
        //This helps the instrumented build to properly resolve includes to be able to compile the data_printers
        //And in the same time should be fine for the default build
        std::string pchFile = buildDir + "/common.pch";
        if(boost_fs::exists(pchFile))
        {
            cmdLine += "-include-pch " + pchFile + " ";
        }
        else
        {
            cmdLine += "-include common.h ";
        }
        
        cmdLine += include;
        
        if(options & BUILD_PRINT_TEST) {
            cmdLine += "-DCOMPILE_TEST ";
        }
        
        std::string objFile;
        if (options & BUILD_UNIT_TEST) {
            objFile = nodeDir + "/test/main";
        }
        else {
            objFile = binDir + "/";
            objFile += m_brief.func_name;
        }
        
        objFile += ".o";
        
        cmdLine += " -o ";
        cmdLine += objFile;
        
        std::string srcFile = nodeDir + "/";
        if(options & BUILD_UNIT_TEST) {
            srcFile += "test/";
        }
        
        if (options & BUILD_UNIT_TEST) {
            srcFile += "main.cpp";
        }
        else {
            srcFile += m_brief.func_name + ".cpp";
        }
        
        cmdLine += " ";
        cmdLine += srcFile;
        
        return cmdLine;
    }

    std::shared_ptr<CompileInfo> CCodeNode::getCompilationInfo(const std::string& platform, uint32_t options) const
    {
        std::shared_ptr<CompileInfo> compileInfo = std::make_shared<CompileInfo>();
        
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string binDir = buildDir + "/" + platform;
        
        //std::string include = "-I";
        //include += buildDir;
        compileInfo->m_includeDirs.push_back(buildDir);
        
        //include += " -I";
        //include += buildDir + "/" + dagPath;
        compileInfo->m_includeDirs.push_back(buildDir + "/" + buildSourcePath);
        
        //include += " ";
        
        std::string clangOptions = "-std=c++17 -arch arm64 -Werror=format -fno-diagnostics-show-note-include-stack -c";
        
        if(options & BUILD_DEBUG) {
            clangOptions += " -g -O0 -fno-inline-functions -fkeep-inline-functions";
            
            //This helps the instrumented build to properly resolve includes to be able to compile the data_printers
            //And in the same time should be fine for the default build
            clangOptions += " -include common.h ";
        }
        
        if(options & BUILD_PRINT_TEST) {
            clangOptions += " -DCOMPILE_TEST";
        }
        
        compileInfo->m_options = clangOptions;
        
        std::string objFile;
        if (options & BUILD_UNIT_TEST) {
            objFile = nodeDir + "/test/";
        }
        else {
            objFile = binDir + "/";
        }
        
        objFile += m_brief.func_name;
        objFile += ".o";
        
        //cmdLine += " -o ";
        //cmdLine += objFile;
        compileInfo->m_objFilePath = objFile;
        
        std::string srcFile = nodeDir + "/";
        if(options & BUILD_UNIT_TEST) {
            srcFile += "test/";
        }
        
        srcFile += m_brief.func_name + ".cpp";
        compileInfo->m_sourceFilePath = srcFile;
        
        return compileInfo;
    }

    std::string CCodeNode::linkCommand(const std::string& platform, uint32_t options) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string binDir = buildDir + "/" + platform;
        
        bool buildTest = options & BUILD_UNIT_TEST;
        std::string executable = nodeDir + "/";
        //if(buildTest) {
            executable += "test/";
        //}
        
        //executable += m_brief.func_name;
        executable += "main";
        
        std::string command = "clang++ -v -std=c++17 -arch arm64 -o " + executable;
        
        //std::string testObjFile = nodeDir + "/test/" + m_brief.func_name + ".o";
        std::string testObjFile = nodeDir + "/test/main.o";
        if(buildTest)
        {
            command += " " + testObjFile;
        }
        
        if(options & BUILD_DEBUG)
        {
            command += " -fsanitize=address,undefined";
        }
        
        for (const auto& file : boost_fs::directory_iterator(binDir))
        {
            if (file.path().extension() == ".o") {
                
                std::string objFilePath = file.path().string();
                if(!buildTest || !hasMainFunction(objFilePath))
                {
                    command += " " + objFilePath;
                }
            }
        }
        
        return command;
    }

    std::string CCodeNode::exec(const std::string& cmd, const std::string& workingDir, const std::string& operation) const
    {
        return hen::exec(cmd, workingDir, operation, false);
    }

    std::string CCodeNode::analyzeTemplatedCalls(const std::set<std::string>& unmachedFunctions) const
    {
        std::string complexCode;
        for(auto snippet : m_stats.m_complexCode)
        {
            std::string foundFunctions;
            for(auto func : unmachedFunctions)
            {
                if(snippet.find(func) != std::string::npos)
                {
                    if(!foundFunctions.empty())
                    {
                        foundFunctions += ", ";
                    }
                    foundFunctions += func;
                }
            }
            
            if(!foundFunctions.empty())
            {
                complexCode += "\nThe following fragment from the code:\n";
                complexCode += snippet;
                complexCode += "\nSeems to call functions: ";
                complexCode += foundFunctions + "\n\n";
            }
        }
        
        if(!complexCode.empty())
        {
            complexCode += "\nNote, the above code fragments call two or more templated functions and might be very hard to understand the right data types that must be used to call the functions. Consider simplifying the code fragments in a way that each call to templated function will be a single line and there is no 'auto' keywords to represent data types.\n";
        }
        
        return complexCode;
    }

    void CCodeNode::pushCompileProgress()
    {
        //TODO: Only push when we use a model or model with 128K context
        
        if(m_stats.m_compileProgress.empty()) {
            return;
        }
        
        //This is summarized artificial chain of thoughts for the compilation progress so far
        
        uint32_t startFrom = 0;
        if(m_stats.m_compileProgress.size() > MAX_COMPILE_ATTEMPTS_HISTORY)
        {
            std::string msg = "Note: previous compilation review steps have been collapsed! Following with the last ";
            msg += std::to_string(MAX_COMPILE_ATTEMPTS_HISTORY) + "\n\n";
            
            startFrom = uint32_t(m_stats.m_compileProgress.size()) - MAX_COMPILE_ATTEMPTS_HISTORY;
            msg += m_stats.m_compileProgress[startFrom].m_initialImplementation;
            
            pushMessage(msg, "user");
        }
        
        for(uint32_t s = startFrom; s < m_stats.m_compileProgress.size(); ++s)
        {
            const auto& step = m_stats.m_compileProgress[s];
            std::stringstream output;
            
            if(step.m_compilationOutput.empty() || step.m_proposedSolution.empty())
                continue;
            
            output << "I've tried to compile the function:" << std::endl;
            output << m_prototype.declaration << std::endl << std::endl;
            output << "Here is the output from the Clang compiler:" << std::endl << std::endl;
            output << step.m_compilationOutput << std::endl << std::endl;
            output << "Review the output for errors and try to suggest a solution how to fix it by modifying the '";
            output << m_brief.func_name << "' source." << std::endl << std::endl;
            pushMessage(output.str(), "user");
            
            pushMessage(step.m_proposedSolution, "assistant");
            
            std::stringstream revise;
            revise << "Please revise the implementation of the function '" << m_brief.func_name;
            revise << "' by taking into account solutions we've previously discussed and tried! " << std::endl;
            pushMessage(revise.str(), "user");
            
            pushMessage(step.m_revisedImplementation, "assistant");
        }
    }

    std::string CCodeNode::compileProgressMessage()
    {
        std::string message;
        
        if(!m_stats.m_compileProgress.size())
        {
            return message;
        }
        
        {
            const auto& step = m_stats.m_compileProgress.back();
            
            if(!step.m_compilationOutput.empty())
            {
                message += "\nClang compiler output:\n\n";
                message += step.m_compilationOutput + "\n\n";
            }
            else
            {
                message += "\nMissing clang compiler output!\n\n";
            }
            
            if(!step.m_proposedSolution.empty())
            {
                
                message += "\nAnalysis and proposed solution:\n\n";
                message += step.m_proposedSolution + "\n\n";
            }
            else
            {
                message += "\nMissing analysis and ideas for solution!\n\n";
            }
        }
        
        return message;
    }

    std::string CCodeNode::analyzeCompilation(const std::string& output, std::set<std::string>& owners,
                                              std::set<std::string>& structs,
                                              std::set<std::string>& enums)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::set<std::string> funcNode;
        //Ensure all mentioned app defined data types and functions are in the context
        std::set<std::string> appIdentifiers = proj->analyzeForAppIdentifiers(output);
        owners = proj->filterIdentifiers(appIdentifiers, funcNode, structs, enums);
        //Make sure all owner paths for error-reported functions and data types will be included in the .cpp file
        for(auto dep : owners) {
            addToSet(m_implementation.dependencies, dep);
        }
        
        std::string identifiersCsv = getAsCsv(appIdentifiers);
        std::set<std::string> unmachedFunctions = analyzeForUnmatchedFunctions(output);
        std::set<std::string> empty1, empty2, empty3;
        proj->filterIdentifiers(unmachedFunctions, empty1, empty2, empty3);
        std::set<std::string> dataDefWithFunc = analyzeForProblematicTypes(output);
        proj->filterIdentifiers(dataDefWithFunc, empty1, empty2, empty3);
        
        std::string unmachedFunctionsCsv = getAsCsv(unmachedFunctions);
        std::string dataDefWithFuncCsv = getAsCsv(dataDefWithFunc);
        
        std::string analysis;
        if(!identifiersCsv.empty())
        {
            analysis += "Here are a few hints from my analysis:\n\n";
            analysis += "The following application defined data types and functions are mentioned in the errors:\n";
            analysis += identifiersCsv;
            analysis += "\nI've provided information for all of them. ";
            analysis += "Please have a look and use this information when preparing plan for how to fix the errors. If you can see data types and functions from the provided information above, you can assume next compilation attempt they will be included/visible in the scope of the compiled code\n";
            if(!unmachedFunctionsCsv.empty())
            {
                analysis += "The following functions are called with missmatched arguments:\n";
                analysis += unmachedFunctionsCsv;
                analysis += "\nPlese consider provided declarations and function descriptions when analyzing the relevant errors\n";
                
                if(m_reviewLevel > CCodeNode::ReviewLevel_1)
                {
                    analysis += analyzeTemplatedCalls(unmachedFunctions);
                }
            }
            if(!dataDefWithFuncCsv.empty())
            {
                analysis += "The following data types have problems with consturctors, destructors, member functions or function pointers:\n";
                analysis += dataDefWithFuncCsv;
                analysis += "\nAs a remainder, all data types defined in the aplication don't have constructors, destructors, member functions and function pointers. All data containers and operations with those types must be aligned with this principle\n";
            }
        }
        
        std::set<std::string> referencedNodes;
        referencedNodes.clear();
        std::string code = getDataApi(true, true, true, referencedNodes);
        code += "\n";
        code += m_implementation.m_source;
        code += "\n";
        
        const auto& errorAnalyzers = proj->getFocusedAnalyzers();
        for(auto analyzer : errorAnalyzers)
        {
            analysis += analyzer->analyze(code, output);
        }
        
        return analysis;
    }

    std::string CCodeNode::reviewAppFunctionsUse(LibFunctionList& libFunctions, const std::string& context,
                                           const std::string& output, const std::string& api,
                                           const std::string& analysis, const std::string& functionsList)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        captureContext();
     
        std::string currentAnalysis = analysis;
        
        std::set<std::string> emptyEx;
        std::string existingFunctions = proj->listAllFunctions({}, -1, true, false, false, emptyEx);
        
        std::string favoriteFunctions;
        for(auto func : libFunctions.items)
        {
            favoriteFunctions += proj->getFunctionInfo(func->func_name);
            favoriteFunctions += "How to use '" + func->func_name + "' in the implementation: ";
            favoriteFunctions += func->motivation + "\n\n";
        }
        
        std::string reviewCompilationMessge = proj->review_compilation_lib.prompt({
            {"function", m_brief.func_name},
            {"declaration", m_prototype.declaration},
            {"function_ctx", context},
            {"output", output},
            {"api", api},
            {"favorites", favoriteFunctions},
            {"existing_functions", functionsList},
            {"analysis", currentAnalysis},
        });
        
        std::string cache = "";
        libFunctions.items.clear();
        inference(cache, reviewCompilationMessge, &libFunctions);
        popContext();
        
        return reviewCompilationMessge;
    }

    void CCodeNode::inferenceReviewLoop(CodeReview& reviewData, const std::string& context, const std::string& compileCL,
                                    const std::string& output, const std::string& api,
                                    const std::string& analysis)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        LibFunctionList libFunctions;
        
        std::string functionList;
        for(auto node : proj->nodeMap())
        {
            auto ccNode = (const CCodeNode*)node.second;
            
            if(ccNode == this)
            {
                continue;
            }
            
            auto exIt = m_excludeCalls.find(ccNode->getName());
            if(exIt != m_excludeCalls.end())
            {
                continue;
            }
            
            std::stack<const CCodeNode*> dependencyPath;
            if(proj->isADependency(dependencyPath, ccNode->getName(), getName()))
            {
                continue;
            }
            
            //We can't aford a function to be interpreted differently
            //based on the needs of the funtcion being implemented
            //So we can provide only already fully defined functions
            if(!ccNode->m_defined)
                continue;
            
            functionList += ccNode->m_prototype.brief;
            functionList += "\n";
            functionList += ccNode->m_prototype.declaration;
            functionList += "\n\n";
            
            std::string currentAnalysis = analysis;
            //TODO: Add prev review to analysis
            if(functionList.size() > MAX_CHARACTERS_IN_COMP_REVIEW)
            {
                reviewAppFunctionsUse(libFunctions, context, output, api, currentAnalysis, functionList);
                functionList.clear();
            }
        }
        
        std::string currentAnalysis = analysis;
        //TODO: Add prev review to analysis
        if(!functionList.empty())
        {
            reviewAppFunctionsUse(libFunctions, context, output, api, currentAnalysis, functionList);
        }
        
        //TODO: inference the final error analysis and review
        {
            pushCompileProgress();
            
            //captureContext();
            std::string checklist = proj->source_checklist.prompt({{"function", m_prototype.declaration}});
            
            //Let's reasoning with the LLM about the best solution.
            //This will help warm up the context with useful information
            //and will support the LLM attempt to fix the problem on the subsequent inference
            std::string strategyOptions;
            if(proj->m_refactoringDepth == 1) {
                strategyOptions = proj->review_compilation_options.prompt({{"function", m_prototype.declaration}});
            }
            else {
                strategyOptions = proj->review_refactoring_options.prompt({{"function", m_prototype.declaration}});
            }
            
            std::string favoriteFunctions;
            for(auto func : libFunctions.items)
            {
                favoriteFunctions += proj->getFunctionInfo(func->func_name);
            }
            
            std::string reviewCompilationMessge = proj->review_compilation_slef.prompt({
                {"function", m_brief.func_name},
                {"declaration", m_prototype.declaration},
                {"function_ctx", context},
                {"command", compileCL},
                {"output", output},
                {"api", api},
                {"existing_functions", favoriteFunctions},
                {"checklist", checklist},
                {"analysis", currentAnalysis},
                {"options", strategyOptions}
            });
            
            std::string cache = "";
            inference(cache, reviewCompilationMessge, &reviewData);
        }
    }

    std::string CCodeNode::getDataDefinitions(std::set<std::string>& structs, std::set<std::string>& enums,
                                              bool declaration, bool definition, bool reportDiff) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        //Format: dataTypeName, set of changed members (empty set means new datatype)
        std::map<std::string, std::set<std::string>> diff;
        
        if(reportDiff)
        {
            diff = proj->diffWithDataSnapshot();
        }
        
        std::string dataDefinitions;
        std::string dataDeclarations;
        for(auto e : enums)
        {
            std::string owningPath;
            auto dataType = proj->findData(e, owningPath);
            if(dataType)
            {
                dataDeclarations += "enum class ";
                dataDeclarations += dataType->m_typeDef.m_name;
                dataDeclarations += " : uint32_t;\n";
             
                if(reportDiff)
                {
                    auto it = diff.find(dataType->m_typeDef.m_name);
                    if(it != diff.end())
                    {
                        if(it->second.empty())
                        {
                            dataDefinitions += "//New enum\n";
                        }
                        else
                        {
                            dataDefinitions += "//Revised enum, new fields:\n//";
                            dataDefinitions += getAsCsv(it->second);
                            dataDefinitions += "\n";
                        }
                    }
                }
                dataDefinitions += dataType->m_typeDef.m_definition;
                dataDefinitions += ";\n\n";
            }
        }
        
        for(auto s : structs)
        {
            std::string owningPath;
            auto dataType = proj->findData(s, owningPath);
            if(dataType)
            {
                if(reportDiff)
                {
                    auto it = diff.find(dataType->m_typeDef.m_name);
                    if(it != diff.end())
                    {
                        if(it->second.empty())
                        {
                            dataDefinitions += "//New struct\n";
                        }
                        else
                        {
                            dataDefinitions += "//Revised struct, new members:\n//";
                            dataDefinitions += getAsCsv(it->second);
                            dataDefinitions += "\n";
                        }
                    }
                }
                dataDeclarations += "struct ";
                dataDeclarations += dataType->m_typeDef.m_name;
                dataDeclarations += ";\n";
                
                dataDefinitions += dataType->m_typeDef.m_definition;
                dataDefinitions += ";\n\n";
            }
        }
        
        std::string api;
        if(declaration && !dataDeclarations.empty())
        {
            api += dataDeclarations;
            api += "\n";
        }
        if(definition)
        {
            api += dataDefinitions;
        }
        return api;
    }

    std::string CCodeNode::getApiForReview(std::set<std::string>& owners,
                                std::set<std::string>& structs,
                                std::set<std::string>& enums,
                                bool getData) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::string functionsDef;
        //std::string api;
        //TODO: Move get API here
        getFullVisibility(false, owners, structs, enums);
        
        for(auto funcPath : owners)
        {
            const CCodeNode* node = (const CCodeNode*)proj->getNode(funcPath);
            
            //TODO: I'm quite sure we must consider some kind of error if there is no node with the same name.
            if(!node) continue;
            
            functionsDef += "Description of function: ";
            functionsDef += node->m_prototype.declaration + "\n\n";
            functionsDef += node->m_prototype.brief + "\n\n";
        }
        
        std::string api;
        if(getData)
        {
            api += getDataDefinitions(structs, enums, false, true, false);
        }
        api += functionsDef;
        
        return api;
    }

    std::string CCodeNode::getDataApi(bool getDetached, bool declaration, bool definition, std::set<std::string>& owners) const
    {
        std::set<std::string> structs;
        std::set<std::string> enums;
        
        getFullVisibility(getDetached, owners, structs, enums);
        return getDataDefinitions(structs, enums, declaration, definition, false);
    }

    std::string CCodeNode::getTypesInfo(const std::vector<std::shared_ptr<std::string>>& unknownTypes, bool& additionalInfo, const std::set<std::string>& owners)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string selectedFunctions;
        for(auto type : unknownTypes)
        {
            std::string owningPath;
            if(proj->findData(*type, owningPath))
            {
                if(owners.find(owningPath) == owners.end())
                {
                    addToSet(m_implementation.dependencies, owningPath);
                    
                    if(!additionalInfo)
                    {
                        selectedFunctions += "Here is the information for the requested data types and functions descriptions ";
                        selectedFunctions += "required for the implementation of '";
                        selectedFunctions += m_brief.func_name + "'\n";
                        
                        additionalInfo = true;
                    }
                    
                    selectedFunctions += proj->declareData(false, owningPath);
                    selectedFunctions += proj->defineData(false, owningPath);
                }
            }
        }
        
        return selectedFunctions;
    }

    std::string CCodeNode::getFunctionsInfo(const CodeReview& reviewData, bool& additionalInfo)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::string selectedFunctions;
        for(auto func : reviewData.existing_functions)
        {
            const CCodeNode* node = nullptr;
            auto it = proj->nodeMap().find(*func);
            if(it == proj->nodeMap().end())
            {
                continue;
            }
            
            node = (const CCodeNode*)it->second;
            if(!node)
            {
                continue;
            }
            
            if(!additionalInfo)
            {
                selectedFunctions += "Here is the information for the requested data types and functions descriptions ";
                selectedFunctions += "required for the implementation of '";
                selectedFunctions += m_brief.func_name + "'\n";
                additionalInfo = true;
            }
            
            selectedFunctions += "Description of function: ";
            selectedFunctions += node->m_prototype.declaration + "\n\n";
            //Is this need to be brief or full description? We add the implementation for each function
            selectedFunctions += node->m_prototype.brief + "\n\n";
            
            if(!isFromLibrary(*func))
            {
                LibFunctionItem libFunction;
                libFunction.func_name = *func;
                //We could have LibFunctionList for existing_functions instead of strings if we need better motivation
                libFunction.motivation = "Potential usage in the implementation of " + m_brief.func_name;
                m_libCalls.items.push_back(std::make_shared<LibFunctionItem>(libFunction));
            }
        }
        
        return selectedFunctions;
    }

    bool CCodeNode::refactorFunctions(const CodeReview& reviewData)
    {
        if(reviewData.new_functions.empty())
        {
            return false;
        }
        
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        //TODO: Do one for loop to only create child nodes, then save the curren node, then do another for loop to build the sub-nodes
        
        bool updateOrder = false;
        std::vector<CCodeNode*> newNodes;
        
        for(auto func : reviewData.new_functions)
        {
            if(func->func_name == m_brief.func_name)
                continue;
            
            //Seems the LLM concluded to fix the problem we must breakdown the current node
            //Let's spawn sub-nodes and butild hierarchies
            captureContext();
            
            //TODO: Do we need to summarize something here.
            
            FunctionItem brief = *func;
            
            bool isShared = false;
            
            //Very unlikely since we provided list with existing functions
            auto it = proj->nodeMap().find(brief.func_name);
            if(it != proj->nodeMap().end())
            {
                //We have a conflict with the name of an existing function
                //Let's ask the LLM to compare both functions and decide if they are similar and can be reused
                //Or to adjust the name and description of the current function
                auto existingFuncNode = (const CCodeNode*)it->second;
                isShared = resolveName(existingFuncNode, brief);
            }
            
            m_calls.items.push_back(std::make_shared<FunctionItem>(brief));
            updateOrder = true;
            
            CCodeNode* child = proj->shareNode<CCodeNode>(brief.func_name, this);
            if(child->refCount() == 1)//We do expect this BTW!
            {
                child->m_brief = brief;
                newNodes.push_back(child);
            }
            
            popContext();
        }
        
        if(updateOrder)
        {
            m_calls.updateOrder();
            if(proj->m_save)
            {
                save();
            }
        }
        
        if(!newNodes.empty())
        {
            uint32_t archiveId = storeContext(m_compilationStartMessage);
            
            std::string createdNewFunctions = newNodes.size() > 1 ?
            "The following new functions have been created:\n" :
            "The following new function has been created:\n";
            
            for(auto node : newNodes)
            {
                captureContext();
                
                node->defineAndBuild();
                
                createdNewFunctions += node->m_prototype.declaration + "\n";
                
                popContext();
            }
            
            restoreContext(archiveId);
            
            pushMessage(createdNewFunctions, "user");
        }
        
        return updateOrder;
    }

    bool CCodeNode::compileSource(const std::string& compileCL, std::string& output) const
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        
        // Diagnostic guard: verify build source on disk still contains current in-memory implementation.
        const std::string srcFile = getSourceFilePath();
        const std::string diskSource = getFileContent(srcFile);

        if(diskSource.empty() || diskSource.find(m_implementation.m_source) == std::string::npos)
        {
            std::cout << "[WARN][compileSource] Source mismatch for '" << m_brief.func_name << "'\n";
            std::cout << "  file: " << srcFile << "\n";
            std::cout << "  mem_chars: " << m_implementation.m_source.size()
                      << " disk_chars: " << diskSource.size() << std::endl;
        }
        
        //Recompile the updated sources
        output = exec(compileCL, testDir, "Compile");
        
        //TODO: add object to the build chache directory
        bool result = objectExists();
        
        if(result)
        {
            size_t hash = getNodeHash();
            saveNodeHash(hash);
            cacheObject();
        }
        
        return result;
    }

    bool CCodeNode::updateSource(CodeType type, CCodeNode* parent, const std::string& message, const std::string& compileCL, std::string& output, bool forceRefactoring)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        //It should be totally fine to update all externals here based on the current declaration
        updateExternals();
        
        //Finaly, try to refactor the function and hopefully it will be successful
        //This is the best we can do
        std::string source = "cpp";
        bool truncated = false;
        
        std::string cache = "";
        inference(cache, message, source, &truncated);
        popMessages(1);
        if(truncated || forceRefactoring)
        {
            std::string newFunction = refactorTruncatedSource(source, type);
        }
        
        //Extracts Doxygen single line brief function description from source
        std::string brief = extractBrief(source);
        
        pushMessage(source, "assistant");
        
        //We need to make sure the code will compile but also meet the necessary requirements
        codeReview(source, type, -1, true, false);
        
        //We want to add nodes for the new calls, but will keep unused
        updateCallsUsage(true, false);
        m_calls.updateOrder();//Shouldn't be a problem to update the order.
        
        std::map<std::string, TypeDefinition> dataTypes;
        std::map<std::string, std::string> functions;
        extractFromSource(source, false, dataTypes, functions);
        
        bool genParentSrc = false;
        for(const auto& func : functions)
        {
            if(func.first == m_brief.func_name) {
                m_implementation.m_source = func.second;
                if(updateDeclaration())
                {
                    updateExternals();
                }
            }
            else if(parent && func.first == parent->m_brief.func_name) {
                parent->m_implementation.m_source = func.second;
                bool parentDataUpdated = parent->updateDeclaration();
                if(parentDataUpdated)
                {
                    parent->updateExternals();
                }
                genParentSrc = genParentSrc || parentDataUpdated;
            }
        }
        
        if(parent && genParentSrc)
        {
            parent->generateAllSources(true);
        }
        
        //Regenerate source files
        generateAllSources(true);
        
        bool result = compileSource(compileCL, output);
        
        //Update the brief if any
        if(result && !brief.empty())
        {
            m_brief.brief = brief;
            m_prototype.brief = brief;
            m_description.brief = brief;
        }
        
        return result;
    }

    bool CCodeNode::reviewCompilation(CCodeNode* parent, CompilationReviewType approach, const std::string& compileCL, std::string& output)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        std::set<std::string> owners;
        std::set<std::string> structs;
        std::set<std::string> enums;
        
        //It should be totally fine to update all externals here based on the current declaration
        updateExternals();
        
        std::string analysis = analyzeCompilation(output, owners, structs, enums);
        
        auto identiriers = proj->getAllIdentifiers();
        std::string cleanOutput = cleanClangOutput(output, proj->getProjDir(), identiriers, true);
        
        CompilationReview thisReview;
        thisReview.m_compilationOutput = cleanOutput;
        thisReview.m_initialImplementation = m_implementation.m_source;
        
        std::string api = getApiForReview(owners, structs, enums);
        
        std::string context;
        {
            context += "Description of function: ";
            context += m_prototype.declaration + "\n\n";
            context += m_prototype.description + "\n\n";
            context += "Function definition:\n";
            context += m_implementation.m_source + "\n";
        }
        
        std::string selectedFunctions;
        bool additionalInfo = false;
        std::string cache = "";
        
        LLMRole llmRole = client.getLLM();
        
        if(approach == CompilationReviewType::OPTIMISTIC)
        {
            pushCompileProgress();
            
            CodeReviewLite reviewData;
            
            std::string checklist = proj->source_checklist.prompt({{"function", m_prototype.declaration}});
            
            std::string panicMessge = proj->review_compilation_luck.prompt({
                {"function", m_brief.func_name},
                {"declaration", m_prototype.declaration},
                {"function_ctx", context},
                {"command", compileCL},
                {"output", cleanOutput},
                {"api", api},
                {"checklist", checklist},
                {"analysis", analysis}
            });
            
            inference(cache, panicMessge, &reviewData);
            
            selectedFunctions += getTypesInfo(reviewData.unknown_types, additionalInfo, owners);
            
            thisReview.m_proposedSolution = reviewData.errors_brief + " " + reviewData.review_brief;
        }
        else if(approach == CompilationReviewType::PANIC)
        {
            pushCompileProgress();
            
            CodeReviewLite reviewData;
            
            std::string panicMessge = proj->review_compilation_panic.prompt({
                {"function", m_brief.func_name},
                {"declaration", m_prototype.declaration},
                {"function_ctx", context},
                {"command", compileCL},
                {"output", cleanOutput},
                {"api", api}
            });
            
            inference(cache, panicMessge, &reviewData);
            
            selectedFunctions += getTypesInfo(reviewData.unknown_types, additionalInfo, owners);
            
            thisReview.m_proposedSolution = reviewData.errors_brief + " " + reviewData.review_brief;
        }
        else
        {
            CodeReview reviewData;
            
            inferenceReviewLoop(reviewData, context, compileCL, cleanOutput, api, analysis);
            
            selectedFunctions += getTypesInfo(reviewData.unknown_types, additionalInfo, owners);
            
            bool updateOrder = false;
            if(!reviewData.new_functions.empty() && proj->m_refactoringDepth <= 3)
            {
                updateOrder = refactorFunctions(reviewData);
            }
            
            if(updateOrder)
            {
                m_calls.updateOrder();
            }
            
            //At this point do we need the old lib functions found during breakdown ?!?!?
            m_libCalls.items.clear();
            
            if(!reviewData.existing_functions.empty())
            {
                selectedFunctions += getFunctionsInfo(reviewData, additionalInfo);
            }
            
            mergeLibCalls();
            thisReview.m_proposedSolution = reviewData.errors_brief + " " + reviewData.review_brief;
        }
        
        //reset in case of change during refactoring!
        client.setLLM(llmRole);
        
        std::string fixCompilationMessge = proj->fix_compilation.prompt({
            {"declaration", m_prototype.declaration},
            {"function", m_brief.func_name},
            {"api", selectedFunctions}
        });
        
        m_stats.reset();
        
        bool compiled_ok = updateSource(CodeType::FUNC_CMPL, parent, fixCompilationMessge, compileCL, output, false);
        
        thisReview.m_revisedImplementation = m_implementation.m_source;
        m_stats.m_compileProgress.push_back(thisReview);
        
        if(compiled_ok)
        {
            std::string commitMessage = "\nFix compilation for function: " + m_brief.func_name + "\n\n";
            commitMessage += compileProgressMessage();
            commitMessage += "\n";
            proj->commit(commitMessage);
        }
        
        return compiled_ok;
    }

    std::string CCodeNode::getSourceFilePath() const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();

        std::string nodeBuildDir = proj->getProjDir() + "/build/source/";
        nodeBuildDir += m_name;
        
        std::string path = nodeBuildDir + "/" + m_name + ".cpp";
        return path;
    }

    std::string CCodeNode::getObectFilePath() const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        std::string binDir = buildDir + "/" + platform;
        
        //It is great to have mechanism to avoid unnecessary inferencing
        //but I'm not convinced this is the right one
        std::string objFile = binDir + "/" + m_brief.func_name + ".o";
        
        return objFile;
    }

    bool CCodeNode::objectExists() const
    {
        std::string objFile = getObectFilePath();
        return boost_fs::exists(objFile);
    }

    bool CCodeNode::objectIsValid() const
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::size_t hash = getNodeHash();
        std::size_t cachedHash = getCachedNodeHash();
        
        std::cout << "objectIsValid node: " << m_name << " hash: " << hash << " cachedHash: " << cachedHash << std::endl;
        return hash == cachedHash;
    }

    size_t CCodeNode::getNodeHash() const
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::string dagPath = getDAGPath("/");
        
        std::string data;
        
        //We need declareData for enums
        std::set<std::string> referencedNodes;
        referencedNodes.clear();
        data += getDataApi(false, true, true, referencedNodes);
        
        std::string srcFile = getSourceFilePath();
        data += getFileContent(srcFile);
        
        return boost::hash_range(data.begin(), data.end());
    }

    size_t CCodeNode::getCachedNodeHash() const
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        // Build the cache directory path
        boost_fs::path cacheDir = proj->getBuildCacheDir();
        cacheDir /= getName();

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
        
        //Only two files are expected the cached object and the .txt with hash
        if (fileCount != 2)
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

    void CCodeNode::saveNodeHash(size_t hash) const
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        // Build the cache directory path
        boost_fs::path cacheDir = proj->getBuildCacheDir();
        cacheDir /= getName();

        // Path to the hash file
        boost_fs::path hashFile = cacheDir / "hash.txt";

        boost_fs::remove(hashFile);
        
        std::string hashStr = std::to_string(hash);
        
        std::cout << "saveNodeHash node: " << m_name << " hash: " << hash << std::endl;
        saveToFile(hashStr, hashFile.string());
    }

    void CCodeNode::cacheObject() const
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        // Build the cache directory path
        boost_fs::path cacheDir = proj->getBuildCacheDir();
        cacheDir /= getName();
        
        std::string objFile = getObectFilePath();
        boost_fs::path cachedObjectPath = cacheDir / (getName() + ".o");
        if(boost_fs::exists(cachedObjectPath))
        {
            boost_fs::remove(cachedObjectPath);
        }
        
        boost_fs::copy(objFile, cachedObjectPath);
    }

    void CCodeNode::restoreCachedObject() const
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        // Build the cache directory path
        boost_fs::path cacheDir = proj->getBuildCacheDir();
        cacheDir /= getName();
        
        std::string objFile = getObectFilePath();
        boost_fs::path cachedObjectPath = cacheDir / (getName() + ".o");
        if(boost_fs::exists(cachedObjectPath))
        {
            if(boost_fs::exists(objFile))
            {
                boost_fs::remove(objFile);
            }
            
            boost_fs::copy(cachedObjectPath, objFile);
        }
    }

    bool CCodeNode::compile(int attempts)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::string nodeBuildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + nodeBuildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        std::string binDir = buildDir + "/" + platform;
        
        m_stats.m_compileProgress.clear();
        m_stats.m_initialImplementation = m_implementation.m_source;
        
        //TODO: Implement multithreaded builinding on load with CCodeProject::compileSource
        if(objectIsValid())
        {
            restoreCachedObject();
            return true;
        }
        
        std::string objFile = getObectFilePath();
        boost_fs::remove(objFile);
        
        generateAllSources(true);
        boost_fs::create_directories(testDir);
        boost_fs::create_directories(binDir);
        
        CCodeNode* parent = nullptr;
        if(m_this->m_parent)
        {
            parent = (CCodeNode*)m_this->m_parent->m_data;
            assert(parent);
        }
        
        std::string compileCL = compileCommand(platform, BUILD_PRINT_TEST | BUILD_DEBUG);
        std::string output;
        compileSource(compileCL, output);
        int tryID = 0;
        
        std::map<std::string, DataInfo> dataSnapshot = proj->getDataShapshot();
        std::string sourceSnapshot = m_implementation.m_source;
        Function definitionSnapshot = m_prototype;
        
        int maxAttempts = (attempts > 0 && attempts < COMPILE_ATTEMPTS_MAX) ? attempts : COMPILE_ATTEMPTS_MAX;
        CompilationReviewType lastApproach = CompilationReviewType::COUNT;
        int escalations = 0;
        while (!objectExists() && tryID < maxAttempts) {
            
            if(tryID == 0)
            {
                Client::getInstance().agentToServer("\n\nTHINKING...\n\n");
            }
            
            m_compilationStartMessage = captureContext();
            
            CompilationReviewType approach = CompilationReviewType::NORMAL;
            if(tryID < COMPILE_ATTEMPTS_OPTIMISTIC)
            {
                approach = CompilationReviewType::OPTIMISTIC;
                client.selectLLM(InferenceIntent::FIX_OPTIMISTIC);
            }
            else if(tryID >= COMPILE_ATTEMPTS_MAX-COMPILE_ATTEMPTS_PANIC)
            {
                approach = CompilationReviewType::PANIC;
                client.selectLLM(InferenceIntent::FIX_PANIC);
            }
            else
            {
                client.selectLLM(InferenceIntent::FIX);
            }
            
            //Handle LLM escalations
            //-----------------------------------
            if(approach == lastApproach)
            {
                for(int i=0; i<escalations; ++i)
                {
                    client.escalateLLM();
                }
                escalations++;
            }
            else
            {
                //TODO: Consider if we switch from optimistic to full fix to reset the data and source
                if(lastApproach == CompilationReviewType::OPTIMISTIC)
                {
                    proj->restoreDataSnapshot(dataSnapshot);
                    m_implementation.m_source = sourceSnapshot;
                    //TODO: What if there was declaration change?
                    m_prototype = definitionSnapshot;
                    
                    // Keep diagnostics aligned with the restored source.
                    generateAllSources(true);
                    
                    const bool restored_ok = compileSource(compileCL, output);
                    if (restored_ok)
                    {
                        popContext();
                        continue;
                    }
                }
                
                escalations = 0;
            }
            lastApproach = approach;
            //-----------------------------------
            
            proj->m_refactoringDepth++;
            reviewCompilation(parent, approach, compileCL, output);
            proj->m_refactoringDepth--;
            
            popContext();
            
            tryID++;
        }
        
        if(tryID)
        {
            std::string report = generateReport();
            Client::getInstance().agentToServer(report);
        }
        
        //Does this need to be saved?
        m_stats.m_compileProgress.clear();
        m_stats.m_initialImplementation.clear();
        
        bool objectIsValid = objectExists();
        if(objectIsValid)
        {
            size_t hash = getNodeHash();
            saveNodeHash(hash);
            cacheObject();
        }
        
        return objectIsValid;
    }

    void CCodeNode::preBuild()
    {
        if(!m_defined)
        {
            std::cout << "Attempt to build node: '" << m_brief.func_name << "' that is not defined yet" << std::endl;
            return;
        }
        
        generateSources(false);
    }

    void CCodeNode::build()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        //TODO:  We should not worry if this function is called upstream graph,
        // it will be built when all children are ready
        //TODO: This idea requires serious testing!!!
        if(proj->m_buildingNow.find(m_brief.func_name) != proj->m_buildingNow.end())
        {
            std::cout << "Attempt to build node: '" << m_brief.func_name << "' while it is being built" << std::endl;
            return;
        }
        
        if(!m_defined)
        {
            std::cout << "Attempt to build node: '" << m_brief.func_name << "' that is not defined yet" << std::endl;
            return;
        }
        
        proj->m_buildingNow.insert(m_brief.func_name);
        
        std::string nodeDagPath = getDAGPath(">");
        std::cout << "Building: " << m_prototype.declaration << " type: " << m_brief.func_name << std::endl;
        std::cout << "Path: " << nodeDagPath << std::endl;
        
        Client::getInstance().setContextLogDir(proj->getProjDir() + "/logs/nodes/" + m_brief.func_name);
        
        compile();
    
        if(proj->m_save)
        {
            save();
        }
        
        proj->m_buildingNow.erase(m_brief.func_name);
    }

    void CCodeNode::getFullVisibility(bool getDetached,
                                      std::set<std::string>& owners,
                                      std::set<std::string>& structs,
                                      std::set<std::string>& enums) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        proj->getVisibleTypes(getDetached, structs, enums, owners, getDAGPath("/"));
        
        for(auto ext : m_implementation.externals)
        {
            proj->getVisibleTypes(getDetached, structs, enums, owners, *ext);
        }
        
        for(auto dep : m_implementation.dependencies)
        {
            proj->getVisibleTypes(getDetached, structs, enums, owners, *dep);
        }
        
        for(auto func : m_calls.items)
        {
            const CCodeNode* node = nullptr;
            auto it = proj->nodeMap().find(func->func_name);
            if(it != proj->nodeMap().end())
            {
                node = (const CCodeNode*)it->second;
            }
            
            //TODO: I'm quite sure we must signal error if there is no node with the same name.
            if(!node) continue;
            //For whatever reason this node is undefined
            if(node->m_prototype.declaration.empty()) continue;
            
            std::string path = node->getDAGPath("/");
            owners.insert(path);
            proj->getVisibleTypes(getDetached, structs, enums, owners, path);
            
            for(auto ext : node->m_implementation.externals)
            {
                proj->getVisibleTypes(getDetached, structs, enums, owners, *ext);
            }
        }
    }

    std::string CCodeNode::generateIncludes(bool checkIncludes) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::stringstream sout;

        std::set<std::string> owners;
        std::set<std::string> structs;
        std::set<std::string> enums;
        
        getFullVisibility(false, owners, structs, enums);
        
        sout << proj->listIncludes(owners, checkIncludes);
        
        return sout.str();
    }

    std::string CCodeNode::generateProjectIncludes() const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::stringstream sout;

        std::set<std::string> owners;
        std::set<std::string> structs;
        std::set<std::string> enums;
        
        getFullVisibility(false, owners, structs, enums);
        
        for(auto owner : owners)
        {
            std::string nodeName = getLastToken(owner, '/');
            
            sout << "#include \"" << nodeName << ".h\"" << std::endl;
        }
        
        return sout.str();
    }

    std::string CCodeNode::generateDeclarations(/*const std::string& skip*/) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::stringstream sout;
        if(!m_calls.items.empty())
        {
            sout << "//Functions called by '" + m_brief.func_name + "' :" << std::endl;
        }
        
        for(auto func : m_calls.items)
        {
            const CCodeNode* node = nullptr;
            auto it = proj->nodeMap().find(func->func_name);
            if(it != proj->nodeMap().end())
            {
                node = (const CCodeNode*)it->second;
            }
            
            //TODO: This is not expected!
            if(!node) continue;
            
            if(node->m_prototype.declaration.empty())
            {
                sout << "// ";
                sout << func->func_name;
                sout << " - not yet defined" << std::endl;
            }
            else
            {
                sout << node->m_prototype.declaration << std::endl;
            }
        }
        
        if(!m_calls.items.empty())
        {
            sout << std::endl << std::endl;
        }
        
        return sout.str();
    }

    std::string CCodeNode::getNodeBuildSourcePath() const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::string buildSourcePath;
        buildSourcePath += proj->getBuildSourcePath() + "/";
        buildSourcePath += getName();
        return buildSourcePath;
    }

    void CCodeNode::generateHeader() const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::set<std::string> stdIncludes;
        std::set<std::string> includes;
        std::set<std::string> includeDirs;

        std::string dagPath = getDAGPath("/");
        std::string buildSourcePath = getNodeBuildSourcePath();
        
        std::string nodeBuildDir = proj->getProjDir() + "/build/";
        nodeBuildDir += buildSourcePath;
        std::replace(nodeBuildDir.begin(), nodeBuildDir.end(), '>', '/');
        
        std::string path = nodeBuildDir + "/";
        
        boost_fs::create_directories(path);
        std::string headerPath = path + m_name + ".h";
        std::ofstream header(headerPath);
        
        std::string disclamer = Peer::getDisclamer();
        printAsComment(disclamer, header);
        header << std::endl;
        
        header << "#pragma once" << std::endl << std::endl;
        
        header << "#include \"common.h\"" << std::endl;
        
        header << std::endl;
        
        for(auto ext : m_implementation.externals)
        {
            std::string nodeName = getLastAfter(*ext, "/");
            std::string include = proj->getBuildSourcePath() + "/";
            include += nodeName + "/" + nodeName;
            header << "#include \"" << include << ".h\"" << std::endl;
        }
        
        header << std::endl;
        
        header << proj->declareData(false, dagPath) << std::endl << std::endl;
        header << proj->defineReferencedData(dagPath) << std::endl << std::endl;
        
        header << std::endl << std::endl;
        printAsComment(m_prototype.brief, header);
        header << std::endl;
        
        header << m_prototype.declaration << std::endl << std::endl;
        
        header.close();
    }

    void CCodeNode::generateSources(bool checkIncludes) const
    {
        //Generate header first
        generateHeader();
        
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::set<std::string> stdIncludes;
        std::set<std::string> includes;
        std::set<std::string> includeDirs;

        std::string dagPath = getDAGPath("/");
        std::string buildSourcePath = getNodeBuildSourcePath();

        std::string nodeBuildDir = proj->getProjDir() + "/build/";
        nodeBuildDir += buildSourcePath;
        std::replace(nodeBuildDir.begin(), nodeBuildDir.end(), '>', '/');
        
        std::string path = nodeBuildDir + "/";
        
        boost_fs::create_directories(path);
        std::ofstream cpp(path + m_name + ".cpp");
        
        std::string disclamer = Peer::getDisclamer();
        
        std::string thisInclude = "#include \"";
        thisInclude += m_name;
        thisInclude += ".h\"";
        
        //cpp << disclamer << std::endl;
        printAsComment(disclamer, cpp);
        cpp << thisInclude << std::endl;
        
        
        cpp << generateIncludes(checkIncludes);
        //The LLM sees the parent implementation while implementing this function
        //and can decide to use some of the functions visible for the parent
        //We need to include all visible functions in the cpp source

        cpp << std::endl;
        
        printAsComment(m_prototype.description, cpp);
        cpp << std::endl;
        cpp << m_implementation.m_source << std::endl;
        
        cpp.close();
    }

    void CCodeNode::generateAllSources(bool checkIncludes) const
    {
        //Ensure headers for all dependencies are regenerated
        
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        //We need this for all headers included in the .cpp file
        std::set<std::string> owners;
        std::set<std::string> structs;
        std::set<std::string> enums;
        getFullVisibility(false, owners, structs, enums);
        
        for(auto include : owners)
        {
            std::string nodeName = getLastAfter(include, "/");
            CCodeNode* ccNode = proj->getNodeByName(nodeName);
            
            if(!ccNode){
                continue;
            }
            
            ccNode->generateSources(true);
        }
        
        generateSources(true);
    }

    void CCodeNode::generateProjectSources() const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::set<std::string> stdIncludes;
        std::set<std::string> includes;
        std::set<std::string> includeDirs;

        std::string dagPath = getDAGPath("/");

        std::string path = proj->getProjDir() + "/project/sources/";
        
        boost_fs::create_directories(path);
        
        std::string headerPath = path + m_name + ".h";
        std::ofstream header(headerPath);
        std::ofstream cpp(path + m_name + ".cpp");
        
        std::string disclamer;
        disclamer += "//This file has been auto-generated by a machine-learning model for educational purposes only.\n";
        disclamer += "//Use with caution – the compiled code may contain defects, security vulnerabilities, or other issues.\n";
        disclamer += "//Do not integrate this file into production or sensitive code bases without thorough review and testing!\n";
        
        header << disclamer << std::endl;
        
        header << "#pragma once" << std::endl << std::endl;
        
        header << "#include \"common.h\"" << std::endl;
        
        for(auto ext : m_implementation.externals)
        {
            std::string nodeName = getLastAfter(*ext, "/");
            header << "#include \"" << nodeName << ".h\"" << std::endl;
        }
        
        header << std::endl;
        
        header << proj->declareData(false, dagPath) << std::endl << std::endl;
        header << proj->defineReferencedData(dagPath) << std::endl << std::endl;
        
        printAsComment(m_prototype.brief, header);
        header << std::endl;
        
        header << m_prototype.declaration << std::endl << std::endl;
        
        std::string thisInclude = "#include \"";
        thisInclude += m_name;
        thisInclude += ".h\"";
        
        cpp << disclamer << std::endl;
        cpp << thisInclude << std::endl;
        
        cpp << generateProjectIncludes();
        
        cpp << std::endl;
        
        printAsComment(m_prototype.description, cpp);
        cpp << std::endl;
        cpp << m_implementation.m_source << std::endl;
        
        header.close();
        cpp.close();
    }

    void CCodeNode::reasonAboutCode(std::string& source, CodeType srcType,
                     int tryToRecover,
                     bool enableSelfReview,
                     bool enableCache)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        bool wasOnAuto = client.run();
        
        std::string cache;
        if(enableCache)
        {
            cache = "implementation.json:definition";
        }
        std::string reviseMessage;
        bool selfReviewPass = true;
        
        reviewImplementation(source, srcType);
        
        if(tryToRecover == 0)
            return;
        else if(tryToRecover > 0)
        {
            //We are in a controlled review loop. Let's not do the self review.
            enableSelfReview = false;
        }
        
        if(enableSelfReview)
        {
            client.selectLLM(InferenceIntent::REASON_IMPLEMENT);
            
            std::string reviewMessage = proj->review_source_self.prompt({
                {"function", m_brief.func_name} });
            
            selfReviewPass = inference(cache, reviewMessage, true);
            
            if(!selfReviewPass)
            {
                reviseMessage += "Would you please fix this and update the source?";
            }
        }
        
        bool reviewPass = m_codeReview.str().empty();
        if(!reviewPass)
        {
            reviseMessage += " And a few other things that may also require attention, but aren't necessarily a problem: \n";
            reviseMessage += m_codeReview.str();
        }
        
        if(!selfReviewPass || !reviewPass)
        {
            if(!wasOnAuto) client.stop();
            
            InferenceIntent intent = tryToRecover > 0 ? InferenceIntent::IMPLEMENT_OPTIMISTIC : InferenceIntent::IMPLEMENT;
            client.selectLLM(intent);
            
            reviseMessage += "\nReview the above findings, consult with our discussion so far for what/how we try to fix the issues and errors, ensure we aren't (re)introuduce new problems, and if it makes sense, consider modifying the code in your response! Please provide the complete source code in your reply, whether it has been modified or remains unchanged.\n";
        
            source = "cpp";
            bool truncated = false;
            inference(cache, reviseMessage, source, &truncated);
            if(truncated)
            {
                popMessages(1); //remove the returned source from the context
                std::string newFunction = refactorTruncatedSource(source, srcType);
            }
        }
    }

    void CCodeNode::codeReviewLoop(std::string& source, CodeType srcType, FileName& prompt, bool wasOnAuto, int tryToRecover)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        bool loop = true;
        int attmpts = 0;
        int escalateThreshold = 0;
        
        uint32_t startReviewTag = captureContext();
        
        std::string srcMessage = "```cpp\n";
        srcMessage += source;
        srcMessage += "```\n";
        pushMessage(srcMessage, "assistant");
        
        while(loop && attmpts < MAX_REVIEW_ATTPMPTS && (tryToRecover < 0 || attmpts < tryToRecover))
        {
            if(!wasOnAuto) client.stop();
            
            reviewImplementation(source, srcType);
            
            if(m_codeReview.str().empty())
                break;
            
            if(escalateThreshold >= ESCALATE_AFTER_FAILED_REVIEWS)
            {
                client.escalateLLM();
                escalateThreshold = 0;
            }
            
            escalateThreshold++;
            
            //std::string reviseMessage = proj->review_source.prompt({
            std::string reviseMessage = prompt.prompt({
                //{"source", srcMessage},
                {"function", m_brief.func_name},
                {"review", m_codeReview.str()} });
            
            if(attmpts > MAX_REVIEW_HISTORY)
            {
                //Erase the first review request and response
                proj->eraseFromContext(startReviewTag, 2);
            }
            
            source = "cpp";
            std::string cache; //no cache for reviews
            bool truncated = false;
            loop = inference(cache, reviseMessage, source, &truncated);
            
            if(truncated)
            {
                uint32_t archiveId = storeContext(startReviewTag);
                //Call refactorTruncatedSource since it leaves a messge for refactoring in the context
                std::string newFunction = refactorTruncatedSource(source, srcType);
                restoreContext(archiveId);
            }
            
            //Remove original source
            popMessages(1);
            
            srcMessage = "```cpp\n";
            srcMessage += source;
            srcMessage += "```\n";
            pushMessage(srcMessage, "assistant");
            
            attmpts++;
        }
        
        popContext();

        if(!wasOnAuto) client.stop();
    }

    void CCodeNode::codeReview(std::string& source, CodeType srcType, int tryToRecover, bool enableSelfReview, bool enableCache)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        bool wasOnAuto = client.run();
        
#ifdef ENABLE_CODE_SELF_REVIEW
        captureContext();
        reasonAboutCode(source, srcType, tryToRecover, enableSelfReview, enableCache);
        popContext();
#endif //ENABLE_CODE_SELF_REVIEW
        
        popMessages(1);//pop the original source from the context
        
        InferenceIntent intent = tryToRecover > 0 ? InferenceIntent::IMPLEMENT_OPTIMISTIC : InferenceIntent::IMPLEMENT;
        client.selectLLM(intent);
    
        codeReviewLoop(source, srcType, proj->review_source, wasOnAuto, tryToRecover);
    }

    bool CCodeNode::implement(bool speculative)
    {
        CCodeNode* parent = nullptr;
        
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        assert(m_this);
        
        m_stats.reset();
        
        captureContext();
        
        if(m_this->m_parent)
        {
            parent = (CCodeNode*)m_this->m_parent->m_data;
            assert(parent);
        }
        
        std::string call_api = "None";
        if(m_calls.items.size())
        {
            call_api = summarizeCalls(true, false, true);
        }
        
        std::string callers;
        std::set<std::string> referencedNodes;
        callers += proj->getDetachedData();
        referencedNodes.insert("__DETACHED__");
        
        if(parent)
        {
            callers += "Note: ";
            callers += m_prototype.declaration;
            callers += "\nis called in the implementatin of: ";
            callers += parent->m_brief.func_name;
            callers += "\n";
            callers += parent->getContexInfo(false, true, true, referencedNodes);
            if(!m_inRefactoringHint.empty())
            {
                //In case of refactoring, do it with the smartest model
                Client::getInstance().setLLM(LLMRole::DIRECTOR);
                
                callers += m_inRefactoringHint;
                m_inRefactoringHint.clear();
            }
        }
        
        InferenceIntent intent = speculative ? InferenceIntent::IMPLEMENT_OPTIMISTIC : InferenceIntent::IMPLEMENT;
        client.selectLLM(intent);
        
        std::string checklist = proj->source_checklist.prompt({{"function", m_brief.func_name}});
        std::string implement_function = proj->implement.prompt({
            {"declaration", m_prototype.declaration},
            {"function", m_brief.func_name},
            //{"func_desc", m_prototype.description},
            {"call_api", call_api},
            {"callers", callers},
            {"checklist", checklist},
            {"struct_members", proj->define_struct_members.prompt()}});
        
        std::string source = "cpp";
        std::string cache = "implementation.json:definition";
        bool truncated = false;
        inference(cache, implement_function, source, &truncated);
        popMessages(1);
        if(truncated)
        {
            refactorTruncatedSource(source, CodeType::FUNC_IMPL);
        }
        pushMessage(source, "assistant");
        
        //TODO: We must reveiw atleast once to find which m_calls are actually in use
        
        bool cached = true;
        if(!proj->m_cache || cache == "na")
        {
            cached = false;
            int tryToRecover = speculative ? 2 : -1;
            codeReview(source, CodeType::FUNC_IMPL, tryToRecover);
        }
        
        m_implementation.m_source = source;
        //TODO: need to updateDeclaration here is worth investigating
        if(updateDeclaration())
        {
            updateExternals();
        }
        
        popContext();
        
        return cached;
    }

    void CCodeNode::storeUnitTestContent()
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        
        std::string soruceFile = testDir + "/main.cpp";
        
        saveJson(m_unitTest.definition.to_json(), testDir + "/test.json");
        
        if(!m_unitTest.implementation.empty())
        {
            boost_fs::remove(soruceFile);
            saveToFile(m_unitTest.implementation, soruceFile);
        }
        
        for(auto file : m_unitTest.input_files)
        {
            std::string inputFile = testDir + "/" + file->file_name;
            if(!file->content.empty())
            {
                boost_fs::remove(inputFile);
                saveToFile(file->content, inputFile);
            }
        }
    }

    void CCodeNode::pushUnitTestDef()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::stringstream sout;
        
        sout << "UNIT TEST SPECIFICATION:" << std::endl << std::endl;
        
        sout << "```json" << std::endl;
        sout << utility::conversions::to_utf8string(m_unitTest.definition.to_json().serialize());
        sout << "\n```" << std::endl;
        
        std::string summary = sout.str();
        proj->pushMessage(summary, "assistant", true);
    }

    void CCodeNode::generateUnitTestInputFiles()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        Client::getInstance().selectLLM(InferenceIntent::WRITE_TESTS);
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        m_unitTest.input_files.clear();
        
        std::set<std::string> inputFiles = m_unitTest.definition.getInputFiles();
        for(const auto& file : inputFiles)
        {
            captureContext();
            
            boost_fs::path fileName(file);
            std::string source = getSourceType(fileName.extension().string());
            std::string fileContent = proj->define_test_file.prompt({
                {"filename", file},
                {"function", m_brief.func_name},
                {"command", m_unitTest.definition.test.command},
                {"description", ""}, //TODO: Need to remove description from the prompt
                {"srctype", source}
            });
            
            std::string cache = "";
            inference(cache, fileContent, source);
            
            //Review the content of the input file
            bool wasOnAuto = Client::getInstance().run();
            std::string reviewFileContent = proj->review_test_file.prompt({
                {"filename", m_brief.func_name},
                {"function", m_brief.func_name}
            });
            
            bool selfReviewPass = inference(cache, reviewFileContent, true);

            if(!wasOnAuto) Client::getInstance().stop();
            
            if(!selfReviewPass)
            {
                source = getSourceType(fileName.extension().string());
                inference(cache, "Do your best effort to fix all that and revise the content!", source);
            }
            
            File inputFile;
            inputFile.content = source;
            inputFile.file_name = fileName.filename().string();
            m_unitTest.input_files.push_back(std::make_shared<File>(inputFile));
            
            //Store the file
            boost_fs::create_directories(testDir);
            std::string inputFilePath = testDir + "/" + file;
            
            std::ofstream ofsInputFile(inputFilePath);
            ofsInputFile << source;
            ofsInputFile.close();
            
            popContext();
            
            //TODO: Do I need to leave file content in the context?!?!
        }
    }

    bool CCodeNode::reviewUnitTest(const std::string& compileCL, const std::string& feedback, std::string& output)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        Client::getInstance().selectLLM(InferenceIntent::WRITE_TESTS);
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        
        std::string reviewMessge = proj->review_test_source.prompt({
            {"test", m_brief.func_name},
            {"command", compileCL},
            {"output", output},
            {"feedback", feedback}
        });
        
        std::string source = "cpp";
        std::string cache = "";
        inference(cache, reviewMessge, source);
        
        
        std::string headers = getUnitTestHeaders();
        //Save source
        std::string cppFile = testDir + "/main.cpp";
        boost_fs::create_directories(testDir);
        //boost_fs::create_directories(buildDir + "/" + testDir);
        //std::string cppFile = buildDir + "/" + testDir + "/main.cpp";
        
        /*std::ofstream cpp(cppFile);
        cpp << headers;
        cpp << source;
        cpp.close();*/
        
        m_unitTest.implementation = headers + source;
        std::ofstream cpp(cppFile);
        cpp << headers;
        cpp << source;
        cpp.close();
        
        //Recompile the updated sources
        output = exec(compileCL, testDir, "CompileTest");
        
        return unitTestObjectExists();
    }

    std::string CCodeNode::getUnitTestHeaders()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        //std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string nodeDir = buildSourcePath;
        std::string testDir = nodeDir + "/test";
        
        std::stringstream sout;
        
        sout << "//INCLUDE SECTION MANAGED BY THE BUILD SYSTEM START\n";
        sout << "//All files here are also created by the build system\n";
        sout << "//Do not edit this section\n";
        sout << "#include \"common.h\"" << std::endl;
        sout << "#include \"data_defs.h\"" << std::endl;
        
        sout << std::endl;
        
        std::string thisInclude = nodeDir + "/" + m_brief.func_name;
        sout << "#include \"" << thisInclude << ".h\"" << std::endl;
        sout << "//INCLUDE SECTION MANAGED BY THE BUILD SYSTEM END\n\n\n";
        
        return sout.str();
    }

    void CCodeNode::generateUnitTestSource()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        //std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string nodeDir = buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        std::string otherNotes;
        std::string stdoutChecks = m_unitTest.definition.checksStdout();
        if(!stdoutChecks.empty())
        {
            otherNotes += "- Some commands in this test use ECMAScript regex to FULLY match stdout. ";
            otherNotes += "Use std::cout with the << operator (avoid other methods) for output that must match these patterns. ";
            otherNotes += "Use PRINT_TEST for diagnostic output during debugging—it does not appear in stdout.\n";
        }
        
        std::string regexContractJson;
        if(m_unitTest.definition.hasRegexChecks())
        {
            regexContractJson += "\n\nTEST REGEX CONTRACT DEFINITION\n";
            regexContractJson += "\n```json\n";
            regexContractJson += utility::conversions::to_utf8string(m_unitTest.regex_contract.to_json().serialize());
            regexContractJson += "\n```\n\n";
            regexContractJson += "Sometimes the regex patterns from the contract could be different from the patterns from the test. ";
            regexContractJson += "If so, use the patterns from the contract as they have been tested to fully match the provided examples in the contract\n\n";
        }
        
        std::set<std::string> referencedNodes;
        std::string dataDef = getDataTypes(false, referencedNodes);
        std::string implement_test = proj->implement_test.prompt({
            {"function", m_brief.func_name},
            {"other_notes", otherNotes},
            {"regex_contract", regexContractJson},
        });
        
        std::string source = "cpp";
        std::string cache = "";
        m_unitTest.implementation.clear();
        inference(cache, implement_test, source);
        
        //Does a code review makes sense here, we have compile implement-compile loop?
        
        std::string headers = getUnitTestHeaders();
        m_unitTest.implementation = headers + source;
        
        boost_fs::create_directories(buildDir + "/" + testDir);
        std::string cppFile = buildDir + "/" + testDir + "/main.cpp";
        
        std::ofstream cpp(cppFile);
        cpp << headers;
        cpp << source;
        cpp.close();
    }

    bool CCodeNode::unitTestExists()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        //It is great to have mechanism to avoid unnecessary inferencing
        //but I'm not convinced this is the right one
        std::string testExecutable = testDir + "/main";
        return boost_fs::exists(testExecutable);
    }

    bool CCodeNode::unitTestObjectExists()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        //It is great to have mechanism to avoid unnecessary inferencing
        //but I'm not convinced this is the right one
        std::string testExecutable = testDir + "/main.o";
        return boost_fs::exists(testExecutable);
    }

    // -----------------------------
    // Small CXString helper
    // -----------------------------
    static std::string cxToString(CXString s) {
        const char* c = clang_getCString(s);
        std::string out = c ? c : "";
        clang_disposeString(s);
        return out;
    }

    static bool isInMainFile(CXCursor c) {
        CXSourceLocation loc = clang_getCursorLocation(c);
        return clang_Location_isFromMainFile(loc) != 0;
    }

    // Build a scope prefix: ns1::Class::Inner::
    static std::string scopePrefix(CXCursor c) {
        std::vector<std::string> scopes;
        CXCursor p = clang_getCursorSemanticParent(c);
        while (!clang_Cursor_isNull(p) && clang_getCursorKind(p) != CXCursor_TranslationUnit) {
            CXCursorKind pk = clang_getCursorKind(p);

            if (pk == CXCursor_Namespace ||
                pk == CXCursor_StructDecl || pk == CXCursor_ClassDecl || pk == CXCursor_UnionDecl ||
                pk == CXCursor_EnumDecl ||
                pk == CXCursor_ClassTemplate) {

                std::string n = cxToString(clang_getCursorSpelling(p));
                if (!n.empty()) scopes.push_back(n);
            }

            p = clang_getCursorSemanticParent(p);
        }

        std::reverse(scopes.begin(), scopes.end());

        std::string out;
        for (auto& s : scopes) {
            out += s;
            out += "::";
        }
        return out;
    }

    // Qualified name (types: spelling; functions: display name for overload signature)
    static std::string qualifiedName(CXCursor c, bool preferDisplayName) {
        std::string leaf = preferDisplayName
            ? cxToString(clang_getCursorDisplayName(c))
            : std::string();

        if (leaf.empty())
            leaf = cxToString(clang_getCursorSpelling(c));
        if (leaf.empty())
            return {};

        return scopePrefix(c) + leaf;
    }

    static std::string leafName(CXCursor c) {
        return cxToString(clang_getCursorSpelling(c));
    }

    static bool hasRealSpellingLocation(CXCursor c) {
        CXSourceLocation loc = clang_getCursorLocation(c);
        CXFile file = nullptr;
        unsigned line = 0, col = 0, off = 0;
        clang_getExpansionLocation(loc, &file, &line, &col, &off);
        return (file != nullptr && line != 0);
    }

    // -----------------------------
    // Visitor context
    // -----------------------------
    struct VisitorCtx {
        CCodeNode::SourceSymbols* out = nullptr;

        // Dedup by USR so overloads/templates/redecls don't spam you.
        std::unordered_set<std::string> seenUSR;
    };

    static CXChildVisitResult symbolVisitor(CXCursor c, CXCursor /*parent*/, CXClientData clientData) {
        auto* ctx = static_cast<VisitorCtx*>(clientData);

        // Only symbols originating from this .cpp (not headers)
        if (!isInMainFile(c))
            return CXChildVisit_Recurse;

        // Skip implicit compiler-generated stuff
        if (!hasRealSpellingLocation(c))
            return CXChildVisit_Recurse;

        const CXCursorKind k = clang_getCursorKind(c);
        
        std::cout << "DEBUG: " << getCursorName(c) << " kind=" << clang_getCursorKind(c) << std::endl;

        auto addUnique = [&](bool isFunc, const std::string& name) {
            if (name.empty()) return;

            std::string usr = cxToString(clang_getCursorUSR(c));
            if (!usr.empty()) {
                if (!ctx->seenUSR.insert(usr).second) return; // already have it
            }
            if (isFunc) ctx->out->functions.push_back(name);
            else        ctx->out->types.push_back(name);
        };

        // ---- Functions (declared or defined in this file) ----
        switch (k) {
            case CXCursor_FunctionDecl:
            case CXCursor_CXXMethod:
            case CXCursor_Constructor:
            case CXCursor_Destructor:
            case CXCursor_ConversionFunction:
            case CXCursor_FunctionTemplate: {
                addUnique(true, leafName(c));
                break;
            }
            default: break;
        }

        // ---- Types defined/declared in this file ----
        switch (k) {
            case CXCursor_StructDecl:
            case CXCursor_ClassDecl:
            case CXCursor_UnionDecl:
            case CXCursor_EnumDecl:
            case CXCursor_ClassTemplate: {
                // Skip anonymous types (e.g., anonymous structs)
                std::string n = leafName(c);
                if (!n.empty()) addUnique(false, n);
                break;
            }
            default: break;
        }

        return CXChildVisit_Recurse;
    }

    static const char* cxErrorToStr(CXErrorCode e) {
        switch (e) {
            case CXError_Success: return "CXError_Success";
            case CXError_Failure: return "CXError_Failure";
            case CXError_Crashed: return "CXError_Crashed";
            case CXError_InvalidArguments: return "CXError_InvalidArguments";
            case CXError_ASTReadError: return "CXError_ASTReadError";
            default: return "CXError_Unknown";
        }
    }

    static void printTUDiagnostics(CXTranslationUnit tu, const char* headerTag = "libclang") {
        if (!tu) {
            std::cout << "[" << headerTag << "] No translation unit; cannot print diagnostics.\n";
            return;
        }

        unsigned n = clang_getNumDiagnostics(tu);
        if (n == 0) {
            std::cout << "[" << headerTag << "] No diagnostics.\n";
            return;
        }

        std::cout << "[" << headerTag << "] Diagnostics (" << n << "):\n";
        for (unsigned i = 0; i < n; ++i) {
            CXDiagnostic d = clang_getDiagnostic(tu, i);

            // Format includes file:line:col + severity + message
            CXString formatted = clang_formatDiagnostic(
                d, clang_defaultDiagnosticDisplayOptions()
            );

            std::cout << "  " << cxToString(formatted) << "\n";

            clang_disposeDiagnostic(d);
        }
    }

    static void printClangArgs(const std::vector<std::string>& args, const char* headerTag = "libclang") {
        std::cout << "[" << headerTag << "] clang args (" << args.size() << "):\n";
        for (const auto& a : args) std::cout << "  " << a << "\n";
    }

    // -----------------------------
    // Main API: parse + collect
    // -----------------------------
    CCodeNode::SourceSymbols CCodeNode::extractSymbolsFromSource(const CCodeNode::CompilationInfo& ci) {
        SourceSymbols out;

        // Convert args to const char*
        std::vector<const char*> cargs;
        cargs.reserve(ci.clangArgs.size());
        for (auto& a : ci.clangArgs) cargs.push_back(a.c_str());

        CXIndex index = clang_createIndex(/*excludeDeclsFromPCH*/0, /*displayDiagnostics*/0);

        CXTranslationUnit tu = nullptr;
        CXErrorCode err = clang_parseTranslationUnit2(
            index,
            ci.sourceFile.c_str(),
            cargs.empty() ? nullptr : cargs.data(),
            (int)cargs.size(),
            nullptr, 0,
            // skip bodies = faster; still finds decls/defs
            CXTranslationUnit_SkipFunctionBodies,
            &tu
        );

        if (err != CXError_Success || !tu) {
            std::cout << "[libclang] Failed to parse translation unit.\n";
            std::cout << "[libclang] Source: " << ci.sourceFile << "\n";
            std::cout << "[libclang] Error: " << cxErrorToStr(err) << " (" << (int)err << ")\n";
            printClangArgs(ci.clangArgs);

            // Sometimes tu is null; sometimes it exists but has errors.
            printTUDiagnostics(tu);

            if (tu) clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return out;
        }

        VisitorCtx ctx;
        ctx.out = &out;

        CXCursor root = clang_getTranslationUnitCursor(tu);
        clang_visitChildren(root, symbolVisitor, &ctx);

        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);

        return out;
    }

    CCodeNode::CompilationInfo CCodeNode::getCompilationInfoForSymbols(const std::string& platform, uint32_t options) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();

        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;

        // source file (exactly like your compileCommand)
        std::string srcFile = nodeDir + "/";
        if (options & BUILD_UNIT_TEST) srcFile += "test/main.cpp";
        else srcFile += m_brief.func_name + ".cpp";

        CompilationInfo ci;
        ci.sourceFile = srcFile;
        
        std::string sysroot = hen::getSysRoot();
        std::string resourceDir = hen::getClangResourceDir();
        std::string cxxInclude  = hen::getCppInclude();
        std::string cxxIncludeOpt = "-I" + cxxInclude;
         
         /*const char* clang_args[] = {
             "-x", "c++",
             "-std=c++17",
             "-stdlib=libc++",
             "-DCOMPILE_TEST",
             "-Werror=format",
             "-D_LIBCPP_HAS_NO_WIDE_CHARACTERS",//Without this we get "couldn't find stdarg.h" error
             "-isysroot", sysroot.c_str(),
             "-resource-dir", resourceDir.c_str(), // ← critical for stdarg.h, stdint.h, intrinsics, etc.
             cxxIncludeOpt.c_str(), // ← libc++ headers
         */

        auto& a = ci.clangArgs;
        a = {
            "-x", "c++",
            "-std=c++17",
            "-arch", "arm64",
            "-Werror=format",
            "-fno-diagnostics-show-note-include-stack",
            "-c",
            "-isysroot", sysroot.c_str(),
            "-resource-dir", resourceDir.c_str(), // ← critical for stdarg.h, stdint.h, intrinsics, etc.
            cxxIncludeOpt.c_str(), // ← libc++ headers
            "-I" + buildDir,
            "-I" + (buildDir + "/" + buildSourcePath),
        };

        if (options & BUILD_DEBUG) {
            a.push_back("-fsanitize=address,undefined");
            a.push_back("-fno-sanitize-recover=undefined");
            a.push_back("-fno-omit-frame-pointer");
            a.push_back("-g");
            a.push_back("-O0");
            a.push_back("-fno-inline-functions");
            a.push_back("-fno-optimize-sibling-calls");
        }

        std::string pchFile = buildDir + "/common.pch";
        /*if (boost_fs::exists(pchFile)) {
            a.push_back("-include-pch");
            a.push_back(pchFile);
        } else*/
        {
            a.push_back("-include");
            a.push_back("common.h");
        }

        if (options & BUILD_PRINT_TEST) {
            a.push_back("-DCOMPILE_TEST");
        }

        return ci;
    }

    std::string CCodeNode::validateUnitTestSource()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string feedback;
        std::string usedFunctions;
        std::string usedTypes;
        
        CompilationInfo ci = getCompilationInfoForSymbols(
                                  getPlatform(), BUILD_PRINT_TEST|BUILD_UNIT_TEST|BUILD_DEBUG);
        
        SourceSymbols symbols = CCodeNode::extractSymbolsFromSource(ci);
        
        for(auto func : symbols.functions)
        {
            if(func == "main") continue;
            
            if(proj->nodeMap().find(func) != proj->nodeMap().end())
            {
                usedFunctions += func + " ";
            }
        }
        
        for(auto type : symbols.types)
        {
            std::string owningPath;
            if(proj->findData(type, owningPath))
            {
                usedTypes += type + " ";
            }
        }
        
        if(!usedFunctions.empty() || !usedTypes.empty())
        {
            feedback += "Here is also a feedback on the usage of application defined functions and types in the test\n\n";
        }
        
        if(!usedFunctions.empty())
        {
            feedback += "The test declares or defines functions that are already defined in the application source:\n";
            feedback += usedFunctions + "\n";
            feedback += "Remove the above function declarations/definitions from the test.\n";
        }
        
        if(!usedTypes.empty())
        {
            feedback += "The test declares or defines data types that are already defined in the application source:\n";
            feedback += usedFunctions + "\n";
            feedback += "Remove the above function data declarations/definitions from the test.\n";
        }
        
        if(!usedFunctions.empty() || !usedTypes.empty())
        {
            feedback += "It is not allowed to declare or define functions and data types that are already defined in the application\n\n";
        }
        
        return feedback;
    }

    bool CCodeNode::validateUnitTestRegexContract()
    {
        if(!m_unitTest.definition.hasRegexChecks())
        {
            return false;
        }
        
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        Prompt contract("RegexContractDefinition.txt", {
            //TODO: bind strings here
        });
        
        std::string cache = "";
        
        std::string prompt = contract.str();
        
        captureContext();
        
        captureContext();
        inference(cache, prompt, &m_unitTest.regex_contract);
        popContext();
        
        pushMessage(prompt, "user");
        
        prompt = m_unitTest.regex_contract.verify();
        prompt += m_unitTest.definition.validate(m_unitTest.regex_contract);
        
        uint32_t attempts = 0;
        while(!prompt.empty() && attempts < 5)
        {
            captureContext();
            
            std::string regexContractJson = "```json\n";
            regexContractJson += utility::conversions::to_utf8string(m_unitTest.regex_contract.to_json().serialize());
            regexContractJson += "\n```\n";
            pushMessage(regexContractJson, "assistant");
         
            prompt += "\n\nProvide updated version of the regex contract addressing the mentioned problems!\n\n";
            inference(cache, prompt, &m_unitTest.regex_contract);
            
            auto testJson = m_unitTest.definition.to_json();
            prompt = m_unitTest.regex_contract.verify();
            prompt += m_unitTest.definition.validate(m_unitTest.regex_contract);
            
            popContext();
            
            attempts++;
        }
        
        popContext();
        
        m_unitTest.definition.swapInvalid(m_unitTest.regex_contract);
        
        return true;
    }

    bool CCodeNode::compileUnitTestSource()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        std::string compileCL = compileCommand(platform, BUILD_PRINT_TEST|BUILD_UNIT_TEST|BUILD_DEBUG);
    
        std::string output = exec(compileCL, testDir, "CompileTest");
        int compileAttempt = 0;
        
        std::string feedback = validateUnitTestSource();
        
        while ((!unitTestObjectExists() || !feedback.empty())
               && compileAttempt++ < 3)
        {
            //Let the human know!
            std::cout << "Test compilation error: " << output << std::endl;
            std::cout << "Command: " << compileCL << std::endl;
            std::cout << output << std::endl;
        
            reviewUnitTest(compileCL, feedback, output);
            feedback = validateUnitTestSource();
        }
        
        if(compileAttempt >= 3)
        {
            std::cout << "Failed to compile the unit test: " << buildSourcePath << std::endl;
            return false;
        }
        
        return true;
    }

    bool CCodeNode::compileUnitTest()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        Client::getInstance().selectLLM(InferenceIntent::WRITE_TESTS);
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        captureContext();
        
        generateUnitTestSource();
        
        bool result = compileUnitTestSource();

        popContext();
        
        if(!result) return false;
        
        std::stringstream strout;
        strout << "Unit test source for: " << m_brief.func_name << std::endl;
        strout << "```cpp" << std::endl;
        strout << m_unitTest.implementation << std::endl;
        strout << "```" << std::endl;
        
        std::string summary = strout.str();
        proj->pushMessage(summary, "user", true);
        
        return true;
    }

    bool CCodeNode::linkUnitTest(bool enableSanitizer)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        CCodeNode* parent = nullptr;
        if(m_this->m_parent)
        {
            parent = (CCodeNode*)m_this->m_parent->m_data;
            assert(parent);
        }
        
        uint32_t linkOptions = 0;
        if(parent) {
            linkOptions |= BUILD_UNIT_TEST;
        }
        
        if(enableSanitizer)
        {
            linkOptions |= BUILD_DEBUG;
        }
        
        std::string linkCL = linkCommand(platform, linkOptions);
        std::string linkOutput = exec(linkCL, testDir, "LinkTest");
        int linkAttempt = 0;
        
        std::string feedback = validateUnitTestSource();
        while ((!unitTestExists() || !feedback.empty())
               && linkAttempt < 3) {
            //Let human know!
            std::cout << "Test link error: " << buildSourcePath << std::endl;
            std::cout << "Command: " << linkCL << std::endl;
            std::cout << linkOutput << std::endl;
            
            uint32_t flags = enableSanitizer ? BUILD_DEBUG : 0;
            if(parent)
            {
                flags |= BUILD_PRINT_TEST|BUILD_UNIT_TEST;
                std::string compileCL = compileCommand(platform, flags);
                reviewUnitTest(compileCL, feedback, linkOutput);
            }
            else
            {
                Client::getInstance().selectLLM(InferenceIntent::WRITE_TESTS);
                
                flags |= BUILD_PRINT_TEST;
                std::string compileCL = compileCommand(platform, flags);
                
                CompilationReviewType approach = CompilationReviewType::NORMAL;
                
                proj->m_refactoringDepth++;
                reviewCompilation(nullptr, approach, compileCL, linkOutput);
                proj->m_refactoringDepth--;
            }
            
            //Recompile the updated sources
            linkOutput += exec(linkCL, testDir, "LinkTest");
            feedback = validateUnitTestSource();
            
            linkAttempt++;
        }
        
        if(linkAttempt < 3)
        {
            return true;
        }
        
        std::cout << "Unable to link unit test: " << m_unitTest.definition.name << std::endl;
        return false;
    }

    bool CCodeNode::rebuildUnitTest(bool enableSanitizer)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        
        uint32_t flags = enableSanitizer ? BUILD_DEBUG : 0;
        flags |= BUILD_PRINT_TEST|BUILD_UNIT_TEST;
        std::string compileCL = compileCommand(platform, flags);
        exec(compileCL, testDir, "CompileTest");
        
        CCodeNode* parent = nullptr;
        if(m_this->m_parent)
        {
            parent = (CCodeNode*)m_this->m_parent->m_data;
            assert(parent);
        }
        
        uint32_t linkOptions = 0;
        if(parent) {
            linkOptions |= BUILD_UNIT_TEST;
        }
        
        if(enableSanitizer)
        {
            linkOptions |= BUILD_DEBUG;
        }
        
        std::string linkCL = linkCommand(platform, linkOptions);
        std::string linkOutput = exec(linkCL, testDir, "LinkTest");
        
        return unitTestExists();
    }

    void CCodeNode::inferenceUnitTestDef(const std::string& message)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string cache = "";
        
        captureContext();
        inference(cache, message, &m_unitTest.definition);
        popContext();
        
        bool wasOnAuto = Client::getInstance().run();
        
        //Self-review the produced unit test definition
        std::string reviewTestSelf = proj->review_test_self.prompt({
            {"function", m_brief.func_name}});
        
        int atttempts = 0;
        const int maxAttempts = 5;
        
        bool testIsValid = false;
        
        pushMessage(message, "user");
        
        while(!testIsValid && atttempts < maxAttempts)
        {
            captureContext();
            
            std::string testDef = utility::conversions::to_utf8string(m_unitTest.definition.to_json().serialize());
            pushMessage(testDef, "assistant");
            
            std::string feedback = reviewTestSelf;
            std::string review = m_unitTest.definition.validate(false);
            if(!review.empty())
            {
                feedback += "\n\nHere are a few other things that may also require attention, but aren't necessarily a problem:\n";
                feedback += review;
            }
            //else //Only consume attempt if the test passes validation
            {
                atttempts++;
            }
            
            testIsValid = inference(cache, feedback, true);
            if(!testIsValid)
            {
                feedback = "\nDo your best effort to fix all this and revise the response!";
                inference(cache, feedback, &m_unitTest.definition);
            }
            
            popContext();
        }
        
        std::string testDef = utility::conversions::to_utf8string(m_unitTest.definition.to_json().serialize());
        pushMessage(testDef, "assistant");
        
        if(!wasOnAuto) Client::getInstance().stop();
    }

    void CCodeNode::defineUnitTest2(const std::string& fullTestPath, const std::string& prevFullTestPath, const std::string& recommendation)
    {
        TestDef fullTestDef;
        fullTestDef.load(fullTestPath + "/test.json");
        
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        Client::getInstance().selectLLM(InferenceIntent::WRITE_TESTS);
        
        captureContext();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        CCodeNode* parent = nullptr;
        std::string parentCtx;
        std::set<std::string> referencedNodes;
        if(m_this->m_parent)
        {
            parent = (CCodeNode*)m_this->m_parent->m_data;
            assert(parent);
            
            parentCtx = "\nAlso, this function will be called by the '";
            parentCtx += parent->m_brief.func_name;
            parentCtx += "' and here is more information about that function:\n";
            parentCtx += parent->getContexInfo(true, true, false, referencedNodes);
        }
        
        std::string functionCtx = getContexInfo(true, false, false, referencedNodes);
        
        std::string testFrameworkMan = getFileContent(client.getEnvironmentDir() + "/Prompts/TestFramework.txt");
        
        std::string systemData = proj->getHighLevelAppInfo("main", 3, 3);
        
        systemData += "\n\nSome of the above functions are designated as subsystems and as such ";
        systemData += "require a precise contract defining dataflow, ownership, and initialization\n\n";
        
        std::string fullTest = fullTestDef.getDescription(fullTestPath);
        std::string prevFullTest;
        std::string prevFullTestNote;
        if(!prevFullTestPath.empty())
        {
            TestDef prevFullTestDef;
            prevFullTestDef.load(prevFullTestPath + "/test.json");
            
            prevFullTest += "\n\nPREVIOUS FULL APPLICATION TEST (SUCCEEDED)\n\n";
            prevFullTest += prevFullTestDef.getDescription(prevFullTestPath) + "\n\n";

            prevFullTestNote += "\n\nNote - the previous full test passed. When generating this unit test, ";
            prevFullTestNote += "consider which features—both existing and newly added—are relevant to its scope. ";
            prevFullTestNote += "Among relevant features, prioritize coverage of new additions; ";
            prevFullTestNote += "they are the most likely source of issues, though regressions remain possible.\n\n";
        }
        
        std::string recoNote;
        if(!recommendation.empty())
        {
            recoNote = "\n\nAnalysis form the most recent session debugging the test '" + fullTestDef.name + "' is provided above. ";
            recoNote += "Consult the analysis and recommendations and, if it makes sense, ";
            recoNote += "prioritize unit testing the features currently blocking the full application test from passing.\n";
            recoNote += "The recommendation and analysis might sound more general-purpose or to suggest a specific unsupported workflow ";
            recoNote += "but you need to adapt them to our unit tests specification (see the manual)\n\n";
        }
        
        std::string unitTestName = fullTestDef.name + "_" + getName();
        std::string defineTest = proj->define_test.prompt({
            {"function", m_brief.func_name},
            {"function_ctx", functionCtx},
            {"full_test", fullTest},
            {"prev_full_test", prevFullTest},
            {"prev_full_test_note", prevFullTestNote},
            {"recommendation", recommendation},
            {"recommendation_note", recoNote},
            {"system_data", systemData},
            {"test_framework_manual", testFrameworkMan},
            {"parent_ctx", parentCtx},
            {"test_name", unitTestName}
        });
        
        inferenceUnitTestDef(defineTest);
        
        popContext();
        
        proj->pushMessage(defineTest, "user", true);
    }

    void CCodeNode::implementUnitTest()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        //If we have parent, that means we need to build special unit test executable
        //If there is no parent, that means we are building the 'main' exectuable
        //and we are going only to generate test description and input files
        CCodeNode* parent = nullptr;
        if(m_this->m_parent)
        {
            parent = (CCodeNode*)m_this->m_parent->m_data;
            assert(parent);
        }
        
        std::string pretestLog;
        bool pretestOK = true;
        uint32_t fixPretestAttempts = 0;
        
        std::string defineTestMsg;
        
        do
        {
            captureContext();
            
            pushUnitTestDef();
            
            validateUnitTestRegexContract();
            
            generateUnitTestInputFiles();
            
            if(parent) //parent == nullptr means compiling the main
            {
                if(!compileUnitTest())
                {
                    std::cout << "Unable to compile unit test for function: " << getName() << std::endl << std::endl;
                }
            }
            
            if(m_unitTest.definition.pretest.commands.size() > 0)
            {
                storeUnitTestContent();
                
                pretestOK = Debugger::getInstance().debugPretest(proj, getName(), testDir, pretestLog);
                if(!pretestOK)
                {
                    std::string defineTestMsg = "Execution of the pretest step for unit test '" + m_unitTest.definition.name + "' exits with error:\n\n";
                    defineTestMsg += pretestLog + "\n\n";
                    defineTestMsg += "Consider to fix and redefine the unit test to avoid this! ";
                    defineTestMsg += "Then, if necessary, we can reimplement the test's main.cpp and the input files (if any)\n";
                    defineTestMsg += "Reminder that the test steps (pretest, test, posttest) must not generate, compile and list as input/outut file ";
                    defineTestMsg += "the test driver (main.cpp). The build system will generate and compile that file implicitly.\n";
                    defineTestMsg += "Note: I'm not asking here for the content of the test main.cpp or any of the input files. ";
                    defineTestMsg += "In the test description, you can specify the test, describe the test cases and the required input files ";
                    defineTestMsg += "(if any, excluding the test driver main.cpp) as explained in the test framework manual. ";
                    defineTestMsg += "Then, based on the test description, in a next phase, we will define the content of the main.cpp and any input files\n\n";
                    
                    
                    fixPretestAttempts++;
                    
                    captureContext();
                    inferenceUnitTestDef(defineTestMsg);
                    popContext();
                }
            }
            else
            {
                pretestOK = true;
            }
            
            popContext();
        }
        while(!pretestOK && fixPretestAttempts < 5);
        
        if(fixPretestAttempts >= 5)
        {
            std::cout << "Unable to verify the pretest step for the unit test for function: " << getName() << std::endl << std::endl;
        }
        
        pushUnitTestDef();
        
        linkUnitTest(true);
    }

    void CCodeNode::summarizeUnitTestDesc()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        captureContext();
        
        std::string unitTestDesc = "review";
        bool truncated = false;
        
        std::string message = "\nUNIT TEST\n\n";
        message += m_unitTest.getDescription();
        message += "Summarize the unit test description in one or two sentences. ";
        message += "Provide the summary formated as a plain text. No .json or other formatting\n";
        
        Cache cache;
        proj->inference(cache, message, unitTestDesc, &truncated);
        
        //TODO: Probably we need verification loop for the size of the summary
        m_unitTest.definition.description = unitTestDesc;
        
        popContext();
    }

    void CCodeNode::buildUnitTest(const std::string& fullTestPath, const std::string& prevFullTestPath, const std::string& recommendation)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        
        //If we have parent, that means we need to build special unit test executable
        //If there is no parent, that means we are building the 'main' exectuable
        //and we are going only to generate test description and input files
        CCodeNode* parent = nullptr;
        if(m_this->m_parent)
        {
            parent = (CCodeNode*)m_this->m_parent->m_data;
            assert(parent);
        }
        
        if(unitTestExists())
            return;
        
        proj->generateDataHeader();

        captureContext();
        
        boost_fs::create_directories(testDir);
        
        defineUnitTest2(fullTestPath, prevFullTestPath, recommendation);
        
#if 1
        implementUnitTest();
        popContext();
#else
        std::string pretestLog;
        bool pretestOK = true;
        uint32_t fixPretestAttempts = 0;
        do
        {
            generateUnitTestInputFiles();
            
            if(parent) //parent == nullptr means compiling the main
            {
                if(!compileUnitTest())
                {
                    popContext(); //Pop the last unit test definition
                    popContext(); //Pop back to "Start unit test definition"
                    
                    {
                        std::cout << "Unable to compile unit test for function: " << getName() << std::endl << std::endl;
                        return;
                    }
                }
            }
            
            if(m_unitTest.definition.pretest.commands.size() > 0)
            {
                storeUnitTestContent();
                
                pretestOK = Debugger::getInstance().debugPretest(proj, getName(), testDir, pretestLog);
                if(!pretestOK)
                {
                    std::string defineTestMsg = "Execution of the pretest step for unit test '" + m_unitTest.definition.name + "' exits with error:\n\n";
                    defineTestMsg += pretestLog + "\n\n";
                    defineTestMsg += "Consider to fix and redefine the unit test to avoid this! ";
                    defineTestMsg += "Then, if necessary, we can reimplement the test's main.cpp and the input files (if any)\n";
                    defineTestMsg += "Reminder that the test steps (pretest, test, posttest) must not generate, compile and list as input/outut file ";
                    defineTestMsg += "the test driver (main.cpp). The build system will generate and compile that file implicitly.\n";
                    defineTestMsg += "Note: I'm not asking here for the content of the test main.cpp or any of the input files. ";
                    defineTestMsg += "In the test description, you can specify the test, describe the test cases and the required input files ";
                    defineTestMsg += "(if any, excluding the test driver main.cpp) as explained in the test framework manual. ";
                    defineTestMsg += "Then, based on the test description, in a next phase, we will define the content of the main.cpp and any input files\n\n";
                    
                    inferenceUnitTestDef(defineTestMsg);
                    fixPretestAttempts++;
                    
                    //==========================
                    popContext();//Pop the last unit test definition
                    captureContext();
                    pushUnitTestDef();
                }
            }
        }
        while(!pretestOK && fixPretestAttempts < 5);
        
        if(fixPretestAttempts >= 5)
        {
            std::cout << "Unable to verify the pretest step for the unit test for function: " << getName() << std::endl << std::endl;
            return;
        }
        
        linkUnitTest(true);
        
        popContext(); //Pop the last unit test definition
        popContext(); //Pop back to "Start unit test definition"
#endif
        summarizeUnitTestDesc();
        
        saveJson(m_unitTest.definition.to_json(), testDir + "/test.json");
        
        save();
    }

    bool CCodeNode::reviewTestResult(const std::string& testCL, const std::string& output)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::string dagPath = getDAGPath("/");
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        std::string binDir = buildDir + "/" + platform;
        
        captureContext();
        
        std::set<std::string> referencedNodes;
        std::string functionCtx = getContexInfo(true, true, false, referencedNodes);
        std::string reviewTestResult = proj->review_test_result.prompt({
            {"test", dagPath},
            {"function", m_brief.func_name},
            {"command", testCL},
            {"output", output},
            {"test_src", m_unitTest.implementation},
            {"function_ctx", functionCtx},
        });
        
        std::string cache = "";

        bool fixFunction = inference(cache, reviewTestResult, true);
        
        if(!fixFunction)
        {
            std::ofstream note(testDir + "/TestNote.txt");
            //TODO: What to log here ??
            //note << response;
            note.close();
            popContext();
            return false;
        }
        else
        {
            std::string implementation = "```cpp\n";
            implementation += m_implementation.m_source;
            implementation += "\n```";
            
            std::string checklist = proj->source_checklist.prompt({{"function", m_prototype.declaration}});
            std::string fixSource = proj->fix_source_after_test.prompt({
                {"function", m_brief.func_name},
                {"implementation", implementation},
                {"test", dagPath},
                {"checklist", checklist}
            });
            
            std::string source = "cpp";
            inference(cache, fixSource, source);
            
            m_implementation.m_source = source;
            if(updateDeclaration())
            {
                updateExternals();
            }
            
            std::string objFile = binDir + "/" + m_brief.func_name + ".o";
            if(boost_fs::exists(objFile)) {
                boost_fs::remove(objFile);
            }
            
            compile();
            
            bool result = compileUnitTestSource();
            
            if(result)
            {
                std::string testExecutable = testDir + "/" + m_brief.func_name;
                if(boost_fs::exists(testExecutable)) {
                    boost_fs::remove(testExecutable);
                }
                
                result = linkUnitTest(true);
            }
            
            if(!result)
            {
                popContext();
                return false;
            }
        }
        
        popContext();
        return true;
    }

    void CCodeNode::deleteUnitTest()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        
        boost_fs::remove_all(testDir);
        
        m_unitTest.clear();
    }

    void CCodeNode::test()
    {
        //TODO: Get the default test here
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string fullTestPath = proj->getProjDir() + "/tests/default/public";
        buildUnitTest(fullTestPath, std::string(), std::string());
        
        std::string buildSourcePath = getNodeBuildSourcePath();
        std::string buildDir = proj->getProjDir() + "/build";
        std::string nodeDir = buildDir + "/" + buildSourcePath;
        std::string testDir = nodeDir + "/test";
        std::string platform = getPlatform() + "_test";
        std::string binDir = buildDir + "/" + platform;
        
        std::cout << "Running unit test for: " << buildSourcePath << std::endl;
        
        int attempt = 0;
        bool failure = false;
        do {
            std::string testCL = m_unitTest.definition.test.command;
            if(testCL == "none" || testCL == m_brief.func_name || testCL.empty())
            {
                testCL = "./" + m_brief.func_name;
            }
            else if(testCL.substr(0,2) != "./")
            {
                std::string newCL = "./";
                newCL += testCL;
                testCL = newCL;
            }
            
            //TODO: Here execute with the debugger
            std::string output = exec(testCL, testDir, "UnitTest");
            
            failure = isAFailure(output);
            
            if(failure)
            {
                std::cout << "Test result for: " << m_brief.func_name << std::endl;
                std::cout << "Command line: " << testCL << std::endl;
                std::cout << output << std::endl;
                
                reviewTestResult(testCL, output);
            }
            
        } while(failure && attempt++ < 3);
        
        if(proj->m_save)
        {
            save();
        }
    }

    void CCodeNode::integrate()
    {
        std::cout << "Integrating: " << m_prototype.declaration << std::endl;
        std::cout << "Path: " << getDAGPath(">") << std::endl;
    }

    void CCodeNode::save()
    {
        if(!m_defined) return;
        
        std::string directory = getNodeDirectory();
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        try {
            boost_fs::create_directories(directory);
        }
        catch (const boost_fs::filesystem_error& e) {
            std::cout << "Unable to create directory " << directory << std::endl;
            std::cout << "Skipping further processing of this node" << std::endl;
            return;
        }

        saveJson(m_brief.to_json(), directory + "/brief.json");
        saveJson(m_prototype.to_json(), directory + "/prototype.json");
        saveJson(m_description.to_json(), directory + "/description.json");
        web::json::value callsJson = m_calls.to_json();
        callsJson[U("order")] = json::value::string(utility::conversions::to_string_t(m_calls.m_order));
        saveJson(callsJson, directory + "/calls.json");
        saveJson(m_libCalls.to_json(), directory + "/lib_calls.json");
        saveJson(m_implementation.to_json(), directory + "/implementation.json");
        saveToFile(m_implementation.m_source, directory + "/" + m_brief.func_name + ".cpp");

        if(m_dataDef.is_object() && m_dataDef.as_object().size() > 0)
        {
            saveJson(m_dataDef, directory + "/data_def.json");
        }
        
        saveJson(m_unitTest.to_json(), directory + "/test.json");
        
        proj->saveReferences();
        proj->saveDataDefinitions();
        
        //TODO: Save node stats
        proj->saveStats();
        Client::getInstance().flushLog();
    }

    void CCodeNode::load()
    {
        std::string directory = getNodeDirectory();
        
        if(!boost_fs::exists(directory))
        {
            std::cout << "Directory for node '" << directory << "' doesn't exist" << std::endl;
            return;
        }
        
        if(m_defined)
        {
            std::cout << "Node '" << directory << "' is already defined" << std::endl;
            return;
        }
        
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        bool missingFiles = false;
        if(!loadFromJson(directory + "/brief.json", &m_brief)) {
            std::cout << "Unable to open file: " << directory << "/brief.json" << std::endl;
            missingFiles = true;
        }
        if(!loadFromJson(directory + "/prototype.json", &m_prototype)) {
            std::cout << "Unable to open file: " << directory << "/prototype.json" << std::endl;
            missingFiles = true;
        }
        if(!loadFromJson(directory + "/description.json", &m_description)) {
            std::cout << "Unable to open file: " << directory << "/description.json" << std::endl;
            missingFiles = true;
        }
        m_prototype.description = m_description.description;
        m_prototype.brief = m_description.brief;
        m_prototype.m_signature = parseFunctionSignature(m_prototype.declaration);
        
        if(!loadFromJson(directory + "/calls.json", &m_calls)) {
            std::cout << "Unable to open file: " << directory << "/calls.json" << std::endl;
            missingFiles = true;
        }
        if(!loadFromJson(directory + "/lib_calls.json", &m_libCalls)) {
            std::cout << "Unable to open file: " << directory << "/lib_calls.json" << std::endl;
            missingFiles = true;
        }
        if(!loadFromJson(directory + "/implementation.json", &m_implementation)) {
            std::cout << "Unable to open file: " << directory << "/implementation.json" << std::endl;
            missingFiles = true;
        }
        
        m_implementation.m_source = getFileContent(directory + "/" + m_brief.func_name + ".cpp");
        if(m_implementation.m_source.empty())
        {
            std::cout << "Unable to open file: " << directory << "/" << m_brief.func_name << ".cpp" << std::endl;
            missingFiles = true;
        }
        
        bool hasData = false;
        if(boost_fs::exists(directory + "/data_def.json"))
        {
            if(!loadJson(m_dataDef, directory + "/data_def.json")) {
                std::cout << "Unable to open file: " << directory << "/data_def.json" << std::endl;
                missingFiles = true;
            }
            hasData = true;
        }
        
        loadFromJson(directory + "/test.json", &m_unitTest);
        
        if(!loadOrder())
        {
            std::cout << "Unable to load calls order for node: " << directory << std::endl;
        }
        
        std::string childDirectory = directory + "/dag/";
        
        if(missingFiles)
        {
            if(boost_fs::exists(childDirectory))
            {
                std::cout << "Critical error: node '" << directory << "' couldn't load specific .json files but has child nodes!";
            }
        }
        
        bool defined = isDefined();
        if(!defined)
        {
            std::cout << "Node is not defined: " << directory << std::endl;
            if(boost_fs::exists(childDirectory))
            {
                std::cout << "Critical error: node '" << directory << "' is not fully defined but has child nodes!";
            }
            
            return;
        }
        
        //At this point we assume the node is fully defined all child nodes will be created,
        //then fully defined children will skip decomposition
        m_calls.applyOrder();
        for(auto call : m_calls.items)
        {
            std::string callDirectory = childDirectory;
            callDirectory += call->func_name;
            
            auto callNameIt = proj->m_tempGraph.find(call->func_name);
            bool owner = false;
            if(callNameIt != proj->m_tempGraph.end())
            {
                std::string path = getDAGPath("/");
                path += "/" + call->func_name;
                
                if(callNameIt->second == path) {
                    owner = true;
                }
            }
            else
            {
                std::cout << "Unable to find node in the saved graph: " << callDirectory << std::endl;
            }
            
            if(owner)
            {
                if(!boost_fs::exists(callDirectory))
                {
                    //If the node stays defined and we skip child creation the missing child
                    //will neve be created since the node will skip the decomposition pass
                    
                    std::cout << "Missing directory for owned child node: " << callDirectory << std::endl;
                }
                
                CCodeNode* child = proj->shareNode<CCodeNode>(call->func_name, this);
                if(child->m_brief.func_name.empty())
                {
                    assert(child->refCount()==1);
                    child->m_brief = *call;
                }
            }
            else
            {
                if(proj->nodeMap().find(call->func_name) != proj->nodeMap().end())
                {
                    CCodeNode* child = proj->shareNode<CCodeNode>(call->func_name, this);
                    if(child->refCount()==1) //We are re-referencing the node
                    {
                        child->m_brief = *call;
                    }
                }
                else
                {
                    std::cout << "Node: " << directory << std::endl;
                    std::cout << "Unable to reference child: " << call->func_name << std::endl << std::endl;
                }
            }
        }
        
        if(defined)
        {
            //TODO: This needs serious testing!
            //Let's try to load other child nodes that were created by this one but currently not referenced
            //However, they might be referenced from other places
            boost_fs::path dir(childDirectory);

            if (boost_fs::exists(dir) && boost_fs::is_directory(dir))
            {
                for (boost_fs::directory_iterator it(dir), end; it != end; ++it)
                {
                    if (boost_fs::is_directory(*it))
                    {
                        std::string func_name = it->path().filename().string();
                        auto itNode = proj->nodeMap().find(func_name);
                        
                        //We create only nodes that aren't created yet
                        if(// !callsFunction(func_name) &&
                           itNode == proj->nodeMap().end())
                        {
                            assert(!callsFunction(func_name));
                            CCodeNode* child = proj->shareNode<CCodeNode>(func_name, this);
                            if(child->m_brief.func_name.empty())
                            {
                                assert(child->refCount()==1);
                                FunctionItem call;
                                call.func_name = func_name;
                                call.brief = "Temporary";
                                child->m_brief = call;
                                
                                //OK, now remove reference since this node is not called directly
                                //and wait others to reference it
                                child->releaseReference(this);
                            }
                        }
                    }
                }
            }
        }
        
        m_defined = defined;
    }

    void CCodeNode::onDelete()
    {
        for(auto child : m_this->m_children)
        {
            CCodeNode* ccNode = (CCodeNode*)child->m_data;
            ccNode->releaseReference(this);
        }
    }

    std::string CCodeNode::summarizeCalls(bool brief, bool path, bool decl) const
    {
        std::string summary;
        //Get detailed definition for the function
        if(m_calls.items.size())
        {
            CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
            
            summary += "To fully implement its functionality '";
            summary += m_brief.func_name + "' function calls the following functions:\n";
            
            for(auto func : m_calls.items)
            {
                std::string declaration;
                if(decl)
                {
                    auto it = proj->nodeMap().find(func->func_name);
                    if(it != proj->nodeMap().end())
                    {
                        auto ccNode = (const CCodeNode*)it->second;
                        if(ccNode->m_defined)
                        {
                            declaration = ccNode->m_prototype.declaration;
                        }
                    }
                }
                
                if(!declaration.empty())
                {
                    summary += "   Function declaration: ";
                    summary += declaration;
                }
                else
                {
                    summary += "   Function name: ";
                    summary += func->func_name;
                }
                
                if(brief)
                {
                    summary += "\n   Brief description for ";
                    summary += func->func_name;
                    summary += ": ";
                    summary += func->brief;
                }
                
                if(path)
                {
                    summary += "\n   Definition path for ";
                    summary += func->func_name;
                    //TODO: This now represent the call stack, but what we really want is the definiton path if the node is fully defined
                    summary += ": ";
                    summary += getDAGPath("/") + "/" + func->func_name;
                }
                
                summary += "\n";
            }
        }
        
        return summary;
    }

    void CCodeNode::updateExternals()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::set<std::string> appTypes = proj->getAppTypesFromDecl(m_prototype.declaration);
        m_implementation.externals.clear();
        for(auto type : appTypes)
        {
            std::string owningPath;
            if(proj->findData(type, owningPath))
            {
                if(owningPath != "__DETACHED__")
                {
                    addToSet(m_implementation.externals, owningPath);
                }
            }
        }
    }

    std::string CCodeNode::summarize(bool brief) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::string summary = "Function description: ";
        if(brief)
        {
            //TODO: This is not well tested!
            if(!m_prototype.brief.empty())
            {
                summary += m_prototype.brief;
            }
            else
            {
                summary += m_brief.brief;
            }
        }
        else
        {
            summary += m_prototype.description;
        }
        bool hasImplementation = m_defined && m_implementation.m_source.size();
        if(brief || !hasImplementation)
        {
            summary += "\nFunction declaration: ";
            summary += m_prototype.declaration;
            summary += "\n";
        }
        
        if(!brief)
        {
            if(hasImplementation)//Implementation is ready
            {
                summary += "\nFunction implementation: ";
                summary += m_implementation.m_source;
                summary += "\n";
            }
            else if(m_calls.items.size())
            {
                summary += summarizeCalls(true, false, false);
                summary += "\nWe are going to get detailed definition for each one of these functions.\n";
            }
        }
        
        return summary;
    }

    bool CCodeNode::hasCachedData()
    {
        std::string directory = getNodeDirectory();
        std::string dataDefFile = directory + "/data_def.json";
        
        if(boost_fs::exists(dataDefFile))
        {
            return true;
        }
        
        return false;
    }

    bool CCodeNode::updateDeclaration()
    {
        std::string decl = extractFunctionDeclaration(m_implementation.m_source);
        if(decl.length() < 7)
        {
            //No way to be a valid declaration
            return false;
        }
        
        std::string declToCmp = removeWhitespace(decl);
        std::string oldDeclToCmp = removeWhitespace(m_prototype.declaration);
        
        if(declToCmp != oldDeclToCmp)
        {
            captureContext();
            Client& client = Client::getInstance();
            CCodeProject* proj = (CCodeProject*)client.project();     
            std::set<std::string> referencedNodes;
            std::string context = getContexInfo(true, false, false, referencedNodes);
            
            std::string reviseFunctionMessage = proj->revise_function.prompt({
                {"function", m_brief.func_name},
                {"context", context},
                {"source", m_implementation.m_source}
            });
            
            client.selectLLM(InferenceIntent::DEFINE);
            
            //Do we need cache here
            bool wasOnAuto = Client::getInstance().run();
            std::string cache;
            inference(cache, reviseFunctionMessage, &m_prototype);
            ensureEndsWith(m_prototype.declaration, ';');
            reflectFunction();
            std::string newDeclToCmp = removeWhitespace(m_prototype.declaration);
            if(declToCmp != newDeclToCmp) {
                std::cout << "Unmatching signatures:" << std::endl;
                std::cout << "From source: " << declToCmp << std::endl;
                std::cout << "From LLM revise: " << newDeclToCmp << std::endl;
            }
            if(!wasOnAuto) Client::getInstance().stop();
            
            //TODO: We must update references to the data here
            
            popContext();
            return true;
        }
        
        return false;
    }

    bool CCodeNode::isDefined()
    {
        if(m_brief.func_name.empty())
            return false;
        
        if(m_brief.brief.empty())
            return false;
        
        if(m_prototype.description.empty())
            return false;
        
        if(m_prototype.declaration.empty())
            return false;
        
        if(m_implementation.m_source.empty())
            return false;
        
        return true;
    }

    void CCodeNode::synchronizeFunctionCalls(const std::set<std::string>& calledFunctions, CodeType type)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        bool needsOrderUpdate = false;
        for(const auto& call : calledFunctions)
        {
            bool listed = callsFunction(call) != nullptr;
            
            if(!listed && (call != m_brief.func_name))
            {
                auto it = proj->nodeMap().find(call);
                if(it != proj->nodeMap().end())
                {
                    CCodeNode* existingFuncNode = proj->shareNode<CCodeNode>(call, this);
                    m_calls.items.push_back(std::make_shared<FunctionItem>(existingFuncNode->m_brief));
                    needsOrderUpdate = true;
                }
                else
                {
                    bool isCppIdentifier = proj->getCppIdentifiers().find(call) != proj->getCppIdentifiers().end();
                    std::string owningPath;
                    bool isDataType = proj->findData(call, owningPath).has_value();
                    if((type == FUNC_IMPL || type == FUNC_FIX) &&
                       !isCppIdentifier &&
                       !isDataType &&
                       proj->m_refactoringDepth == 0 &&
                       m_codeReview.str().length() == 0)
                    {
                        auto newItem = std::make_shared<FunctionItem>();
                        newItem->brief = "This function was added in the implementation of '" + m_brief.func_name;
                        newItem->brief += "'. Description of the function needs to be based on how it is called in the implementation of '";
                        newItem->brief += m_brief.func_name + "'";
                        newItem->func_name = call;
                        m_calls.items.push_back(newItem);
                    }
                    
                    m_stats.m_callingNonExistingFunction.insert(call);
                }
            }
        }
        
        if(!m_stats.m_callingNonExistingFunction.empty() &&
           m_stats.m_callingNonExistingFunctionCount == 0)
        {
            m_stats.m_callingNonExistingFunctionCount++;
        }
        
        if(needsOrderUpdate)
        {
            m_calls.updateOrder();
        }
    }

    bool CCodeNode::purgeUnusedNodes(bool deleteNode)
    {
        std::set<DAGNode<Node*>*> nodesToRemove;
        for(auto child : m_this->m_children)
        {
            if(child->m_data)
            {
                const std::string& childName = child->m_data->getName();
                auto it = std::find_if(m_calls.items.begin(), m_calls.items.end(),
                                       [&childName](const std::shared_ptr<FunctionItem>& item) {
                    return item->func_name == childName;
                });
                
                //Remove the current child node, if is not for a function called by this node
                if (it == m_calls.items.end())
                {
                    nodesToRemove.insert(child);
                }
            }
        }
        
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        for(auto child : nodesToRemove)
        {
            uint32_t leftRefCount = 0;
            if(child->m_data)
            {
                leftRefCount = child->m_data->releaseReference(this);
            }
            
            //It shouldn't be a probelm to delete node durging the composition
            //since it doesn't have direct children that could be referenced from other functions
            if(deleteNode && child->m_children.size()==0)
            {
                if(child->m_data &&
                   child->m_data->m_this == child)
                {
                    child->m_data->m_this = nullptr;
                }
                m_this->removeChild(child);
            }
        }
    }

    bool CCodeNode::loadOrder()
    {
        web::json::value json;
        if(loadJson(json, getNodeDirectory() + "/calls.json"))
        {
            if(json.has_field(U("order")))
            {
                auto content = json.at(U("order")).as_string();
                m_calls.m_order = utility::conversions::to_utf8string(content);
                
                return true;
            }
        }
        
        return false;
    }

    void CCodeNode::reviewLibFunctions(const std::string& functionList)
    {
        if(functionList.empty())
            return;
        
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        captureContext();
        
        std::string favorites;
        for(auto libFn : m_libCalls.items)
        {
            favorites += proj->getFunctionInfo(libFn->func_name);
            favorites += "How to use '" + libFn->func_name + "' in the implementation: ";
            favorites += libFn->motivation;
            favorites += "\n\n";
        }
        
        if(favorites.empty())
        {
            favorites = "none";
        }
        
        std::string findInLibMessage = proj->find_in_library.prompt({
            {"declaration", m_prototype.declaration},
            {"function", m_brief.func_name},
            {"favorites", favorites},
            {"list", functionList}
        });
        
        client.selectLLM(InferenceIntent::SEARCH_LIB);
        
        std::string cache;
        inference(cache, findInLibMessage, &m_libCalls);
        
        popContext();
    }

    bool CCodeNode::searchLibrary(const std::set<std::string>& exclude)
    {
        Client& client = Client::getInstance();
        CCodeProject* proj = (CCodeProject*)client.project();
        
        web::json::value jsonCalls;
        if(proj->m_cache && loadJson(jsonCalls, getNodeDirectory() + "/lib_calls.json"))
        {
            m_libCalls.from_json(jsonCalls);
            return !m_libCalls.items.empty();
        }
        
        //m_libCalls.caller_func.clear();
        m_libCalls.items.clear();
        std::string functionList;
        for(auto node : proj->nodeMap())
        {
            auto ccNode = (const CCodeNode*)node.second;
            
            if(ccNode == this)
            {
                continue;
            }
            
            auto exIt = exclude.find(ccNode->getName());
            if(exIt != exclude.end())
            {
                continue;
            }
            
            //We can't aford a function to be interpreted differently 
            //based on the needs of the funtcion being implemented
            //So we can provide only already fully defined functions
            if(!ccNode->m_defined)
                continue;
            
            std::stack<const CCodeNode*> dependencyPath;
            if(proj->isADependency(dependencyPath, ccNode->getName(), getName()))
            {
                continue;
            }
            
            functionList += ccNode->m_prototype.brief;
            functionList += "\n";
            functionList += ccNode->m_prototype.declaration;
            functionList += "\n\n";
            
            if(functionList.size() > (3*MAX_CHARACTERS_IN_CONTENT_CHUNK))
            {
                reviewLibFunctions(functionList);
                functionList.clear();
            }
        }
        
        reviewLibFunctions(functionList);
        
        //TODO: Check for recursive calls and final decision based on LLM reviewing the implementatoins
        
        return !m_libCalls.items.empty();
    }

    bool CCodeNode::isFromLibrary(const std::string& functionName) const
    {
        return std::find_if(m_libCalls.items.begin(), m_libCalls.items.end(),
        [&functionName](const std::shared_ptr<LibFunctionItem>& ptr) {
            return ptr && ptr->func_name == functionName;
        }) != m_libCalls.items.end();
    }

    const std::shared_ptr<FunctionItem> CCodeNode::callsFunction(const std::string& functionName) const
    {
        auto it = std::find_if(m_calls.items.begin(), m_calls.items.end(),
        [&functionName](const std::shared_ptr<FunctionItem>& ptr) {
            return ptr && ptr->func_name == functionName;
            //return ptr;
        });
        
        return it != m_calls.items.end() ? *it : nullptr;
    }

    std::set<std::string> CCodeNode::getCalledFunctions() const
    {
        std::set<std::string> functions;
        std::transform(m_calls.items.begin(), m_calls.items.end(),
                       std::inserter(functions, functions.end()),
                       [](const auto& func) { return func->func_name; });
        return functions;
    }

    std::string CCodeNode::refactorTruncatedSource(std::string& source, CodeType srcType)
    {
        //WE MUST NOT BE HERE IF THE NODE IS CACHED!
        
        size_t linesCount = countLines(source);
        if(linesCount <= MAX_LINES_IN_SOURCE_SNIPPET && source.length() <= MAX_CHARACTERS_IN_SOURCE_SNIPPET )
        {
            //Most probably nothing to worry about and will be fixed in the next review
            return "";
        }
        
        Client& client = Client::getInstance();
        LLMRole savedLLM = client.getLLM();
        client.selectLLM(InferenceIntent::REASON_BREAKDOWN);
        
        std::string sourceBeforeRefactoring = source;
        
        captureContext();
        
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        FunctionItem newFunction;
        
        std::set<std::string> owners, structs, enums;
        std::string api = getApiForReview(owners, structs, enums, false);
        
        std::string refactorMessage = proj->refactor_truncated_source.prompt({
            {"function", m_brief.func_name},
            {"declaration", m_prototype.declaration},
            {"truncated", source}
        });
        
        std::string cache;
        inference(cache, refactorMessage, &newFunction);
        
        bool wasOnAuto = Client::getInstance().run();
        std::string reviewRefactorBrief = proj->review_refactor_brief.prompt({
            {"function", newFunction.func_name},
            {"original_function", m_prototype.declaration}
        });
        
        bool selfReviewPass = inference(cache, reviewRefactorBrief, true);
        
        if(!wasOnAuto) Client::getInstance().stop();
        
        if(!selfReviewPass)
        {
            inference(cache, "Do your best to fix all this and revise the answer without asking further questions!", &newFunction);
        }
        
        //Very unlikely since we provided list with existing functions
        auto it = proj->nodeMap().find(newFunction.func_name);
        if(it != proj->nodeMap().end())
        {
            //We have a conflict with the name of an existing function
            //Let's ask the LLM to compare both functions and decide if they are similar and can be reused
            //Or to adjust the name and description of the current function
            auto existingFuncNode = (const CCodeNode*)it->second;
            resolveName(existingFuncNode, newFunction);
        }
        
        //This is not expected but let's check
        auto itCall = std::find_if(m_calls.items.begin(),
                               m_calls.items.end(),
                               [&newFunction](const std::shared_ptr<FunctionItem>& funct_ptr) {
            return funct_ptr->func_name == newFunction.func_name;
        });
        
        client.selectLLM(InferenceIntent::REASON_BREAKDOWN);
        
        if(itCall == m_calls.items.end())
        {
            m_calls.items.push_back(std::make_shared<FunctionItem>(newFunction));
        }
        
        //Now generate the source again and this is the best we can do!
        std::string refactorOriginalSource = proj->refactor_original_source.prompt({
            {"function", m_brief.func_name},
            {"declaration", m_prototype.declaration},
            {"new_function", newFunction.func_name}
        });
        
        source = "cpp";
        inference(cache, refactorOriginalSource, source);
        bool refactoringSuccessful = srcType == CodeType::FUNC_IMPL;
        CCodeNode* newChild = nullptr;
        if(srcType == CodeType::FUNC_CMPL ||
           srcType == CodeType::FUNC_FIX)
        {
            //NOTES !!!
            //Calling defineAndBuild() for the refactored child will call codeAndDataReview for the parent (this node) when doing,
            //so we need to take care for a few things:
             
            // * Destructive declaration changes should be identified in the review
            // * A call to the new refactored child call will be enfoced during the review
            // * By doing a snapsot of the data before verify() call no data changes would be allowed during the verification
            // * verify() call will evaluate/review/fix the source for syntactical correctness
            
            newChild = proj->shareNode<CCodeNode>(newFunction.func_name, this);
            if(newChild->refCount() == 1)//This is expected!
            {
                newChild->m_brief = newFunction;
            }
            
            //We need to make sure the code will compile but also meet the necessary requirements
            codeReview(source, srcType, -1, true, false);
            if(m_codeReview.str().empty())
            {
                //Doing datasnapshot now, no destructive data changes will be allowed during the verification
                proj->dataSnapshot();
                
                std::string oldSource = m_implementation.m_source;
                std::string oldDecl = m_prototype.declaration;
                m_implementation.m_source = source;
                
                if(updateDeclaration())
                {
                    //TODO: This needs testing!
                    updateExternals();
                }
                
                if(!verify())
                {
                    //Revert to the old source, the verify() couldn't fix the new one
                    //This will bring us
                    m_implementation.m_source = oldSource;
                    m_prototype.declaration = oldDecl;
                    refactoringSuccessful = false;
                }
                else
                {
                    refactoringSuccessful = true;
                    save();
                    generateAllSources(true);
                }
            }
        }
        
        popContext();
        
        if(!refactoringSuccessful)
        {
            //Revert to the initial version of the source
            source = sourceBeforeRefactoring;
            //We can't do much :(
            client.setLLM(savedLLM);
            return "";
        }
        
        size_t newLinesCount = countLines(source);
        std::cout << "The source code for the function '" << m_brief.func_name << "' has been refactured." << std::endl;
        std::cout << "Old source lines of code: " << linesCount << std::endl;
        std::cout << "New source lines of code: " << newLinesCount << std::endl << std::endl;
        
        //TODO: Decompose and build the node here
        if(refactoringSuccessful && (srcType == CodeType::FUNC_CMPL ||
                                     srcType == CodeType::FUNC_FIX))
        {
            
            
            if(newChild->m_implementation.m_source.empty())
            {
                if(srcType == CodeType::FUNC_FIX)
                {
                    std::string inRefactoringHint;
                    inRefactoringHint += "\n********** REFACTORING HINT STARTS **********\n";
                    inRefactoringHint += "NOTE! the function '" + newFunction.func_name + "' is being implemented as a result of refactoring the function '";
                    inRefactoringHint += m_brief.func_name + "'. Source code in the refactoring prompt is BEFORE the refactoring of '" + m_brief.func_name + "' to it's current version.";
                    inRefactoringHint += " Please consider the information from the refactoring prompt while implementing " + newFunction.func_name;
                    inRefactoringHint += "\n\nHere is the refactiong prompt:\n";
                    inRefactoringHint += refactorMessage;
                    inRefactoringHint += "\n********** REFACTORING HINT ENDS **********\n";
                    
                    newChild->m_inRefactoringHint = inRefactoringHint;
                }
                
                uint32_t archiveId = storeContext(m_compilationStartMessage);
                proj->m_refactoringDepth++;
                newChild->defineAndBuild();
                proj->m_refactoringDepth--;
                restoreContext(archiveId);
            }
        }
        
        std::string refactoredMsg = "Note that function '" + m_brief.func_name;
        refactoredMsg += "' has been refactored in order to decrease its implementation size. Now the function has to call a new function '";
        refactoredMsg += newFunction.func_name + "' Here is a brief description of " + newFunction.func_name + ":\n";
        refactoredMsg += newFunction.brief;
        pushMessage(refactoredMsg, "user");
        
        client.setLLM(savedLLM);
        
        return newFunction.func_name;
    }

    bool CCodeNode::mergeLibCalls()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        bool libCallsAdded = false;
        //Merge libCalls with calls suggested by the LLM. We want implement() to see them all.
        for(auto libCall : m_libCalls.items)
        {
            if(!callsFunction(libCall->func_name))
            {
                //If we read from cache shouldn't be here!!!
                
                auto itLibCall = proj->nodeMap().find(libCall->func_name);
                if(itLibCall != proj->nodeMap().end())
                {
                    FunctionItem libItem;
                    CCodeNode* libNode = (CCodeNode*)itLibCall->second;
                    
                    m_calls.items.push_back(std::make_shared<FunctionItem>(libNode->m_brief));
                    libCallsAdded = true;
                }
            }
        }
        
        return libCallsAdded;
    }

    bool CCodeNode::updateCallsUsage(bool createNodes, bool deleteUnusedNodes)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        //Reomove unused function calls. We don't want child nodes for those
        std::vector<std::shared_ptr<FunctionItem>> items;
        for (auto& func : m_calls.items)
        {
            bool isUsed = !deleteUnusedNodes || m_stats.m_unusedFunctions.find(func->func_name) == m_stats.m_unusedFunctions.end();
            if(isUsed)
            {
                items.push_back(func);
                if(createNodes && getChild(func->func_name) == nullptr)
                {
                    CCodeNode* child = proj->shareNode<CCodeNode>(func->func_name, this);
                    if(child->refCount() == 1)
                    {
                        child->m_brief = *func;
                    }
                }
            }
        }
        
        m_stats.m_unusedFunctions.clear();
        
        if(m_calls.items.size() != items.size()) {
            //We should never be here for cached node
            m_calls.items = items;
            m_calls.updateOrder();
        }
        
        m_libCalls.items.erase(
            std::remove_if(m_libCalls.items.begin(), m_libCalls.items.end(),
                [this](const std::shared_ptr<LibFunctionItem>& item) {
                    return !callsFunction(item->func_name);
                }), m_libCalls.items.end());
        
        purgeUnusedNodes(deleteUnusedNodes);
    }

    void CCodeNode::getRefactorExcludeCalls(std::set<std::string>& exclude)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::vector<Node*> nodes;
        getDAGPathNodes(true, nodes);
        for(uint32_t i = 0; i < proj->m_refactoringDepth; ++i)
        {
            if(nodes.size() > i)//I think this has to be an assert
            {
                exclude.insert(nodes[i]->getName());
            }
        }
    }

    void CCodeNode::defineAndBuild()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        //Wherever we are, switching to decompose context for this node should be the right thing to do
        Context* prevCtx = proj->switchToDecomposeContext(getDAGPath("/"));
        captureContext();
        decompose();
        popContext();
        
        generateAllSources(true);
        build();
        proj->setActiveContext(prevCtx);
    }

    bool hen::CCodeNode::doesItCall(std::stack<const CCodeNode*>& path,
                                     const CCodeNode* callee) const
    {
        // This implementation assumes callers pass an empty path for a fresh query.
        // To be robust (and to avoid a slow/incorrect partial state), we clear it.
        while (!path.empty()) path.pop();

        hen::CCodeProject* proj = (hen::CCodeProject*)Client::getInstance().project();

        // O(1) membership check for cycle detection along the current DFS path.
        std::unordered_set<const CCodeNode*> in_path;
        in_path.reserve(128);

        // Per-query "does not lead to callee" pruning (big win on dense graphs).
        std::unordered_set<const CCodeNode*> visited;
        visited.reserve(512);

        // Tiny per-thread cache to avoid repeating string -> nodeMap() lookups.
        struct Cache { std::unordered_map<std::string, const CCodeNode*> map; };
        Cache cache;
        cache.map.reserve(4096);

        auto resolve = [&](const std::string& name) -> const hen::CCodeNode* {
            auto itc = cache.map.find(name);
            if (itc != cache.map.end()) return itc->second;

            auto it = proj->nodeMap().find(name);
            const hen::CCodeNode* ptr =
                (it == proj->nodeMap().end()) ? nullptr
                                              : static_cast<const hen::CCodeNode*>(it->second);
            cache.map.emplace(name, ptr);
            return ptr;
        };

        // C++14-friendly recursive lambda (no <functional> / std::function needed).
        auto dfs = [&](const hen::CCodeNode* node, auto&& self) -> bool {
            if (!node) return false;

            // Cycle? (O(1) using in_path)
            if (!in_path.insert(node).second)
                return false;

            path.push(node);
            if (node == callee)
                return true; // keep path intact on success

            // Traverse outgoing call edges.
            const auto callsSnapshot = node->m_calls.items;
            for (const auto& call : callsSnapshot) {
                if (!call) continue;

                const hen::CCodeNode* next = resolve(call->func_name);
                if (!next) continue;
                if (visited.find(next) != visited.end()) continue;

                if (self(next, self))
                    return true; // success: leave path as the winning chain
            }

            // Backtrack
            path.pop();
            in_path.erase(node);
            visited.insert(node);
            return false;
        };

        return dfs(this, dfs);
    }

    bool CCodeNode::evaluateNewDeclaration(const std::string& decl)
    {
        std::string oldDecl = m_prototype.declaration;
        ParsedFunction oldSignature = m_prototype.m_signature;
        std::string initialReview = m_codeReview.str();
        
        m_prototype.declaration = decl;
        reflectFunction(); //reflectFunction will reset the review
        std::string newDeclReview = m_codeReview.str();
        
        //We have the reviwe, now revert the state
        m_prototype.declaration = oldDecl;
        m_prototype.m_signature = oldSignature;
        updateExternals();
        m_codeReview.str("");
        m_codeReview.clear();
        m_codeReview << initialReview;
        
        if(!newDeclReview.empty())
        {
            m_codeReview << std::endl << newDeclReview;
            return false;
        }
        
        return true;
    }

    bool CCodeNode::checkAppTypeQualification(const std::string& appType,
                                              const std::string& type,
                                              const std::string& decl,
                                              std::set<std::string>& inConflict,
                                              std::set<std::string>& needsSharedPtr,
                                              std::set<std::string>& qualifyEnums) const
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        if(!isValidCppType(type) /*|| !isValidCppType(decl)*/)
        {
            //Invalid types, will be reported by other checks
            //Return to avoid regex exceptions
            return;
        }
        
        bool hasIssues = false;
        bool hasConflict = proj->getCppIdentifiers().find(appType) != proj->getCppIdentifiers().end();
        if(hasConflict)
        {
            inConflict.insert(decl);
            hasIssues = true;
        }
        else //if(!hasConflict)
        {
            auto findings = findNonSharedPtrUsages(type, appType);
            if(!findings.empty())
            {
                std::string path;
                auto def = proj->findData(appType, path);
                if(def && def->m_typeDef.m_type == TypeDefinition::STRUCT)
                {
                    if(isValidEnumTypeName(appType))
                    {
                        std::cout << "struct '" << appType << "' has name that qualifes as enum type. Investigate the root cause!!!" << std::endl;
                    }
                    
                    needsSharedPtr.insert(decl);
                    hasIssues = true;
                }
                else if(!isValidEnumTypeName(appType))
                {
                    qualifyEnums.insert(decl);
                    hasIssues = true;
                }
            }
            else //if(findings.empty())
            {
                if(isValidEnumTypeName(appType))
                {
                    std::string path;
                    auto def = proj->findData(appType, path);
                    if(!def)
                    {
                        qualifyEnums.insert(decl);
                        hasIssues = true;
                    }
                    else if(def->m_typeDef.m_type == TypeDefinition::ENUM)
                    {
                        std::cout << "enum '" << appType << "' Used with stared_ptr. Investigate the root cause!!!" << std::endl;
                        
                        qualifyEnums.insert(decl);
                        hasIssues = true;
                    }
                }
            }
        }
        
        return hasIssues;
    }

    CCodeNode* CCodeNode::getParent()
    {
        if(m_this && m_this->m_parent && m_this->m_parent->m_data)
        {
            CCodeNode* parent = (CCodeNode*)m_this->m_parent->m_data;
            return parent;
        }
        
        return nullptr;
    }

    std::string CCodeNode::printCallsInfo()
    {
        std::string message;
        if(m_calls.items.size() > 1)
        {
            message += "Function '" + m_brief.func_name + "' calls:\n";
            for(auto call : m_calls.items)
            {
                message += "  " + call->func_name + "\n";
            }
        }
        
        return message;
    }

    std::string CCodeNode::generateReport()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        std::stringstream report;
        
        report << "\n\nCRAFTING...\n\n";
        report << "```cpp" << std::endl;
        std::set<std::string> owners;
        std::set<std::string> structs;
        std::set<std::string> enums;
        getFullVisibility(true, owners, structs, enums);
        std::string dataDefs = getDataDefinitions(structs, enums, true, true, true);
        if(!dataDefs.empty())
        {
            report << std::endl << std::endl << "//Visible data definitions" << std::endl;
            report << dataDefs;
        }
        report << generateDeclarations(/*const std::string& skip*/);
        printAsComment(m_prototype.description, report);
        report << std::endl << m_implementation.m_source << std::endl;
        report << "```" << std::endl;
        
        return report.str();
    }

    static std::string regex_escape(const std::string& s) {
        static const std::string specials = R"(\.^$|()[]{}*+?!)";
        std::string out; out.reserve(s.size()*2);
        for (char ch : s) {
            if (specials.find(ch) != std::string::npos) out.push_back('\\');
            out.push_back(ch);
        }
        return out;
    }

    bool CCodeNode::calledInTheSource(const std::string& otherFunction)
    {
        const std::regex re("\\b" + regex_escape(otherFunction) + "\\s*\\(", std::regex::ECMAScript);
        
        std::string comments;
        std::string code = removeComments(m_implementation.m_source, comments);

        // Keep only the body to avoid matching prototypes
        auto brace = code.find('{');
        if (brace != std::string::npos) code.erase(0, brace);

        if (std::regex_search(code, re)) {
            return true;
        }
        
        return false;
    }

    bool CCodeNode::hasPathToMain()
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        CallGraph g;
        return g.is_called_from(getName(), "main",
        [&, proj, this](const std::string& c) -> CallGraph::CallerSet {
            CallGraph::CallerSet s;
            const std::regex re("\\b" + regex_escape(c) + "\\s*\\(", std::regex::ECMAScript);
            auto* ccCaller = proj->getNodeByName(c);
            if(!ccCaller) return s;//No node for this caller
            
            for (auto ref : ccCaller->m_referencedBy) {
                auto* ccRef = dynamic_cast<const CCodeNode*>(ref);
                if (!ccRef) continue;
                if (ccRef->m_implementation.m_source.empty()) continue;
                if (ccRef->getName() == c) continue; // ignore self

                std::string comments;
                std::string code = removeComments(ccRef->m_implementation.m_source, comments);

                // Keep only the body to avoid matching prototypes
                auto brace = code.find('{');
                if (brace != std::string::npos) code.erase(0, brace);

                if (std::regex_search(code, re)) {
                    s.insert(ccRef->getName());
                }
            }
            return s;
        });
    }

    std::string CCodeNode::callsConflictsWithData(const FunctionList& calls, bool listDataTypes)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        std::string message;
        
        std::set<std::string> functions;
        
        for(auto call : calls.items)
        {
            functions.insert(call->func_name);
        }
        
        std::set<std::string> conflicted = proj->getConflictsWithData(functions);
        
        std::string funcList;
        for(auto func : conflicted)
        {
            if(!funcList.empty()) funcList += ", ";
            funcList += func;
        }
        
        if(!funcList.empty())
        {
            message += "The following function names are in conflict with already existing data type definitions:\n";
            message += funcList;
            message += "\nConsider renaming these functions!\n\n";
            
            if(listDataTypes)
            {
                message += "Here is a list with all existing data types in the project:\n";
                message += proj->listAllDataTypeNames();
                message += "\n\n";
            }
        }
        
        return message;
    }

    bool CCodeNode::unitTestIsBroken(const std::string& prevTestTrajectory, const std::string& fullTestDesc, const std::string& testExecLog)
    {
        CCodeProject* proj = (CCodeProject*)Client::getInstance().project();
        
        storeUnitTestContent();
        
        std::string oldTest = m_unitTest.getDescription();
        
        std::string testFrameworkMan = getFileContent(Client::getInstance().getEnvironmentDir() + "/Prompts/TestFramework.txt");
        
        Prompt needToImprove("NeedsToImproveUnitTest.txt", {
            { "test_framework_manual",  testFrameworkMan  },
            { "function",  getName()  },
            { "trajectory", prevTestTrajectory },
            { "old_test", oldTest },
            { "old_test_name", m_unitTest.definition.name },
            { "full_test", fullTestDesc},
            { "test_exec_log", testExecLog}
        });
        
        Cache cache;
        
        bool needsImrpovement = false;
        std::string needToImproveMsg = needToImprove.str();
        std::string response;
        bool result = proj->inference(cache, needToImproveMsg, true, response, true);
        
        return !result;
    }

    bool CCodeNode::improveUnitTest()
    {
        CCodeNode* parent = nullptr;
        std::string parentCtx;
        std::set<std::string> referencedNodes;
        if(m_this->m_parent)
        {
            parent = (CCodeNode*)m_this->m_parent->m_data;
            assert(parent);
            
            parentCtx = "\nAlso, this function will be called by the '";
            parentCtx += parent->m_brief.func_name;
            parentCtx += "' and here is more information about that function:\n";
            parentCtx += parent->getContexInfo(true, true, false, referencedNodes);
        }
        
        std::string functionCtx = getContexInfo(true, false, false, referencedNodes);
        
        Prompt improve("ImproveUnitTest.txt", {
            { "function",  getName()  },
            { "function_ctx", functionCtx},
            { "parent_ctx", parentCtx},
            { "test_name", m_unitTest.definition.name }
        });
        
        deleteUnitTest(); //Ensure we first delete the old one
        
        captureContext();
        inferenceUnitTestDef(improve.str());
        popContext();
        
        pushMessage(improve.str(), "user");
#if 1
        captureContext();
        implementUnitTest();
        popContext();
#else
        generateUnitTestInputFiles();
        
        generateUnitTestSource();
        
        compileUnitTestSource();
        
        linkUnitTest(true);
#endif
        
        summarizeUnitTestDesc();
        
        //Save the unit test
        {
            Client& client = Client::getInstance();
            CCodeProject* proj = (CCodeProject*)client.project();
            
            std::string buildSourcePath = getNodeBuildSourcePath();
            std::string buildDir = proj->getProjDir() + "/build";
            std::string nodeDir = buildDir + "/" + buildSourcePath;
            std::string testDir = nodeDir + "/test";
            
            std::string soruceFile = testDir + "/main.cpp";
            
            saveJson(m_unitTest.definition.to_json(), testDir + "/test.json");
        }
    
        return true;
    }
}
