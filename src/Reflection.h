#pragma once

#include <cpprest/json.h>
#include <vector>
#include <memory>
#include <functional>
#include <map>
#include <stack>
#include <type_traits>
#include <assert.h>

#include "Indent.h"

namespace hen {

	class ReflectionTypes
	{
	public:
		static std::vector<std::function<void(std::ostream&, Indent&)>>& printers()
		{
			static std::vector<std::function<void(std::ostream&, Indent&)>> _printers;
			return _printers;
		}

		static void print(std::ostream& os)
		{
			Indent indent("    ");
			for(auto printer : printers())
			{
				printer(os, indent);
				os << std::endl;
			}
		}
	};

	template <typename Derived>
	class Reflection
	{
	public:
		
		static std::vector<std::function<void(const void*, web::json::value&)>>& serializers()
		{
			static std::vector<std::function<void(const void*, web::json::value&)>> _serializers;
			return _serializers;
		}
		static std::vector<std::function<void(void*, const web::json::value&)>>& deserializers()
		{
			static std::vector<std::function<void(void*, const web::json::value&)>> _deserializers;
			return _deserializers;
		}
		static std::vector<std::function<void(std::ostream&, bool, Indent&)>>& printers()
		{
			static std::vector<std::function<void(std::ostream&, bool, Indent&)>> _printers;
			return _printers;
		}

		virtual ~Reflection() = default;

		// Serialize this object to a JSON value
		virtual web::json::value to_json() const = 0;

		// Deserialize this object from a JSON value
		virtual void from_json(const web::json::value& j) = 0;

		friend web::json::value& operator << (web::json::value& j, const Reflection<Derived>& obj)
		{
			j = obj.to_json();
			return j;
		}

		friend const web::json::value& operator >> (const web::json::value& j, Reflection<Derived>& obj)
		{
			obj.from_json(j);
			return j;
		}
	};

	inline const web::json::value& operator >> (const web::json::value& j, int32_t& i) {
		i = j.as_integer();
		return j;
	}

	inline const web::json::value& operator >> (const web::json::value& j, uint32_t& ui) {
		ui = j.as_number().to_uint32();
		return j;
	}

	inline const web::json::value& operator >> (const web::json::value& j, utility::string_t& s) {
		s = j.as_string();
		return j;
	}

#ifdef _WIN32
	inline const web::json::value& operator >> (const web::json::value& j, std::string& s) {
		s = utility::conversions::to_utf8string(j.as_string());
		return j;
	}
#endif

	inline const web::json::value& operator >> (const web::json::value& j, bool& b) {
		b = j.as_bool();
		return j;
	}

	inline const web::json::value& operator >> (const web::json::value& j, float& f) {
		f = (float)j.as_number().to_double();
		return j;
	}

	inline web::json::value& operator << (web::json::value& j, int32_t i) {
		j = i;
		return j;
	}

	inline web::json::value& operator << (web::json::value& j, const utility::string_t& s) {
		j = static_cast<web::json::value>(s);
		return j;
	}

#ifdef _WIN32
	inline web::json::value& operator << (web::json::value& j, const std::string& s) {
		j = static_cast<web::json::value>(utility::conversions::to_string_t(s));
		return j;
	}
#endif

	inline web::json::value& operator << (web::json::value& j, uint32_t ui) {
		j = ui;
		return j;
	}

	inline web::json::value& operator << (web::json::value& j, bool b) {
		j = b;
		return j;
	}

	inline web::json::value& operator << (web::json::value& j, float f) {
		j = (float)f;
		return j;
	}

    template <typename T>
    web::json::value& operator << (web::json::value& j, const std::vector<std::shared_ptr<T>>& a)
    {
        auto ja = web::json::value::array();
        for (int32_t i=0; i<a.size(); ++i)
        {
            ja[i] << *(a[i]);
        }
        
        j = ja;
        return j;
    }

    template <typename T>
    const web::json::value& operator >> (const web::json::value& j, std::vector<std::shared_ptr<T>>& a)
    {
        a.clear();
        a.reserve(j.size());
        for (int32_t i = 0; i < j.size(); ++i)
        {
            auto temp = std::make_shared<T>(); // Create a shared_ptr to a new instance of T
            j.at(i) >> *temp; // Dereference the shared_ptr to operate on the actual T instance
            a.push_back(temp); // Add the shared_ptr to the vector
        }
        return j;
    }

	static const std::map<std::string, const char*> g_json_types = { 
		{"int32_t", "\"number\""},
        {"uint32_t", "\"number\""},
		{"std::string", "\"string\""},
		{"utility::string_t", "\"string\""}, 
		{"int", "\"number\""}
	};

    template<typename T>
    struct is_user_defined_type : std::integral_constant<bool,
    !std::is_fundamental<T>::value &&
    !std::is_enum<T>::value &&
    !std::is_pointer<T>::value &&
    !std::is_array<T>::value &&
    !std::is_same<T, std::string>::value &&
    !std::is_same<T, utility::string_t>::value> {};

