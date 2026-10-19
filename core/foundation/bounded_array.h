// A fixed-capacity array in caller memory (ADR-0014). It never grows: a push
// into a full array fails. Elements are trivially copyable and destructible,
// so removing one copies bytes and nothing needs destroying.

#ifndef AYNI_CORE_FOUNDATION_BOUNDED_ARRAY_H
#define AYNI_CORE_FOUNDATION_BOUNDED_ARRAY_H

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace ayni::foundation {

template <typename T> class BoundedArray {
    static_assert(std::is_trivially_copyable<T>::value && std::is_trivially_destructible<T>::value,
                  "elements are copied as bytes and never destroyed");

  public:
    static constexpr std::size_t storage_alignment = alignof(T);
    static constexpr std::size_t storage_size(uint32_t capacity) { return sizeof(T) * capacity; }

    // storage holds storage_size(capacity) bytes aligned to storage_alignment
    // and outlives the array. It may be null only when capacity is 0.
    BoundedArray(void *storage, uint32_t capacity)
        : items_(static_cast<T *>(storage)), capacity_(capacity) {}

    BoundedArray(const BoundedArray &) = delete;
    BoundedArray &operator=(const BoundedArray &) = delete;

    uint32_t size() const { return size_; }
    bool full() const { return size_ == capacity_; }

    // index < size().
    T &operator[](uint32_t index) { return items_[index]; }
    const T &operator[](uint32_t index) const { return items_[index]; }

    // Appends a copy of item. Returns false, changing nothing, when full.
    bool push_back(const T &item) {
        if (full()) {
            return false;
        }
        new (items_ + size_) T(item);
        ++size_;
        return true;
    }

    // Removes the element at index < size() by moving the last one into its
    // place, so the order of the others is not kept.
    void remove_at(uint32_t index) {
        items_[index] = items_[size_ - 1U];
        --size_;
    }

  private:
    T *items_;
    uint32_t capacity_;
    uint32_t size_ = 0;
};

} // namespace ayni::foundation

#endif // AYNI_CORE_FOUNDATION_BOUNDED_ARRAY_H
