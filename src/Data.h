#pragma once
#include <set>

#include "IncludeBoost.h"

#include "Reflection.h"
#include "File.h"


namespace stdrave {

    class DataDef : public Reflection<DataDef>
    {
    public:
        DECLARE_TYPE(DataDef, "Defines data type")
        DECLARE_FIELD(std::string, type_name, "The name of the data type.")
        DECLARE_FIELD(std::string, description, "Brief description of the data type. One or two sentences")
        DECLARE_FIELD(std::string, motivation, "Briefly explain the motivation to introduce this new data type or to update the existing one. "\
                                               "Explain what struct or enum members need to be added, their name and data type")
        DECLARE_ENUM_FIELD(type, "\"struct\",\"enum\"", "Type of the data type. We can have 'struct' or 'enum'")
    };

    class DataDefList : public Reflection<DataDefList>
    {
    public:
        
        DECLARE_TYPE(DataDefList, "List with data definition.")
        DECLARE_ARRAY_FIELD(DataDef, items, "List with data definition.")
        
        boost::optional<const DataDef&> findData(const std::string& name);
    };

}
