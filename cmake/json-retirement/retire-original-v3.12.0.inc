            if (t == value_t::array || t == value_t::object)
            {
                // flatten the current json_value to a heap-allocated stack
                std::vector<basic_json> stack;

                // move the top-level items to stack
                if (t == value_t::array)
                {
                    stack.reserve(array->size());
                    std::move(array->begin(), array->end(), std::back_inserter(stack));
                }
                else
                {
                    stack.reserve(object->size());
                    for (auto&& it : *object)
                    {
                        stack.push_back(std::move(it.second));
                    }
                }

                while (!stack.empty())
                {
                    // move the last item to local variable to be processed
                    basic_json current_item(std::move(stack.back()));
                    stack.pop_back();

                    // if current_item is array/object, move
                    // its children to the stack to be processed later
                    if (current_item.is_array())
                    {
                        std::move(current_item.m_data.m_value.array->begin(), current_item.m_data.m_value.array->end(), std::back_inserter(stack));

                        current_item.m_data.m_value.array->clear();
                    }
                    else if (current_item.is_object())
                    {
                        for (auto&& it : *current_item.m_data.m_value.object)
                        {
                            stack.push_back(std::move(it.second));
                        }

                        current_item.m_data.m_value.object->clear();
                    }

                    // it's now safe that current_item get destructed
                    // since it doesn't have any children
                }
            }

