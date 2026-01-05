#include "Function.h"
#include "IncludeBoost.h"

namespace stdrave {

    DEFINE_TYPE(FunctionItem)
    DEFINE_FIELD(FunctionItem, func_name)
    DEFINE_FIELD(FunctionItem, brief)
    
    DEFINE_TYPE(FunctionList)
    DEFINE_ARRAY_FIELD(FunctionList, items)
    DEFINE_FIELD(FunctionList, motivation)

    void FunctionList::updateOrder()
    {
        m_order.clear();
        for(auto function : items)
        {
            m_order += function->func_name;
            m_order += "|";
        }
    }

    void FunctionList::applyOrder()
    {
        std::vector<std::string> nodes;
        boost::split(nodes, m_order, boost::is_any_of("|"));
        
        // Create a map to store the position of each node
        std::unordered_map<std::string, size_t> nodePositions;
        for (size_t i = 0; i < nodes.size(); ++i) {
            nodePositions[nodes[i]] = i;
        }

        // Sort the items based on the position of their func_name in the nodes vector
        std::sort(items.begin(), items.end(), [&nodePositions](const auto& a, const auto& b) {
            auto posA = nodePositions.find(a->func_name);
            auto posB = nodePositions.find(b->func_name);

            // If both func_names are in the nodes vector, compare their positions
            if (posA != nodePositions.end() && posB != nodePositions.end()) {
                return posA->second < posB->second;
            }
            // If only one func_name is in the nodes vector, prioritize it
            else if (posA != nodePositions.end()) {
                return true;
            }
            else if (posB != nodePositions.end()) {
                return false;
            }
            // If neither func_name is in the nodes vector, maintain their relative order
            else {
                return false;
            }
        });
    }

    DEFINE_TYPE(LibFunctionItem)
    DEFINE_FIELD(LibFunctionItem, func_name)
    DEFINE_FIELD(LibFunctionItem, motivation)

    DEFINE_TYPE(LibFunctionList)
    //DEFINE_FIELD(LibFunctionList, caller_func)
    DEFINE_ARRAY_FIELD(LibFunctionList, items)

    DEFINE_TYPE(InfoRequest)
    DEFINE_ARRAY_FIELD(InfoRequest, functions)
    DEFINE_ARRAY_FIELD(InfoRequest, data_types)
    DEFINE_ARRAY_FIELD(InfoRequest, graph_and_brief)
    DEFINE_ARRAY_FIELD(InfoRequest, match_regex)

    bool InfoRequest::empty()
    {
        return functions.empty() && data_types.empty() && graph_and_brief.empty() && match_regex.empty();
    }

    void InfoRequest::clear()
    {
        functions.clear();
        data_types.clear();
        graph_and_brief.clear();
        match_regex.clear();
    }

	//Function
	DEFINE_TYPE(Function)
	DEFINE_FIELD(Function, brief)
    DEFINE_FIELD(Function, declaration)

    DEFINE_TYPE(FunctionDefinition)
    DEFINE_FIELD(FunctionDefinition, brief)
    DEFINE_FIELD(FunctionDefinition, description)

    DEFINE_TYPE(Code)
    DEFINE_ARRAY_FIELD(Code, externals)
    DEFINE_ARRAY_FIELD(Code, dependencies)
    DEFINE_FIELD(Code, definition)

    DEFINE_TYPE(CodeReview)
    DEFINE_FIELD(CodeReview, review)
    DEFINE_FIELD(CodeReview, errors_brief)
    DEFINE_FIELD(CodeReview, review_brief)
    DEFINE_ARRAY_FIELD(CodeReview, new_functions)
    DEFINE_ARRAY_FIELD(CodeReview, existing_functions)
    DEFINE_ARRAY_FIELD(CodeReview, unknown_types)

    void CodeReview::clear()
    {
        review.clear();
        
        errors_brief.clear();
        
        review_brief.clear();
        
        new_functions.clear();
        
        existing_functions.clear();
        
        unknown_types.clear();
    }

//*************
    DEFINE_TYPE(CodeReviewNoRefactor)
    DEFINE_FIELD(CodeReviewNoRefactor, review)
    DEFINE_FIELD(CodeReviewNoRefactor, errors_brief)
    DEFINE_FIELD(CodeReviewNoRefactor, review_brief)
    DEFINE_ARRAY_FIELD(CodeReviewNoRefactor, existing_functions)
    DEFINE_ARRAY_FIELD(CodeReviewNoRefactor, unknown_types)

    void CodeReviewNoRefactor::clear()
    {
        review.clear();
        
        errors_brief.clear();
        
        review_brief.clear();
        
        existing_functions.clear();
        
        unknown_types.clear();
    }
//*************

    DEFINE_TYPE(CodeReviewLite)
    DEFINE_FIELD(CodeReviewLite, review)
    DEFINE_FIELD(CodeReviewLite, errors_brief)
    DEFINE_FIELD(CodeReviewLite, review_brief)
    DEFINE_ARRAY_FIELD(CodeReviewLite, unknown_types)

    void CodeReviewLite::clear()
    {
        review.clear();
        
        errors_brief.clear();
        
        review_brief.clear();
        
        unknown_types.clear();
    }
}
