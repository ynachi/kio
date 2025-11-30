//
// Created by Yao ACHI on 12/11/2025.
//

#ifndef KIO_METRICS_H
#define KIO_METRICS_H
#include "core/include/metrics/collector.h"
#include "worker.h"

namespace kio::io
{
    class WorkerMetricsCollector final : public IMetricsCollector
    {
        Worker& worker_;

    public:
        /**
         * @brief Creates a collector for a single Worker.
         * @param worker The standalone worker to scrape metrics from.
         */
        explicit WorkerMetricsCollector(Worker& worker) : worker_(worker) {}

        void Collect(MetricSnapshot& snapshot) override;
    };
}  // namespace kio::io

#endif  // KIO_METRICS_H