    template<typename T>
    typename std::enable_if<is_user_defined_type<T>::value, void>::type
    conditional_print_schema(std::ostream& os, Indent& indentation) {
        T::print_schema(os, indentation); // Only valid if T is a user-defined type
    }

    // Overload for when T is not a user-defined type, does nothing
    template<typename T>
    typename std::enable_if<!is_user_defined_type<T>::value, void>::type
    conditional_print_schema(std::ostream&, Indent&) {
        // Do nothing for built-in types
    }

	// Templated helper to hold field metadata and provide serialization
	template<typename ClassType, typename FieldType, FieldType ClassType::* MemberPtr, typename StringLiteral >
	struct BaseField
	{
		BaseField() {
			
			Reflection<ClassType>::serializers().push_back([](const void* obj, web::json::value& j) {
				const ClassType* actualObj = static_cast<const ClassType*>(obj);
				auto uname = utility::conversions::to_string_t(StringLiteral::name());
				j[uname] << actualObj->*MemberPtr;
				});

			Reflection<ClassType>::deserializers().push_back([](void* obj, const web::json::value& j) {
				ClassType* actualObj = static_cast<ClassType*>(obj);
				auto uname = utility::conversions::to_string_t(StringLiteral::name());
                //ucout << uname << std::endl;
                if(j.has_field(uname))
                {
                    web::json::value v = j.at(uname);
                    v >> actualObj->*MemberPtr;
                }
				});
		}
	};

	// Templated helper to hold field metadata and provide serialization
    template<typename ClassType, typename FieldType, FieldType ClassType::* MemberPtr, typename StringLiteral >
    struct Field : public BaseField<ClassType, FieldType, MemberPtr, StringLiteral>
    {
        Field() {

            Reflection<ClassType>::printers().push_back([](std::ostream& os, bool last, Indent& indentation) {
                
                std::string name = StringLiteral::name();
                os << indentation << "\"" << name << "\": {" << std::endl;
                indentation++;
                
                auto itType = g_json_types.find(StringLiteral::type());
                bool object = itType == g_json_types.end();
                std::string type = !object ? g_json_types.at(StringLiteral::type()) : "\"object\"";
                os << indentation << "\"type\": " << type << "," << std::endl;
                std::string desc = StringLiteral::description();
                os << indentation << "\"description\": \"" << desc << "\"";
                
                if (!object)
                {
                    os << std::endl;
                }
                else
                if(object)
                {
                    os << "," << std::endl;
                    os << indentation << "\"properties\": {" << std::endl;
                    indentation++;
                    conditional_print_schema<FieldType>(os, indentation);
                    indentation--;
                    os << indentation << "}" << std::endl;
                }
                
                indentation--;
                os << indentation << "}";
                if (!last) os << ",";
                os << std::endl;
                });
        }
    };


    // Templated helper to hold field metadata and provide serialization
    template<typename ClassType, typename FieldType, FieldType ClassType::* MemberPtr, typename StringLiteral >
    struct EnumField : public BaseField<ClassType, FieldType, MemberPtr, StringLiteral>
    {
        EnumField() {

            Reflection<ClassType>::printers().push_back([](std::ostream& os, bool last, Indent& indentation) {
                
                std::string name = StringLiteral::name();
                os << indentation << "\"" << name << "\": {" << std::endl;
                indentation++;
                
                os << indentation << "\"type\": \"string\"," << std::endl;
                std::string enumList = StringLiteral::list();
                os << indentation << "\"enum\": [" << enumList << "]," << std::endl;
                std::string desc = StringLiteral::description();
                os << indentation << "\"description\": \"" << desc << "\"" << std::endl;
                
                indentation--;
                os << indentation << "}";
                if (!last) os << ",";
                os << std::endl;
                });
        }
    };

	// Templated helper to hold field metadata and provide serialization
	template<typename ClassType, typename FieldType, std::vector < std::shared_ptr<FieldType>> ClassType::* MemberPtr, typename StringLiteral >
	struct ArrayField : public BaseField<ClassType, std::vector < std::shared_ptr<FieldType>>, MemberPtr, StringLiteral>
	{
		ArrayField() {
            Reflection<ClassType>::printers().push_back([](std::ostream& os, bool last, Indent& indentation) {
                std::string name = StringLiteral::name();
                os << indentation << "\"" << name << "\": {" << std::endl;
                indentation++;
                os << indentation << "\"type\": \"array\"," << std::endl;
                os << indentation << "\"items\": {" << std::endl;
                indentation++;
                
                auto itType = g_json_types.find(StringLiteral::type());
                bool object = itType == g_json_types.end();
                std::string type = !object ? g_json_types.at(StringLiteral::type()) : "\"object\"";
                
                os << indentation << "\"type\": " << type << "," << std::endl;
                std::string desc = StringLiteral::description();
                os << indentation << "\"description\": \"" << desc << "\"";
                if (!object)
                {
                    os << std::endl;
                    //os << indentation << "\"description\": \"" << StringLiteral::description() << "\"" << std::endl;
                }
                else
                if(object)
                {
                    os << "," << std::endl;
                    os << indentation << "\"properties\": {" << std::endl;
                    indentation++;
                    conditional_print_schema<FieldType>(os, indentation);
                    indentation--;
                    os << indentation << "}" << std::endl; //close "properties"
                }
                indentation--;
                os << indentation << "}" << std::endl; //close "items"
                indentation--;
                os << indentation << "}";
                if(!last) os << ",";
                os << std::endl;
                });
		}
	};

