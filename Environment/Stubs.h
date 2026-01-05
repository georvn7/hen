#include <type_traits>
#include <utility>

// Helper for returning a dummy value of any type R.
template <typename R>
struct ReturnValueHelper {
    static R get() {
        if constexpr (std::is_void_v<R>) {
            // void return type: do nothing
        } else if constexpr (std::is_reference_v<R>) {
            using RefType = std::remove_reference_t<R>;
            static RefType dummy{};
            return dummy;  // Return a reference to a static object
        } else if constexpr (std::is_enum_v<R>) {
            return static_cast<R>(0);  // Return zero as an enum
        } else if constexpr (std::is_pointer_v<R>) {
            return static_cast<R>(nullptr);  // Return nullptr for pointers
        } else if constexpr (std::is_arithmetic_v<R> || std::is_default_constructible_v<R>) {
            return R{}; // Default-constructible or arithmetic
        } else {
            // Non-default-constructible type; return a dummy object.
            // This is just to compile, not for runtime correctness.
            alignas(R) static unsigned char buffer[sizeof(R)];
            return *reinterpret_cast<R*>(buffer);
        }
    }
};

// A type that can convert into ANY requested type when needed.
struct AnyReturn {
    template <typename R,
        // Exclude std::string, const char*, char, initializer_list<char>, and possibly std::string_view
        std::enable_if_t<
            !std::is_same_v<R, std::string> &&
            !std::is_same_v<R, const char*> &&
            !std::is_same_v<R, char> &&
            !std::is_same_v<R, std::initializer_list<char>>
            , int> = 0>
    operator R() const {
        return ReturnValueHelper<R>::get();
    }

    // Provide a single direct conversion to const char*
    operator const char*() const {
        return "dummy";
    }
    
    // Provide a direct conversion to std::string
    operator std::string() const {
        return std::string("dummy_string");
    }
};
