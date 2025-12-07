//
// Created by Yao ACHI on 12/11/2025.
//

#include "metrics_collector.h"

#include "../sync/sync_wait.h"

namespace kio::io
{
    void WorkerMetricsCollector::Collect(MetricSnapshot& snapshot)
    {
        // Define the task that safely hops to the worker and copies its stats
        auto get_stats_task = [](Worker& worker) -> Task<WorkerStats>
        {
            // Switch to the worker's thread for thread safety
            co_await SwitchToWorker(worker);

            // Now we are safely on the worker's thread, so we can
            // read its non-atomic WorkerStats struct
            co_return worker.get_stats();
        };

        // Run the task on the worker to collect the stats
        // This blocks the *scraper thread*, runs the task on the
        // *worker thread*, and safely copies the non-atomic stats
        const WorkerStats stats = SyncWait(get_stats_task(worker_));

        std::string id_str = std::to_string(worker_.get_id());

        // Build metric families and populate with worker stats

        auto& bytes_read_family = snapshot.BuildFamily("kio_worker_bytes_read_total", "Total bytes read by kio workers", MetricType::Counter);
        bytes_read_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.bytes_read_total));

        auto& bytes_written_family = snapshot.BuildFamily("kio_worker_bytes_written_total", "Total bytes written by kio workers", MetricType::Counter);
        bytes_written_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.bytes_written_total));

        // Operation count metrics
        auto& read_ops_family = snapshot.BuildFamily("kio_worker_read_ops_total", "Total read operations by kio workers", MetricType::Counter);
        read_ops_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.read_ops_total));

        auto& write_ops_family = snapshot.BuildFamily("kio_worker_write_ops_total", "Total write operations by kio workers", MetricType::Counter);
        write_ops_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.write_ops_total));

        // Connection metrics
        auto& connections_accepted_family = snapshot.BuildFamily("kio_worker_connections_accepted_total", "Total connections accepted by kio workers", MetricType::Counter);
        connections_accepted_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.connections_accepted_total));

        auto& connect_ops_family = snapshot.BuildFamily("kio_worker_connect_ops_total", "Total connect operations by kio workers", MetricType::Counter);
        connect_ops_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.connect_ops_total));

        auto& open_ops_family = snapshot.BuildFamily("kio_worker_open_ops_total", "Total open operations by kio workers", MetricType::Counter);
        open_ops_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.open_ops_total));

        // Coroutine metrics
        auto& coroutine_resizes_family = snapshot.BuildFamily("kio_worker_coroutine_pool_resizes_total", "Total coroutine pool resizes by kio workers", MetricType::Counter);
        coroutine_resizes_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.coroutines_pool_resize_total));

        auto& active_coroutines_family = snapshot.BuildFamily("kio_worker_active_coroutines", "Number of active coroutines in kio workers", MetricType::Gauge);
        active_coroutines_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.active_coroutines));

        auto& errors_family = snapshot.BuildFamily("kio_worker_io_errors_total", "Total I/O errors encountered by kio workers", MetricType::Counter);
        errors_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.io_errors_total));
    }


}  // namespace kio::io
