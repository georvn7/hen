#pragma once

#include <vector>
#include <functional>

namespace hen {

template <typename T>
DAGNode<T>* DAGNode<T>::getNextChild(std::set<DAGNode<T>*>& visited)
{
    for (auto* child : m_children)
    {
        if(visited.find(child) == visited.end())
        {
            visited.insert(child);
            return child;
        }
    }
    
    return nullptr;
}

template <typename T>
void DAGraph<T>::depthFirstTraversal(DAGNode<T>* node,
                                  std::function<void(DAGNode<T>*, DAGraph<T>&)> visitCallback,
                                  std::function<void(DAGNode<T>*)> postVisitCallback)
{
    if (!node) return;

    if(node->m_data)
    {
        node->m_data->captureContext();
    }
    visitCallback(node, *this); // Callback when visiting the node, with access to the graph

    for (DAGNode<T>* child : node->m_children) {
        if(child->m_enalbedForTraversal)
        {
            depthFirstTraversal(child, visitCallback, postVisitCallback);
        }
    }

    postVisitCallback(node); // Callback after visiting all children
    if(node->m_data)
    {
        node->m_data->popContext();
    }
}

template <typename T>
void DAGraph<T>::depthFirstTraversal(DAGNode<T>* node, std::function<void(DAGNode<T>*, DAGraph<T>&)> visitCallback)
{
    depthFirstTraversal(node, visitCallback, [](DAGNode<T>* node) {});
}

template <typename T>
void DAGraph<T>::depthFirstTraversalWithRefs(DAGNode<T>* node,
                                             std::function<void(DAGNode<T>*, DAGraph<T>&)> visitCallback,
                                             std::function<void(DAGNode<T>*)> postVisitCallback)
{
    if (!node) return;

    if(node->m_data)
    {
        node->m_data->captureContext();
    }
    visitCallback(node, *this); // Callback when visiting the node, with access to the graph

    for (DAGNode<T>* child : node->m_children) {
        //Enable traversal only fot the first reference
        if(child->m_enalbedForTraversal || node->m_enalbedForTraversal)
        {
            depthFirstTraversal(child, visitCallback, postVisitCallback);
        }
    }

    postVisitCallback(node); // Callback after visiting all children
    if(node->m_data)
    {
        node->m_data->popContext();
    }
}

template <typename T>
void DAGraph<T>::depthFirstTraversalWithRefs(DAGNode<T>* node, std::function<void(DAGNode<T>*, DAGraph<T>&)> visitCallback)
{
    depthFirstTraversalWithRefs(node, visitCallback, [](DAGNode<T>* node) {});
}

}
