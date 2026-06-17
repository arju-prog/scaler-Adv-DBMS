// Lab 7 — Shunting-Yard SQL WHERE evaluator
// Arjun, 24BCS10109
//
// Pipeline:
//   1. Tokenize the WHERE clause.
//   2. Convert infix → Reverse Polish Notation using Dijkstra's
//      shunting-yard algorithm (an operator stack + a precedence table).
//   3. For each candidate row, walk the RPN list once with an int stack
//      to produce 0 / 1 — true rows make it into the result set.
//
// No tree, no recursion, no smart pointers. The RPN order itself
// dictates what to evaluate first; we never have to re-parse.

#include <cctype>
#include <iostream>
#include <stack>
#include <string>
#include <vector>

namespace {

enum class TokKind { Ident, Number, Op, LParen, RParen };

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

// Precedence: comparisons bind tightest, then AND, then OR.
// Parens are not in the precedence table — they are handled separately.
int precedenceOf(const std::string& op) {
    if (op == "OR")  return 1;
    if (op == "AND") return 2;
    if (op == "=" || op == "<" || op == ">" || op == "<=" || op == ">=")
        return 3;
    return 0;
}

std::vector<Token> tokenize(const std::string& sql) {
    std::vector<Token> out;
    const std::size_t n = sql.size();
    std::size_t i = 0;

    while (i < n) {
        unsigned char c = static_cast<unsigned char>(sql[i]);

        if (std::isspace(c)) { ++i; continue; }

        // identifier / keyword
        if (std::isalpha(c) || c == '_') {
            std::size_t j = i;
            while (j < n) {
                unsigned char d = static_cast<unsigned char>(sql[j]);
                if (!std::isalnum(d) && d != '_') break;
                ++j;
            }
            std::string word   = sql.substr(i, j - i);
            std::string upcase = toUpper(word);
            if (upcase == "AND" || upcase == "OR")
                out.push_back({TokKind::Op, upcase});
            else
                out.push_back({TokKind::Ident, word});
            i = j;
            continue;
        }

        // integer literal
        if (std::isdigit(c)) {
            std::size_t j = i;
            while (j < n && std::isdigit(static_cast<unsigned char>(sql[j])))
                ++j;
            out.push_back({TokKind::Number, sql.substr(i, j - i)});
            i = j;
            continue;
        }

        // two-char comparisons (>=, <=)
        if ((c == '>' || c == '<') && i + 1 < n && sql[i + 1] == '=') {
            out.push_back({TokKind::Op, std::string{static_cast<char>(c), '='}});
            i += 2;
            continue;
        }

        // single-char operator / paren
        if (c == '>' || c == '<' || c == '=') {
            out.push_back({TokKind::Op, std::string(1, static_cast<char>(c))});
            ++i; continue;
        }
        if (c == '(') { out.push_back({TokKind::LParen, "("}); ++i; continue; }
        if (c == ')') { out.push_back({TokKind::RParen, ")"}); ++i; continue; }

        // anything else: skip silently so we don't crash on stray punctuation
        ++i;
    }
    return out;
}

// Dijkstra's shunting-yard. We treat AND / OR / comparisons identically
// — all binary, all left-associative — so the rule is simply "pop while
// the stack top has precedence ≥ ours."
std::vector<Token> shuntingYard(const std::vector<Token>& in) {
    std::vector<Token> rpn;
    std::stack<Token>  ops;

    for (const Token& t : in) {
        switch (t.kind) {
            case TokKind::Ident:
            case TokKind::Number:
                rpn.push_back(t);
                break;

            case TokKind::Op:
                while (!ops.empty()
                       && ops.top().kind == TokKind::Op
                       && precedenceOf(ops.top().text) >= precedenceOf(t.text)) {
                    rpn.push_back(ops.top());
                    ops.pop();
                }
                ops.push(t);
                break;

            case TokKind::LParen:
                ops.push(t);
                break;

            case TokKind::RParen:
                while (!ops.empty() && ops.top().kind != TokKind::LParen) {
                    rpn.push_back(ops.top());
                    ops.pop();
                }
                if (!ops.empty()) ops.pop();   // discard the matching '('
                break;
        }
    }
    while (!ops.empty()) { rpn.push_back(ops.top()); ops.pop(); }
    return rpn;
}

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
    return 0;
}

// Walks the RPN list once with an int stack. Operands are pushed as their
// resolved values; operators pop two, compute, push the result back.
bool evaluate(const std::vector<Token>& rpn, const Student& row) {
    std::stack<int> st;
    for (const Token& t : rpn) {
        if (t.kind == TokKind::Number) {
            st.push(std::stoi(t.text));
        } else if (t.kind == TokKind::Ident) {
            st.push(columnValue(t.text, row));
        } else {
            int b = st.top(); st.pop();
            int a = st.top(); st.pop();
            const std::string& op = t.text;
            if      (op == "=")   st.push(a == b);
            else if (op == "<")   st.push(a <  b);
            else if (op == ">")   st.push(a >  b);
            else if (op == "<=")  st.push(a <= b);
            else if (op == ">=")  st.push(a >= b);
            else if (op == "AND") st.push(a && b);
            else if (op == "OR")  st.push(a || b);
        }
    }
    return !st.empty() && st.top() != 0;
}

} // anonymous namespace

int main() {
    std::cout << "Lab 7 — shunting-yard (Arjun, 24BCS10109)\n\n";

    const std::string clause = "marks >= 80 AND (age < 20 OR id = 5)";
    std::cout << "Infix WHERE : " << clause << "\n";

    const auto tokens = tokenize(clause);
    const auto rpn    = shuntingYard(tokens);

    std::cout << "Postfix RPN : ";
    for (const auto& t : rpn) std::cout << t.text << ' ';
    std::cout << "\n\n";

    const std::vector<Student> students = {
        {1, "Priya", 19, 88},
        {2, "Rohan", 22, 67},
        {3, "Sneha", 20, 91},
        {4, "Arjun", 23, 74},
        {5, "Meera", 21, 95},
        {6, "Karan", 18, 59},
    };

    std::cout << "Matching rows (SELECT name ...):\n";
    for (const Student& s : students) {
        if (evaluate(rpn, s))
            std::cout << "  " << s.name
                      << "  (id=" << s.id
                      << ", age="   << s.age
                      << ", marks=" << s.marks << ")\n";
    }
    return 0;
}
