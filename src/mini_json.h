#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// A small JSON reader for the files the game itself reads (a texture pack's
// pack.json): objects, arrays, strings, numbers, true, false and null.
// Strings are bytes as written; \u escapes become '?' (the names here are
// ASCII).
namespace rg::json
{

struct Value
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    const Value *find(const char *name) const
    {
        for (const auto &member : members)
        {
            if (member.first == name)
            {
                return &member.second;
            }
        }
        return nullptr;
    }
};

class Reader
{
public:
    explicit Reader(const std::string &text) : m_text(text) {}

    // The whole text must be one value.
    bool parse(Value &out)
    {
        if (!value(out, 0))
        {
            return false;
        }
        skip();
        return m_at == m_text.size();
    }

private:
    const std::string &m_text;
    size_t m_at = 0u;

    void skip()
    {
        while (m_at < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_at])))
        {
            ++m_at;
        }
    }

    bool word(const char *text)
    {
        const size_t length = std::strlen(text);
        if (m_text.compare(m_at, length, text) != 0)
        {
            return false;
        }
        m_at += length;
        return true;
    }

    bool string(std::string &out)
    {
        if (m_at >= m_text.size() || m_text[m_at] != '"')
        {
            return false;
        }
        ++m_at;
        while (m_at < m_text.size() && m_text[m_at] != '"')
        {
            char c = m_text[m_at++];
            if (c == '\\' && m_at < m_text.size())
            {
                c = m_text[m_at++];
                switch (c)
                {
                case 'n':
                    c = '\n';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'u':
                    m_at = std::min(m_at + 4u, m_text.size());
                    c = '?';
                    break;
                default:
                    break;
                }
            }
            out.push_back(c);
        }
        if (m_at >= m_text.size())
        {
            return false;
        }
        ++m_at;
        return true;
    }

    bool value(Value &out, int depth)
    {
        skip();
        if (m_at >= m_text.size() || depth > 16)
        {
            return false;
        }
        const char c = m_text[m_at];
        if (c == '"')
        {
            out.type = Value::Type::String;
            return string(out.string);
        }
        if (c == '{' || c == '[')
        {
            return container(out, c == '{', depth);
        }
        if (word("true"))
        {
            out.type = Value::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (word("false"))
        {
            out.type = Value::Type::Bool;
            return true;
        }
        if (word("null"))
        {
            return true;
        }
        char *end = nullptr;
        out.number = std::strtod(m_text.c_str() + m_at, &end);
        if (end == m_text.c_str() + m_at)
        {
            return false;
        }
        out.type = Value::Type::Number;
        m_at = static_cast<size_t>(end - m_text.c_str());
        return true;
    }

    bool container(Value &out, bool object, int depth)
    {
        out.type = object ? Value::Type::Object : Value::Type::Array;
        ++m_at;
        skip();
        if (m_at < m_text.size() && m_text[m_at] == (object ? '}' : ']'))
        {
            ++m_at;
            return true;
        }
        for (;;)
        {
            std::string name;
            if (object)
            {
                skip();
                if (!string(name))
                {
                    return false;
                }
                skip();
                if (m_at >= m_text.size() || m_text[m_at++] != ':')
                {
                    return false;
                }
            }
            Value child;
            if (!value(child, depth + 1))
            {
                return false;
            }
            if (object)
            {
                out.members.emplace_back(std::move(name), std::move(child));
            }
            else
            {
                out.items.push_back(std::move(child));
            }
            skip();
            if (m_at >= m_text.size())
            {
                return false;
            }
            const char next = m_text[m_at++];
            if (next == (object ? '}' : ']'))
            {
                return true;
            }
            if (next != ',')
            {
                return false;
            }
        }
    }
};

} // namespace rg::json
