// Lab 7 — Recursive-Descent SQL parser
// Arjun, 24BCS10109
//
// Pipeline:
//   1. Lex the query into typed tokens.
//   2. Recursive-descent parse into an AST. Precedence is encoded by
//      the grammar itself — `parseOr` calls `parseAnd` calls
//      `parseCompare`, so OR binds loosest and comparisons bind
//      tightest, without any precedence table.
//   3. Evaluate the AST with a tree walk, one walk per candidate row.
//
// Grammar:
//   query   := SELECT name FROM name WHERE expr
//   expr    := and_ (OR and_)*
//   and_    := cmp (AND cmp)*
//   cmp     := '(' expr ')' | name op number
//   op      := '=' | '<' | '>' | '<=' | '>='

#include <cctype>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ----- tokens -----

enum class TokKind {
    KwSelect, KwFrom, KwWhere, KwAnd, KwOr,
    Ident, Number, CmpOp, LParen, RParen, Eof
};

struct Token {
    TokKind     kind;
    std::string text;
};

std::string toUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

std::vector<Token> lex(const std::string& sql) {
    std::vector<Token> toks;
    const std::size_t n = sql.size();
    std::size_t i = 0;

    while (i < n) {
        unsigned char c = static_cast<unsigned char>(sql[i]);

        if (std::isspace(c)) { ++i; continue; }

        if (std::isalpha(c) || c == '_') {
            std::size_t j = i;
            while (j < n) {
                unsigned char d = static_cast<unsigned char>(sql[j]);
                if (!std::isalnum(d) && d != '_') break;
                ++j;
            }
            std::string word = sql.substr(i, j - i);
            std::string up   = toUpper(word);
            i = j;

            if      (up == "SELECT") toks.push_back({TokKind::KwSelect, word});
            else if (up == "FROM")   toks.push_back({TokKind::KwFrom,   word});
            else if (up == "WHERE")  toks.push_back({TokKind::KwWhere,  word});
            else if (up == "AND")    toks.push_back({TokKind::KwAnd,    word});
            else if (up == "OR")     toks.push_back({TokKind::KwOr,     word});
            else                     toks.push_back({TokKind::Ident,    word});
            continue;
        }

        if (std::isdigit(c)) {
            std::size_t j = i;
            while (j < n && std::isdigit(static_cast<unsigned char>(sql[j])))
                ++j;
            toks.push_back({TokKind::Number, sql.substr(i, j - i)});
            i = j;
            continue;
        }

        if ((c == '>' || c == '<') && i + 1 < n && sql[i + 1] == '=') {
            toks.push_back({TokKind::CmpOp, std::string{static_cast<char>(c), '='}});
            i += 2;
            continue;
        }

        if (c == '>' || c == '<' || c == '=') {
            toks.push_back({TokKind::CmpOp, std::string(1, static_cast<char>(c))});
            ++i; continue;
        }
        if (c == '(') { toks.push_back({TokKind::LParen, "("}); ++i; continue; }
        if (c == ')') { toks.push_back({TokKind::RParen, ")"}); ++i; continue; }

        throw std::runtime_error(
            std::string("unexpected character '") + static_cast<char>(c) + "'");
    }
    toks.push_back({TokKind::Eof, ""});
    return toks;
}

// ----- AST -----
//
// All three node shapes share a single struct. `kind` says which
// fields are meaningful:
//   - kind=Column   → text = column name, l/r unused
//   - kind=Literal  → text = integer literal, l/r unused
//   - kind=Binary   → text = operator ("AND" / "OR" / ">" / ">=" / etc.)
//                     l/r = children

enum class NodeKind { Column, Literal, Binary };

struct Node {
    NodeKind              kind;
    std::string           text;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;
};

std::unique_ptr<Node> makeColumn(std::string n) {
    auto node = std::make_unique<Node>();
    node->kind = NodeKind::Column;
    node->text = std::move(n);
    return node;
}
std::unique_ptr<Node> makeLiteral(std::string v) {
    auto node = std::make_unique<Node>();
    node->kind = NodeKind::Literal;
    node->text = std::move(v);
    return node;
}
std::unique_ptr<Node> makeBinary(std::string op,
                                 std::unique_ptr<Node> a,
                                 std::unique_ptr<Node> b) {
    auto node = std::make_unique<Node>();
    node->kind  = NodeKind::Binary;
    node->text  = std::move(op);
    node->left  = std::move(a);
    node->right = std::move(b);
    return node;
}

