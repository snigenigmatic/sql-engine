#pragma once

#include "common/value.h"
#include "storage/buffer_pool.h"
#include "storage/rid.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sql
{

    struct BTreeEntry
    {
        Value key;
        RID rid; // Location of the tuple in the table heap
    };

    // Disk-resident B+ tree mapping column values to RIDs.
    //
    // Every entry is a unique (key, RID) pair ordered by key then RID, so
    // duplicate keys need no special handling and a specific row's entry can
    // be removed exactly. Internal nodes hold (key, RID) separators; leaves
    // are linked left to right for range scans.
    //
    // Each node occupies one page and is (de)serialized on access:
    //   [0..4)   page LSN (reserved for the WAL)
    //   [4]      is_leaf
    //   [8..12)  next leaf page id (leaves only)
    //   [12..14) entry count
    //   [16..)   leaf:     { key, rid } * count
    //            internal: child0, { key, rid, child } * count
    // Keys use Value's binary encoding. The root page id never changes (a
    // root split moves the old root's contents to a new page), so the catalog
    // only records it once.
    //
    // Deletes remove entries without merging underfull nodes; empty leaves
    // stay linked and are skipped by scans.
    class BTree
    {
    public:
        // Maximum encoded key size; guarantees at least three entries per node
        static constexpr size_t MAX_KEY_SIZE = 1024;

        // Allocate an empty tree. Throws on allocation failure.
        static std::unique_ptr<BTree> Create(BufferPoolManager *bpm);

        // Attach to an existing tree
        BTree(BufferPoolManager *bpm, page_id_t root_page_id);

        page_id_t GetRootPageId() const { return root_page_id_; }

        // Throws std::invalid_argument if the key is NULL or too large to
        // index. Use CheckKey to validate before modifying the table.
        static void CheckKey(const Value &key);

        // Insert (key, rid). Returns false if that exact pair already exists.
        bool Insert(const Value &key, const RID &rid);

        // Remove the exact (key, rid) entry. Returns false if absent.
        bool Remove(const Value &key, const RID &rid);

        // Point lookup: RIDs of all entries with this key, in RID order
        std::vector<RID> Search(const Value &key) const;

        // Range scan: RIDs with key in [low, high] (bounds optional,
        // inclusiveness configurable), in key order
        std::vector<RID> RangeScan(const std::optional<Value> &low, bool low_inclusive,
                                   const std::optional<Value> &high, bool high_inclusive) const;

        // All entries in order (debugging / tests)
        std::vector<BTreeEntry> GetAllEntries() const;

        bool IsEmpty() const;

        // Height of the tree (1 = root is a leaf)
        int GetHeight() const;

        // Leaf page where a lookup for key starts (diagnostics and tests)
        page_id_t GetLeafPageForKey(const Value &key) const { return FindLeafForKey(key); }

        // Return every page to the free list. The tree is unusable afterwards.
        // Throws std::runtime_error (before freeing anything) if a page is
        // still pinned, or if the pager fails to free a page.
        void Drop();

        // Total order used by the tree: by type first, then by value
        static int CompareKeys(const Value &a, const Value &b);

    private:
        struct Node
        {
            bool is_leaf = true;
            page_id_t next_leaf = INVALID_PAGE_ID;
            std::vector<Value> keys;
            std::vector<RID> rids;
            std::vector<page_id_t> children; // internal: keys.size() + 1
        };

        struct Split
        {
            Value key;
            RID rid;
            page_id_t right_page;
        };

        Node Load(page_id_t page_id) const;
        static std::string Encode(const Node &node);
        void Store(page_id_t page_id, const Node &node);
        void StoreEncoded(page_id_t page_id, const Node &node, const std::string &body);
        page_id_t AllocateNode(const Node &node);
        static size_t EncodedSize(const Node &node);

        std::optional<Split> InsertInto(page_id_t page_id, const Value &key, const RID &rid, bool *inserted);
        Split SplitNode(Node &node, Node *right);

        // Leaf page holding the first entry whose key is >= key
        page_id_t FindLeafForKey(const Value &key) const;
        page_id_t FindLeftmostLeaf() const;

        BufferPoolManager *bpm_;
        page_id_t root_page_id_;
    };

} // namespace sql
