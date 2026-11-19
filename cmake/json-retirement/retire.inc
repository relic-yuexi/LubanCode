            if (t == value_t::array || t == value_t::object)
            {
                // Destructive postorder traversal uses already-owned container
                // frames. No heap stack, no recursive walk, no nesting limit.
                auto* root = t == value_t::object ? &container_owner(object)->frame
                                                  : &container_owner(array)->frame;
                root->parent = nullptr;
                auto* current = root;
                for (;;)
                {
                    basic_json* child = nullptr;
                    if (current->kind == value_t::object)
                    {
                        auto* values = static_cast<object_t*>(current->container);
                        if (!values->empty())
                        {
                            if constexpr (detail::is_ordered_map<object_t>::value)
                                child = &values->back().second;
                            else
                                child = &values->begin()->second;
                        }
                    }
                    else
                    {
                        auto* values = static_cast<array_t*>(current->container);
                        if (!values->empty()) child = &values->back();
                    }
                    if (child != nullptr)
                    {
                        if (child->is_structured() && !child->empty())
                        {
                            auto* next = child->is_object()
                                ? &container_owner(child->template get_ptr<object_t*>())->frame
                                : &container_owner(child->template get_ptr<array_t*>())->frame;
                            next->parent = current;
                            current = next;
                            continue;
                        }
                        // Scalar or already-empty child: its normal destructor
                        // only frees storage. Ordered-map erase would shift and
                        // copy const keys, so release its final element instead.
                        if (current->kind == value_t::object)
                        {
                            auto* values = static_cast<object_t*>(current->container);
                            if constexpr (detail::is_ordered_map<object_t>::value)
                                values->pop_back();
                            else
                                values->erase(values->begin());
                        }
                        else static_cast<array_t*>(current->container)->pop_back();
                        continue;
                    }
                    if (current == root) break;
                    current = current->parent;
                }
            }

