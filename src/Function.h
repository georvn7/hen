#pragma once


#include "Reflection.h"
#include "File.h"
#include "Data.h"
#include "UtilsCodeAnalysis.h"


namespace hen {

    class FunctionItem : public Reflection<FunctionItem>
    {
    public:
        DECLARE_TYPE(FunctionItem, "Minimal description of an C Function. Contains the name and brief description")
        DECLARE_FIELD(std::string, func_name, "The name of this Function. "\
        "Note this is gonna be used as an ID and must be consistent and unique. Preferably in Snake Case name convention")
        DECLARE_FIELD(std::string, brief, "Describes operations performed by this function and what subset of the project features this function is responsible for. "\
                                          "Explains what is the expected input to the function. What is the produced output by the function. "\
                                          "What data structures are necessary for the function input arguments and the output result. "\
                                          "In its implementation, the function must not access global variables except from the 'available libraries'. " \
                                          "Access to all required input to the function must be provided via function arguments. "\
                                          "All data, produced or updated by this function, that need to be accessed by other functions "\
                                          "not called directly or indirectly by the function, must be exposed as mutable arguments or return result.")
    };

    class FunctionList : public Reflection<FunctionList>
    {
    public:
        DECLARE_TYPE(FunctionList, "Contains a list of Functions")
        DECLARE_ARRAY_FIELD(FunctionItem, items, "The list with the requested Functions")
        DECLARE_FIELD(std::string, motivation, "Explain what is the motivation to list these functions. "\
                                               "The explanation should be 5-7 sentences.")
        
        //We need to preserve the order of child nodes between restarts to keep cache valid
        //and avoid generating the same functions for newly added nodes
        std::string m_order;
        
        void updateOrder();
        void applyOrder();
    };

    class LibFunctionItem : public Reflection<LibFunctionItem>
    {
    public:
        DECLARE_TYPE(LibFunctionItem, "Minimal description of an C Function. Contains the name and brief description")
        DECLARE_FIELD(std::string, func_name, "The name of this function as it is already listed in the library")
        DECLARE_FIELD(std::string, motivation, "What is the motivation to select this function from the library and how it could be directly called in the implementation")
    };

    class InfoRequest : public Reflection<InfoRequest>
    {
    public:
        DECLARE_TYPE(InfoRequest, "Request for additional information about the software structure. List only names of the existing functions or data types")
        DECLARE_ARRAY_FIELD(std::string, functions, "List of existing function names for which to request source and documentation. If not required leave empty array.")
        DECLARE_ARRAY_FIELD(std::string, data_types, "List of existing struct or enum definitions for which to request the source. If not required leave empty array.")
        DECLARE_ARRAY_FIELD(std::string, graph_and_brief, "List of existing functions for which to print the direct sub call graph, "\
                            "limited to a certain depth, along with declaration and brief description for each function. If not required leave empty array.")
        DECLARE_ARRAY_FIELD(std::string, match_regex, "List of regex patterns to search the existing project sources. If not required leave empty array.")
        
        bool empty();
        void clear();
    };

    class LibFunctionList : public Reflection<LibFunctionList>
    {
    public:
        DECLARE_TYPE(LibFunctionList, "Contains a list of names")
        DECLARE_ARRAY_FIELD(LibFunctionItem, items, "The list with functions found from the library")
    };

	class Function : public Reflection<Function>
	{
	public:
        
		DECLARE_TYPE(Function, "Defines a function, all fields must be provided!")
		
        ParsedFunction m_signature;
        
        DECLARE_FIELD(std::string, brief, "One sentence to explain operations performed by this function. "\
                                          "Along with declaration, this will be used for functions library search and indexing")
        
        std::string description;
        
        DECLARE_FIELD(std::string, declaration,"Declaration of the function statement exaclty as required by the programming language specification, "\
                                               "including the ; character at the end. "\
                                               "Consult with 'available libraries' section to leverage features and data types. Don't assume any namespaces from the libraries are defined")
	};

    class FunctionDefinition : public Reflection<FunctionDefinition>
    {
        public:
        
        DECLARE_TYPE(FunctionDefinition, "Defines a function, all fields must be provided!")
        
