//
// Created by Yao ACHI on 12/11/2025.
//

#ifndef KIO_UTILS_H
#define KIO_UTILS_H
#include "model.h"

namespace kio
{
    constexpr const char* ToString(const MetricType type) noexcept
    {
        switch (type)
        {
            case MetricType::Counter:
                return "counter";
            case MetricType::Gauge:
                return "gauge";
        }
        return "untyped";
    }

    inline std::string SanitizeLabelValue(const std::string& v)
    {
        bool needs_escape = false;
        for (char c : v) {
            if (c == '\\' || c == '"' || c == '\n') {
                needs_escape = true;
                break;
            }
        }

        if (!needs_escape) {
            return v; 
        }

        std::string out;
        for (const char c: v)
        {
            switch (c)
            {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                default:
                    out += c;
                    break;
            }
        }
        return out;
    }

    inline std::string SanitizeLabelKey(const std::string& key)
    {
        std::string out;
        for (size_t i = 0; i < key.size(); ++i)
        {
            char c = key[i];
            if ((i == 0 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) || (i > 0 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')))
            {
                out += c;
            }
            else
            {
                out += '_';
            }
        }
        return out;
    }

}  // namespace kio

#endif  // KIO_UTILS_H
