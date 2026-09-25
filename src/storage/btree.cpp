#include "storage/btree.h"
#include <cstring>
#include <stdexcept>
#include <string>

namespace sql
{

    namespace
    {
        constexpr size_t OFF_IS_LEAF = 4;
        constexpr size_t OFF_NEXT_LEAF = 8;
        constexpr size_t OFF_COUNT = 12;
        constexpr size_t HEADER_SIZE = 16;
        constexpr size_t CAPACITY = PAGE_SIZE - HEADER_SIZE;
        constexpr size_t RID_SIZE = sizeof(int32_t) + sizeof(uint32_t);
        constexpr size_t CHILD_SIZE = sizeof(page_id_t);

        size_t KeySize(const Value &key)
        {
            std::string buf;
            key.SerializeTo(&buf);
            return buf.size();
        }

        int CompareRid(const RID &a, const RID &b)
        {
            if (a < b)
                return -1;
            if (b < a)
                return 1;
            return 0;
        }

        template <typename T>
        void Put(std::string *out, T v)
        {
            out->append(reinterpret_cast<const char *>(&v), sizeof(T));
        }

        template <typename T>
        T Take(const char **cursor, const char *end)
        {
            if (end - *cursor < static_cast<std::ptrdiff_t>(sizeof(T)))
                throw std::runtime_error("Corrupt B+tree node");
            T v;
            std::memcpy(&v, *cursor, sizeof(T));
            *cursor += sizeof(T);
            return v;
        }
    } // namespace

    int BTree::CompareKeys(const Value &a, const Value &b)
    {
        if (a.GetType() != b.GetType())
            return static_cast<int>(a.GetType()) < static_cast<int>(b.GetType()) ? -1 : 1;
        if (a.IsNull() || b.IsNull())
            return a.IsNull() == b.IsNull() ? 0 : (a.IsNull() ? -1 : 1);
        if (a < b)
            return -1;
        if (b < a)
            return 1;
        return 0;
    }

    void BTree::CheckKey(const Value &key)
    {
        if (key.IsNull())
            throw std::invalid_argument("NULL values are not indexed");
        const size_t size = KeySize(key);
        if (size > MAX_KEY_SIZE)
            throw std::invalid_argument("Index key too large: " + std::to_string(size) +
                                        " bytes (max " + std::to_string(MAX_KEY_SIZE) + ")");
    }

    // ── Node (de)serialization ───────────────────────────────────────────────

    size_t BTree::EncodedSize(const Node &node)
    {
        size_t size = node.is_leaf ? 0 : CHILD_SIZE;
        for (const auto &key : node.keys)
            size += KeySize(key) + RID_SIZE + (node.is_leaf ? 0 : CHILD_SIZE);
        return size;
    }

    BTree::Node BTree::Load(page_id_t page_id) const
    {
        PageGuard guard = bpm_->FetchPageGuarded(page_id);
        if (!guard)
            throw std::runtime_error("Buffer pool exhausted or unreadable index page " + std::to_string(page_id));
        const Page *page = guard.GetPage();

        Node node;
        node.is_leaf = page->Read<uint8_t>(OFF_IS_LEAF) != 0;
        node.next_leaf = page->Read<page_id_t>(OFF_NEXT_LEAF);
        const auto count = page->Read<uint16_t>(OFF_COUNT);

        const char *cursor = page->GetData() + HEADER_SIZE;
        const char *end = page->GetData() + PAGE_SIZE;
        // +1 leaves room for the insert that usually follows a load
        node.keys.reserve(count + 1u);
        node.rids.reserve(count + 1u);
        if (!node.is_leaf)
        {
            node.children.reserve(count + 2u);
            node.children.push_back(Take<page_id_t>(&cursor, end));
        }
        for (uint16_t i = 0; i < count; ++i)
        {
            node.keys.emplace_back();
            if (!Value::DeserializeFrom(&cursor, end, &node.keys.back()))
                throw std::runtime_error("Corrupt B+tree key on page " + std::to_string(page_id));
            RID rid;
            rid.page_id = Take<int32_t>(&cursor, end);
            rid.slot = Take<uint32_t>(&cursor, end);
            node.rids.push_back(rid);
            if (!node.is_leaf)
                node.children.push_back(Take<page_id_t>(&cursor, end));
        }
        return node;
    }

    std::string BTree::Encode(const Node &node)
    {
        std::string body;
        if (!node.is_leaf)
            Put(&body, node.children[0]);
        for (size_t i = 0; i < node.keys.size(); ++i)
        {
            node.keys[i].SerializeTo(&body);
            Put(&body, node.rids[i].page_id);
            Put(&body, node.rids[i].slot);
            if (!node.is_leaf)
                Put(&body, node.children[i + 1]);
        }
        return body;
    }

    void BTree::Store(page_id_t page_id, const Node &node)
    {
        StoreEncoded(page_id, node, Encode(node));
    }

