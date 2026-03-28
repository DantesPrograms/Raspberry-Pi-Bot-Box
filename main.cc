#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/statvfs.h>
#include <unistd.h>

#if !defined(__linux__)
#error This file is meant to be built on Raspberry Pi OS / Linux.
#endif

#if __has_include(<U8glib.h>)
#include <U8glib.h>
#else
#error U8glib headers were not found. Install U8glib or switch this file to U8g2.
#endif

/*
  OLED wiring:
    SDA -> GPIO2
    SCL -> GPIO3

  Display note:
    The module marking "1.3\" OLED IIC ADD_SELECT 0x78 / 0x7A" usually means
    the display can use one of two I2C write addresses depending on solder/jumper
    configuration. In 7-bit Linux tools this is commonly shown as 0x3C / 0x3D.

    Many 1.3" I2C OLED modules are SH1106-based. If yours is SSD1306, build with:
      -DOLED_USE_SSD1306

  Compatibility note:
    The official U8glib project documents Arduino/AVR/ARM targets, not Raspberry Pi
    Linux specifically. The dashboard/page logic below is ready either way, but if the
    display still refuses to initialize on the Pi, the library/display pairing is the
    likely blocker rather than the page code.
*/

#ifdef OLED_USE_SSD1306
U8GLIB_SSD1306_128X64 g_display(U8G_I2C_OPT_NONE);
#else
U8GLIB_SH1106_128x64 g_display(U8G_I2C_OPT_NONE);
#endif

namespace {

using namespace std::chrono_literals;

constexpr int kScreenWidth = 128;
constexpr int kHeaderHeight = 12;
constexpr int kBodyTop = 21;
constexpr int kBodyLineHeight = 9;
constexpr std::size_t kMaxBodyRows = 5;
constexpr std::size_t kMaxBodyChars = 24;
constexpr auto kPollDelay = 100ms;
constexpr auto kSnapshotInterval = 1s;
constexpr auto kPageSwitchInterval = 5s;

enum class Page : int {
  System = 0,
  Python = 1,
  Network = 2,
  Storage = 3,
  Count = 4,
};

struct CpuTimes {
  unsigned long long idle = 0;
  unsigned long long total = 0;
};

struct MemoryStats {
  long long total_kb = 0;
  long long available_kb = 0;
};

struct DiskStats {
  unsigned long long total_bytes = 0;
  unsigned long long available_bytes = 0;
  unsigned long long used_bytes = 0;
};

struct PythonProcess {
  int pid = 0;
  std::string label;
};

struct DashboardSnapshot {
  double cpu_usage_percent = 0.0;
  double cpu_temp_c = 0.0;
  std::string load_average = "n/a";
  MemoryStats memory;
  double uptime_seconds = 0.0;
  std::string hostname = "n/a";
  std::vector<std::string> ipv4_addresses;
  DiskStats disk;
  std::vector<PythonProcess> python_processes;
  std::string updated_at = "--:--:--";
};

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }

  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::string trim(std::string value) {
  auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };

  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !is_space(ch); }));
  value.erase(
      std::find_if(value.rbegin(), value.rend(),
                   [&](unsigned char ch) { return !is_space(ch); })
          .base(),
      value.end());
  return value;
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool isDigitsOnly(const std::string& value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::vector<std::string> splitNullSeparated(const std::string& raw) {
  std::vector<std::string> parts;
  std::string current;

  for (char ch : raw) {
    if (ch == '\0') {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
      continue;
    }

    current.push_back(ch);
  }

  if (!current.empty()) {
    parts.push_back(current);
  }

  return parts;
}

std::string baseName(const std::string& path) {
  if (path.empty()) {
    return {};
  }

  return std::filesystem::path(path).filename().string();
}

std::string fitText(std::string text, std::size_t max_chars = kMaxBodyChars) {
  text = trim(text);
  if (text.size() <= max_chars) {
    return text;
  }

  if (max_chars <= 3) {
    return text.substr(0, max_chars);
  }

  return text.substr(0, max_chars - 3) + "...";
}

std::string formatPercent(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << value << '%';
  return out.str();
}

std::string formatTemperature(double value) {
  if (value <= 0.0) {
    return "n/a";
  }

  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << value << " C";
  return out.str();
}

std::string formatMiB(long long kibibytes) {
  if (kibibytes <= 0) {
    return "0 MiB";
  }

  const double mib = static_cast<double>(kibibytes) / 1024.0;
  std::ostringstream out;
  out << std::fixed << std::setprecision(mib < 100.0 ? 1 : 0) << mib << " MiB";
  return out.str();
}

std::string formatGiB(unsigned long long bytes) {
  const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  std::ostringstream out;
  out << std::fixed << std::setprecision(gib < 10.0 ? 1 : 0) << gib << " GiB";
  return out.str();
}

std::string formatUptime(double total_seconds) {
  if (total_seconds <= 0.0) {
    return "n/a";
  }

  long long seconds = static_cast<long long>(total_seconds);
  const long long days = seconds / 86400;
  seconds %= 86400;
  const long long hours = seconds / 3600;
  seconds %= 3600;
  const long long minutes = seconds / 60;

  std::ostringstream out;
  if (days > 0) {
    out << days << "d ";
  }
  out << hours << "h " << minutes << "m";
  return out.str();
}

std::string nowClockText() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm {};
#if defined(_WIN32)
  localtime_s(&local_tm, &now_time);
#else
  localtime_r(&now_time, &local_tm);
#endif

  std::ostringstream out;
  out << std::put_time(&local_tm, "%H:%M:%S");
  return out.str();
}

