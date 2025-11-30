//
// Created by Yao ACHI on 12/11/2025.
//

#include "kio/include/io/metrics.h"

#include "kio/include/sync_wait.h"

namespace kio::io
{
    void WorkerMetricsCollector::Collect(MetricSnapshot& snapshot)
    {
        auto& bytes_read_family = snapshot.BuildFamily("kio_worker_bytes_read_total", "Total bytes read by kio workers", MetricType::Counter);
        // TODO register the others


        // Define the task that safely hops to the worker and copies its IoStats
        auto get_stats_task = [](Worker& worker) -> Task<WorkerStats>
        {
            // switch to the worker's thread for thread safety
            co_await SwitchToWorker(worker);

            // Now we are safely on the worker's thread, so we can
            // read its non-atomic IoStats struct.
            co_return worker.get_stats();
        };

        // Run the task on the worker to collect the stats
        // This blocks the *scraper thread*, runs the task on the
        // *worker thread*, and safely copies the non-atomic stats.
        const WorkerStats stats = SyncWait(get_stats_task(worker_));

        std::string id_str = std::to_string(worker_.get_id());

        // 4. Populate the Prometheus metrics using the copied stats

        // Add worker WorkerStats
        bytes_read_family.Add({{"worker_id", id_str}}, static_cast<double>(stats.bytes_read_total));
        // TODO
        // bytes_written_family.Add({{"worker_id", id_str}}, stats.bytes_written_total);
        // read_ops_family.Add({{"worker_id", id_str}}, stats.read_ops_total);
        // write_ops_family.Add({{"worker_id", id_str}}, stats.write_ops_total);
        // connections_accepted_family.Add({{"worker_id", id_str}}, stats.connections_accepted_total);
        // connect_ops_family.Add({{"worker_id", id_str}}, stats.connect_ops_total);
        // open_ops_family.Add({{"worker_id", id_str}}, stats.open_ops_total);
        // close_ops_family.Add({{"worker_id", id_str}}, stats.close_ops_total);
        // sleep_ops_family.Add({{"worker_id", id_str}}, stats.sleep_ops_total);
    }

}  // namespace kio::io
