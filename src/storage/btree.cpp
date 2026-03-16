#include "storage/btree.h"
#include <cassert>

namespace sql
{

    std::shared_ptr<BTreeNode> BTree::FindLeaf(const Value &key) const
    {
        auto node = root_;
        while (!node->is_leaf)
        {
            size_t i = 0;
            while (i < node->keys.size() && !(key < node->keys[i]))
                i++;
            node = node->children[i];
        }
        return node;
    }

    std::shared_ptr<BTreeNode> BTree::FindLeftmostLeaf() const
    {
        auto node = root_;
        while (!node->is_leaf)
            node = node->children[0];
        return node;
    }

    void BTree::Insert(const Value &key, size_t row_index)
    {
        auto result = InsertInternal(root_, key, row_index);
        if (result)
        {
            // Root was split, create new root
            auto new_root = std::make_shared<BTreeNode>();
            new_root->is_leaf = false;
            new_root->keys.push_back(result->median_key);
            new_root->children.push_back(root_);
            new_root->children.push_back(result->new_node);
            root_ = new_root;
        }
    }

    std::optional<BTree::SplitResult> BTree::InsertInternal(
        std::shared_ptr<BTreeNode> node, const Value &key, size_t row_index)
    {
        if (node->is_leaf)
        {
            // Find insertion position
            size_t pos = 0;
            while (pos < node->keys.size() && node->keys[pos] < key)
                pos++;

            node->keys.insert(node->keys.begin() + static_cast<long>(pos), key);
            node->row_indices.insert(node->row_indices.begin() + static_cast<long>(pos), row_index);

            // Check if we need to split
            if (static_cast<int>(node->keys.size()) > BTREE_MAX_KEYS)
            {
                auto new_leaf = std::make_shared<BTreeNode>();
                new_leaf->is_leaf = true;

                size_t mid = node->keys.size() / 2;

                new_leaf->keys.assign(node->keys.begin() + static_cast<long>(mid), node->keys.end());
                new_leaf->row_indices.assign(node->row_indices.begin() + static_cast<long>(mid),
                                             node->row_indices.end());

                Value median = new_leaf->keys[0];

                node->keys.resize(mid);
                node->row_indices.resize(mid);

                // Maintain leaf linked list
                new_leaf->next_leaf = node->next_leaf;
                node->next_leaf = new_leaf;

                return SplitResult{median, new_leaf};
            }
            return std::nullopt;
        }
        else
        {
            // Internal node: find child
            size_t i = 0;
            while (i < node->keys.size() && !(key < node->keys[i]))
                i++;

            auto result = InsertInternal(node->children[i], key, row_index);
            if (!result)
                return std::nullopt;

            // Insert the median key from child split
            size_t pos = i;
            node->keys.insert(node->keys.begin() + static_cast<long>(pos), result->median_key);
            node->children.insert(node->children.begin() + static_cast<long>(pos) + 1, result->new_node);

            if (static_cast<int>(node->keys.size()) > BTREE_MAX_KEYS)
            {
                auto new_internal = std::make_shared<BTreeNode>();
                new_internal->is_leaf = false;

                size_t mid = node->keys.size() / 2;
                Value median = node->keys[mid];

                new_internal->keys.assign(node->keys.begin() + static_cast<long>(mid) + 1,
                                          node->keys.end());
                new_internal->children.assign(node->children.begin() + static_cast<long>(mid) + 1,
                                              node->children.end());

                node->keys.resize(mid);
                node->children.resize(mid + 1);

                return SplitResult{median, new_internal};
            }
            return std::nullopt;
        }
    }

    void BTree::Remove(const Value &key)
    {
        // Simple removal: find leaf, remove all matching entries
        // (No rebalancing for simplicity - educational project)
        auto leaf = FindLeaf(key);
        size_t i = 0;
        while (i < leaf->keys.size())
        {
            if (leaf->keys[i] == key)
            {
                leaf->keys.erase(leaf->keys.begin() + static_cast<long>(i));
                leaf->row_indices.erase(leaf->row_indices.begin() + static_cast<long>(i));
            }
            else
            {
                i++;
            }
        }
    }

    std::vector<size_t> BTree::Search(const Value &key) const
    {
        std::vector<size_t> results;
        auto leaf = FindLeaf(key);

        // Scan this leaf and subsequent leaves for matching keys (handles duplicates)
        auto node = leaf;
        while (node)
        {
            for (size_t i = 0; i < node->keys.size(); ++i)
            {
                if (node->keys[i] == key)
                    results.push_back(node->row_indices[i]);
                else if (key < node->keys[i])
                    return results; // Past our key, done
            }
            node = node->next_leaf;
        }
        return results;
    }

    std::vector<size_t> BTree::RangeScan(const std::optional<Value> &low, bool low_inclusive,
                                          const std::optional<Value> &high, bool high_inclusive) const
    {
        std::vector<size_t> results;

        std::shared_ptr<BTreeNode> node;
        if (low.has_value())
            node = FindLeaf(low.value());
        else
            node = FindLeftmostLeaf();

        while (node)
        {
            for (size_t i = 0; i < node->keys.size(); ++i)
            {
                const Value &k = node->keys[i];

                // Check low bound
                if (low.has_value())
                {
                    if (low_inclusive)
                    {
                        if (k < low.value()) continue;
                    }
                    else
                    {
                        if (k < low.value() || k == low.value()) continue;
                    }
                }

                // Check high bound
                if (high.has_value())
                {
                    if (high_inclusive)
                    {
                        if (high.value() < k) return results;
                    }
                    else
                    {
                        if (high.value() < k || k == high.value()) return results;
                    }
                }

                results.push_back(node->row_indices[i]);
            }
            node = node->next_leaf;
        }
        return results;
    }

    std::vector<BTreeEntry> BTree::GetAllEntries() const
    {
        std::vector<BTreeEntry> entries;
        auto node = FindLeftmostLeaf();
        while (node)
        {
            for (size_t i = 0; i < node->keys.size(); ++i)
                entries.push_back({node->keys[i], node->row_indices[i]});
            node = node->next_leaf;
        }
        return entries;
    }

    void BTree::BulkLoad(const std::vector<std::pair<Value, size_t>> &entries)
    {
        // Reset tree
        root_ = std::make_shared<BTreeNode>();

        // Sort and insert (simple approach)
        auto sorted = entries;
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });

        for (const auto &e : sorted)
            Insert(e.first, e.second);
    }

} // namespace sql
