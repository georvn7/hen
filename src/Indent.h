#pragma once

#include <stack>
#include <vector>
#include <string>

namespace hen {

    struct Indent : public std::stack<char, std::vector<char>>
    {
        std::string s;
        Indent(const std::string& sequence) :s(sequence) {}
        // Pre-increment operator
        Indent& operator++()
        {
            // Example action: push a character to represent increasing indentation
            for (auto c : s) this->push(c); // Assuming space as indentation character
            return *this;
        }

        // Pre-decrement operator
        Indent& operator--()
        {
            if (!this->empty())
            {
                // Example action: pop a character to represent decreasing indentation
                for (int i = 0; i < s.size(); ++i) this->pop();
            }
            return *this;
        }

        // If you also need post-increment and post-decrement operators, they would be defined like this:
        // Note: These return a value, not a reference, to reflect the state before modification

        // Post-increment operator
        Indent operator++(int)
        {
            Indent temp = *this; // Copy current state
            ++(*this); // Use pre-increment to modify the object
            return temp; // Return the original state
        }

        // Post-decrement operator
        Indent operator--(int)
        {
            Indent temp = *this; // Copy current state
            --(*this); // Use pre-decrement to modify the object
            return temp; // Return the original state
        }

        operator std::string() const {
            std::string str(this->c.begin(), this->c.end());
            return str;
        }

        // Override << operator for std::ostream and indent
        friend std::ostream& operator<<(std::ostream& os, const Indent& ind) {
            os << std::string(ind);
            return os; // Return the ostream object to allow chaining
        }
    };
}
