#pragma once

#include <typeindex>
#include "IncludeBoost.h"
#include "File.h"
#include "Graph.h"
#include "Utils.h"

#include "Inferencing.h"

using namespace utility; // For conversions

namespace hen {

    class Node;

    struct CaseInsensitiveLess
    {
        bool operator()(const std::string &lhs, const std::string &rhs) const
        {
            return std::lexicographical_compare(
                lhs.begin(), lhs.end(),
                rhs.begin(), rhs.end(),
                [](unsigned char c1, unsigned char c2)
                {
                    return std::tolower(c1) < std::tolower(c2);
                }
            );
        }
    };

    class CodeProject : public Reflection<CodeProject>
    {
    public:
        DECLARE_TYPE(CodeProject, "Description")
        
        DECLARE_FIELD(std::string, name, "Description")
        DECLARE_FIELD(std::string, func_name, "Description")
        DECLARE_FIELD(std::string, brief, "Description")
        DECLARE_FIELD(std::string, type, "Description")
        DECLARE_FIELD(std::string, entry_point, "Description")
        DECLARE_FIELD(FileName, role, "Description")
        DECLARE_FIELD(FileName, sample_description, "Description")
        
        std::string description;
    };

	class Project : public Reflection<Project>
	{
    public:

        std::unordered_map<std::type_index, std::shared_ptr<void>> pools;

        template <typename T>
        boost::object_pool<T>& getPool() {
            auto type = std::type_index(typeid(T));
            if (pools.find(type) == pools.end()) {
                auto pool = std::make_shared<boost::object_pool<T>>();
                pools[type] = pool;
            }
            return *static_cast<boost::object_pool<T>*>(pools[type].get());
        }

        template <typename T, typename... Args>
        T* construct(Args&&... args) {
            return getPool<T>().construct(std::forward<Args>(args)...);
        }

        template <typename T>
        void destroy(T* object) {
            getPool<T>().destroy(object);
        }

        template <typename T>
        T* shareNode(const std::string& name, const Node* parent);
        
        template <typename T>
        DAGNode<T>* getNode(DAGNode<T>* parent, std::vector<std::string>& nodeNames);

	protected:
        
        DAGraph<Node*> m_dag;
        
#ifndef NO_NODE_MESSAGES
		web::json::value m_messages;
#else
        Context     m_compileContext;
        Context*    m_activeContext;
#endif
        
        web::json::value m_schemas;
        web::json::value m_stats;
        
        std::map<std::string, Node*, CaseInsensitiveLess> m_nodeMap;
        std::string m_projDir;
        
        void clear();
        
    public:
        Project():
        //m_dag(this),
        m_schemas(web::json::value::array()),
#ifndef NO_NODE_MESSAGES
        m_messages(web::json::value::array()),
#else
        m_activeContext(&m_compileContext),
#endif
        m_save(true),
        m_cache(true),
        m_decompose(true),
        m_integrate(true),
        m_brainstorm(true),
        m_reorder(false)
        {
        }
        
	public:
        bool m_save;
        bool m_cache;
        bool m_decompose;
        bool m_integrate;
        bool m_brainstorm;
        bool m_reorder;
        
		DECLARE_TYPE(Project, "Description")
        
        CodeProject m_description;

        virtual const web::json::value& getMessages() const;

        virtual const web::json::value* getSchemas() { return &m_schemas; }
        
        virtual Node* setup(const std::string& projectDir)=0;
        void setup(const std::string& projectDir, Node* root);
        const std::string& getProjDir() const { return m_projDir; }
        virtual void buildHierarchy(DAGNode<Node*>* root);
        virtual void onDeleteHierarchy(DAGNode<Node*>* root);
        virtual void traversal();
        virtual void save(const std::string& directory, bool overwrite);
        virtual void load();
        virtual void reload();
        virtual bool handleResponse(web::json::value& response, web::json::value* object = nullptr, bool print=true);
        bool sendRequest(web::json::value* schemas, std::string& content, web::json::value* object);
        uint32_t pushMessage(const std::string& content, const std::string& role, bool print);
        std::vector<std::pair<std::string, std::string>> popMessages(uint32_t count);
        virtual int executeCommand(const std::string& command, const std::string& cli, const boost_opt::variables_map& args);
        const std::map<std::string, Node*, CaseInsensitiveLess>& nodeMap() const {return m_nodeMap;}
        bool chat(const std::string& content);
        Node* getNode(const std::string& path);
        std::vector<Node*> getNodesForPath(const std::string& dagPath);
        
        virtual void saveReferences();
        virtual void loadReferences();
        
        virtual void saveStats();
        virtual void loadStats();
        
        virtual void setupBuild() = 0;
        virtual void finalizeBuild() = 0;
        
        uint32_t captureContext(const std::string& label);
        uint32_t popContext();
        
        uint32_t storeContext(uint32_t backToMessage);
        void restoreContext(uint32_t archiveId);
        void eraseFromContext(uint32_t startFrom, uint32_t count);
        
        Context* setActiveContext(Context* ctx);
        Context* switchToCompileContext();
        
        std::string getInstrumentationMessage(web::json::value& schema);
        
        template <typename T>
        bool inference(Cache& cache, const std::string& message, T* object = nullptr);
        bool inference(Cache& cache,
                       const std::string& message,
                       web::json::value& schema,
                       web::json::value& object,
                       bool pushSchema = true);
        
        bool inference(Cache& cache, const std::string& message, std::string& source, bool* truncated);
        
        bool inference(Cache& cache, const std::string& question, bool enforceBinaryAnswer, std::string& response, bool defaultResponse);
        
        std::set<std::string> getNodeNames() const;
	};

}
