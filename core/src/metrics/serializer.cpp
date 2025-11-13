//
// Created by Yao ACHI on 12/11/2025.
//
#include "core/include/metrics/serializer.h"

#include <sstream>

#include "core/include/metrics/utils.h"

namespace kio
{
    std::string PrometheusTextSerializer::Serialize(const MetricSnapshot& snapshot)
    {
        std::ostringstream ss;
        for (const auto& family: snapshot.GetFamilies())
        {
            ss << "# HELP " << family.name << " " << family.help << "\n";
            ss << "# TYPE " << family.name << " " << ToString(family.type) << "\n";

            for (const auto& sample: family.samples)
            {
                ss << family.name;
                if (!sample.labels.empty())
                {
                    ss << "{";
                    bool first = true;
                    for (const auto& pair: sample.labels)
                    {
                        if (!first) ss << ",";
                        ss << SanitizeLabelKey(pair.first) << "=\"" << SanitizeLabelValue(pair.second) << "\"";
                        first = false;
                    }
                    ss << "}";
                }
                ss << " " << sample.value << "\n";
            }
            // Extra newline between families
            ss << "\n";
        }
        return ss.str();
    }

    std::string PrometheusJsonMetricSerializer::Serialize(const MetricSnapshot& snapshot)
    {
        return "";
        // TODO
        // nlohmann::json j;
        // for (auto& fam : snapshot.GetFamilies()) {
        //     nlohmann::json jf;
        //     jf["name"] = fam.name;
        //     jf["type"] = ToString(fam.type);
        //     jf["help"] = fam.help;
        //     for (auto& s : fam.samples) {
        //         jf["samples"].push_back({
        //             {"labels", s.labels},
        //             {"value", s.value}
        //         });
        //     }
        //     j["metrics"].push_back(jf);
        // }
        // return j.dump(2);
    }


}  // namespace kio
