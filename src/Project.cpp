#include <tuple>
#include "Project.h"
#include "Utils.h"
#include "Client.h"
#include "Node.h"
#include "Project.hpp"
#include "Graph.hpp"

#include "Distillery.h"


using namespace web; // For convenience
using namespace utility; // For conversions

namespace hen {

    static bool extractMessageContent(const web::json::value& message, utility::string_t& content)
    {
        if (!message.is_object() || !message.has_field(U("content")))
        {
            return false;
        }

        const web::json::value& contentValue = message.at(U("content"));
        if (contentValue.is_string())
        {
            content = contentValue.as_string();
            return true;
        }

        if (contentValue.is_object())
        {
            if (contentValue.has_field(U("text")) && contentValue.at(U("text")).is_string())
            {
                content = contentValue.at(U("text")).as_string();
                return true;
            }

            if (contentValue.has_field(U("content")) && contentValue.at(U("content")).is_string())
            {
                content = contentValue.at(U("content")).as_string();
                return true;
            }

            return false;
        }

        if (!contentValue.is_array())
        {
            return false;
        }

        std::string mergedContent;
        for (const auto& part : contentValue.as_array())
        {
            if (part.is_string())
            {
                mergedContent += utility::conversions::to_utf8string(part.as_string());
                continue;
            }

            if (!part.is_object())
            {
                continue;
            }

            if (part.has_field(U("type")) &&
                part.at(U("type")).is_string() &&
                part.at(U("type")).as_string() == U("thinking"))
            {
                continue;
            }

            if (part.has_field(U("text")) && part.at(U("text")).is_string())
            {
                mergedContent += utility::conversions::to_utf8string(part.at(U("text")).as_string());
            }
            else if (part.has_field(U("content")) && part.at(U("content")).is_string())
            {
                mergedContent += utility::conversions::to_utf8string(part.at(U("content")).as_string());
            }
        }

        if (mergedContent.empty())
        {
            return false;
        }

        content = utility::conversions::to_string_t(mergedContent);
        return true;
    }

    static bool extractFunctionCallObject(const web::json::value& message, web::json::value& object, std::string& name)
    {
        if (!message.is_object())
        {
            return false;
        }

        web::json::value functionCall;
        bool hasFunctionCall = false;

        if (message.has_field(U("function_call")))
        {
            const web::json::value& functionCallValue = message.at(U("function_call"));
            if (functionCallValue.is_object())
            {
                functionCall = functionCallValue;
                hasFunctionCall = true;
            }
        }

        if (!hasFunctionCall &&
            message.has_field(U("tool_calls")) &&
            message.at(U("tool_calls")).is_array())
        {
            for (const auto& toolCall : message.at(U("tool_calls")).as_array())
            {
                if (!toolCall.is_object())
                {
                    continue;
                }

                if (toolCall.has_field(U("function")) && toolCall.at(U("function")).is_object())
                {
                    functionCall = toolCall.at(U("function"));
                    hasFunctionCall = true;
                    break;
                }

                if (toolCall.has_field(U("name")) && toolCall.has_field(U("arguments")))
                {
                    functionCall = toolCall;
                    hasFunctionCall = true;
                    break;
                }
            }
        }

        if (!hasFunctionCall ||
            !functionCall.has_field(U("name")) ||
            !functionCall.at(U("name")).is_string() ||
            !functionCall.has_field(U("arguments")))
        {
            return false;
        }

        name = utility::conversions::to_utf8string(functionCall.at(U("name")).as_string());

        const web::json::value& argumentsValue = functionCall.at(U("arguments"));
        if (argumentsValue.is_string())
        {
            object = web::json::value::parse(argumentsValue.as_string());
            return true;
        }

        if (argumentsValue.is_object() || argumentsValue.is_array())
        {
            object = argumentsValue;
            return true;
        }

        return false;
    }
    
    DEFINE_TYPE(Project)
    
