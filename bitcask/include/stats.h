// //
// // Created by Yao ACHI on 14/11/2025.
// //
//
// #ifndef KIO_STATS_H
// #define KIO_STATS_H
// #include <cstdint>
// #include <unordered_map>
//
// namespace bitcask
// {
//     struct DataFileStats
//     {
//         /// the size of the datafile in size, this info is already available in the Datafile class
//         uint64_t size;
//         /// bytes actually referenced by the keydir
//         uint64_t live_bytes;
//         uint64_t records_count;
//         uint64_t tombstones_count;
//     };
//
//     struct Stats
//     {
//         std::unordered_map<uint64_t, DataFileStats> data_files;
//     };
// }  // namespace bitcask
//
// #endif  // KIO_STATS_H