    void BTree::StoreEncoded(page_id_t page_id, const Node &node, const std::string &body)
    {
        if (body.size() > CAPACITY)
            throw std::logic_error("B+tree node overflow");

        PageGuard guard = bpm_->FetchPageGuarded(page_id);
        if (!guard)
            throw std::runtime_error("Buffer pool exhausted or unreadable index page " + std::to_string(page_id));
        Page *page = guard.GetPage();
        std::memset(page->GetData() + OFF_IS_LEAF, 0, PAGE_SIZE - OFF_IS_LEAF); // keep the LSN
        page->Write<uint8_t>(OFF_IS_LEAF, node.is_leaf ? 1 : 0);
        page->Write<page_id_t>(OFF_NEXT_LEAF, node.next_leaf);
        page->Write<uint16_t>(OFF_COUNT, static_cast<uint16_t>(node.keys.size()));
        std::memcpy(page->GetData() + HEADER_SIZE, body.data(), body.size());
        guard.MarkDirty();
    }

    page_id_t BTree::AllocateNode(const Node &node)
    {
        page_id_t page_id;
        {
            PageGuard guard = bpm_->NewPageGuarded(&page_id);
            if (!guard)
                throw std::runtime_error("Unable to allocate index page");
        }
        Store(page_id, node);
        return page_id;
    }

    // ── Construction ─────────────────────────────────────────────────────────

    std::unique_ptr<BTree> BTree::Create(BufferPoolManager *bpm)
    {
        auto tree = std::make_unique<BTree>(bpm, INVALID_PAGE_ID);
        tree->root_page_id_ = tree->AllocateNode(Node{});
        return tree;
    }

    BTree::BTree(BufferPoolManager *bpm, page_id_t root_page_id)
        : bpm_(bpm), root_page_id_(root_page_id) {}

    // ── Insert ───────────────────────────────────────────────────────────────

    BTree::Split BTree::SplitNode(Node &node, Node *right)
    {
        // Split by encoded bytes so both halves fit even with uneven keys
        const size_t total = EncodedSize(node);
        size_t acc = node.is_leaf ? 0 : CHILD_SIZE;
        size_t mid = 0;
        while (mid < node.keys.size() - 1 && acc < total / 2)
        {
            acc += KeySize(node.keys[mid]) + RID_SIZE + (node.is_leaf ? 0 : CHILD_SIZE);
            ++mid;
        }
        if (mid == 0)
            mid = 1;

        right->is_leaf = node.is_leaf;
        Split split;
        if (node.is_leaf)
        {
            // Right half starts at mid; its first entry becomes the separator
            right->keys.assign(node.keys.begin() + static_cast<long>(mid), node.keys.end());
            right->rids.assign(node.rids.begin() + static_cast<long>(mid), node.rids.end());
            node.keys.resize(mid);
            node.rids.resize(mid);
            split.key = right->keys.front();
            split.rid = right->rids.front();
        }
        else
        {
            // Entry at mid moves up; its right child starts the new node
            split.key = node.keys[mid];
            split.rid = node.rids[mid];
            right->keys.assign(node.keys.begin() + static_cast<long>(mid) + 1, node.keys.end());
            right->rids.assign(node.rids.begin() + static_cast<long>(mid) + 1, node.rids.end());
            right->children.assign(node.children.begin() + static_cast<long>(mid) + 1, node.children.end());
            node.keys.resize(mid);
            node.rids.resize(mid);
            node.children.resize(mid + 1);
        }
        return split;
    }

    std::optional<BTree::Split> BTree::InsertInto(page_id_t page_id, const Value &key, const RID &rid, bool *inserted)
    {
        Node node = Load(page_id);

        // First position whose entry is > (key, rid)
        size_t pos = 0;
        while (pos < node.keys.size())
        {
            int c = CompareKeys(node.keys[pos], key);
            if (c == 0)
                c = CompareRid(node.rids[pos], rid);
            if (c > 0)
                break;
            if (c == 0 && node.is_leaf)
            {
                *inserted = false; // exact duplicate
                return std::nullopt;
            }
            ++pos;
        }

        if (node.is_leaf)
        {
            node.keys.insert(node.keys.begin() + static_cast<long>(pos), key);
            node.rids.insert(node.rids.begin() + static_cast<long>(pos), rid);
            *inserted = true;
        }
        else
        {
            // children[pos] holds entries in [sep[pos-1], sep[pos])
            auto child_split = InsertInto(node.children[pos], key, rid, inserted);
            if (!child_split)
                return std::nullopt;
            node.keys.insert(node.keys.begin() + static_cast<long>(pos), child_split->key);
            node.rids.insert(node.rids.begin() + static_cast<long>(pos), child_split->rid);
            node.children.insert(node.children.begin() + static_cast<long>(pos) + 1, child_split->right_page);
        }

        std::string body = Encode(node);
        if (body.size() <= CAPACITY)
        {
            StoreEncoded(page_id, node, body);
            return std::nullopt;
        }

        Node right;
        Split split = SplitNode(node, &right);
        if (node.is_leaf)
            right.next_leaf = node.next_leaf;
        split.right_page = AllocateNode(right);
        if (node.is_leaf)
            node.next_leaf = split.right_page;
        Store(page_id, node);
        return split;
    }

