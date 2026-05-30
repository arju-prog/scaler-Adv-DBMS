// Lab 6 — B-Tree
// Arjun, 24BCS10109
//
// A CLRS-style B-Tree of minimum degree t:
//   - every internal node has between t-1 and 2t-1 keys (root may have less),
//   - every internal node with k keys has exactly k+1 children,
//   - all leaves sit at the same depth.
//
// Insertion uses the "proactive split" strategy: on the way down, any
// full child (2t-1 keys) is split before we descend into it, so insertion
// can be done in a single root-to-leaf pass with no upward fix-up.

#include <iostream>
#include <vector>

class BTree {
    struct Node {
        std::vector<int>   keys;
        std::vector<Node*> children;
        bool               leaf;

        explicit Node(bool is_leaf) : leaf(is_leaf) {}
        ~Node() { for (Node* c : children) delete c; }

        bool full(int t) const { return static_cast<int>(keys.size()) == 2 * t - 1; }
    };

    Node* root_ = nullptr;
    int   t_;                       // minimum degree

public:
    explicit BTree(int t) : t_(t) {
        if (t_ < 2) t_ = 2;         // t=1 is degenerate, not a real B-tree
    }

    ~BTree() { delete root_; }

    BTree(const BTree&)            = delete;
    BTree& operator=(const BTree&) = delete;

    // ----- public ops --------------------------------------------------

    void insert(int key) {
        if (!root_) {
            root_ = new Node(true);
            root_->keys.push_back(key);
            return;
        }

        // If the root is full we grow the tree's height by 1 — create a
        // new root, make the old root its only child, then split it.
        if (root_->full(t_)) {
            Node* new_root = new Node(false);
            new_root->children.push_back(root_);
            splitChild(new_root, 0);
            root_ = new_root;
        }
        insertNonFull(root_, key);
    }

    bool search(int key) const {
        return root_ && searchIn(root_, key);
    }

    void traverse(std::ostream& os) const {
        if (!root_) { os << "<empty>"; return; }
        bool first = true;
        traverseRec(root_, os, first);
    }

    // Indented tree picture, one line per node.
    void prettyPrint(std::ostream& os) const {
        if (!root_) { os << "<empty>\n"; return; }
        prettyRec(root_, 0, os);
    }

    // Returns "" if the B-tree invariants all hold, otherwise a diagnostic.
    std::string verify() const {
        if (!root_) return "";
        int leaf_depth = -1;
        return verifyRec(root_, /*is_root=*/true, /*depth=*/0, leaf_depth);
    }

private:
    // ----- search ------------------------------------------------------

    bool searchIn(const Node* n, int key) const {
        int i = 0;
        while (i < static_cast<int>(n->keys.size()) && key > n->keys[i]) ++i;
        if (i < static_cast<int>(n->keys.size()) && n->keys[i] == key) return true;
        if (n->leaf) return false;
        return searchIn(n->children[i], key);
    }

    // ----- split -------------------------------------------------------

    // Splits a full child of `parent` at slot `i`. After the split, the
    // median key of the child moves up into `parent` and the child's
    // upper half becomes a brand-new sibling.
    void splitChild(Node* parent, int i) {
        Node* y = parent->children[i];
        Node* z = new Node(y->leaf);

        // Move keys[t..2t-2] from y to z (t-1 keys).
        z->keys.assign(y->keys.begin() + t_, y->keys.end());

        // If y isn't a leaf, move its upper t children to z.
        if (!y->leaf) {
            z->children.assign(y->children.begin() + t_, y->children.end());
            y->children.erase(y->children.begin() + t_, y->children.end());
        }

        // The median key (y->keys[t-1]) is promoted into parent.
        int median = y->keys[t_ - 1];
        y->keys.erase(y->keys.begin() + (t_ - 1), y->keys.end());

        parent->children.insert(parent->children.begin() + i + 1, z);
        parent->keys.insert(parent->keys.begin() + i, median);
    }

    // ----- insert ------------------------------------------------------

    void insertNonFull(Node* n, int key) {
        int i = static_cast<int>(n->keys.size()) - 1;

        if (n->leaf) {
            n->keys.push_back(0);                              // make room
            while (i >= 0 && key < n->keys[i]) {
                n->keys[i + 1] = n->keys[i];
                --i;
            }
            n->keys[i + 1] = key;
            return;
        }

        // Find the child to descend into.
        while (i >= 0 && key < n->keys[i]) --i;
        ++i;

        // If that child is full, split it first so we stay in a single
        // top-down pass.
        if (n->children[i]->full(t_)) {
            splitChild(n, i);
            if (key > n->keys[i]) ++i;
        }
        insertNonFull(n->children[i], key);
    }

