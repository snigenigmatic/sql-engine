#pragma once

#include "common/value.h"
#include <vector>
#include <memory>
#include <optional>
#include <algorithm>
#include <functional>

namespace sql{
    //Basic B+ tree implementation for indexing (not fully featured, just a starting point)
    constexpr int BTREE_ORDER = 4; // Max keys per node
    // max keys - order-1
    constexpr int BTREE_MAX_KEYS = BTREE_ORDER - 1;
    constexpr int BTREE_MIN_KEYS = (BTREE_ORDER / 2) - 1; // Min keys per node (except root)

    struct BTreeEntry{
        Value key;
        size_t row_index; // Points to the tuple in the table
    };

    struct BTreeNode{
        bool is_leaf = true;
        std::vector<Value> keys;
        std::vector<size_t> row_indices;                  // only for leaf nodes, parallel to keys
        std::vector<std::shared_ptr<BTreeNode>> children; // only for internal nodes
        std::shared_ptr<BTreeNode> next_leaf = nullptr;   // leaf linked list

        BTreeNode() = default;
    };

    class BTree
    {
    public:
        BTree() : root_(std::make_shared<BTreeNode>()) {}

        // Insert a key with its row index
        void Insert(const Value &key, size_t row_index);

        // Remove all entries with the given key
        void Remove(const Value &key);

        // Point lookup: find all row indices matching key
        std::vector<size_t> Search(const Value &key) const;

        // Range scan: find all row indices where key is in [low, high]
        // Pass nullopt for unbounded side
        std::vector<size_t> RangeScan(const std::optional<Value> &low, bool low_inclusive,
                                       const std::optional<Value> &high, bool high_inclusive) const;

        // Get all entries (for debugging / full scan fallback)
        std::vector<BTreeEntry> GetAllEntries() const;

        // Rebuild the index from scratch given column values and their row indices
        void BulkLoad(const std::vector<std::pair<Value, size_t>> &entries);

        bool IsEmpty() const { return root_->keys.empty(); }

    private:
        struct SplitResult
        {
            Value median_key;
            std::shared_ptr<BTreeNode> new_node;
        };

        std::optional<SplitResult> InsertInternal(std::shared_ptr<BTreeNode> node,
                                                   const Value &key, size_t row_index);

        // Find the leaf node where key should go
        std::shared_ptr<BTreeNode> FindLeaf(const Value &key) const;

        // Find the leftmost leaf
        std::shared_ptr<BTreeNode> FindLeftmostLeaf() const;

        std::shared_ptr<BTreeNode> root_;
    };
} // namespace sql