#pragma once

// Concurrent skiplist (LevelDB's design): one writer at a time (serialized
// by the commit path), any number of lock-free readers. Nodes are
// arena-allocated and never freed individually; next-pointers use
// release/acquire so a reader that observes a node also observes its
// contents. Keys are immutable once inserted.

#include <atomic>
#include <cassert>

#include "util/arena.h"
#include "util/random.h"

namespace strata {

template <typename Key, class Comparator> class SkipList {
  private:
    struct Node;

  public:
    SkipList(Comparator cmp, Arena* arena)
        : compare_(cmp), arena_(arena), head_(new_node(Key(), kMaxHeight)), max_height_(1),
          rnd_(0xdeadbeef) {
        for (int i = 0; i < kMaxHeight; ++i) {
            head_->set_next(i, nullptr);
        }
    }

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // REQUIRES: nothing equal to key is present (internal keys are unique).
    void insert(const Key& key) {
        Node* prev[kMaxHeight];
        Node* x = find_greater_or_equal(key, prev);
        assert(x == nullptr || !equal(key, x->key));

        const int height = random_height();
        if (height > get_max_height()) {
            for (int i = get_max_height(); i < height; ++i) {
                prev[i] = head_;
            }
            // Relaxed is fine: a racing reader seeing the old height just
            // misses the new upper levels; the level-0 chain is authoritative.
            max_height_.store(height, std::memory_order_relaxed);
        }

        x = new_node(key, height);
        for (int i = 0; i < height; ++i) {
            x->no_barrier_set_next(i, prev[i]->no_barrier_next(i));
            prev[i]->set_next(i, x); // release: publishes the node
        }
    }

    bool contains(const Key& key) const {
        const Node* x = find_greater_or_equal(key, nullptr);
        return x != nullptr && equal(key, x->key);
    }

    class Iterator {
      public:
        explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

        bool valid() const {
            return node_ != nullptr;
        }

        const Key& key() const {
            assert(valid());
            return node_->key;
        }

        void next() {
            assert(valid());
            node_ = node_->next(0);
        }

        void seek(const Key& target) {
            node_ = list_->find_greater_or_equal(target, nullptr);
        }

        void seek_to_first() {
            node_ = list_->head_->next(0);
        }

      private:
        const SkipList* list_;
        Node* node_;
    };

  private:
    static constexpr int kMaxHeight = 12;
    static constexpr int kBranching = 4;

    int get_max_height() const {
        return max_height_.load(std::memory_order_relaxed);
    }

    Node* new_node(const Key& key, int height) {
        char* mem = arena_->allocate_aligned(
            sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<std::size_t>(height - 1));
        return new (mem) Node(key);
    }

    int random_height() {
        int height = 1;
        while (height < kMaxHeight && rnd_.one_in(kBranching)) {
            ++height;
        }
        return height;
    }

    bool equal(const Key& a, const Key& b) const {
        return compare_(a, b) == 0;
    }

    bool key_is_after_node(const Key& key, Node* n) const {
        return n != nullptr && compare_(n->key, key) < 0;
    }

    // First node >= key. If prev != nullptr, fills prev[0..kMaxHeight)
    // with the immediate predecessors at each level.
    Node* find_greater_or_equal(const Key& key, Node** prev) const {
        Node* x = head_;
        int level = get_max_height() - 1;
        while (true) {
            Node* next = x->next(level);
            if (key_is_after_node(key, next)) {
                x = next;
            } else {
                if (prev != nullptr) {
                    prev[level] = x;
                }
                if (level == 0) {
                    return next;
                }
                --level;
            }
        }
    }

    Comparator const compare_;
    Arena* const arena_;
    Node* const head_;
    std::atomic<int> max_height_;
    Random rnd_;
};

template <typename Key, class Comparator> struct SkipList<Key, Comparator>::Node {
    explicit Node(const Key& k) : key(k) {}

    Key const key;

    Node* next(int level) {
        return next_[level].load(std::memory_order_acquire);
    }
    void set_next(int level, Node* x) {
        next_[level].store(x, std::memory_order_release);
    }
    Node* no_barrier_next(int level) {
        return next_[level].load(std::memory_order_relaxed);
    }
    void no_barrier_set_next(int level, Node* x) {
        next_[level].store(x, std::memory_order_relaxed);
    }

  private:
    // Length == node height; the tail is allocated past the struct.
    std::atomic<Node*> next_[1];
};

} // namespace strata
