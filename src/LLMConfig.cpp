#include "LLMConfig.h"
#include "IncludeBoost.h"

namespace hen {

    DEFINE_TYPE(LLMConfig)
    DEFINE_FIELD(LLMConfig, provider)
    DEFINE_FIELD(LLMConfig, url)
    DEFINE_FIELD(LLMConfig, model)
    DEFINE_FIELD(LLMConfig, reasoning_effort)
    DEFINE_FIELD(LLMConfig, verbosity)
    DEFINE_FIELD(LLMConfig, rate_limit)
    DEFINE_FIELD(LLMConfig, rate_limit_rpm)
    DEFINE_FIELD(LLMConfig, context_size)
    DEFINE_FIELD(LLMConfig, roles)
    DEFINE_FIELD(LLMConfig, input_tokens_price)
    DEFINE_FIELD(LLMConfig, output_tokens_price)

    DEFINE_TYPE(LLMRegistry)
    DEFINE_ARRAY_FIELD(LLMRegistry, llms)

    void LLMConfig::findRolesMask()
    {
        std::vector<std::string> llmRoles;
        boost::split(llmRoles, roles, boost::is_any_of(","));
        
        m_rolesMask = 0;
        
        for(auto role : llmRoles)
        {
            if(role == "director")
            {
                m_rolesMask |= LLMRole::DIRECTOR;
            }
            else if(role == "expert")
            {
                m_rolesMask |= LLMRole::EXPERT;
            }
            else if(role == "developer")
            {
                m_rolesMask |= LLMRole::DEVELOPER;
            }
        }
    }
}