        DECLARE_FIELD(std::string, brief, "One sentence to explain operations performed by this function. "\
                                          "Along with declaration, this will be used for functions library search and indexing. Keep to 3 sentences or fewer, under 300 characters.")
        
        DECLARE_FIELD(std::string, description, "Describes operations performed by this function and what subset of the project features this function is responsible for. "\
                                                "Explain what is the expected input to the function. What is the produced output by the function. "\
                                                "Access to all required input by the function must be provided via function arguments. "\
                                                "All data, produced or updated by this function, that need to be accessed by other functions not called directly or indirectly "\
                                                "by the function, must be exposed as mutable arguments or return result."\
                                                "What data structures are necessary for the function input arguments and the output result. "\
                                                "In its implementation, the function must not access global variables except from the 'available libraries'. "\
                                                "There must be very clear explanation how each of the function outputs depends on the function input "\
                                                "and what operations are required over the input data to produce the given output. "\
                                                "Keep to 3 paragraphs or fewer, under 2000 characters total.")
    };

    class Code : public Reflection<Code>
    {
    public:
        DECLARE_TYPE(Code, "Contains elements required for the function implementation. "\
                                     "These elements will be used to construct and compile a file")
        
        DECLARE_ARRAY_FIELD(std::string, externals, "Definition path of all data types, from other parts of this software, "\
                            "used int the declaration of this function")
        DECLARE_ARRAY_FIELD(std::string, dependencies, "Definition path of all functions and data types, from other parts of this software, "\
                            "called in the implementation of this function")
        //DECLARE_FIELD(std::string, definition,"Source code with implementation of the functions. Don't translate this string on a new line with the backslash ASCII character. Don't provide include statements in the 'definition' section. They need to be provided separately as specified in the 'includes' section")
        std::string m_source;
    };

    class CodeReview : public Reflection<CodeReview>
    {
    public:
        DECLARE_TYPE(CodeReview, "Contains code review of a previously discussed source code")
        DECLARE_FIELD(std::string, review, "Code review, containing analysis of the problems and suggesting potential solution")
        DECLARE_FIELD(std::string, errors_brief, "Up to three sentences summary of the problem from the compilation output errors and provided analysis")
        DECLARE_FIELD(std::string, review_brief, "Up to three sentences summary of the code review highlighting the proposed solution")
        DECLARE_ARRAY_FIELD(FunctionItem, new_functions, "List of new functions the reviewed source will call to refactor its implementation")
        DECLARE_ARRAY_FIELD(std::string, existing_functions, "List of the names of existing functions likely to be used in the reviewed source")
        DECLARE_ARRAY_FIELD(std::string, unknown_types, "Data types defined by the application which full definition is unknown from the context")
        
        void clear();
    };

    class CodeReviewNoRefactor : public Reflection<CodeReviewNoRefactor>
    {
    public:
        DECLARE_TYPE(CodeReviewNoRefactor, "Contains code review of a previously discussed source code")
        DECLARE_FIELD(std::string, review, "Code review, containing analysis of the problems and suggesting potential solution")
        DECLARE_FIELD(std::string, errors_brief, "Up to three sentences summary of the problem from the compilation output errors and provided analysis")
        DECLARE_FIELD(std::string, review_brief, "Up to three sentences summary of the code review highlighting the proposed solution")
        DECLARE_ARRAY_FIELD(std::string, existing_functions, "List of the names of existing functions likely to be used in the reviewed source")
        DECLARE_ARRAY_FIELD(std::string, unknown_types, "Data types defined by the application which full definition is unknown from the context")
        
        void clear();
    };

    class CodeReviewLite : public Reflection<CodeReviewLite>
    {
    public:
        DECLARE_TYPE(CodeReviewLite, "Contains code review of a previously discussed source code")
        DECLARE_FIELD(std::string, review, "Code review, containing analysis of the problems and suggesting potential solution")
        DECLARE_FIELD(std::string, errors_brief, "Up to three sentences summary of the problem from the compilation output errors and provided analysis")
        DECLARE_FIELD(std::string, review_brief, "Up to three sentences summary of the code review highlighting the proposed solution")
        DECLARE_ARRAY_FIELD(std::string, unknown_types, "Data types defined by the application which full definition is unknown from the context")
        
        void clear();
    };
}
