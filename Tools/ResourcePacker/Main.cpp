#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <cstring>

#include "EnderVFiles.hpp"

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

class EnderProgressBar {
private:
    size_t m_total;
    size_t m_current;
    int m_barWidth;
    std::string m_title;
    std::chrono::steady_clock::time_point m_startTime;

public:
    EnderProgressBar(size_t total, const std::string& title = "Progress", int width = 50)
        : m_total(total), m_current(0), m_barWidth(width), m_title(title) {
        m_startTime = std::chrono::steady_clock::now();
    }

    void update(size_t current) {
        m_current = current;
        float ratio = static_cast<float>(m_current) / m_total;
        int pos = static_cast<int>(m_barWidth * ratio);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_startTime).count();

        // 计算ETA
        int eta = 0;
        if (m_current > 0 && elapsed > 0) {
            float rate = static_cast<float>(m_current) / elapsed;
            eta = static_cast<int>((m_total - m_current) / rate);
        }

        std::cout << "\r" << m_title << " [";
        for (int i = 0; i < m_barWidth; ++i) {
            if (i < pos) std::cout << "=";
            else if (i == pos) std::cout << ">";
            else std::cout << " ";
        }

        std::cout << "] " << std::fixed << std::setprecision(1) << (ratio * 100.0) << "% ";
        std::cout << "(" << formatBytes(m_current) << "/" << formatBytes(m_total) << ") ";
        std::cout << "ETA: " << formatTime(eta) << "    " << std::flush;
    }

    void finish() {
        update(m_total);
        std::cout << std::endl;
    }

    static std::string formatBytes(size_t bytes) {
        const char* units[] = { "B", "KB", "MB", "GB", "TB" };
        int unitIndex = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024.0 && unitIndex < 4) {
            size /= 1024.0;
            unitIndex++;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
        return oss.str();
    }

    static std::string formatTime(int seconds) {
        if (seconds < 0) return "--:--";
        int mins = seconds / 60;
        int secs = seconds % 60;
        std::ostringstream oss;
        oss << std::setw(2) << std::setfill('0') << mins << ":"
            << std::setw(2) << std::setfill('0') << secs;
        return oss.str();
    }
};

class EnderSecureInput {
public:
    static std::string readPassword(const std::string& prompt = "Password: ") {
        std::cout << prompt;

#ifdef _WIN32
        std::string password;
        char ch;
        while ((ch = _getch()) != '\r') {
            if (ch == '\b') {
                if (!password.empty()) {
                    password.pop_back();
                    std::cout << "\b \b";
                }
            }
            else {
                password.push_back(ch);
                std::cout << '*';
            }
        }
        std::cout << std::endl;
        return password;
#else
        struct termios oldt, newt;
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);

        std::string password;
        std::getline(std::cin, password);

        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        std::cout << std::endl;
        return password;
#endif
    }
};

class EnderPackApp {
private:
    struct EnderPackOptions {
        std::string m_mode;           // pack, extract, list, verify
        std::string m_inputPath;
        std::string m_outputPath;
        std::string m_baseName;
        std::string m_password;
        uint64_t m_volumeSize;
        uint32_t m_maxVolumes;
        bool m_verbose;
        bool m_force;
    };

