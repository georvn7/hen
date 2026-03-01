#pragma once
#include "IncludeBoost.h"
#include "Reflection.h"
#include "Graph.h"
#include "Utils.h"
#include "Inferencing.h"

namespace hen {

class Project;

//this is what the base project sees as a node interface
class Node
{
private:
    void getDAGPath(std::string& name, const std::string& separator) const;
public:
    
    DAGNode<Node*>* m_this;
    
#ifndef NO_NODE_MESSAGES
    std::stack<std::pair<int32_t,std::string>> m_msgContext;
    std::map<uint32_t, std::vector<std::pair<std::string,std::string>>> m_storedMessages;
#endif
    
    std::set<const Node*> m_referencedBy;

protected:
    //TODO: Handle synonyms!!!
    std::string m_name;
    
#ifndef NO_NODE_MESSAGES
    web::json::value m_messages;
#endif
    
    web::json::value m_schemas;
    
protected:
    template <typename T>
    void setSchema()
    {
        web::json::value schema;
        hen::setupSchema<T>(schema);

        setSchemas(schema);
    }

    template <typename T>
    bool inference(std::string& cache, const std::string& message, T* object = nullptr);
    bool inference(std::string& cache, const std::string& message, std::string& source, bool* truncated=nullptr);
    bool inference(std::string& cache, const std::string& question, bool enforceBinaryResponse, bool defaultResponse=true);
    
    void cleanDirectory(const std::string& directory);
    bool saveJson(const web::json::value& json, const std::string& path);
    bool loadJson(web::json::value& json, const std::string& path);
    template <typename T>
    bool loadFromJson(const std::string& path, T* object);
    bool nodeFileExists(const std::string& fileName) const;
    
public:

    virtual ~Node() {}
    Node():
    m_this(nullptr),
#ifndef NO_NODE_MESSAGES
    m_messages(web::json::value::array()),
#endif
    m_schemas(web::json::value::array())
    {
    }
    
    void setName(const std::string& name) {m_name = name;}
    const std::string& getName() const {return m_name;}
    void setSchemas(const web::json::value& schemas);
#ifndef NO_NODE_MESSAGES
    void addMessage(const web::json::value& message);
#endif
    virtual void pushMessage(const std::string& content, const std::string& role="user");
    virtual void popMessages(uint32_t count);
    virtual void decompose() = 0;
    virtual void integrate() = 0;
    virtual void save() = 0;
    virtual void load() = 0;
    virtual void build() = 0;
    virtual void preBuild() = 0;
    virtual void test() = 0;
    virtual void onDelete() = 0;
    virtual std::string summarize(bool brief) const = 0;
    std::string getDAGPath(const std::string& separator) const;
    uint32_t getDepth() const;
    void getDAGPathNodes(bool excludeSelf, std::vector<Node*>& nodes);
    std::string getNodeDirectory(const std::string& projDir) const;
    std::string getNodeDirectory() const;
    uint32_t captureContext(const std::string& label = std::string());
    uint32_t popContext();
    uint32_t storeContext(uint32_t backToMessage);
    void restoreContext(uint32_t archiveId);
    Node* getChild(const std::string& name);
    uint32_t refCount() const { return (uint32_t)m_referencedBy.size(); }
    uint32_t releaseReference(const Node* reference);
};

}
