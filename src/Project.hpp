#pragma once

#include "Project.h"
#include "Node.h"

using namespace utility;

namespace hen {

template <typename T>
T* Project::shareNode(const std::string& name, const Node* parent)
{
    auto it = m_nodeMap.find(name);
    if (it != m_nodeMap.end())
    {
        //For nodes that already exists we creat new DAG node, but we reference the existing hen::Node
        //Not sure how this works, but for now seems as the right thing to do
        if(parent && it->second->m_referencedBy.find(parent) == it->second->m_referencedBy.end())
        {
            auto newDAGNode = m_dag.newNode(parent->m_this, it->second);
            it->second->m_referencedBy.insert(parent);
            
            //Do not traverse nodes that reference existing data
            newDAGNode->m_enalbedForTraversal = false;
        }
        
        //return static_cast<T*>(it->second);
        
        T* typed = dynamic_cast<T*>(it->second);
        if (!typed) throw std::runtime_error("shareNode type mismatch for: " + name);
        return typed;
    }
    else
    {
        T* node = construct<T>();
        assert(name.length() > 0);
        node->setName(name);
        node->m_this = m_dag.newNode(parent ? parent->m_this : nullptr, node);
        m_nodeMap[name] = node;
        
        if(parent)
        {
            node->m_referencedBy.insert(parent);
        }
        
        return node;
    }
}

template <typename T>
DAGNode<T>* Project::getNode(DAGNode<T>* parent, std::vector<std::string>& nodeNames)
{
    auto child = parent->getChild(*nodeNames.begin());
    if(child)
    {
        nodeNames.erase(nodeNames.begin());
        if(nodeNames.size() > 0)
        {
            return getNode(child, nodeNames);
        }
    }
    
    return child;
}

template <typename T>
bool Project::inference(Cache& cache, const std::string& message, T* object)
{
    web::json::value jsonObject;
    web::json::value schema;
    
    if (object)
    {
        setupSchema<T>(schema);
    }

    bool result = inference(cache, message, schema, jsonObject);
    
    if (object)
    {
        object->from_json(jsonObject);
    }
    
    return result;
}

}