CpuTimes readCpuTimes() {
  std::ifstream input("/proc/stat");
  if (!input) {
    return {};
  }

  std::string cpu_label;
  unsigned long long user = 0;
  unsigned long long nice = 0;
  unsigned long long system = 0;
  unsigned long long idle = 0;
  unsigned long long iowait = 0;
  unsigned long long irq = 0;
  unsigned long long softirq = 0;
  unsigned long long steal = 0;

  input >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >>
      softirq >> steal;

  CpuTimes times;
  times.idle = idle + iowait;
  times.total = user + nice + system + idle + iowait + irq + softirq + steal;
  return times;
}

class CpuMeter {
 public:
  CpuMeter() : previous_(readCpuTimes()) {}

  double samplePercent() {
    const CpuTimes current = readCpuTimes();
    const auto total_delta = current.total - previous_.total;
    const auto idle_delta = current.idle - previous_.idle;
    previous_ = current;

    if (total_delta == 0) {
      return last_percent_;
    }

    const double active = 100.0 * (1.0 - static_cast<double>(idle_delta) /
                                             static_cast<double>(total_delta));
    last_percent_ = std::clamp(active, 0.0, 100.0);
    return last_percent_;
  }

 private:
  CpuTimes previous_;
  double last_percent_ = 0.0;
};

MemoryStats readMemoryStats() {
  std::ifstream input("/proc/meminfo");
  if (!input) {
    return {};
  }

  MemoryStats stats;
  std::string key;
  long long value = 0;
  std::string unit;

  while (input >> key >> value >> unit) {
    if (key == "MemTotal:") {
      stats.total_kb = value;
    } else if (key == "MemAvailable:") {
      stats.available_kb = value;
    }

    if (stats.total_kb > 0 && stats.available_kb > 0) {
      break;
    }
  }

  return stats;
}

double readCpuTemperatureC() {
  const std::string raw = trim(readFile("/sys/class/thermal/thermal_zone0/temp"));
  if (raw.empty()) {
    return 0.0;
  }

  try {
    return static_cast<double>(std::stoll(raw)) / 1000.0;
  } catch (...) {
    return 0.0;
  }
}

double readUptimeSeconds() {
  std::ifstream input("/proc/uptime");
  if (!input) {
    return 0.0;
  }

  double uptime = 0.0;
  input >> uptime;
  return uptime;
}

std::string readLoadAverage() {
  std::ifstream input("/proc/loadavg");
  if (!input) {
    return "n/a";
  }

  double one = 0.0;
  double five = 0.0;
  double fifteen = 0.0;
  input >> one >> five >> fifteen;

  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << one << ' ' << five << ' ' << fifteen;
  return out.str();
}

std::string readHostname() {
  std::array<char, 256> hostname {};
  if (gethostname(hostname.data(), hostname.size()) != 0) {
    return "n/a";
  }

  hostname.back() = '\0';
  return hostname.data();
}

