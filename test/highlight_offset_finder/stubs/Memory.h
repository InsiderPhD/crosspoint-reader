#pragma once
#include <memory>
#include <new>
template <class T> std::unique_ptr<T> makeUniqueNoThrow(size_t n) { using E = std::remove_extent_t<T>; return std::unique_ptr<T>(new (std::nothrow) E[n]); }