    EnderPackOptions m_options;

public:
    int run(int argc, char* argv[]) {
        if (!parseArguments(argc, argv)) {
            printUsage();
            return 1;
        }

        try {
            if (m_options.m_mode == "pack") {
                return doPack();
            }
            else if (m_options.m_mode == "extract") {
                return doExtract();
            }
            else if (m_options.m_mode == "list") {
                return doList();
            }
            else if (m_options.m_mode == "verify") {
                return doVerify();
            }
            else {
                std::cerr << "Unknown mode: " << m_options.m_mode << std::endl;
                return 1;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }
    }

private:
    bool parseArguments(int argc, char* argv[]) {
        if (argc < 2) return false;

        m_options.m_mode = argv[1];
        m_options.m_volumeSize = 0; // 0表示不分卷
        m_options.m_maxVolumes = 0;
        m_options.m_verbose = false;
        m_options.m_force = false;
        m_options.m_baseName = "Archive";

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "-i" || arg == "--input") {
                if (++i >= argc) return false;
                m_options.m_inputPath = argv[i];
            }
            else if (arg == "-o" || arg == "--output") {
                if (++i >= argc) return false;
                m_options.m_outputPath = argv[i];
            }
            else if (arg == "-n" || arg == "--name") {
                if (++i >= argc) return false;
                m_options.m_baseName = argv[i];
            }
            else if (arg == "-p" || arg == "--password") {
                if (++i >= argc) return false;
                m_options.m_password = argv[i];
            }
            else if (arg == "--password-interactive") {
                m_options.m_password = EnderSecureInput::readPassword();
                if (m_options.m_password.empty()) {
                    std::cerr << "Password cannot be empty!" << std::endl;
                    return false;
                }
            }
            else if (arg == "-v" || arg == "--volume-size") {
                if (++i >= argc) return false;
                m_options.m_volumeSize = parseSize(argv[i]);
            }
            else if (arg == "--max-volumes") {
                if (++i >= argc) return false;
                m_options.m_maxVolumes = std::stoul(argv[i]);
            }
            else if (arg == "--verbose") {
                m_options.m_verbose = true;
            }
            else if (arg == "-f" || arg == "--force") {
                m_options.m_force = true;
            }
            else if (arg == "-h" || arg == "--help") {
                return false;
            }
        }

        // 验证必要参数
        if (m_options.m_mode != "pack" && m_options.m_mode != "extract" &&
            m_options.m_mode != "list" && m_options.m_mode != "verify") {
            return false;
        }

        if (m_options.m_inputPath.empty()) {
            if (m_options.m_mode == "pack") {
                std::cerr << "Error: Input directory is required for packing" << std::endl;
                return false;
            }
            else {
                std::cerr << "Error: Archive path is required" << std::endl;
                return false;
            }
        }

        if (m_options.m_outputPath.empty() && m_options.m_mode == "pack") {
            m_options.m_outputPath = "./output";
        }

        if (m_options.m_password.empty()) {
            std::cerr << "Warning: No password provided, using default key (INSECURE!)" << std::endl;
            m_options.m_password = "default_insecure_key";
        }

