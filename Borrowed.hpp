#pragma once

#include <cassert>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

template <typename T, T invalid = T()>
class Borrowed {
public:
    Borrowed() = default;
    Borrowed(T borrowed) : _h(borrowed) {}

    Borrowed(const Borrowed &) = delete;
    Borrowed &operator=(const Borrowed &) = delete;
    constexpr Borrowed(Borrowed &&other) noexcept {
        swap(*this, other);
    }
    constexpr Borrowed &operator=(Borrowed &&other) noexcept {
        if (this->_h != other._h) {
            Dispose();
            swap(*this, other);
        }
        return *this;
    }
    constexpr ~Borrowed() {
        Dispose();
    }
    constexpr T Release() noexcept {
        return std::exchange(_h, invalid);
    }
    constexpr void Reset() noexcept {
        Dispose();
    }
    constexpr T Get() const noexcept {
        return _h;
    }
    explicit constexpr operator bool() const noexcept {
        return _h != invalid;
    }
    constexpr T *operator&() {
        assert(!*this);
        return &_h;
    }

    constexpr friend void swap(Borrowed &self, Borrowed &other) noexcept {
        using std::swap;
        swap(self._h, other._h);
    }

private:
    void Dispose() noexcept {
        _h = invalid;
    }

    T _h = invalid;
};
