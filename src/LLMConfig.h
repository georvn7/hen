#pragma once


#include "Reflection.h"
#include "File.h"
#include "Data.h"
#include "UtilsCodeAnalysis.h"


namespace hen {

    enum LLMRole
    {
        DEVELOPER = 1,
        EXPERT = 2,
        DIRECTOR = 4,
        DEBUGGER = 8,
    };

    class LLMConfig : public Reflection<LLMConfig>
    {
    public:
        DECLARE_TYPE(LLMConfig, "description")
        DECLARE_ENUM_FIELD(provider, "\"openai\",\"anthropic\",\"google\",\"groq\",\"xAI\",\"cerebras\",\"zai\",\"minimax\",\"mistral\",\"ollama\",\"vllm\"", "description")
        DECLARE_FIELD(std::string, url, "description")
        DECLARE_FIELD(std::string, model, "description")
        DECLARE_ENUM_FIELD(reasoning_effort, "\"na\",\"low\",\"medium\",\"high\"", "description")
        DECLARE_ENUM_FIELD(verbosity, "\"na\",\"low\",\"medium\",\"high\"", "description")
        DECLARE_FIELD(int, rate_limit, "Rate limits provided by the vendor for a given model. "\
                                       "Tokens per second. Negative number means no need to enforce a limit")
        DECLARE_FIELD(int, rate_limit_rpm, "Rate limits provided by the vendor for a given model. "\
                                       "Requests per minute. Negative number means no need to enforce a limit")
        DECLARE_FIELD(int, context_size, "Size of the context window in KB")
        DECLARE_FIELD(std::string, roles, "description")
        DECLARE_FIELD(float, input_tokens_price, "description")
        DECLARE_FIELD(float, cache_write_tokens_price, "Optional per-1M token cache write price. Negative means use provider fallback logic.")
        DECLARE_FIELD(float, cache_read_tokens_price, "Optional per-1M token cache read price. Negative means use provider fallback logic.")
        DECLARE_FIELD(float, output_tokens_price, "description")
        
        std::string api_key;
        
        uint32_t tokens_last_infer;
        std::chrono::time_point<std::chrono::high_resolution_clock> time_last_infer;
        
        uint32_t    m_rolesMask;
        
        void findRolesMask();
        bool takesOnRole(LLMRole role) const { return m_rolesMask & role; }
        
        LLMConfig():
        input_tokens_price(0.0f),
        cache_write_tokens_price(-1.0f),
        cache_read_tokens_price(-1.0f),
        output_tokens_price(0.0f),
        tokens_last_infer(0) {}
    };

    class LLMRegistry : public Reflection<LLMRegistry>
    {
    public:
        DECLARE_TYPE(LLMRegistry, "description")
        DECLARE_ARRAY_FIELD(LLMConfig, llms, "description")
    };
}
