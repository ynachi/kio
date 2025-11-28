// #include <filesystem>
//
// static bool ensure_directory(const BitcaskConfig& config) {
//     try {
//         // Create directories if they don't exist
//         std::filesystem::create_directories(config.directory);
//
//         // Convert mode_t to filesystem perms
//         auto perms = static_cast<std::filesystem::perms>(config.dir_mode);
//         std::filesystem::permissions(config.directory, perms);
//
//         return true;
//     } catch (...) {
//         return false;
//     }
// }