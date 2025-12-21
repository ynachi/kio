//
// Created by Yao ACHI on 12/11/2025.
//

#ifndef KIO_COLLECTOR_H
#define KIO_COLLECTOR_H
#include "model.h"
namespace kio
{
/**
 * @brief An interface for any component that can expose
 * metrics to the central registry.
 *
 * Each implementation is responsible for safely gathering its
 * stats and adding them to the provided registry.
 */
class IMetricsCollector
{
public:
    virtual ~IMetricsCollector() = default;

    /**
     * @brief Safely collect and add metrics to the registry.
     * This is called by the scraper thread.
     * @param snapshot The transient metrics snapshot for this scrape.
     */
    virtual void Collect(MetricSnapshot &snapshot) = 0;
};
}  // namespace kio

#endif  // KIO_COLLECTOR_H
