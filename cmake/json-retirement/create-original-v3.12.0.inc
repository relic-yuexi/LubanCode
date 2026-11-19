    /// helper for exception-safe object creation
    template<typename T, typename... Args>
    JSON_HEDLEY_RETURNS_NON_NULL
    static T* create(Args&& ... args)
    {
        AllocatorType<T> alloc;
        using AllocatorTraits = std::allocator_traits<AllocatorType<T>>;

        auto deleter = [&](T * obj)
        {
            AllocatorTraits::deallocate(alloc, obj, 1);
        };
        std::unique_ptr<T, decltype(deleter)> obj(AllocatorTraits::allocate(alloc, 1), deleter);
        AllocatorTraits::construct(alloc, obj.get(), std::forward<Args>(args)...);
        JSON_ASSERT(obj != nullptr);
        return obj.release();
    }

