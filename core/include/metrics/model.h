//
// Created by Yao ACHI on 12/11/2025.
//

#ifndef KIO_MODEL_H
#define KIO_MODEL_H
#include <map>
#include <string>
#include <vector>

namespace kio
{
    /// The type of metric (for the # TYPE line)
    enum class MetricType
    {
        Counter,
        Gauge,
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
        std::string name;
        std::string help;
        MetricType type;
        std::vector<MetricSample> samples;

        // Helper for collectors to add data
        void Add(std::map<std::string, std::string> labels, const double value) { samples.push_back({std::move(labels), value}); }
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
         * @return A reference to the new family, to which samples can be added.
         */
        MetricFamily& BuildFamily(std::string name, std::string help, const MetricType type)
        {
            families_.emplace_back(MetricFamily{std::move(name), std::move(help), type, {}});
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
