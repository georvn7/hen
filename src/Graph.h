#pragma once

#include <vector>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace stdrave {

    class Project;

    template<typename T>
    class DAGNode {
    public:
        T m_data;
        DAGNode<T>* m_parent;
        std::vector< DAGNode<T>* > m_children;
        bool m_enalbedForTraversal;

        DAGNode(const T& data) :
        m_data(data),
        m_parent(nullptr),
        m_enalbedForTraversal(true) {}
        ~DAGNode()
        {
            for(auto child : m_children)
            {
                delete child;
            }
            m_children.clear();
        }

        void addChild(DAGNode<T>* child) {
            m_children.push_back(child);
            child->m_parent = this;
        }
        
        void removeChild(DAGNode<T>* child)
        {
            // Find the child in the m_children vector
            auto it = std::find(m_children.begin(), m_children.end(), child);
            if (it != m_children.end())
            {
                // Remove the child from the vector
                m_children.erase(it);
                child->m_parent = nullptr;
                delete child;
            }
        }
        
        DAGNode<T>* getNextChild(std::set<DAGNode<T>*>& visited);
        
        DAGNode<T>* getChild(const std::string& name)
        {
            //TODO: Rethink the node:name association. Consider mapping names to nodes
            for(auto child : m_children)
            {
                if(child->m_data->getName() == name)
                {
                    return child;
                }
            }
            
            return nullptr;
        }
    };

    template<typename T>
    class DAGraph 
    {
    public:
    

    public:
        DAGNode<T>* m_root;

        DAGraph() : m_root(nullptr) {}

        void depthFirstTraversal(DAGNode<T>* node, std::function<void(DAGNode<T>*, DAGraph<T>&)> visitCallback,
            std::function<void(DAGNode<T>*)> postVisitCallback);
        void depthFirstTraversal(DAGNode<T>* node, std::function<void(DAGNode<T>*, DAGraph<T>&)> visitCallback);
        
        void depthFirstTraversalWithRefs(DAGNode<T>* node, std::function<void(DAGNode<T>*, DAGraph<T>&)> visitCallback,
            std::function<void(DAGNode<T>*)> postVisitCallback);
        void depthFirstTraversalWithRefs(DAGNode<T>* node, std::function<void(DAGNode<T>*, DAGraph<T>&)> visitCallback);

        DAGNode<T>* newNode(DAGNode<T>* parentNode, const T& data) {
            DAGNode<T>* newNode = new DAGNode<T>(data);
            if (parentNode)
            {
                parentNode->addChild(newNode);
            }
            return newNode;
        }
    };

    class Graph {
        public:
            // Add 'node' under 'parent'. If parent is empty, 'node' becomes a root.
            void addNode(const std::string& node, const std::string& parent) {
                
                 if (!parent.empty()) {
                     children_[parent].push_back(node);
                     roots_.erase(node);           // node is no longer a root
                 } else {
                     roots_.insert(node);          // new root
                 }

                 // Ensure every node has an entry in the children map.
                 if (children_.find(node) == children_.end()) {
                     children_[node] = {};
                 }
            }

            // 1-based depth: start node = depth 1. maxDepth limits how deep to descend.
            // If maxDepth < 1, no limit.
            void traverse(const std::string& start,
                          const std::function<void(const std::string&, int)>& func,
                          int maxDepth = -1) const
            {
                std::unordered_set<std::string> visited;
                std::unordered_set<std::string> processedFull;
                dfs(start, func, 1, maxDepth, visited, processedFull);
            }

            // Traverse from every root
            void traverseAll(const std::function<void(const std::string&, int)>& func,
                             int maxDepth = -1) const
            {
                for (const auto& root : roots_) {
                    traverse(root, func, maxDepth);
                }
            }

        private:
            void dfs(const std::string& node,
                     const std::function<void(const std::string&, int)>& func,
                     int depth,
                     int maxDepth,
                     std::unordered_set<std::string>& visited,
                     std::unordered_set<std::string>& processedFull) const
            {
                // If already fully processed, skip
                if (processedFull.count(node)) return;
                
                // Cycle detection
                if (visited.count(node)) return;
                
                visited.insert(node);
                func(node, depth);  // Always call the callback
                
                // Check if we've reached the depth limit
                if (maxDepth >= 1 && depth >= maxDepth) {
                    visited.erase(node);
                    return;  // Don't recurse further
                }

                auto it = children_.find(node);
                if (it != children_.end()) {
                    for (const auto& child : it->second) {
                        dfs(child, func, depth + 1, maxDepth, visited, processedFull);
                    }
                }
                
                // Mark as fully processed and remove from cycle detection
                processedFull.insert(node);
                visited.erase(node);
            }

            std::unordered_map<std::string, std::vector<std::string>> children_;
            std::unordered_set<std::string> roots_;
    };

    class GraphPrinter {
    public:
        //------------------------------------------------------------------
        // Insert a directed “parent → node” edge.  An empty parent denotes
        // a new root.  Multiple parents per node are allowed.
        //------------------------------------------------------------------
        void addNode(const std::string& node, const std::string& parent)
        {
            if (!parent.empty()) {
                children[parent].push_back(node);
                roots.erase(node);           // node is no longer a root
            } else {
                roots.insert(node);          // new root
            }

            // Ensure every node has an entry in the children map.
            if (children.find(node) == children.end()) {
                children[node] = {};
            }
        }
        
        //------------------------------------------------------------------
        // Pretty-print starting from a specific node (instead of all roots)
        //------------------------------------------------------------------
        std::string print(const std::string& startNode, int maxDepth = -1) const
        {
            std::ostringstream oss;
            printedFull.clear();  // reset between calls
            
            // Check if the node exists
            if (children.find(startNode) == children.end()) {
                oss << "Node '" << startNode << "' not found in graph\n";
                return oss.str();
            }
            
            std::set<std::string> visited;
            printSubtree(startNode, /*prefix=*/"", /*isLast=*/true, oss, visited, maxDepth, 0);
            
            return oss.str();
        }

        //------------------------------------------------------------------
        // Pretty‑print *every* root and its full subtree (may repeat shared
        // subtrees unless they have already been shown once).
        //------------------------------------------------------------------
        std::string print(int maxDepth = -1) const
        {
            std::ostringstream oss;
            printedFull.clear();              // reset between calls

            bool firstRoot = true;
            for (const auto& r : roots) {
                std::set<std::string> visited;
                printSubtree(r, /*prefix=*/"", /*isLast=*/true, oss, visited, maxDepth, 0);
                if (!firstRoot) oss << '\n';
                firstRoot = false;
            }
            return oss.str();
        }

        //------------------------------------------------------------------
        // Pretty‑print only the hierarchy **from `root` down to any node in
        // `endNodes`**, collapsing every other branch to a single “…”.
        //------------------------------------------------------------------
        std::string print(const std::string&           root,
                          const std::set<std::string>& endNodes,
                          const std::string&           hint) const
        {
            std::ostringstream oss;
            if (children.find(root) == children.end()) return oss.str();

            // Pass 1 – memoised DFS: does each subtree contain an end‑node?
            std::unordered_map<std::string, bool> hasEnd;
            std::unordered_set<std::string>       recursionStack;
            containsEnd(root, endNodes, hasEnd, recursionStack);

            // Pass 2 – selective pretty‑printer.
            std::set<std::string> visited;
            printSubtreeSelective(root, "", /*isLast=*/true,
                                  oss, endNodes, hint, hasEnd, visited);
            return oss.str();
        }

    private:
        //------------------------------------------------------------------
        // Data
        //------------------------------------------------------------------
        std::map<std::string, std::vector<std::string>> children;  // parent → kids
        std::set<std::string>                           roots;     // forest roots
        mutable std::set<std::string>                   printedFull; // for full print

        //------------------------------------------------------------------
        // Full‑tree printer with cycle detection and subtree‑deduplication.
        //------------------------------------------------------------------
        void printSubtree(const std::string& node,
                          const std::string& prefix,
                          bool               isLast,
                          std::ostringstream& oss,
                          std::set<std::string>& visited,
                          int                maxDepth,
                          int                currentDepth) const
        {
            // 1) Already printed this entire subtree once → collapse.
            if (printedFull.count(node)) {
                oss << prefix << (isLast ? "└── " : "├── ")
                    << node << " …\n";
                return;
            }

            // 2) Cycle detection.
            if (visited.count(node)) {
                oss << prefix << (isLast ? "└── " : "├── ")
                    << node << " (cycle)\n";
                return;
            }

            // 3) Check depth limit
            bool atDepthLimit = (maxDepth > 0 && (currentDepth + 1) >= maxDepth);
            
            // 4) Print the node itself.
            visited.insert(node);
            oss << prefix << (isLast ? "└── " : "├── ") << node;
            
            // Add "+" suffix if at depth limit and node has children
            if (atDepthLimit) {
                const auto& kids = children.at(node);
                if (!kids.empty()) {
                    oss << " +";
                }
                oss << '\n';
                visited.erase(node);
                return;  // Don't recurse further
            }
            
            oss << '\n';

            // 5) Recurse into children.
            const auto& kids = children.at(node);
            std::string childPrefix = prefix + (isLast ? "    " : "│   ");
            for (std::size_t i = 0; i < kids.size(); ++i) {
                bool kidIsLast = (i + 1 == kids.size());
                printSubtree(kids[i], childPrefix, kidIsLast, oss, visited, maxDepth, currentDepth + 1);
            }

            // 6) Mark subtree as printed; backtrack cycle guard.
            printedFull.insert(node);
            visited.erase(node);
        }

        //------------------------------------------------------------------
        // Pass 1 helper: memoised “does this subtree contain an end‑node?”
        //------------------------------------------------------------------
        bool containsEnd(const std::string&                     node,
                         const std::set<std::string>&           endNodes,
                         std::unordered_map<std::string, bool>& memo,
                         std::unordered_set<std::string>&       stack) const
        {
            // Cycle guard.
            if (stack.count(node))         return false;

            // Memoised result?
            auto it = memo.find(node);
            if (it != memo.end())          return it->second;

            // Direct hit.
            if (endNodes.count(node)) {
                memo[node] = true;
                return true;
            }

            // Recurse into children.
            stack.insert(node);
            bool found = false;
            auto kidIt = children.find(node);
            if (kidIt != children.end()) {
                for (const auto& k : kidIt->second) {
                    if (containsEnd(k, endNodes, memo, stack)) {
                        found = true;
                        break;
                    }
                }
            }
            stack.erase(node);
            memo[node] = found;
            return found;
        }

        //------------------------------------------------------------------
        // Pass 2 helper: selective printer that collapses “uninteresting”
        // branches (those whose subtree does NOT contain an end‑node).
        //------------------------------------------------------------------
        void printSubtreeSelective(
                    const std::string&                          node,
                    const std::string&                          prefix,
                    bool                                        isLast,
                    std::ostringstream&                         oss,
                    const std::set<std::string>&                endNodes,
                    const std::string&                          hint,
                    const std::unordered_map<std::string, bool>&hasEnd,
                    std::set<std::string>&                      visited) const
        {
            // 1) Print this node (decorate if it is an end‑node).
            oss << prefix << (isLast ? "└── " : "├── ") << node;
            bool isEnd = endNodes.count(node);
            if (isEnd) oss << " (" << hint << ")";
            oss << '\n';
            if (isEnd) return;                      // stop expansion

            // 2) Cycle guard.
            if (visited.count(node)) {
                oss << prefix << (isLast ? "    " : "│   ")
                    << "└── (cycle)\n";
                return;
            }
            visited.insert(node);

            // 3) Recurse or collapse.
            auto it = children.find(node);
            if (it != children.end()) {
                const auto& kids = it->second;
                std::string childPrefix = prefix + (isLast ? "    " : "│   ");

                for (std::size_t i = 0; i < kids.size(); ++i) {
                    const auto& k  = kids[i];
                    bool kidIsLast = (i + 1 == kids.size());

                    auto hIt = hasEnd.find(k);
                    bool kidHasEnd = (hIt != hasEnd.end()) && hIt->second;

                    if (!kidHasEnd) {               // collapse here
                        oss << childPrefix << (kidIsLast ? "└── " : "├── ")
                            << k << " …\n";
                        continue;
                    }
                    printSubtreeSelective(k, childPrefix, kidIsLast,
                                          oss, endNodes, hint, hasEnd, visited);
                }
            }
            visited.erase(node);
        }
    };

    struct CallGraph {
        using CallerSet = std::unordered_set<std::string>;

        // Your stored reverse edges: callee -> { callers }
        std::unordered_map<std::string, CallerSet> callers_of;

        // (Optional) thread-safety if you mutate/scan concurrently
        mutable std::mutex m;

        // Add one discovered edge: caller -> callee
        void add_call(const std::string& caller, const std::string& callee) {
            if (caller == callee) return; // ignore self-loop if you like
            std::lock_guard<std::mutex> lk(m);
            callers_of[callee].insert(caller);
        }

        // Core query:
        // Returns true if rootCaller can (directly/indirectly) call callee.
        //
        // fetch: callable with signature
        //   CallerSet fetch(const std::string& callee)
        // It should return any additional callers not yet in callers_of[callee].
        // If you have nothing extra, pass a lambda that returns {}.
        template <class Fetch>
        bool is_called_from(const std::string& callee,
                            const std::string& rootCaller,
                            Fetch fetch) const
        {
            if (callee == rootCaller) return true;

            std::unordered_set<std::string> visited;
            visited.insert(callee);
            std::deque<std::string> q(1, callee);

            while (!q.empty()) {
                std::string cur = q.front(); q.pop_front();

                // 1) snapshot known callers (don’t hold the lock while visiting)
                CallerSet known;
                {
                    std::lock_guard<std::mutex> lk(m);
                    auto it = callers_of.find(cur);
                    if (it != callers_of.end()) known = it->second;
                }

                // helper to push a caller
                auto push = [&](const std::string& c) -> bool {
                    if (c == rootCaller) return true;
                    if (visited.insert(c).second) q.push_back(c);
                    return false;
                };

                // 2) visit callers we’ve already stored
                for (const auto& c : known) {
                    if (push(c)) return true;
                }

                // 3) visit extra callers supplied by fetch(callee)
                CallerSet extra = fetch(cur); // may be empty
                for (const auto& c : extra) {
                    if (known.count(c)) continue; // skip duplicates
                    if (push(c)) return true;
                }
            }
            return false;
        }

        // Convenience overload when you don’t want a provider
        bool is_called_from(const std::string& callee, const std::string& rootCaller) const {
            auto noop = [](const std::string&) -> CallerSet { return {}; };
            return is_called_from(callee, rootCaller, noop);
        }
    };

}
