#pragma once
#include "Node.h"
#include "Client.h"
#include "Utils.h"

namespace hen {

template <typename T>
bool Node::inference(std::string& cache, const std::string& message, T* object)
{
    Project* proj = Client::getInstance().project();
    
    std::string dir = getNodeDirectory();
    
    web::json::value jsonObject;
    web::json::value schema;
    if (object)
    {
        setSchema<T>();
        schema = m_schemas.as_array().at(0);
    }
    
    Cache _cache(dir, cache);
    bool result = proj->inference(_cache, message, schema, jsonObject);
    
    if(!_cache.available())
    {
        cache = "na";
    }
    
    if (object)
    {
        object->from_json(jsonObject);
    }
    
    return result;
}

template <typename T>
bool Node::loadFromJson(const std::string& path, T* object)
{
    return objFromJson(path, object);
}
    
}
