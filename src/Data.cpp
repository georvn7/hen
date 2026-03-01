#include <unordered_set>

#include "Data.h"
#include "Utils.h"
#include "CCodeProject.h"
#include "UtilsCodeAnalysis.h"

namespace hen {
    DEFINE_TYPE(DataDef)
    DEFINE_FIELD(DataDef, type_name)
    DEFINE_FIELD(DataDef, description)
    DEFINE_FIELD(DataDef, motivation)
    DEFINE_FIELD(DataDef, type)
     
    DEFINE_TYPE(DataDefList)
    DEFINE_ARRAY_FIELD(DataDefList, items)
    
    boost::optional<const DataDef&> DataDefList::findData(const std::string& name)
    {
        for(auto item : items)
        {
            if(item->type_name == name)
            {
                return *item;
            }
        }
        
        return boost::none;
    }
}