        return true;
    }

    uint64_t parseSize(const std::string& str) {
        uint64_t size = 0;
        char unit = 0;
        std::istringstream iss(str);
        iss >> size >> unit;

        switch (std::tolower(unit)) {
        case 'k': size *= 1024; break;
        case 'm': size *= 1024 * 1024; break;
        case 'g': size *= 1024 * 1024 * 1024; break;
        case 't': size *= 1024ULL * 1024 * 1024 * 1024; break;
        }
        return size;
    }

    void printUsage() {
        std::cout << "ResourcePacker - Virtual File System Packager for Ender Engine\n";
        std::cout << "Usage: ResourcePacker <mode> [options]\n\n";

        std::cout << "Modes:\n";
        std::cout << "  pack      Pack directory into encrypted archive\n";
        std::cout << "  extract   Extract files from archive\n";
        std::cout << "  list      List contents of archive\n";
        std::cout << "  verify    Verify archive integrity\n\n";

        std::cout << "Options:\n";
        std::cout << "  -i, --input <path>          Input directory (pack) or archive index (extract/list/verify)\n";
        std::cout << "  -o, --output <path>         Output directory (default: ./output)\n";
        std::cout << "  -n, --name <name>           Base name for volumes (default: Archive)\n";
        std::cout << "  -p, --password <pwd>        Encryption password (INSECURE, use --password-interactive)\n";
        std::cout << "  --password-interactive      Securely prompt for password\n";
        std::cout << "  -v, --volume-size <size>    Volume size (e.g., 100M, 1G, 0 for no split)\n";
        std::cout << "  --max-volumes <num>         Maximum number of volumes (0=unlimited)\n";
        std::cout << "  -f, --force                 Overwrite existing files\n";
        std::cout << "  --verbose                   Show detailed information\n";
        std::cout << "  -h, --help                  Show this help\n\n";

        std::cout << "Examples:\n";
        std::cout << "  ResourcePacker pack -i ./assets -o ./dist -n GameData -v 500M --password-interactive\n";
        std::cout << "  ResourcePacker list -i ./dist/GameData.index -p mypassword\n";
        std::cout << "  ResourcePacker extract -i ./dist/GameData.index -o ./extracted -p mypassword\n";
    }

    int doPack() {
        std::cout << "=== Packing Mode ===" << std::endl;

        // 验证输入目录
        if (!fs::exists(m_options.m_inputPath) || !fs::is_directory(m_options.m_inputPath)) {
            std::cerr << "Error: Input path is not a valid directory" << std::endl;
            return 1;
        }

        // 检查输出目录
        if (fs::exists(m_options.m_outputPath) && !m_options.m_force) {
            std::cerr << "Error: Output directory exists. Use -f to force overwrite." << std::endl;
            return 1;
        }

        // 统计文件
        std::cout << "Scanning files..." << std::endl;
        size_t totalFiles = 0;
        uint64_t totalSize = 0;

        for (const auto& entry : fs::recursive_directory_iterator(m_options.m_inputPath)) {
            if (entry.is_regular_file()) {
                totalFiles++;
                totalSize += entry.file_size();
            }
        }

        std::cout << "Found " << totalFiles << " files, total size: "
            << EnderProgressBar::formatBytes(totalSize) << std::endl;

        // 配置打包器
        EnderArchiveBuilder builder;
        auto key = EnderGenerateKey(m_options.m_password);
        builder.setEncryptionKey(key);

        EnderVolumeConfig volConfig;
        volConfig.m_enabled = (m_options.m_volumeSize > 0);
        volConfig.m_volumeSize = m_options.m_volumeSize;
        volConfig.m_maxVolumes = m_options.m_maxVolumes;
        volConfig.m_baseName = m_options.m_baseName;
        builder.setVolumeConfig(volConfig);

        // 执行打包
        std::cout << "Packing..." << std::endl;
        auto start = std::chrono::high_resolution_clock::now();

        if (!builder.buildFromDirectory(m_options.m_inputPath, m_options.m_outputPath)) {
            std::cerr << "Error: Failed to build archive" << std::endl;
            return 1;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        // 显示结果
        std::cout << "\nPack completed in " << (duration / 1000.0) << " seconds" << std::endl;

        // 列出生成的文件
        std::cout << "\nGenerated files:" << std::endl;
        for (const auto& entry : fs::directory_iterator(m_options.m_outputPath)) {
            auto size = fs::file_size(entry);
            std::cout << "  " << entry.path().filename().string()
                << " (" << EnderProgressBar::formatBytes(size) << ")" << std::endl;
        }

        return 0;
    }

    int doList() {
        std::cout << "=== List Mode ===" << std::endl;

        std::string indexPath = m_options.m_inputPath;
        if (fs::is_directory(indexPath)) {
            // 自动查找index文件
            bool found = false;
            for (const auto& entry : fs::directory_iterator(indexPath)) {
                if (entry.path().extension() == ".index") {
                    indexPath = entry.path().string();
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "Error: No .index file found in directory" << std::endl;
                return 1;
            }
        }

        // 初始化VFS
        EnderVirtualFileSystem vfs;
        auto key = EnderGenerateKey(m_options.m_password);

        if (!vfs.initialize(indexPath, key)) {
            std::cerr << "Error: Failed to initialize VFS (wrong password?)" << std::endl;
            return 1;
        }

        // 获取所有文件
        auto files = vfs.listFiles();

        // 按目录排序和分组
        std::sort(files.begin(), files.end());

        std::cout << "\nTotal files: " << files.size() << "\n" << std::endl;

        std::string currentDir;
        for (const auto& file : files) {
            // 显示目录分隔
            size_t pos = file.find_last_of('/');
            std::string dir = (pos != std::string::npos) ? file.substr(0, pos) : "/";

            if (dir != currentDir) {
                currentDir = dir;
                std::cout << "\n[" << dir << "]" << std::endl;
            }

            std::string filename = (pos != std::string::npos) ? file.substr(pos + 1) : file;
            std::cout << "  " << filename << std::endl;
        }

        return 0;
    }

    int doExtract() {
        std::cout << "=== Extract Mode ===" << std::endl;

        if (m_options.m_outputPath.empty()) {
            m_options.m_outputPath = "./extracted";
        }

        std::string indexPath = m_options.m_inputPath;
        if (fs::is_directory(indexPath)) {
            for (const auto& entry : fs::directory_iterator(indexPath)) {
                if (entry.path().extension() == ".index") {
                    indexPath = entry.path().string();
                    break;
                }
            }
        }

        // 检查输出目录
        if (fs::exists(m_options.m_outputPath)) {
            if (!m_options.m_force) {
                std::cerr << "Error: Output directory exists. Use -f to force overwrite." << std::endl;
                return 1;
            }
            fs::remove_all(m_options.m_outputPath);
        }

        // 初始化VFS
        EnderVirtualFileSystem vfs;
        auto key = EnderGenerateKey(m_options.m_password);

        if (!vfs.initialize(indexPath, key)) {
            std::cerr << "Error: Failed to initialize VFS" << std::endl;
            return 1;
        }

        auto files = vfs.listFiles();
        std::cout << "Extracting " << files.size() << " files..." << std::endl;

        fs::create_directories(m_options.m_outputPath);

        EnderProgressBar progress(files.size(), "Extracting");
        size_t successCount = 0;
        size_t failCount = 0;

        for (size_t i = 0; i < files.size(); ++i) {
            const auto& file = files[i];

            // 构建输出路径
            fs::path outPath = fs::path(m_options.m_outputPath) / file;
            fs::create_directories(outPath.parent_path());

            // 读取并写入
            auto data = vfs.readFile(file);
            if (!data.empty()) {
                std::ofstream ofs(outPath, std::ios::binary);
                if (ofs) {
                    ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
                    successCount++;
                }
                else {
                    std::cerr << "\nFailed to write: " << file << std::endl;
                    failCount++;
                }
            }
            else {
                std::cerr << "\nFailed to read: " << file << std::endl;
                failCount++;
            }

            progress.update(i + 1);
        }

        progress.finish();
        std::cout << "Success: " << successCount << ", Failed: " << failCount << std::endl;

        return (failCount > 0) ? 1 : 0;
    }

    int doVerify() {
        std::cout << "=== Verify Mode ===" << std::endl;

        std::string indexPath = m_options.m_inputPath;
        if (fs::is_directory(indexPath)) {
            for (const auto& entry : fs::directory_iterator(indexPath)) {
                if (entry.path().extension() == ".index") {
                    indexPath = entry.path().string();
                    break;
                }
            }
        }

        EnderVirtualFileSystem vfs;
        auto key = EnderGenerateKey(m_options.m_password);

        if (!vfs.initialize(indexPath, key)) {
            std::cerr << "Error: Failed to initialize VFS" << std::endl;
            return 1;
        }

        auto files = vfs.listFiles();
        std::cout << "Verifying " << files.size() << " files..." << std::endl;

        EnderProgressBar progress(files.size(), "Verifying");
        size_t successCount = 0;
        size_t failCount = 0;

        for (size_t i = 0; i < files.size(); ++i) {
            auto data = vfs.readFile(files[i]);
            if (!data.empty()) {
                successCount++;
            }
            else {
                if (m_options.m_verbose) {
                    std::cerr << "\nCorrupted or missing: " << files[i] << std::endl;
                }
                failCount++;
            }
            progress.update(i + 1);
        }

        progress.finish();

        if (failCount == 0) {
            std::cout << "All files verified successfully!" << std::endl;
            return 0;
        }
        else {
            std::cout << "Warning: " << failCount << " files failed verification!" << std::endl;
            return 1;
        }
    }
};

int main(int argc, char* argv[]) {
    EnderPackApp app;
    return app.run(argc, argv);
}