    DEFINE_TYPE(CodeProject)
    DEFINE_FIELD(CodeProject, name)
    DEFINE_FIELD(CodeProject, func_name)
    DEFINE_FIELD(CodeProject, brief)
    DEFINE_FIELD(CodeProject, type)
    DEFINE_FIELD(CodeProject, entry_point)
    DEFINE_FIELD(CodeProject, role)
    DEFINE_FIELD(CodeProject, sample_description)

    //const web::json::value Project::pushMessage(const std::string& content, const std::string& role)
    uint32_t Project::pushMessage(const std::string& content, const std::string& role, bool print)
    {
        if(print)
        {
            std::cout << ">>" << role << std::endl << std::endl;
            std::cout << content << std::endl << std::endl;
        }

#ifndef NO_NODE_MESSAGES
        json::value message;
        message[U("role")] = json::value::string(utility::conversions::to_string_t(role));
        message[U("content")] = json::value::string(utility::conversions::to_string_t(content));

        auto& messagesArray = m_messages.as_array();
        auto size = messagesArray.size();
        messagesArray[size] = message;
        
        return message;
#else
        return m_activeContext->add(content, role);
#endif
    }

    std::vector<std::pair<std::string, std::string>> Project::popMessages(uint32_t count)
    //void Project::popMessages(uint32_t count)
    {
#ifndef NO_NODE_MESSAGES
        std::vector<std::pair<std::string, std::string>> erasedMessages;
        
        if (!m_messages.is_array() || m_messages.as_array().size()==0)
            return erasedMessages;

        auto& messagesArray = m_messages.as_array();
        count = std::min(static_cast<uint32_t>(messagesArray.size()), count);

        erasedMessages.reserve(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            size_t lastIndex = messagesArray.size() - 1;
            if (messagesArray[lastIndex].has_field(U("content")) &&
                messagesArray[lastIndex].has_field(U("role")))
            {
                auto content = conversions::to_utf8string(messagesArray[lastIndex][U("content")] .as_string());
                auto role = conversions::to_utf8string(messagesArray[lastIndex][U("role")] .as_string());
                
                erasedMessages.push_back(std::make_pair(content, role));
            }
            messagesArray.erase(lastIndex);
        }
        
        std::reverse(erasedMessages.begin(), erasedMessages.end());
        return erasedMessages;
#else
        return m_activeContext->pop(count);
#endif
    }

    void Project::setup(const std::string& projectDir, Node* root)
    {
        m_projDir = projectDir;
        
        std::string envDir = Client::getInstance().getEnvironmentDir();
        
        std::string envPath = envDir + "/Environment.json";
        std::ifstream file(envPath);
        std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto ustr = conversions::to_string_t(str);
        json::value envJson = json::value::parse(ustr);
        from_json(envJson);
        
        std::string promptsDir = envDir + "/Prompts";
        FileName::setPromptDir(promptsDir);

        m_dag.m_root = new DAGNode<Node*>(root);//root;
        root->m_this = m_dag.m_root;
    
        //These messages are general for the project, so they shouldn't be in any node
        pushMessage(m_description.role.prompt(), "system", true);
        
        std::string descriptionPath = m_projDir + "/Description.txt";
        if(boost_fs::exists(descriptionPath))
        {
            std::ifstream file(descriptionPath);
            if (!file.is_open()) {
                throw std::runtime_error("Could not open Description.txt");
            }
            m_description.description = std::string((std::istreambuf_iterator<char>(file)),
                                                     std::istreambuf_iterator<char>());
            
            //pushMessage(m_description.description);
        }
    }

