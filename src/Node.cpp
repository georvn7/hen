#include "Node.h"
#include "Client.h"
#include "Project.h"
#include "Utils.h"

using namespace web;

namespace stdrave {

#ifndef NO_NODE_MESSAGES
void Node::addMessage(const web::json::value& message)
{
    auto& messagesArray = m_messages.as_array();
    auto size = messagesArray.size();
    messagesArray[size] = message;
}
#endif

void Node::setSchemas(const web::json::value& schemas)
{
    auto& schemasArray = m_schemas.as_array();
    schemasArray[0] = schemas;
}

void Node::pushMessage(const std::string& content, const std::string& role)
{
    Project* proj = Client::getInstance().project();
#ifndef NO_NODE_MESSAGES
    auto& json = proj->pushMessage(content, role);
    addMessage(json);
#else
    proj->pushMessage(content, role, true);
#endif
}

void Node::popMessages(uint32_t count)
{
    //TODO: Ensure this works fine with captureContext/popContext
    
    Project* proj = Client::getInstance().project();
    proj->popMessages(count);
}

bool Node::inference(std::string& cache, const std::string& message, std::string& source, bool* truncated)
{
    Project* proj = Client::getInstance().project();
    
    Cache _cache(getNodeDirectory(), cache);
    bool result = proj->inference(_cache, message, source, truncated);
    if(!_cache.available())
    {
        cache = "na";
    }
    return result;
}

bool Node::inference(std::string& cache, const std::string& question, bool enforceBinaryResponse, bool defaultResponse)
{
    Project* proj = Client::getInstance().project();
    
    Cache _cache(getNodeDirectory(), cache);
    std::string response;
    bool result = proj->inference(_cache, question, enforceBinaryResponse, response, defaultResponse);
    if(!_cache.available())
    {
        cache = "na";
    }
    return result;
}

void Node::getDAGPath(std::string& name, const std::string& separator) const
{
    std::string path = m_name;
    if(!name.empty()) {
        path += separator;
        path += name;
    }
    name = path;
    if(m_this && m_this->m_parent)
    {
        Node* parent = (Node*)m_this->m_parent->m_data;
        assert(parent);
        if(parent)
        {
            parent->getDAGPath(name, separator);
        }
    }
}

std::string Node::getDAGPath(const std::string& separator) const
{
    std::string path;
    getDAGPath(path, separator);
    return path;
}

void Node::getDAGPathNodes(bool excludeSelf, std::vector<Node*>& nodes)
{
    if(!excludeSelf)
    {
        nodes.push_back(this);
    }
    
    if(m_this->m_parent)
    {
        Node* parent = (Node*)m_this->m_parent->m_data;
        assert(parent);
        if(parent)
        {
            parent->getDAGPathNodes(false, nodes);
        }
    }
}

uint32_t Node::getDepth() const
{
    uint32_t parentDepth = 0;
    if(m_this->m_parent)
    {
        Node* parent = (Node*)m_this->m_parent->m_data;
        assert(parent);
        if(parent)
        {
            parentDepth = parent->getDepth();
        }
    }
    
    return parentDepth + 1;
}

std::string Node::getNodeDirectory(const std::string& projDir) const {
    std::string dagPath = getDAGPath(">");
    std::vector<std::string> nodeNames;
    boost::split(nodeNames, dagPath, boost::is_any_of(">"));

    std::string directory = projDir;
    directory += "/dag";
    if(nodeNames.size()==0)
    {
        //This should be the root node
        directory += getFirstSubdirectory(directory);
    }
    else
    {
        for (size_t i = 0; i < nodeNames.size(); ++i) {
            directory += "/";
            directory += nodeNames[i];
            if (i < nodeNames.size() - 1)
            {
                directory += "/dag";
            }
        }
    }

    return directory;
}

std::string Node::getNodeDirectory() const
{
    Project* proj = Client::getInstance().project();
    return getNodeDirectory(proj->getProjDir());
}

bool Node::saveJson(const web::json::value& json, const std::string& path)
{
    return stdrave::saveJson(json, path);
}

bool Node::loadJson(web::json::value& json, const std::string& path)
{
    return stdrave::loadJson(json, path);
}

void Node::cleanDirectory(const std::string& directory) {
    
    if (!boost_fs::exists(directory) || !boost_fs::is_directory(directory)) {
        return;
    }

    try {
        boost_fs::directory_iterator end_iter; // Default construction yields past-the-end
        for (boost_fs::directory_iterator dir_itr(directory); dir_itr != end_iter; ++dir_itr) {
            if (boost_fs::is_regular_file(dir_itr->status())) {
                boost_fs::remove(dir_itr->path());
            }
        }
    }
    catch (const boost_fs::filesystem_error& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

uint32_t Node::captureContext(const std::string& label)
{
#ifndef NO_NODE_MESSAGES
    Project* proj = Client::getInstance().project();
    int32_t startMessage = (int32_t)proj->getMessages().as_array().size();
    //TODO: Consider to add the name of the last prompt as a default label
    std::string useLable = !label.empty() ? label : "";
    m_msgContext.push(std::make_pair(startMessage, useLable));
    return startMessage;
#else
    Project* proj = Client::getInstance().project();
    return proj->captureContext(label);
#endif
}

uint32_t Node::popContext()
{
#ifndef NO_NODE_MESSAGES
    Project* proj = Client::getInstance().project();
    auto startMessage = m_msgContext.top().first;
    m_msgContext.pop();
    int32_t messagesPopCount = int32_t(proj->getMessages().as_array().size() - startMessage);
    proj->popMessages(messagesPopCount);
    return messagesPopCount;
#else
    Project* proj = Client::getInstance().project();
    return proj->popContext();
#endif
}

uint32_t Node::storeContext(uint32_t backToMessage)
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
    Project* proj = Client::getInstance().project();
    return proj->storeContext(backToMessage);
#endif
}

void Node::restoreContext(uint32_t archiveId)
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
    Project* proj = Client::getInstance().project();
    proj->restoreContext(archiveId);
#endif
}

bool Node::nodeFileExists(const std::string& fileName) const
{
    std::string dir = getNodeDirectory();
    std::string path = dir + "/" + fileName;
    return boost_fs::exists(path);
}

Node* Node::getChild(const std::string& name)
{
    auto dagChild = m_this->getChild(name);
    if(dagChild)
    {
        return dagChild->m_data;
    }
    
    return nullptr;
}

uint32_t Node::releaseReference(const Node* reference)
{
    m_referencedBy.erase(reference);
    return (uint32_t)m_referencedBy.size();
}

}