	// Macro to declare a serializable field and collect its metadata
#define DECLARE_FIELD(Type, Name, Description) \
public: \
    Type Name; \
public: \
	struct StringLiteral_##Name { static constexpr const char* type() { return #Type; } \
								  static constexpr const char* name() { return #Name; } \
								  static constexpr const char* description() { return Description; }}; \
	typedef Field<Self, Type, &Self::Name, Self::StringLiteral_##Name> Field_##Name; \
    static const Field_##Name field_##Name; \
public:


#define DECLARE_ENUM_FIELD(Name, List, Description) \
public: \
    std::string Name; \
public: \
    struct StringLiteral_##Name { static constexpr const char* type() { return "string"; } \
                                  static constexpr const char* name() { return #Name; } \
                                  static constexpr const char* list() { return List; } \
                                  static constexpr const char* description() { return Description; }}; \
    typedef EnumField<Self, std::string, &Self::Name, Self::StringLiteral_##Name> Field_##Name; \
    static const Field_##Name field_##Name; \
public:


//std::vector<std::shared_ptr<T>>

#define DECLARE_ARRAY_FIELD(Type, Name, Description) \
public: \
    std::vector<std::shared_ptr<Type>> Name; \
public: \
	struct StringLiteral_##Name { static constexpr const char* type() { return #Type; } \
								  static constexpr const char* name() { return #Name; } \
								  static constexpr const char* description() { return Description; }}; \
	typedef ArrayField<Self, Type, &Self::Name, Self::StringLiteral_##Name> ArrayField_##Name; \
    static const ArrayField_##Name field_##Name; \
public:

#define DEFINE_FIELD(Class, Name) \
const Class::Field_##Name Class::field_##Name;

#define DEFINE_ARRAY_FIELD(Class, Name) \
const Class::ArrayField_##Name Class::field_##Name;

	// Macro to declare a class as serializable
#define _DECLARE_TYPE(Class, Description) \
using Self = Class; \
    static const char* typeName() { return #Class; } \
	static const char* classDescription() { return Description; }; \
private: \
	static struct Struct{ Struct() { ReflectionTypes::printers().push_back( \
	[](std::ostream& os, Indent& indentation) {Class::print_schema(os, indentation);}); } } m_struct; \
public: \
    void _to_json(web::json::value& result) const { \
        for (auto& serializer : Reflection<Self>::serializers()) { \
            web::json::value field_value; \
            serializer(this, result); \
        } \
    } \
    void _from_json(const web::json::value& j) { \
        for (auto& deserializer : Reflection<Self>::deserializers()) { \
            deserializer(this, j); \
        } \
    } \
	static void _print_schema(std::ostream& os, Indent& indentation) { \
		++indentation; \
		for (auto it = Reflection<Self>::printers().begin(); it != Reflection<Self>::printers().end(); ++it) { \
            (*it)(os, it == Reflection<Self>::printers().end() - 1, indentation); \
        } \
		--indentation; \
	}

#define DECLARE_TYPE(Class, Description) \
	_DECLARE_TYPE(Class, Description) \
	web::json::value to_json() const override { \
        web::json::value result; \
        _to_json(result); \
        return result; \
    } \
    void from_json(const web::json::value& j) override { \
        _from_json(j); \
    } \
	static void print_schema(std::ostream& os, Indent& indentation) { \
		Self::_print_schema(os, indentation); \
	}

#define DECLARE_DERIVED_TYPE(Class, Base, Description) \
using Parent = Base; \
	_DECLARE_TYPE(Class, Description) \
	web::json::value to_json() const override { \
        web::json::value result = Parent::to_json(); \
        Self::_to_json(result); \
        return result; \
    } \
    void from_json(const web::json::value& j) override { \
		Parent::from_json(j); \
        Self::_from_json(j); \
    } \
	static void print_schema(std::ostream& os, Indent& indentation) { \
		Parent::print_schema(os, indentation); \
		Self::_print_schema(os, indentation); \
	}

#define DEFINE_TYPE(Class) \
	Class::Struct Class::m_struct;
}