    bool BTree::Insert(const Value &key, const RID &rid)
    {
        CheckKey(key);
        bool inserted = false;
        auto split = InsertInto(root_page_id_, key, rid, &inserted);
        if (split)
        {
            // Keep the root page id stable: move the (already split) left half
            // out of the root page and make the root a new internal node.
            Node left = Load(root_page_id_);
            const page_id_t left_page = AllocateNode(left);
            Node root;
            root.is_leaf = false;
            root.keys.push_back(split->key);
            root.rids.push_back(split->rid);
            root.children = {left_page, split->right_page};
            Store(root_page_id_, root);
        }
        return inserted;
    }

    // ── Lookup ───────────────────────────────────────────────────────────────

    page_id_t BTree::FindLeafForKey(const Value &key) const
    {
        page_id_t page_id = root_page_id_;
        while (true)
        {
            Node node = Load(page_id);
            if (node.is_leaf)
                return page_id;
            // Descend into the child after all separators with key < target
            size_t i = 0;
            while (i < node.keys.size() && CompareKeys(node.keys[i], key) < 0)
                ++i;
            page_id = node.children[i];
        }
    }

    page_id_t BTree::FindLeftmostLeaf() const
    {
        page_id_t page_id = root_page_id_;
        while (true)
        {
            Node node = Load(page_id);
            if (node.is_leaf)
                return page_id;
            page_id = node.children[0];
        }
    }

    bool BTree::Remove(const Value &key, const RID &rid)
    {
        if (key.IsNull())
            return false;
        page_id_t page_id = root_page_id_;
        while (true)
        {
            Node node = Load(page_id);
            if (node.is_leaf)
            {
                for (size_t i = 0; i < node.keys.size(); ++i)
                {
                    if (CompareKeys(node.keys[i], key) == 0 && node.rids[i] == rid)
                    {
                        node.keys.erase(node.keys.begin() + static_cast<long>(i));
                        node.rids.erase(node.rids.begin() + static_cast<long>(i));
                        Store(page_id, node);
                        return true;
                    }
                }
                return false;
            }
            // Route by the exact (key, rid) entry
            size_t i = 0;
            while (i < node.keys.size())
            {
                int c = CompareKeys(node.keys[i], key);
                if (c == 0)
                    c = CompareRid(node.rids[i], rid);
                if (c > 0)
                    break;
                ++i;
            }
            page_id = node.children[i];
        }
    }

    std::vector<RID> BTree::Search(const Value &key) const
    {
        return RangeScan(key, true, key, true);
    }

    std::vector<RID> BTree::RangeScan(const std::optional<Value> &low, bool low_inclusive,
                                      const std::optional<Value> &high, bool high_inclusive) const
    {
        std::vector<RID> results;
        page_id_t page_id = low ? FindLeafForKey(*low) : FindLeftmostLeaf();
        while (page_id != INVALID_PAGE_ID)
        {
            Node node = Load(page_id);
            for (size_t i = 0; i < node.keys.size(); ++i)
            {
                const Value &k = node.keys[i];
                if (low)
                {
                    const int c = CompareKeys(k, *low);
                    if (c < 0 || (c == 0 && !low_inclusive))
                        continue;
                }
                if (high)
                {
                    const int c = CompareKeys(k, *high);
                    if (c > 0 || (c == 0 && !high_inclusive))
                        return results;
                }
                results.push_back(node.rids[i]);
            }
            page_id = node.next_leaf;
        }
        return results;
    }

    std::vector<BTreeEntry> BTree::GetAllEntries() const
    {
        std::vector<BTreeEntry> entries;
        page_id_t page_id = FindLeftmostLeaf();
        while (page_id != INVALID_PAGE_ID)
        {
            Node node = Load(page_id);
            for (size_t i = 0; i < node.keys.size(); ++i)
                entries.push_back({node.keys[i], node.rids[i]});
            page_id = node.next_leaf;
        }
        return entries;
    }

    bool BTree::IsEmpty() const
    {
        page_id_t page_id = FindLeftmostLeaf();
        while (page_id != INVALID_PAGE_ID)
        {
            Node node = Load(page_id);
            if (!node.keys.empty())
                return false;
            page_id = node.next_leaf;
        }
        return true;
    }

    int BTree::GetHeight() const
    {
        int height = 1;
        page_id_t page_id = root_page_id_;
        while (true)
        {
            Node node = Load(page_id);
            if (node.is_leaf)
                return height;
            page_id = node.children[0];
            ++height;
        }
    }

    void BTree::Drop()
    {
        std::vector<page_id_t> stack{root_page_id_};
        std::vector<page_id_t> pages;
        while (!stack.empty())
        {
            page_id_t page_id = stack.back();
            stack.pop_back();
            Node node = Load(page_id);
            pages.push_back(page_id);
            if (!node.is_leaf)
                stack.insert(stack.end(), node.children.begin(), node.children.end());
        }
        for (page_id_t page_id : pages)
            bpm_->DeletePage(page_id);
        root_page_id_ = INVALID_PAGE_ID;
    }

} // namespace sql