std::vector<std::string> readIPv4Addresses() {
  std::vector<std::string> addresses;
  std::set<std::string> unique;
  ifaddrs* ifaddr = nullptr;

  if (getifaddrs(&ifaddr) != 0 || ifaddr == nullptr) {
    return addresses;
  }

  for (ifaddrs* current = ifaddr; current != nullptr; current = current->ifa_next) {
    if (current->ifa_addr == nullptr || current->ifa_name == nullptr) {
      continue;
    }

    if (current->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    if ((current->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }

    char ip_buffer[INET_ADDRSTRLEN] {};
    const auto* addr =
        reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
    if (inet_ntop(AF_INET, &addr->sin_addr, ip_buffer, sizeof(ip_buffer)) == nullptr) {
      continue;
    }

    const std::string label =
        std::string(current->ifa_name) + ": " + std::string(ip_buffer);
    if (unique.insert(label).second) {
      addresses.push_back(label);
    }
  }

  freeifaddrs(ifaddr);
  std::sort(addresses.begin(), addresses.end());
  return addresses;
}

DiskStats readDiskStats() {
  struct statvfs info {};
  if (statvfs("/", &info) != 0) {
    return {};
  }

  DiskStats stats;
  stats.total_bytes =
      static_cast<unsigned long long>(info.f_blocks) * info.f_frsize;
  stats.available_bytes =
      static_cast<unsigned long long>(info.f_bavail) * info.f_frsize;
  stats.used_bytes = stats.total_bytes - stats.available_bytes;
  return stats;
}

std::string describePythonProcess(const std::vector<std::string>& args,
                                  const std::string& comm) {
  if (args.empty()) {
    return comm.empty() ? "python" : comm;
  }

  for (std::size_t index = 1; index < args.size(); ++index) {
    const std::string& arg = args[index];

    if (arg == "-m" && index + 1 < args.size()) {
      return "-m " + baseName(args[index + 1]);
    }

    if (arg == "-c") {
      return "-c inline";
    }

    if (!arg.empty() && arg[0] == '-') {
      continue;
    }

    return baseName(arg);
  }

  return baseName(args.front());
}

std::vector<PythonProcess> readPythonProcesses() {
  std::vector<PythonProcess> processes;
  std::error_code error;

  for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
    if (error) {
      break;
    }

    if (!entry.is_directory(error)) {
      continue;
    }

    const std::string pid_text = entry.path().filename().string();
    if (!isDigitsOnly(pid_text)) {
      continue;
    }

    const std::string comm = trim(readFile(entry.path() / "comm"));
    const std::vector<std::string> args =
        splitNullSeparated(readFile(entry.path() / "cmdline"));
    const std::string joined = toLower(comm + " " + (args.empty() ? "" : args.front()));

    if (joined.find("python") == std::string::npos) {
      continue;
    }

    PythonProcess process;
    process.pid = std::stoi(pid_text);
    process.label = describePythonProcess(args, comm);
    processes.push_back(process);
  }

  std::sort(processes.begin(), processes.end(),
            [](const PythonProcess& left, const PythonProcess& right) {
              if (left.label == right.label) {
                return left.pid < right.pid;
              }
              return left.label < right.label;
            });
  return processes;
}

DashboardSnapshot collectSnapshot(CpuMeter& cpu_meter) {
  DashboardSnapshot snapshot;
  snapshot.cpu_usage_percent = cpu_meter.samplePercent();
  snapshot.cpu_temp_c = readCpuTemperatureC();
  snapshot.load_average = readLoadAverage();
  snapshot.memory = readMemoryStats();
  snapshot.uptime_seconds = readUptimeSeconds();
  snapshot.hostname = readHostname();
  snapshot.ipv4_addresses = readIPv4Addresses();
  snapshot.disk = readDiskStats();
  snapshot.python_processes = readPythonProcesses();
  snapshot.updated_at = nowClockText();
  return snapshot;
}

void drawHeader(U8GLIB& u8g, const std::string& title, Page page) {
  const std::string page_text =
      std::to_string(static_cast<int>(page) + 1) + "/" +
      std::to_string(static_cast<int>(Page::Count));
  const std::string safe_title = fitText(title, 16);

  u8g.setFont(u8g_font_6x10);
  u8g.setColorIndex(1);
  u8g.drawBox(0, 0, kScreenWidth, kHeaderHeight);
  u8g.setColorIndex(0);
  u8g.drawStr(2, 9, safe_title.c_str());
  u8g.drawStr(kScreenWidth - u8g.getStrWidth(page_text.c_str()) - 2, 9,
              page_text.c_str());
  u8g.setColorIndex(1);
}

void drawBodyLines(U8GLIB& u8g, const std::vector<std::string>& lines) {
  u8g.setFont(u8g_font_5x7);
  for (std::size_t index = 0; index < lines.size() && index < kMaxBodyRows; ++index) {
    const std::string safe_line = fitText(lines[index]);
    const int y = kBodyTop + static_cast<int>(index) * kBodyLineHeight;
    u8g.drawStr(2, y, safe_line.c_str());
  }
}

std::vector<std::string> systemPageLines(const DashboardSnapshot& snapshot) {
  std::vector<std::string> lines;
  lines.push_back("CPU: " + formatPercent(snapshot.cpu_usage_percent));
  lines.push_back("Temp: " + formatTemperature(snapshot.cpu_temp_c));

  const long long used_kb =
      std::max(0LL, snapshot.memory.total_kb - snapshot.memory.available_kb);
  lines.push_back("RAM: " + formatMiB(used_kb) + " / " +
                  formatMiB(snapshot.memory.total_kb));
  lines.push_back("Up: " + formatUptime(snapshot.uptime_seconds));
  lines.push_back("Load: " + snapshot.load_average);
  return lines;
}

std::vector<std::string> pythonPageLines(const DashboardSnapshot& snapshot) {
  std::vector<std::string> lines;

  if (snapshot.python_processes.empty()) {
    lines.push_back("No Python scripts running");
    lines.push_back("This page only lists");
    lines.push_back("python/python3 processes.");
    lines.push_back(" ");
    lines.push_back("Updated: " + snapshot.updated_at);
    return lines;
  }

  const std::size_t visible =
      std::min<std::size_t>(snapshot.python_processes.size(), kMaxBodyRows - 1);
  for (std::size_t index = 0; index < visible; ++index) {
    const auto& process = snapshot.python_processes[index];
    lines.push_back(std::to_string(process.pid) + " " + process.label);
  }

  if (snapshot.python_processes.size() > visible) {
    lines.push_back("+" + std::to_string(snapshot.python_processes.size() - visible) +
                    " more...");
  } else {
    lines.push_back("Count: " + std::to_string(snapshot.python_processes.size()));
  }

  return lines;
} 

std::vector<std::string> networkPageLines(const DashboardSnapshot& snapshot) {
  std::vector<std::string> lines;
  lines.push_back("Host: " + snapshot.hostname);

  if (snapshot.ipv4_addresses.empty()) {
    lines.push_back("IP: disconnected");
  } else {
    for (std::size_t index = 0;
         index < snapshot.ipv4_addresses.size() && lines.size() < kMaxBodyRows - 1;
         ++index) {
      lines.push_back(snapshot.ipv4_addresses[index]);
    }
  }

  lines.push_back("Updated: " + snapshot.updated_at);
  return lines;
}

std::vector<std::string> storagePageLines(const DashboardSnapshot& snapshot) {
  std::vector<std::string> lines;
  const double used_percent =
      snapshot.disk.total_bytes == 0
          ? 0.0
          : (100.0 * static_cast<double>(snapshot.disk.used_bytes) /
             static_cast<double>(snapshot.disk.total_bytes));

  lines.push_back("Root: " + formatGiB(snapshot.disk.used_bytes) + " / " +
                  formatGiB(snapshot.disk.total_bytes));
  lines.push_back("Free: " + formatGiB(snapshot.disk.available_bytes));
  lines.push_back("Used: " + formatPercent(used_percent));
  lines.push_back("Py procs: " + std::to_string(snapshot.python_processes.size()));
  lines.push_back("Updated: " + snapshot.updated_at);
  return lines;
}

void drawPage(U8GLIB& u8g, Page page, const DashboardSnapshot& snapshot) {
  std::string title;
  std::vector<std::string> lines;

  switch (page) {
    case Page::System:
      title = "System Stats";
      lines = systemPageLines(snapshot);
      break;
    case Page::Python:
      title = "Python";
      lines = pythonPageLines(snapshot);
      break;
    case Page::Network:
      title = "Network";
      lines = networkPageLines(snapshot);
      break;
    case Page::Storage:
      title = "Storage";
      lines = storagePageLines(snapshot);
      break;
    case Page::Count:
      title = "Dashboard";
      lines = {"n/a"};
      break;
  }

  drawHeader(u8g, title, page);
  drawBodyLines(u8g, lines);
}

Page nextPage(Page current) {
  const int total = static_cast<int>(Page::Count);
  const int next = (static_cast<int>(current) + 1) % total;
  return static_cast<Page>(next);
}

void render(U8GLIB& u8g, Page page, const DashboardSnapshot& snapshot) {
  u8g.firstPage();
  do {
    drawPage(u8g, page, snapshot);
  } while (u8g.nextPage());
}

}  // namespace

int main() {
  try {
    CpuMeter cpu_meter;
    DashboardSnapshot snapshot = collectSnapshot(cpu_meter);
    Page current_page = Page::System;

    auto last_snapshot_at = std::chrono::steady_clock::now();
    auto last_page_switch_at = std::chrono::steady_clock::now();
    bool dirty = true;

    while (true) {
      const auto now = std::chrono::steady_clock::now();

      if (now - last_page_switch_at >= kPageSwitchInterval) {
        current_page = nextPage(current_page);
        last_page_switch_at = now;
        dirty = true;
      }

      if (now - last_snapshot_at >= kSnapshotInterval) {
        snapshot = collectSnapshot(cpu_meter);
        last_snapshot_at = now;
        dirty = true;
      }

      if (dirty) {
        render(g_display, current_page, snapshot);
        dirty = false;
      }

      std::this_thread::sleep_for(kPollDelay);
    }
  } catch (const std::exception& error) {
    std::cerr << "Dashboard failed: " << error.what() << '\n';
    return 1;
  }
}
