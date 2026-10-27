#pragma once
#include <cstdint>
#include <cstddef>
#include <new>
#include <utility>

namespace eng {

// Pool de capacidade fixa, sem alocação após construção. Handles com geração
// detectam uso de objetos já liberados.
template <class T, uint32_t N>
class ObjectPool {
public:
    struct Handle {
        uint32_t index = UINT32_MAX;
        uint32_t gen = 0;
        bool valid() const { return index != UINT32_MAX; }
    };

    ObjectPool() {
        for (uint32_t i = 0; i < N; ++i) { next_[i] = i + 1; gen_[i] = 1; live_[i] = false; }
        head_ = 0;
    }
    ~ObjectPool() { for (uint32_t i = 0; i < N; ++i) if (live_[i]) ptr(i)->~T(); }
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <class... A>
    Handle acquire(A&&... a) {
        if (head_ >= N) return {};
        uint32_t i = head_;
        head_ = next_[i];
        new (&storage_[i]) T(std::forward<A>(a)...);
        live_[i] = true;
        ++count_;
        return {i, gen_[i]};
    }

    void release(Handle h) {
        if (!get(h)) return;
        ptr(h.index)->~T();
        live_[h.index] = false;
        ++gen_[h.index];
        next_[h.index] = head_;
        head_ = h.index;
        --count_;
    }

    // Handle a partir do ponteiro (objeto precisa pertencer a este pool)
    Handle handleOf(const T* p) const {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
        const unsigned char* s = &storage_[0][0];
        if (b < s || b >= s + sizeof(storage_)) return {};
        uint32_t i = uint32_t((b - s) / sizeof(T));
        return live_[i] ? Handle{i, gen_[i]} : Handle{};
    }
    void release(const T* p) { release(handleOf(p)); }
    void clear() { for (uint32_t i = 0; i < N; ++i) if (live_[i]) release(Handle{i, gen_[i]}); }

    T* get(Handle h) {
        if (h.index >= N || !live_[h.index] || gen_[h.index] != h.gen) return nullptr;
        return ptr(h.index);
    }

    // Itera objetos vivos sem alocar.
    template <class F> void forEach(F&& f) {
        for (uint32_t i = 0; i < N; ++i) if (live_[i]) f(*ptr(i), Handle{i, gen_[i]});
    }
    uint32_t count() const { return count_; }
    static constexpr uint32_t capacity() { return N; }

private:
    T* ptr(uint32_t i) { return std::launder(reinterpret_cast<T*>(&storage_[i])); }
    alignas(T) unsigned char storage_[N][sizeof(T)];
    uint32_t next_[N];
    uint32_t gen_[N];
    bool live_[N];
    uint32_t head_ = 0, count_ = 0;
};

// Alocador linear por frame. reset() no início de cada frame.
class FrameArena {
public:
    explicit FrameArena(size_t bytes) : size_(bytes), buf_(static_cast<unsigned char*>(::operator new(bytes))) {}
    ~FrameArena() { ::operator delete(buf_); }
    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;

    void* alloc(size_t bytes, size_t align = 16) {
        size_t p = (used_ + (align - 1)) & ~(align - 1);
        if (p + bytes > size_) return nullptr;  // orçamento estourado: tratar no chamador
        used_ = p + bytes;
        return buf_ + p;
    }
    template <class T> T* allocArray(size_t n) { return static_cast<T*>(alloc(sizeof(T) * n, alignof(T))); }
    void reset() { used_ = 0; }
    size_t used() const { return used_; }

private:
    size_t size_, used_ = 0;
    unsigned char* buf_;
};

}  // namespace eng