    // ----- traversal / printing ---------------------------------------

    static void traverseRec(const Node* n, std::ostream& os, bool& first) {
        for (std::size_t i = 0; i < n->keys.size(); ++i) {
            if (!n->leaf) traverseRec(n->children[i], os, first);
            if (!first) os << ' ';
            os << n->keys[i];
            first = false;
        }
        if (!n->leaf) traverseRec(n->children.back(), os, first);
    }

    static void prettyRec(const Node* n, int depth, std::ostream& os) {
        os << std::string(static_cast<std::size_t>(depth) * 4, ' ') << "[";
        for (std::size_t i = 0; i < n->keys.size(); ++i) {
            if (i) os << ' ';
            os << n->keys[i];
        }
        os << "]" << (n->leaf ? " (leaf)" : "") << "\n";
        for (Node* c : n->children) prettyRec(c, depth + 1, os);
    }

    // ----- invariant checker ------------------------------------------

    std::string verifyRec(const Node* n,
                          bool is_root,
                          int depth,
                          int& leaf_depth) const {
        const int k = static_cast<int>(n->keys.size());

        if (k > 2 * t_ - 1)
            return "node has more than 2t-1 keys";
        if (!is_root && k < t_ - 1)
            return "non-root node has fewer than t-1 keys";

        // Keys must be sorted.
        for (int i = 1; i < k; ++i)
            if (n->keys[i] <= n->keys[i - 1])
                return "keys not in strictly increasing order";

        if (n->leaf) {
            if (leaf_depth == -1) leaf_depth = depth;
            else if (leaf_depth != depth)
                return "leaves are at different depths";
            if (!n->children.empty())
                return "leaf has children";
            return "";
        }

        if (static_cast<int>(n->children.size()) != k + 1)
            return "internal node has wrong number of children";

        for (int i = 0; i <= k; ++i) {
            const Node* c = n->children[i];
            // Range check: keys in child i must be < keys[i] (for i<k)
            // and > keys[i-1] (for i>0).
            for (int x : c->keys) {
                if (i < k && x >= n->keys[i])
                    return "child key violates upper bound from parent";
                if (i > 0 && x <= n->keys[i - 1])
                    return "child key violates lower bound from parent";
            }
            std::string e = verifyRec(c, /*is_root=*/false, depth + 1, leaf_depth);
            if (!e.empty()) return e;
        }
        return "";
    }
};

// ---------------------------------------------------------------------
// Demo
// ---------------------------------------------------------------------

static void demoSmall() {
    std::cout << "\n--- Demo 1: B-Tree of minimum degree t=3, insert 10 keys ---\n";
    BTree tree(3);
    const std::vector<int> ks = {10, 20, 5, 6, 12, 30, 7, 17, 25, 15};
    for (int k : ks) tree.insert(k);

    std::cout << "structure:\n";
    tree.prettyPrint(std::cout);

    std::cout << "in-order traversal: ";
    tree.traverse(std::cout);
    std::cout << "\n";

    for (int q : {6, 15, 100}) {
        std::cout << "search(" << q << ") -> "
                  << (tree.search(q) ? "found" : "not found") << "\n";
    }

    auto err = tree.verify();
    std::cout << "invariants: " << (err.empty() ? "OK" : err) << "\n";
}

static void demoSequential() {
    std::cout << "\n--- Demo 2: t=2 (a 2-3-4 tree), insert 1..20 in order ---\n";
    BTree tree(2);
    for (int i = 1; i <= 20; ++i) tree.insert(i);

    tree.prettyPrint(std::cout);
    std::cout << "in-order: ";
    tree.traverse(std::cout);
    std::cout << "\n";

    auto err = tree.verify();
    std::cout << "invariants: " << (err.empty() ? "OK" : err) << "\n";
}

static void demoHeightBound() {
    std::cout << "\n--- Demo 3: t=50, insert 100 000 sequential keys ---\n";
    BTree tree(50);
    for (int i = 0; i < 100000; ++i) tree.insert(i);

    auto err = tree.verify();
    std::cout << "invariants: " << (err.empty() ? "OK" : err) << "\n";

    int hits = 0;
    for (int q : {0, 1, 99, 12345, 50000, 99999, 100000, -1}) {
        if (tree.search(q)) ++hits;
    }
    std::cout << "spot-check searches: " << hits << "/8 found\n";
    std::cout << "(expected 6 hits — the queries 100000 and -1 are out of range)\n";
}

int main() {
    std::cout << "B-Tree demo — Arjun, 24BCS10109\n";
    demoSmall();
    demoSequential();
    demoHeightBound();
    return 0;
}
