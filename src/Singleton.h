#pragma once

// Singleton Base Class
template<typename T>
class Singleton {
public:
    // Delete copy constructor and assignment operator
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;

    // Static method to get the instance of the derived class
    static T& getInstance() {
        static T instance; // Guaranteed to be destroyed and instantiated on first use
        return instance;
    }

protected:
    // Protected constructor to allow derived class instantiation
    Singleton() {}

    // Any other protected members
};
