//
// Created by Yao ACHI on 12/11/2025.
//

#ifndef KIO_MODEL_H
#define KIO_MODEL_H
#include <map>
#include <string>
#include <vector>

#include "kio/core/async_logger.h"

namespace kio
{
constexpr size_t kMetricsMaxSamples = 10000;
/// The type of metric (for the # TYPE line)
enum class MetricType : uint8_t
{
    kCounter,
    kGauge,
};

/// Represents a single data point:
/// kio_bytes_read{worker_id="0"} 1024
struct MetricSample
{
    std::map<std::string, std::string> labels;
    double value;
};

/// Represents a "family" of metrics (all samples for one metric name)
struct MetricFamily
{
    size_t max_samples = kMetricsMaxSamples;
    std::string name;
    std::string help;
    MetricType type;
    std::vector<MetricSample> samples;

    // Helper for collectors to add data
    void Add(std::map<std::string, std::string> labels, const double value)
    {
        if (samples.size() >= max_samples)
        {
            ALOG_WARN("MetricFamily {} has reached max samples ({})", name, max_samples);
            return;
        };
        samples.push_back({std::move(labels), value});
    }
};

/**
 * @brief A lightweight, transient container for metrics
 *
 * This object is created *every time* a scrape happens.
 * It is NOT a singleton. It simply holds the data for
 * one scrape.
 */
class MetricSnapshot
{
public:
    /**
     * @brief Creates or gets a new metric family for this scrape.
     * @param name The metric name (e.g., "kio_worker_bytes_read_total")
     * @param help The help string.
     * @param type The metric type (Counter or Gauge).
     * @param max_samples The maximum number of samples for a metric
     * @return A reference to the new family, to which samples can be added.
     */
    MetricFamily& BuildFamily(std::string name, std::string help, const MetricType type,
                              const size_t max_samples = kMetricsMaxSamples)
    {
        families_.emplace_back(MetricFamily{.max_samples = max_samples,
                                            .name = std::move(name),
                                            .help = std::move(help),
                                            .type = type,
                                            .samples = {}});
        return families_.back();
    }

    [[nodiscard]]
    const std::vector<MetricFamily>& GetFamilies() const
    {
        return families_;
    }

private:
    std::vector<MetricFamily> families_;
};

}  // namespace kio

#endif  // KIO_MODEL_H