// ----- parser -----

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

    struct Query {
        std::string           projection;
        std::string           table;
        std::unique_ptr<Node> where;
    };

    Query parseQuery() {
        expect(TokKind::KwSelect);
        std::string col = expect(TokKind::Ident).text;
        expect(TokKind::KwFrom);
        std::string tbl = expect(TokKind::Ident).text;
        expect(TokKind::KwWhere);
        auto where = parseOr();
        expect(TokKind::Eof);
        return Query{std::move(col), std::move(tbl), std::move(where)};
    }

private:
    std::vector<Token> toks_;
    std::size_t        pos_ = 0;

    const Token& peek() const { return toks_[pos_]; }
    Token consume()           { return toks_[pos_++]; }
    Token expect(TokKind k) {
        if (peek().kind != k)
            throw std::runtime_error("parse error near '" + peek().text + "'");
        return consume();
    }

    // OR binds loosest — handled at the outermost level.
    std::unique_ptr<Node> parseOr() {
        auto left = parseAnd();
        while (peek().kind == TokKind::KwOr) {
            consume();
            left = makeBinary("OR", std::move(left), parseAnd());
        }
        return left;
    }

    std::unique_ptr<Node> parseAnd() {
        auto left = parseCompare();
        while (peek().kind == TokKind::KwAnd) {
            consume();
            left = makeBinary("AND", std::move(left), parseCompare());
        }
        return left;
    }

    // Either a parenthesized sub-expression, or a `col op number` triple.
    std::unique_ptr<Node> parseCompare() {
        if (peek().kind == TokKind::LParen) {
            consume();
            auto inner = parseOr();
            expect(TokKind::RParen);
            return inner;
        }
        std::string col    = expect(TokKind::Ident).text;
        std::string op     = expect(TokKind::CmpOp).text;
        std::string number = expect(TokKind::Number).text;
        return makeBinary(op, makeColumn(col), makeLiteral(number));
    }
};

// ----- evaluator -----

struct Student {
    int         id;
    std::string name;
    int         age;
    int         marks;
};

int columnValue(const std::string& column, const Student& s) {
    if (column == "id")    return s.id;
    if (column == "age")   return s.age;
    if (column == "marks") return s.marks;
    throw std::runtime_error("unknown column '" + column + "'");
}

bool evalNode(const Node* node, const Student& row) {
    // Boolean nodes.
    if (node->text == "AND")
        return evalNode(node->left.get(), row) && evalNode(node->right.get(), row);
    if (node->text == "OR")
        return evalNode(node->left.get(), row) || evalNode(node->right.get(), row);

    // Otherwise: a comparison whose children are Column / Literal.
    int a = columnValue(node->left->text, row);
    int b = std::stoi(node->right->text);
    const std::string& op = node->text;
    if (op == "=")  return a == b;
    if (op == "<")  return a <  b;
    if (op == ">")  return a >  b;
    if (op == "<=") return a <= b;
    if (op == ">=") return a >= b;
    throw std::runtime_error("unknown comparison '" + op + "'");
}

void printAst(const Node* node, int depth = 0) {
    std::cout << std::string(static_cast<std::size_t>(depth) * 2, ' ')
              << node->text << "\n";
    if (node->kind == NodeKind::Binary) {
        printAst(node->left.get(),  depth + 1);
        printAst(node->right.get(), depth + 1);
    }
}

std::string projectionOf(const std::string& column, const Student& s) {
    if (column == "name")  return s.name;
    if (column == "id")    return std::to_string(s.id);
    if (column == "age")   return std::to_string(s.age);
    if (column == "marks") return std::to_string(s.marks);
    return "(?)";
}

} // anonymous namespace

int main() {
    std::cout << "Lab 7 — recursive-descent parser (Arjun, 24BCS10109)\n\n";

    const std::vector<Student> students = {
        {1, "Priya", 19, 88},
        {2, "Rohan", 22, 67},
        {3, "Sneha", 20, 91},
        {4, "Arjun", 23, 74},
        {5, "Meera", 21, 95},
        {6, "Karan", 18, 59},
    };

    const std::string sql =
        "SELECT name FROM students WHERE marks >= 80 AND (age < 20 OR id = 5)";
    std::cout << "Query: " << sql << "\n\n";

    Parser parser(lex(sql));
    auto query = parser.parseQuery();

    std::cout << "WHERE as an AST (precedence encoded in tree shape):\n";
    printAst(query.where.get());
    std::cout << "\n";

    std::cout << "Matching rows (SELECT " << query.projection
              << " FROM " << query.table << "):\n";
    for (const Student& s : students) {
        if (evalNode(query.where.get(), s))
            std::cout << "  " << projectionOf(query.projection, s) << "\n";
    }
    return 0;
}
