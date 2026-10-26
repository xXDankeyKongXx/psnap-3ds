#include "files.hpp"

#include <cstdio>
#include <string>

constexpr std::u8string_view backup_suffix = u8".bak";
constexpr std::u8string_view temp_suffix = u8".temp";

std::ifstream recomp::open_input_backup_file(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::filesystem::path backup_path{filepath};
    backup_path += backup_suffix;
    return std::ifstream{backup_path, mode};
}

std::ifstream recomp::open_input_file_with_backup(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::ifstream ret{filepath, mode};

    // Check if the file failed to open and open the corresponding backup file instead if so.
    if (!ret.good()) {
        return open_input_backup_file(filepath, mode);
    }

    return ret;
}

std::ofstream recomp::open_output_file_with_backup(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::filesystem::path temp_path{filepath};
    temp_path += temp_suffix;
    std::ofstream temp_file_out{ temp_path, mode };

    return temp_file_out;
}

bool recomp::finalize_output_file_with_backup(const std::filesystem::path& filepath) {
    std::filesystem::path backup_path{filepath};
    backup_path += backup_suffix;

    std::filesystem::path temp_path{filepath};
    temp_path += temp_suffix;

#ifdef __3DS__
    // 3DS port: std::filesystem::copy_file fails on the SD card (every game save failed here and
    // left an empty save file), so the files are renamed instead: old save -> backup, new -> save.
    {
        std::string file = filepath.string(), backup = backup_path.string(), temp = temp_path.string();
        std::remove(backup.c_str());
        std::rename(file.c_str(), backup.c_str()); // fails harmlessly when there's no save yet
        return std::rename(temp.c_str(), file.c_str()) == 0;
    }
#endif
    std::error_code ec;
    if (std::filesystem::exists(filepath, ec)) {
        std::filesystem::copy_file(filepath, backup_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return false;
        }
    }
    std::filesystem::copy_file(temp_path, filepath, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    std::filesystem::remove(temp_path, ec);
    return true;
}
