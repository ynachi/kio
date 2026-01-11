//
// Created by Yao ACHI on 12/11/2025.
//

#ifndef KIO_REGISTRY_H
#define KIO_REGISTRY_H
#include <memory>
#include <mutex>
#include <vector>

#include "collector.h"
#include "serializer.h"

namespace kio
{

template<typename T>
concept MetricSerializer = std::is_base_of_v<IMetricSerializer, T>;

template<MetricSerializer SerializerT = PrometheusTextSerializer>
/**
 * @brief The central, singleton registry for all metrics collectors.
 *
 * This registry holds a list of collectors and orchestrates
 * scrapes for the /metrics endpoint.
 *
 * @note It is thread-safe.
 */
class MetricsRegistry
{
public:
    static MetricsRegistry& Instance()
    {
        static MetricsRegistry registry;
        return registry;
    }

    void Register(std::shared_ptr<IMetricsCollector> collector)
    {
        std::lock_guard lock(mutex_);
        collectors_.push_back(std::move(collector));
    }

    // This is called by your HTTP /metrics endpoint
    std::string Scrape()
    {
        MetricSnapshot snapshot;

        {
            std::lock_guard lock(mutex_);
            // Tell every collector to share its metrics
            for (const auto& collector: collectors_)
            {
                collector->Collect(snapshot);
            }
        }

        // 3. Serialize it using our new serializer
        return serializer_.Serialize(snapshot);
    }

private:
    MetricsRegistry() = default;
    std::mutex mutex_;
    std::vector<std::shared_ptr<IMetricsCollector>> collectors_;
    static inline SerializerT serializer_{};
};
}  // namespace kio

#endif  // KIO_REGISTRY_H
