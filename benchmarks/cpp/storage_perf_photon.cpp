#include <gflags/gflags.h>
#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/thread/std-compat.h>
#include <photon/thread/workerpool.h>
#include <chrono>
#include <vector>
#include <numeric>
#include <fcntl.h>

DEFINE_uint64(buf_size, 4096, "Buffer size for IO");
DEFINE_uint64(total_size, 256 * 1024 * 1024, "Total bytes to transfer");
DEFINE_uint64(depth, 64, "Depth for streaming IO");
DEFINE_uint64(workers, 1, "Number of worker threads");

using namespace std::chrono;

struct BenchResult {
    double mib_per_sec;
    double ops_per_sec;
    double median_ms;
};

std::string get_temp_path(const char* prefix) {
    auto now = system_clock::now().time_since_epoch().count();
    return std::string("/tmp/") + prefix + "_" + std::to_string(now) + ".bin";
}

BenchResult run_sequential_write(photon::fs::IFile* file) {
    std::vector<char> buf(FLAGS_buf_size, 0x5a);
    size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    
    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < total_ops; ++i) {
        file->pwrite(buf.data(), buf.size(), i * FLAGS_buf_size);
    }
    auto end = high_resolution_clock::now();
    
    auto dur = duration_cast<nanoseconds>(end - start).count();
    double secs = (double)dur / 1e9;
    return { (FLAGS_total_size / (1024.0 * 1024.0)) / secs, (double)total_ops / secs, secs * 1000.0 };
}

BenchResult run_sequential_read(photon::fs::IFile* file) {
    std::vector<char> buf(FLAGS_buf_size);
    size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    
    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < total_ops; ++i) {
        file->pread(buf.data(), buf.size(), i * FLAGS_buf_size);
    }
    auto end = high_resolution_clock::now();
    
    auto dur = duration_cast<nanoseconds>(end - start).count();
    double secs = (double)dur / 1e9;
    return { (FLAGS_total_size / (1024.0 * 1024.0)) / secs, (double)total_ops / secs, secs * 1000.0 };
}

BenchResult run_streaming_write(photon::fs::IFile* file, photon::WorkPool& wp) {
    size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    size_t rounds = total_ops / FLAGS_depth;
    
    auto start = high_resolution_clock::now();
    for (size_t r = 0; r < rounds; ++r) {
        photon::semaphore sem(0);
        for (size_t i = 0; i < FLAGS_depth; ++i) {
            size_t op_idx = r * FLAGS_depth + i;
            wp.async_call(new auto([&, op_idx] {
                std::vector<char> buf(FLAGS_buf_size, (char)op_idx);
                file->pwrite(buf.data(), buf.size(), op_idx * FLAGS_buf_size);
                sem.signal(1);
            }));
        }
        sem.wait(FLAGS_depth);
    }
    auto end = high_resolution_clock::now();
    
    auto dur = duration_cast<nanoseconds>(end - start).count();
    double secs = (double)dur / 1e9;
    return { (FLAGS_total_size / (1024.0 * 1024.0)) / secs, (double)total_ops / secs, secs * 1000.0 };
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    set_log_output_level(ALOG_FATAL);

    if (photon::init(photon::INIT_EVENT_IOURING, photon::INIT_IO_LIBAIO) != 0) {
        fprintf(stderr, "failed to init photon\n");
        return -1;
    }
    DEFER(photon::fini());

    photon::WorkPool wp(FLAGS_workers, photon::INIT_EVENT_IOURING, photon::INIT_IO_LIBAIO);

    auto fs = photon::fs::new_localfs_adaptor();
    
    auto path = get_temp_path("photon_bench");
    auto file = fs->open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (!file) {
        perror("open");
        return -1;
    }
    DEFER({ delete file; fs->unlink(path.c_str()); });

    printf("[PhotonLib - %lu workers]\n", FLAGS_workers);
    
    auto res = run_sequential_write(file);
    printf("sequential write 4KiB: median %.3f ms, %.0f ops/s, %.1f MiB/s\n", res.median_ms, res.ops_per_sec, res.mib_per_sec);
    
    res = run_sequential_read(file);
    printf("sequential read 4KiB: median %.3f ms, %.0f ops/s, %.1f MiB/s\n", res.median_ms, res.ops_per_sec, res.mib_per_sec);

    res = run_streaming_write(file, wp);
    printf("streaming write depth64 4KiB: median %.3f ms, %.0f ops/s, %.1f MiB/s\n", res.median_ms, res.ops_per_sec, res.mib_per_sec);

    // Direct IO test
    auto path_direct = get_temp_path("photon_direct_bench");
    auto file_direct = fs->open(path_direct.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (file_direct) {
        printf("direct seq write 4KiB: ");
        auto res_d = run_sequential_write(file_direct);
        printf("median %.3f ms, %.0f ops/s, %.1f MiB/s\n", res_d.median_ms, res_d.ops_per_sec, res_d.mib_per_sec);
        delete file_direct;
        fs->unlink(path_direct.c_str());
    }

    return 0;
}
