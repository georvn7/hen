#include "Inferencing.h"
#include "Utils.h"

#include "Client.h"
#include "Project.h"

using namespace web;
using namespace utility;

namespace stdrave {

static web::json::value asJson(const std::string& utf8Str)
{
    string_t uStr = utility::conversions::to_string_t(utf8Str);
    return web::json::value::parse(uStr);
}

static std::string asString(web::json::value& json)
{
    auto uStr = json.serialize();
    return utility::conversions::to_utf8string(uStr);
}

Context::Context(const std::string& messages)
{
    string_t uStr = utility::conversions::to_string_t(messages);
    m_messages = json::value::parse(uStr);
}

Context::Context(web::json::value& messages): m_messages(messages)
{
    
}

Context::Context(const Context& othre)
{
    m_messages = othre.m_messages;
    m_msgContext = othre.m_msgContext;
    m_storedMessages = othre.m_storedMessages;
}

std::vector<std::pair<std::string, std::string>> Context::pop(uint32_t messagesCount)
{
    std::vector<std::pair<std::string, std::string>> erasedMessages;
    
    if (!m_messages.is_array() || m_messages.as_array().size()==0)
        return erasedMessages;

    auto& messagesArray = m_messages.as_array();
    messagesCount = std::min(static_cast<uint32_t>(messagesArray.size()), messagesCount);

    erasedMessages.reserve(messagesCount);

    for (uint32_t i = 0; i < messagesCount; ++i)
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
}

uint32_t Context::add(const std::string& content, const std::string& role)
{
    json::value message;
    message[U("role")] = json::value::string(utility::conversions::to_string_t(role));
    message[U("content")] = json::value::string(utility::conversions::to_string_t(content));

    auto& messagesArray = m_messages.as_array();
    auto size = messagesArray.size();
    messagesArray[size] = message;
    
    return uint32_t(size+1);
}

uint32_t Context::tag(const std::string& label)
{
    int32_t startMessage = (int32_t)m_messages.as_array().size();
    //TODO: Consider to add the name of the last prompt as a default label
    std::string useLable = !label.empty() ? label : "";//Does that make sense at all?
    m_msgContext.push(std::make_pair(startMessage, useLable));
    return startMessage;
}

uint32_t Context::erase(uint32_t startFrom, uint32_t count)
{
    auto& messagesArray = m_messages.as_array();
    uint32_t messagesSize = (uint32_t)messagesArray.size();
    
    if(startFrom >= messagesSize)
    {
        //No messsages to erase
        return 0;
    }
    
    uint32_t endBefore = startFrom + std::min(messagesSize - startFrom, count);
    
    for(uint32_t m = startFrom; m < endBefore; ++m)
    {
        messagesArray.erase(startFrom);
    }
    
    return count;
}

uint32_t Context::popTag()
{
    auto startMessage = m_msgContext.top().first;
    m_msgContext.pop();
    int32_t messagesPopCount = int32_t(m_messages.as_array().size() - startMessage);
    pop(messagesPopCount);
    return messagesPopCount;
}

uint32_t Context::store(uint32_t backToMessage)
{
    if(backToMessage == INVALID_HANDLE_ID || m_messages.as_array().size() <= backToMessage)
    {
        return INVALID_HANDLE_ID;
    }
    
    int32_t messagesPopCount = int32_t(m_messages.as_array().size() - backToMessage);
    uint32_t archiveId = generateUniqueUint32();
    m_storedMessages[archiveId] = pop(messagesPopCount);
    
    tag();
    
    return archiveId;
}

void Context::restore(uint32_t archiveId)
{
    if(archiveId == INVALID_HANDLE_ID || m_storedMessages.find(archiveId) == m_storedMessages.end())
    {
        return;
    }
    
    popTag();
    
    for(auto message : m_storedMessages[archiveId])
    {
        add(message.first, message.second);
    }
    
    m_storedMessages.erase(archiveId);
}

void Context::reset()
{
    m_messages = web::json::value::array();
    m_msgContext = std::stack<std::pair<int32_t,std::string>>();
    m_storedMessages.clear();
}

const std::string& Context::str() const
{
    return utility::conversions::to_utf8string(m_messages.serialize());
}

std::set<std::string> Prompt::m_searchPaths;
std::map<std::string, std::string> Prompt::m_cache;

void Prompt::addSearchPath(const std::string& path)
{
    m_searchPaths.insert(path);
}

void Prompt::clearSearchPaths()
{
    m_searchPaths.clear();
}

Prompt::Prompt(const std::string& filePath, const std::map<std::string, std::string>& params)
{
    m_fileNameNoExt = boost_fs::path(filePath).stem().string();
    
    auto it = m_cache.find(filePath);
    std::string content;
    if(it != m_cache.end())
    {
        content = it->second;
    }
    else
    {
        auto file = openFile(filePath, m_searchPaths);
        if(file.good())
        {
            content = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            m_cache[filePath] = content;
        }
    }
    
    if(!content.empty())
    {
        m_content = buildPrompt(content, params);
    }
}

const std::string& Prompt::str() const
{
    Client::getInstance().setContextPrompt(m_fileNameNoExt);
    return m_content;
}

Prompt::operator std::string() const
{
    Client::getInstance().setContextPrompt(m_fileNameNoExt);
    return m_content;
}

Cache::Cache(const std::string& path, const std::string& entry):
m_path(path),
m_entry(entry),
m_available(false)
{
    
}

bool Cache::getObject(web::json::value& schema, web::json::value& object)
{
    Project* proj = Client::getInstance().project();
    
    if(m_path.empty() && m_entry.empty())
    {
        m_available = false;
        return m_available;
    }
    
    //bool success = false;
    m_available = false;
    std::string dir = m_path;
    //std::string path = dir + "/" + cache;
    
    std::vector<std::string> pathAndField;
    boost::split(pathAndField, m_entry, boost::is_any_of(":"));
    std::string path = makeCanonical(dir + "/" + pathAndField[0]);
    
    if(proj->m_cache && boost_fs::exists(path))
    {
        //std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        web::json::value jsonFile;
        
        if(loadJson(jsonFile, path))
        {
            if(pathAndField.size() == 1)
            {
                object = jsonFile;
            }
            else
            {
                object = findInJson(jsonFile, pathAndField[1]);
            }
            
            //Another chance if invalid json has been loaded from cache validation will force to re-send the request - cool
            const web::json::value& parameters = schema;
            
            assert(parameters.has_object_field(U("parameters")));
            if (parameters.has_object_field(U("parameters")))
            {
                const web::json::value& schema = parameters.at(U("parameters"));
                std::string jsonLog;
                m_available = validateJson(object, schema, jsonLog);
                if(!m_available)
                {
                    //pushMessage(jsonLog);
                    std::cout << jsonLog;
                }
            }
        }
    }
    
    return m_available;
}

bool Cache::getObject(web::json::value& object)
{
    Project* proj = Client::getInstance().project();
    
    if(m_path.empty() && m_entry.empty())
    {
        m_available = false;
        return m_available;
    }
    
    std::string path = m_path + "/" + m_entry;
    if(proj->m_cache && boost_fs::exists(path))
    {
        m_available = loadJson(object, path);
    }
    
    return m_available;
}

static std::vector<std::string> splitJsonField(const std::string& input, char delimiter)
{
    std::vector<std::string> result;
    std::string current;
    int delimiterCount = 0;

    for (size_t i = 0; i < input.length(); ++i) {
        if (input[i] == delimiter) {
            delimiterCount++;
        } else {
            if (delimiterCount == 1) {
                // Single delimiter, split here
                if (!current.empty()) {
                    result.push_back(current);
                    current.clear();
                }
            } else if (delimiterCount > 1) {
                // Multiple delimiters, add them to current
                current += std::string(delimiterCount, delimiter);
            }
            delimiterCount = 0;
            current += input[i];
        }
    }

    // Handle the case where the string ends with delimiters
    if (delimiterCount == 1) {
        if (!current.empty()) {
            result.push_back(current);
        }
    } else if (delimiterCount > 1) {
        current += std::string(delimiterCount, delimiter);
    }

    // Add the last part if it's not empty
    if (!current.empty()) {
        result.push_back(current);
    }

    return result;
}

bool Cache::getSource(std::string& source)
{
    Project* proj = Client::getInstance().project();
    
    if(m_path.empty() && m_entry.empty())
    {
        m_available = false;
        return m_available;
    }
    
    std::string nodeDir = m_path;
    std::vector<std::string> pathAndField = splitJsonField(m_entry, ':');
    if(pathAndField.size())
    {
        std::string path = nodeDir + "/" + pathAndField[0];
        if(proj->m_cache && boost_fs::exists(path))
        {
            std::string content;
            //std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if(pathAndField.size()==1)//The source is in separate file
            {
                content = loadFile(path);
            }
            else
            {
                //We must have a json file
                assert(pathAndField.size()==2);
                web::json::value json;
                loadJson(json, path);
                if(json.has_field(U(pathAndField[1])))
                {
                    content = json[U(pathAndField[1])].as_string();
                }
            }
            
            source = content;
            m_available = !source.empty();
        }
    }
    
    return m_available;
}

bool Cache::available()
{
    return m_available;
}

}
