    // LubanCode private ownership ABI: the retirement frame is allocated with
    // each structured container, never during its noexcept destruction.
    struct json_container_frame
    {
        json_container_frame* parent = nullptr;
        void* container = nullptr;
        value_t kind = value_t::null;
    };
    template<typename T>
    struct json_container_owner
    {
        json_container_frame frame;
        alignas(T) unsigned char storage[sizeof(T)];
    };
    template<typename T>
    static json_container_owner<T>* container_owner(T* value) noexcept
    {
        using Owner = json_container_owner<T>;
        return std::launder(reinterpret_cast<Owner*>(
            reinterpret_cast<unsigned char*>(value) - offsetof(Owner, storage)));
    }
    template<typename T>
    static void retire_container(T* value) noexcept
    {
        auto* owner = container_owner(value);
        AllocatorType<T> value_alloc;
        std::allocator_traits<AllocatorType<T>>::destroy(value_alloc, value);
        AllocatorType<json_container_owner<T>> owner_alloc;
        using Traits = std::allocator_traits<decltype(owner_alloc)>;
        Traits::destroy(owner_alloc, owner);
        Traits::deallocate(owner_alloc, owner, 1);
    }

    /// helper for exception-safe object creation
    template<typename T, typename... Args>
    JSON_HEDLEY_RETURNS_NON_NULL
    static T* create(Args&& ... args)
    {
        if constexpr (std::is_same<T, object_t>::value || std::is_same<T, array_t>::value)
        {
            using Owner = json_container_owner<T>;
            AllocatorType<Owner> owner_alloc;
            using OwnerTraits = std::allocator_traits<decltype(owner_alloc)>;
            auto deleter = [&](Owner* owner)
            {
                OwnerTraits::destroy(owner_alloc, owner);
                OwnerTraits::deallocate(owner_alloc, owner, 1);
            };
            // Construct the trivial frame before constructing any descendants.
            auto raw_deleter = [&](Owner* value) { OwnerTraits::deallocate(owner_alloc, value, 1); };
            std::unique_ptr<Owner, decltype(raw_deleter)> raw(OwnerTraits::allocate(owner_alloc, 1), raw_deleter);
            OwnerTraits::construct(owner_alloc, raw.get());
            std::unique_ptr<Owner, decltype(deleter)> owner(raw.release(), deleter);
            auto* value = reinterpret_cast<T*>(owner->storage);
            AllocatorType<T> value_alloc;
            std::allocator_traits<decltype(value_alloc)>::construct(
                value_alloc, value, std::forward<Args>(args)...);
            value = std::launder(value);
            owner->frame.container = value;
            owner->frame.kind = std::is_same<T, object_t>::value ? value_t::object : value_t::array;
            owner.release();
            return value;
        }
        else
        {
            AllocatorType<T> alloc;
            using AllocatorTraits = std::allocator_traits<AllocatorType<T>>;
            auto deleter = [&](T* obj) { AllocatorTraits::deallocate(alloc, obj, 1); };
            std::unique_ptr<T, decltype(deleter)> obj(AllocatorTraits::allocate(alloc, 1), deleter);
            AllocatorTraits::construct(alloc, obj.get(), std::forward<Args>(args)...);
            JSON_ASSERT(obj != nullptr);
            return obj.release();
        }
    }

