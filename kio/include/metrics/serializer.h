//
// Created by Yao ACHI on 12/11/2025.
//

#ifndef KIO_SERIALIZER_H
#define KIO_SERIALIZER_H
#include "model.h"

namespace kio
{
    class IMetricSerializer
    {
    public:
        virtual ~IMetricSerializer() = default;
        virtual std::string Serialize(const MetricSnapshot& snapshot) = 0;
    };

    class PrometheusTextSerializer final : public IMetricSerializer
    {
    public:
        std::string Serialize(const MetricSnapshot& snapshot) override;
    };

    class PrometheusJsonMetricSerializer final : public IMetricSerializer
    {
    public:
        std::string Serialize(const MetricSnapshot& snapshot) override;
    };
}  // namespace kio

#endif  // KIO_SERIALIZER_H
