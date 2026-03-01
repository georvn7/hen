#pragma once

#include "Reflection.h"
#include "File.h"

#include <set>

namespace hen {

template <typename T>
void printSchema(std::ostream& os, Indent& indent)
{
    os << indent << "{" << std::endl; //opne function item
    ++indent;
    os << indent << "\"name\": \"" << T::typeName() << "\"," << std::endl;
    os << indent << "\"description\": \"" << T::classDescription() << "\"," << std::endl;
    os << indent << "\"parameters\": {" << std::endl;
    ++indent;
    os << indent << "\"type\": \"object\"," << std::endl;
    os << indent << "\"properties\": {" << std::endl;
    ++indent;
    T::print_schema(os, indent);
    --indent;
    os << indent << "}" << std::endl; //close properties
    --indent;
    os << indent << "}" << std::endl; //close parameters
    --indent;
    os << indent << "}" << std::endl; //close function item
}

template <typename T>
void setupSchema(web::json::value& schema)
{
    std::stringstream os;
    Indent indent("    ");
    printSchema<T>(os, indent);
    auto str = utility::conversions::to_string_t(os.str());
    schema = web::json::value::parse(str);
}

class Context
{
    web::json::value m_messages;
    
    std::stack<std::pair<int32_t,std::string>> m_msgContext;
    std::map<uint32_t, std::vector<std::pair<std::string,std::string>>> m_storedMessages;
    
public:
    
    Context():m_messages(web::json::value::array()) {}
    Context(const std::string& messages);
    Context(web::json::value& messages);
    Context(const Context& othre);
    
    uint32_t add(const std::string& content, const std::string& role);
    std::vector<std::pair<std::string, std::string>> pop(uint32_t messagesCount);
    
    uint32_t tag(const std::string& label = std::string());
    uint32_t popTag();
    
    uint32_t store(uint32_t backToMessage);
    void restore(uint32_t archiveId);
    
    void reset();
    
    uint32_t erase(uint32_t startFrom, uint32_t count);
    
    const web::json::value& getMessages() const { return m_messages; }
    
    const std::string& str() const;
    operator std::string() const { return str(); }
};

class Prompt
{
    static std::set<std::string> m_searchPaths;
    static std::map<std::string, std::string> m_cache;
    
    std::string m_fileNameNoExt;
    std::string m_content;
    
public:
    static void addSearchPath(const std::string& path);
    static void clearSearchPaths();
    Prompt(const std::string& filePath, const std::map<std::string, std::string>& params);
    
    const std::string& str() const;
    operator std::string() const;
};

class Cache
{
    std::string m_path;
    std::string m_entry;
    bool m_available;
public:
    Cache():m_available(false) {}
    Cache(const std::string& path, const std::string& entry);
    bool getObject(web::json::value& schema, web::json::value& object);
    bool getObject(web::json::value& object);
    bool getSource(std::string& source);
    bool available();
};

}