    void Project::buildHierarchy(DAGNode<Node*>* root)
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
                //node->m_data->captureContext();
            if(node->m_data)
            {
                if(m_integrate) node->m_data->integrate();
                //node->m_data->popContext();
            }
            });
        
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

    void Project::onDeleteHierarchy(DAGNode<Node*>* root)
    {
        m_dag.depthFirstTraversal(root, [this](DAGNode<Node*>* node, DAGraph<Node*>& g) {
            },
            [this](DAGNode<Node*>* node) {
            if(node->m_data)
            {
                node->m_data->onDelete();
            }
            });
    }

    void Project::traversal()
    {
        load();
        
        m_dag.depthFirstTraversal(m_dag.m_root, [this](DAGNode<Node*>* node, DAGraph<Node*>& g) {
            if(node->m_data)
            {
                node->m_data->captureContext();
                node->m_data->decompose();
                node->m_data->popContext();
                pushMessage(node->m_data->summarize(true), "user", true);
            }
            },
            [this](DAGNode<Node*>* node) {
                //node->m_data->captureContext();
                if(m_integrate) node->m_data->integrate();
                //node->m_data->popContext();
            });
       
        setupBuild();
        
        m_dag.depthFirstTraversal(m_dag.m_root, [this](DAGNode<Node*>* node, DAGraph<Node*>& g) {},
            [this](DAGNode<Node*>* node) {
            if(node->m_data)
            {
                if(m_integrate) node->m_data->preBuild();
            }
            });
        
        m_dag.depthFirstTraversal(m_dag.m_root, [this](DAGNode<Node*>* node, DAGraph<Node*>& g) {
                
            },
            [this](DAGNode<Node*>* node) {
            if(node->m_data)
            {
                node->m_data->captureContext();
                if(m_integrate) node->m_data->build();
                node->m_data->popContext();
            }
            });
        
        //TODO: Delete the build output folder <Platform>_test and start another build traversa to fix the linking
        
        finalizeBuild();
        
        //Always disable auto continue after decompose to give a chance the user to enter commands
        Client::getInstance().stop();
    }

    void Project::save(const std::string& directory, bool overwrite)
    {
        std::string projDirOld = m_projDir;
        if (!directory.empty())
        {
            if (!boost_fs::exists(directory) || !boost_fs::is_directory(directory))
            {
                std::cout << "Invalid directory " << directory << std::endl;
                return;
            }

            if (overwrite)
            {
                boost_fs::remove_all(directory);
            }

            m_projDir = directory;
        }

        m_dag.depthFirstTraversal(m_dag.m_root, [](DAGNode<Node*>* node, DAGraph<Node*>& g) {
            if(node->m_data)
            {
                node->m_data->save();
            }
            });

        m_projDir = projDirOld;
    }

    void Project::load()
    {
        m_dag.depthFirstTraversal(m_dag.m_root, [](DAGNode<Node*>* node, DAGraph<Node*>& g) {
            if(node->m_data)
            {
                node->m_data->load();
            }
            });
        
        loadStats();
    }

    void Project::clear()
    {
        // Drop graph links and owned DAG nodes first
        m_dag.clear();

        // Drop node index
        m_nodeMap.clear();

        // Now it’s safe to drop pooled allocations (Nodes / FunctionItem / etc.)
        pools.clear();

        m_stats = web::json::value();
    }

    void Project::reload()
    {
        clear();
        
        load();
    }

    //TODO: Consider moving this to the Client since not much from the Project is involved here
    bool Project::handleResponse(web::json::value& response, web::json::value* object, bool print)
    {
        std::string role;
        std::string content;
        utility::string_t json;

        try {
                if (response.has_field(U("message")))
                {
                    const web::json::value& message = response.at(U("message"));
                    std::string name_obj;

                    if (object && extractFunctionCallObject(message, *object, name_obj)) {

                        if (print)
                        {
                            std::string objStr = utility::conversions::to_utf8string(object->serialize());
                            std::string objIndentStr = formatJson(objStr, std::string("  "));

                            std::cout << std::endl << ">>object: " << name_obj << std::endl << std::endl;
                            std::cout << objIndentStr << std::endl;
                            std::cout << std::endl << std::endl;
                        }

                        return true;
                    }
                    else
                    {
                        utility::string_t ucontent;
                        utility::string_t urole;

                        if (extractMessageContent(message, ucontent))
                        {
                            json = findJson(ucontent, true);
                        }

                        if (!object && /*json.empty() &&*/ message.has_field(U("role")) && message.at(U("role")).is_string())
                        {
                            urole = message.at(U("role")).as_string();

                            role = utility::conversions::to_utf8string(urole);
                        }
                        else if (object && !json.empty())
                        {
                            *object = web::json::value::parse(json);
                            
                            if (print)
                            {
                                std::string objStr = utility::conversions::to_utf8string(json);
                                std::string objIndentStr = formatJson(objStr, std::string("  "));

                                std::cout << std::endl << ">>object: " /*<< name_obj*/ << std::endl << std::endl;
                                std::cout << objIndentStr << std::endl;
                                std::cout << std::endl << std::endl;
                            }

                            return true;
                        }

                        if(!object && !ucontent.empty() && !urole.empty())
                        {
                            role = utility::conversions::to_utf8string(urole);
                            content = utility::conversions::to_utf8string(ucontent);
                            //std::cout << content << std::endl << std::endl;
                            
                            //Seems this is a chat message, add it to the context.
                            pushMessage(content, role, true);
                        }
                    }
                }
                else {
                    std::cerr << "The JSON structure does not contain the expected fields! JSON content:" << std::endl;
                    ucout << response.serialize() << std::endl;
                }
        }
        catch (const web::json::json_exception& e) {
            
            ucout << "*** RESPONSE DEBUG INFO START ***" << std::endl;
            if (!json.empty()) {
                ucout << "Found JSON: " << std::endl;
                ucout << json << std::endl << std::endl << std::endl;
            }
            
            ucout << "Full response: " << std::endl;
            ucout << response.serialize() << std::endl << std::endl << std::endl;

            std::cerr << "Exception caught parsing JSON: " << e.what() << std::endl;
            ucout << "*** RESPONSE DEBUG INFO END ***" << std::endl;
        }

        return false;
    }

    //TODO: Consider moving this to the Client since not much from the Project is involved here
    bool Project::sendRequest(web::json::value* schemas, std::string& content, web::json::value* object)
    {
        web::json::value response;
        bool success = Client::getInstance().sendRequest(getMessages(), response, schemas);

        //TODO: Implement human-in-the-loop if !hasObject
        if (success)
        {
            if (response.has_field(U("message")) &&
                response[U("message")].is_object())
            {
                utility::string_t ucontent;
                if (extractMessageContent(response[U("message")], ucontent))
                {
                    content = utility::conversions::to_utf8string(ucontent);
                }
            }
            
            bool hasObject = Client::getInstance().project()->handleResponse(response, object, false);
            success = !schemas || (schemas && hasObject);
        }

        return success;
    }

    //return: less than 0 means error and wait
    //0 means wait
    //greater than 0 means continue
    int Project::executeCommand(const std::string& command, const std::string& cli, const boost_opt::variables_map& args)
    {
        int result = 0;
        
        std::cout << "Execute command: " << command << " " << cli << std::endl;
        
        boost::optional<bool> cache;
        if (args.count("cache")) {
            cache = args["cache"].as<bool>();
        }
        if(cache.has_value()) m_cache = cache.value();

        boost::optional<bool> save_flag;
        if (args.count("save")) {
            save_flag = args["save"].as<bool>();
        }
        if(save_flag.has_value()) m_save = save_flag.value();

        //Never start over when we are saving a scene.
        //This will avoid accidental execution of the "start" command
        //if (command == "start" && !oldSave)
        if (command == "start")
        {
            traversal();
            return 1;
        }
        /*else if (command == "load")
        {
            load();
            return 1;
        }*/
        else if (command == "step")
        {
            return 1;
            //Do nothig
        }
        else if (command == "chat")
        {
            chat(cli);
        }

        return result;
    }

    bool Project::chat(const std::string& content)
    {
        pushMessage(content, "user", true);
        
        web::json::value response;
        bool success = Client::getInstance().sendRequest(getMessages(), response, nullptr);
        bool hasObject = false;
        if (success)
        {
            hasObject = Client::getInstance().project()->handleResponse(response, nullptr, false);
        }
        
        return true;
    }

    Node* Project::getNode(const std::string& path)
    {
        std::vector<std::string> nodeNames;
        boost::split(nodeNames, path, boost::is_any_of(">/"));
        if(!nodeNames.size() || !m_dag.m_root || !m_dag.m_root->m_data)
            return nullptr;
        
        if(m_dag.m_root->m_data->getName() != nodeNames[0])
        {
            return nullptr;
        }
        
        nodeNames.erase(nodeNames.begin());
        
        DAGNode<Node*>* node = getNode(m_dag.m_root, nodeNames);
        if(node)
        {
            return node->m_data;
        }
        return m_dag.m_root->m_data;
    }

    void Project::saveReferences()
    {
        json::value jsonRef = json::value::array();
        for(const auto& node : m_nodeMap)
        {
            for(auto ref : node.second->m_referencedBy)
            {
                std::string refName = node.second->getName();
                refName += ":";
                refName += ref->getName();
                auto lastIndex = jsonRef.as_array().size();
                jsonRef.as_array()[lastIndex] = json::value(conversions::to_string_t(refName));
            }
        }
        
        if(!jsonRef.size())
        {
            return;
        }
        
        std::string strJson = conversions::to_utf8string(jsonRef.serialize());
        std::string path = m_projDir + "/dag";
        try {
            boost_fs::create_directories(path);
        }
        catch (const boost_fs::filesystem_error& e) {
            std::cout << "Can't save references! Unable to create directory: " << path << std::endl;
            return;
        }
        
        path += "/references.json";
        
        std::ofstream fileJson(path);
        if (!fileJson.is_open()) {
            std::cout << "Can't save references! Unable to create file " << path << std::endl;
            return false;
        }

        fileJson << strJson << std::endl;
        fileJson.close();
    }

    void Project::loadReferences()
    {
        std::string path = m_projDir + "/dag/references.json";
        std::ifstream file(path);
        if(!file.good()) 
        {
            return;
        }
        
        std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        json::value jsonRef = json::value::parse(conversions::to_string_t(str));
        auto count = jsonRef.as_array().size();
        for(uint32_t i=0; i<count; ++i)
        {
            std::string refStr = conversions::to_utf8string(jsonRef.at(i).as_string());
            std::vector<std::string> refVec;
            boost::split(refVec, refStr, boost::is_any_of(":"));
            if(refVec.size() < 2)
            {
                //Something is wrong here
                continue;
            }
            
            Node* dependency = nullptr;
            Node* dependee = nullptr;
            
            auto itDependency = m_nodeMap.find(refVec[0]);
            if(itDependency != m_nodeMap.end())
            {
                dependency = itDependency->second;
                auto itDependee = m_nodeMap.find(refVec[1]);
                if(itDependee != m_nodeMap.end())
                {
                    dependee = itDependee->second;
                }
            }
            
            if(dependency && dependee)
            {
                dependency->m_referencedBy.insert(dependee);
            }
        }
    }

    void Project::saveStats()
    {
        uint32_t request_id = Client::getInstance().getRequestId();
        m_stats[U("request_id")] = json::value::number(request_id);
        
        std::string filePath = getProjDir() + "/dag/stats.json";
        std::ofstream fileJson(filePath);
        if (!fileJson.is_open()) {
            std::cout << "Can't save project stats! Unable to create file " << filePath << std::endl;
            return ;
        }
        
        std::string strJson = conversions::to_utf8string(m_stats.serialize());
        fileJson << strJson << std::endl;
    }

    void Project::loadStats()
    {
        std::string filePath = getProjDir() + "/dag/stats.json";
        std::ifstream file(filePath);
        
        if(file.good())
        {
            std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            m_stats = json::value::parse(conversions::to_string_t(str));
            uint32_t request_id = m_stats[U("request_id")].as_number().to_uint32();
            Client::getInstance().setRequestId(request_id);
        }
    }

    uint32_t Project::captureContext(const std::string& label)
    {
#ifndef NO_NODE_MESSAGES
        int32_t startMessage = (int32_t)getMessages().as_array().size();
        //TODO: Consider to add the name of the last prompt as a default label
        std::string useLable = !label.empty() ? label : "";
        m_msgContext.push(std::make_pair(startMessage, useLable));
        return startMessage;
#else
        return m_activeContext->tag(label);
#endif
    }

    uint32_t Project::popContext()
    {
#ifndef NO_NODE_MESSAGES
        //Project* proj = Client::getInstance().project();
        auto startMessage = m_msgContext.top().first;
        m_msgContext.pop();
        int32_t messagesPopCount = int32_t(getMessages().as_array().size() - startMessage);
        popMessages(messagesPopCount);
        return messagesPopCount;
#else
        return m_activeContext->popTag();
#endif
    }

    void Project::eraseFromContext(uint32_t startFrom, uint32_t count)
    {
        m_activeContext->erase(startFrom, count);
    }

    uint32_t Project::storeContext(uint32_t backToMessage)
    {
#ifndef NO_NODE_MESSAGES
        Project* proj = Client::getInstance().project();
        
        if(proj->getMessages().as_array().size() <= backToMessage)
        {
            return INVALID_HANDLE_ID;
        }
        
        int32_t messagesPopCount = int32_t(proj->getMessages().as_array().size() - backToMessage);
        uint32_t archiveId = generateUniqueUint32();
        m_storedMessages[archiveId] = proj->popMessages(messagesPopCount);
        
        captureContext();
        
        return archiveId;
#else
        return m_activeContext->store(backToMessage);
#endif
    }


    void Project::restoreContext(uint32_t archiveId)
    {
#ifndef NO_NODE_MESSAGES
        if(m_storedMessages.find(archiveId) == m_storedMessages.end())
        {
            return;
        }
        
        popContext();
        
        Project* proj = Client::getInstance().project();
        for(auto message : m_storedMessages[archiveId])
        {
            proj->pushMessage(message.first, message.second);
        }
        
        m_storedMessages.erase(archiveId);
#else
        m_activeContext->restore(archiveId);
#endif
    }

    const web::json::value& Project::getMessages() const
    {
#ifndef NO_NODE_MESSAGES
        return m_messages;
#else
        return m_activeContext->getMessages();
#endif
    }

    Context* Project::setActiveContext(Context* ctx)
    {
        //Let's stop and give a chance for step-by-step execution again
        //Client::getInstance().stop();
        
        Context* previous = m_activeContext;
        m_activeContext = ctx;
        return previous;
    }

    Context* Project::switchToCompileContext()
    {
        //m_activeContext = &m_compileContext;
        return setActiveContext(&m_compileContext);
    }

    std::string Project::getInstrumentationMessage(web::json::value& schema)
    {
        std::string instrumentedMessage;
        instrumentedMessage += "\n\nProvide in your response only one top-level JSON formatted object defined by the following JSON Schema. In the JSON object return only the fields specified in the JSON Schema. ALL fields must be presented, including those not explicitly marked as required in the JSON Schema. If a field is not needed, you can leave the value empty";
        instrumentedMessage += "! For hints how to set fields you can check the descriptions in the JSON Schema";
        instrumentedMessage += ":\n";
        instrumentedMessage += utility::conversions::to_utf8string(schema[U("parameters")].serialize());
        return instrumentedMessage;
    }

    bool Project::inference(Cache& cache,
                            const std::string& message,
                            web::json::value& schema,
                            web::json::value& object,
                            bool pushSchema)
    {
        Project* proj = Client::getInstance().project();
        
        //We don't want the JSON Schema after this request to stay in the contex, only the prompt message.
        //Capture here and pop later and push only the prompt
        captureContext("");
        std::string instrumentedMessage = message;
        web::json::value schemas = web::json::value::array();
        //if (object)
        {
            auto& schemasArray = schemas.as_array();
            schemasArray[0] = schema;
            
            if(!Client::getInstance().supportsFunctionCalls() && pushSchema)
            {
                instrumentedMessage += getInstrumentationMessage(schema);
            }
            else
            {
                instrumentedMessage += "\nAll fields from the JSON Schema are required and must be provided in the requested JSON!\n";
            }
        }

        web::json::value jsonObject;

        //Immediately print the original message
        std::cout << ">> user" << std::endl << std::endl;
        std::cout << message << std::endl << std::endl;
        
        pushMessage(instrumentedMessage, "user", false);
        bool success = false;
        
        success = cache.getObject(schema, jsonObject);
        
        std::string content;
        if(!success)
        {
            //cache = "na";
            success = proj->sendRequest(&schemas, content, &jsonObject);
        }
        else
        {
            content = utility::conversions::to_utf8string( jsonObject.serialize() );
        }
        
        popContext();
        if (!success) return success;

        //if (object)
        {
            proj->pushMessage(message, "user", false);
            
            object = jsonObject;
            
            std::string json = findJson(content, true);
            if(!json.empty())
            {
                json = formatJson(json, "  ");
                std::string::size_type start_pos, end_pos;
                SourceExtraction result = findSourceSection(content, start_pos, end_pos, std::string("json"));
                if(result == SourceExtraction::Normal)
                {
                    std::string::size_type len = end_pos - start_pos;
                    content.replace(start_pos, len, json);
                }
                else
                {
                    content = json;
                }
            }
            
            pushMessage(content, "assistant", true);
        }

        return Client::getInstance().processUserInput();
    }

    bool Project::inference(Cache& cache, const std::string& message, std::string& source, bool* truncated)
    {
        Project* proj = Client::getInstance().project();
        bool review = source == "review";
        std::string srcType = review ? std::string() : source;

        std::string instrumentedMessage = message;
        if(!review)
        {
            instrumentedMessage += "\n\nProvide the '" + srcType + "' source in your response in single section with markdown syntax. Prefix the source with ```" + srcType;
            instrumentedMessage += " and postfix the source in your response with ```\n\n";
        }
            
        pushMessage(instrumentedMessage, "user", true);

        bool success = false;
        std::string content;

        success = cache.getSource(source);
        if(success && !review)
        {
            content = source;
            pushMessage(content, "assistant", true);
        }
        
        if(review)
        {
            if(success)
            {
                source = "YES: it has been read from cache";
                proj->pushMessage(source, "assistant", true);
                return true;
            }
            else
            {
                source = "";
            }
        }
        
        uint32_t truncatedAttempts = 0;
        bool warnForMultipleSections = false;
        int attempts = 0;
        while(!success && attempts++ < 10)
        {
            web::json::value response;
            success = Client::getInstance().sendRequest(proj->getMessages(), response, nullptr);
            if (response.has_field(U("message")))
            {
                if (response[U("message")].has_field(U("content")))
                {
                    content = conversions::to_utf8string(response[U("message")][U("content")].as_string());
                    bool isTruncated = false;
                    //Some prompts ask LLM to review existing source and to provide update only if not actual
                    
                    SourceExtraction srcResult;
                    if(srcType == "cpp" && startsWithIgnoreCase(content, "ACTUAL"))
                    {
                        srcResult = findSource(content, source, srcType);
                        isTruncated = srcResult == SourceExtraction::Truncated;
                        if(srcResult == SourceExtraction::None)
                        {
                            source = "ACTUAL";
                            srcResult = SourceExtraction::Actual;
                        }
                    }
                    else if(srcType == "cpp" && startsWithIgnoreCase(content, "SKIP"))
                    {
                        source = "SKIP";
                        srcResult = SourceExtraction::Skip;
                    }
                    else
                    {
                        srcResult = findSource(content, source, srcType);
                        isTruncated = srcResult == SourceExtraction::Truncated;
                    }
                    
                    if(truncated) {
                        *truncated = isTruncated;
                        if(isTruncated)
                        {
                            truncatedAttempts++;
                            if(truncatedAttempts >= 3)
                            {
                                if(source.empty() || source == srcType) {
                                    source = content;
                                }
                                
                                break;
                            }
                        }
                    }
                    
                    success = !isTruncated && !source.empty();
                    
                    pushMessage(content, "assistant", true);
                    if(!warnForMultipleSections && srcResult == SourceExtraction::Multiple)
                    {
                        warnForMultipleSections = true;
                        
                        pushMessage("Multiple source markdown sections found in the reponse. All source fragments must be in a single markdown section!", "user", true);
                        
                        bool loop = Client::getInstance().processUserInput();
                        if(!loop)
                        {
                            pushMessage("Let's skip this step for now", "user", true);
                            break;
                        }
                    }
                    else if(!success)
                    {
                        //Let's pop previous message, who knows what's there. We want to save context space
                        popMessages(1);
                        
                        if(review)
                        {
                            pushMessage("Unexpected or missing response. Please provide it again!", "user", true);
                        }
                        else
                        {
                            std::string srcErrorMessage = "Unable to extract source or the \"ACTUAL\" hint from the response. Please provide it again!";
                            if(isTruncated && !source.empty())
                            {
                                uint32_t srcCount = (uint32_t)countLines(source);
                                if(srcCount > 150)
                                {
                                    srcErrorMessage += "\nThe returned response is " + std::to_string(srcCount) + " lines long. Maybe it is too much? Usually snippets are < 150 lines!";
                                }
                            }
                            
                            pushMessage(srcErrorMessage, "user", true);
                        }
                        
                        bool loop = Client::getInstance().processUserInput();
                        if(!loop)
                        {
                            pushMessage("Let's skip this step for now", "user", true);
                            break;
                        }
                    }
                }
            }
        }

        return Client::getInstance().processUserInput();
    }

    bool Project::inference(Cache& cache, const std::string& question, bool enforceBinaryAnswer, std::string& response, bool defaultResponse)
    {
        response = "review";
        bool truncated=false;
        inference(cache, question, response, nullptr);
        bool Yes = startsWithIgnoreCase(response, "YES");
        bool No = startsWithIgnoreCase(response, "NO");
        
        int attempt = 1;
        const int maxAttempts = 5;
        
        while(enforceBinaryAnswer && !Yes && !No && attempt < maxAttempts)
        {
            response = "review";
            inference(cache, "You must start your response with YES or NO", response, nullptr);
            
            Yes = startsWithIgnoreCase(response, "YES");
            No = startsWithIgnoreCase(response, "NO");
            
            attempt++;
        }
        
        if(enforceBinaryAnswer && !Yes && !No)
        {
            return defaultResponse;
        }
        
        return Yes;
    }

    std::vector<Node*> Project::getNodesForPath(const std::string& dagPath)
    {
        std::vector<Node*> nodes;
        
        Node* node = getNode(dagPath);
        while(node)
        {
            nodes.insert(nodes.begin(), node);
            if(node->m_this && node->m_this->m_parent)
            {
                node = node->m_this->m_parent->m_data;
            } else {
                node = nullptr;//no parent node
            }
        }
        
        return nodes;
        
    }

    std::set<std::string> Project::getNodeNames() const
    {
        std::set<std::string> nodeNames;
        
        for (const auto& kv : m_nodeMap) {
            nodeNames.insert(kv.first);
        }
        
        return nodeNames;
    }
}
