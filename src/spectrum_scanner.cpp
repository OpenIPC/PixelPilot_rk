#include "spectrum_scanner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

/* Realtek drivers register their proc directory under the driver name:
 * rtl88x2eu, rtl88x2cu, rtl88x2bu, rtl8812au, ... Only interfaces that expose
 * chan_info (the ACI/quality stats this scanner reads) are picked up. */
constexpr const char* DRIVER_DIR_PREFIX = "rtl";

struct IfaceHandle {
    std::string name;
    fs::path chan_info_path;
};

struct IfaceSnapshot {
    std::string bw;
    std::vector<spectrum_channel_t> rows;
};

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}

/* Best-effort channel -> center frequency fallback when iw's own mapping (see
 * get_channel_freq_map) didn't cover a channel number. */
int channel_to_freq_fallback(int channel) {
    if (channel == 14) return 2484;
    if (channel >= 1 && channel <= 13) return 2407 + 5 * channel;
    if (channel >= 32 && channel <= 177) return 5000 + 5 * channel;
    return 0;
}

/* Run `iw <args...>` and capture stdout. Same no-shell fork/execvp approach. */
std::string run_iw_capture(const std::vector<std::string>& args) {
    int fds[2];
    if (pipe(fds) != 0) return "";
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return "";
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(fds[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("iw"));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp("iw", argv.data());
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return out;
}

/* `iw dev <iface> info` -> "phyN" from the "wiphy N" line. */
std::string get_phy(const std::string& iface) {
    std::string out = run_iw_capture({"dev", iface, "info"});
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        auto pos = line.find("wiphy");
        if (pos == std::string::npos) continue;
        std::istringstream ls(line.substr(pos));
        std::string tok;
        int num = -1;
        ls >> tok >> num;
        if (num >= 0) return "phy" + std::to_string(num);
    }
    return "";
}

/* `iw phy <phy> info` -> channel number -> frequency (MHz), parsed from lines
 * like " * 5745 MHz [149] (...)". */
std::map<int, int> get_channel_freq_map(const std::string& iface) {
    std::map<int, int> result;
    std::string phy = get_phy(iface);
    if (phy.empty()) return result;

    std::string out = run_iw_capture({"phy", phy, "info"});
    static const std::regex re(R"(\*\s*(\d+)\s+MHz\s+\[(\d+)\])");
    std::istringstream iss(out);
    std::string line;
    std::smatch m;
    while (std::getline(iss, line)) {
        if (std::regex_search(line, m, re)) {
            int freq = std::stoi(m[1]);
            int ch = std::stoi(m[2]);
            result.emplace(ch, freq);
        }
    }
    return result;
}

/* Parse /proc/net/<driver>/<iface>/chan_info:
 *   BW  20MHz
 *   Index  CH  Quality(%)  Availability(%)  Utilization(%)  WIFI Util(%)  Interference Util(%)
 *   1      1   98          99               2                1            1
 *   ...
 */
IfaceSnapshot parse_chan_info(const fs::path& path, const std::map<int, int>& freq_map) {
    IfaceSnapshot snap;
    std::ifstream f(path);
    if (!f.is_open()) return snap;

    std::string line;
    bool found_bw = false;
    bool found_header = false;

    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (!found_bw) {
            if (t.rfind("BW", 0) == 0) {
                std::istringstream ls(t);
                std::string tag;
                ls >> tag >> snap.bw;
                found_bw = true;
            }
            continue;
        }
        if (!found_header) {
            if (t.rfind("Index", 0) == 0) found_header = true;
            continue;
        }
        break;  /* row parsing continues below, after the header line */
    }

    while (std::getline(f, line)) {
        std::istringstream ls(line);
        int index, ch, quality, avail, util, wifi_util, interference;
        if (!(ls >> index >> ch >> quality >> avail >> util >> wifi_util >> interference)) continue;

        spectrum_channel_t row{};
        row.channel = ch;
        auto it = freq_map.find(ch);
        row.freq_mhz = (it != freq_map.end()) ? it->second : channel_to_freq_fallback(ch);
        row.quality_pct = quality;
        row.availability_pct = avail;
        row.util_pct = util;
        row.wifi_util_pct = wifi_util;
        row.interference_pct = interference;
        snap.rows.push_back(row);
    }
    return snap;
}

std::vector<IfaceHandle> find_interfaces() {
    std::vector<IfaceHandle> ifaces;
    std::error_code ec;
    for (const auto& driver : fs::directory_iterator("/proc/net", ec)) {
        if (!driver.is_directory(ec)) continue;
        if (driver.path().filename().string().rfind(DRIVER_DIR_PREFIX, 0) != 0) continue;

        for (const auto& iface : fs::directory_iterator(driver.path(), ec)) {
            if (!iface.is_directory(ec)) continue;
            fs::path chan_info = iface.path() / "chan_info";
            if (fs::exists(chan_info)) {
                ifaces.push_back({iface.path().filename().string(), chan_info});
            }
        }
    }
    std::sort(ifaces.begin(), ifaces.end(), [](const IfaceHandle& a, const IfaceHandle& b) { return a.name < b.name; });
    return ifaces;
}

struct IfaceState {
    std::string name;
    std::string bw;
    std::vector<spectrum_channel_t> rows;
};

std::mutex g_mutex;
std::vector<IfaceState> g_state;
std::atomic<bool> g_running{false};
std::thread g_thread;   /* supervisor: starts/stops one worker thread per interface */

/* One interface's own scan thread, so a multi-adapter GS doesn't have one
 * adapter's display freeze for the several seconds another adapter's scan
 * takes — see run_iface() below. */
struct IfaceWorker {
    std::thread thread;
    std::atomic<bool> running{true};
};
std::mutex g_workers_mutex;
std::unordered_map<std::string, std::unique_ptr<IfaceWorker>> g_workers;

/* Start `iw <iface> scan passive` without waiting for it, so the caller can
 * keep re-reading chan_info while the radio walks the channels — the scan
 * takes real air time, and chan_info fills in progressively as it goes,
 * mirroring the source script's "one channel refreshes at a time" behavior
 * instead of only showing a result once the whole scan round is done. */
pid_t start_scan_async(const std::string& iface) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execlp("iw", "iw", iface.c_str(), "scan", "passive", (char*)nullptr);
        _exit(127);
    }
    return pid;
}

/* Merge a fresh read into the shared snapshot for one interface, updating it
 * in place so a partial (mid-scan) read is visible to the UI immediately
 * instead of waiting for the whole scan round to finish. */
void publish_iface_state(const std::string& name, const IfaceSnapshot& snap) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = std::find_if(g_state.begin(), g_state.end(),
                            [&](const IfaceState& s) { return s.name == name; });
    if (it == g_state.end()) {
        g_state.push_back({});
        it = g_state.end() - 1;
        it->name = name;
    }
    it->bw = snap.bw;
    it->rows = snap.rows;
}

void remove_iface_state(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.erase(std::remove_if(g_state.begin(), g_state.end(),
                                  [&](const IfaceState& s) { return s.name == name; }),
                  g_state.end());
}

/* One interface's scan loop: repeatedly scan + poll chan_info, with no pause
 * between rounds — as soon as one `iw scan passive` finishes, the next one
 * starts, so the picture keeps updating continuously instead of stalling. */
void run_iface(IfaceHandle iface, std::atomic<bool>* iface_running) {
    std::map<int, int> freq_map = get_channel_freq_map(iface.name);

    while (iface_running->load() && g_running.load()) {
        pid_t scan_pid = start_scan_async(iface.name);

        /* Re-read chan_info every ~150ms while the scan is in flight, so
         * each channel's row updates as soon as the radio visits it rather
         * than all at once at the end. */
        int status = 0;
        pid_t waited_pid = 0;
        do {
            IfaceSnapshot snap = parse_chan_info(iface.chan_info_path, freq_map);
            publish_iface_state(iface.name, snap);

            if (scan_pid < 0) break;  /* fork failed; still show whatever chan_info has */
            waited_pid = waitpid(scan_pid, &status, WNOHANG);
            if (waited_pid == 0) std::this_thread::sleep_for(std::chrono::milliseconds(150));
        } while (waited_pid == 0 && iface_running->load() && g_running.load());
    }
}

void stop_worker(IfaceWorker& w) {
    w.running = false;
    w.thread.join();
}

/* Supervisor: watches for adapters appearing/disappearing (USB hotplug) and
 * keeps exactly one run_iface() thread alive per adapter currently present. */
void worker_loop() {
    while (g_running.load()) {
        std::vector<IfaceHandle> ifaces = find_interfaces();

        std::lock_guard<std::mutex> lock(g_workers_mutex);

        for (auto it = g_workers.begin(); it != g_workers.end();) {
            bool still_present = std::any_of(ifaces.begin(), ifaces.end(),
                                              [&](const IfaceHandle& i) { return i.name == it->first; });
            if (!still_present) {
                stop_worker(*it->second);
                remove_iface_state(it->first);
                it = g_workers.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& iface : ifaces) {
            if (g_workers.count(iface.name)) continue;
            auto worker = std::make_unique<IfaceWorker>();
            IfaceWorker* w = worker.get();
            IfaceHandle handle = iface;
            w->thread = std::thread([handle, w] { run_iface(handle, &w->running); });
            g_workers.emplace(iface.name, std::move(worker));
        }

        /* Hotplug is rare; a couple of seconds between checks is plenty and
         * keeps this supervisor loop from busy-spinning. Each iface's own
         * scan loop above has no such pause. */
        for (int waited = 0; waited < 2000 && g_running.load(); waited += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::lock_guard<std::mutex> lock(g_workers_mutex);
    for (auto& [name, w] : g_workers) stop_worker(*w);
    g_workers.clear();
}

}  // namespace

void spectrum_scanner_start(void) {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;  /* already running */
    g_thread = std::thread(worker_loop);
}

void spectrum_scanner_stop(void) {
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.clear();
}

int spectrum_scanner_snapshot(spectrum_iface_t* out, int max_ifaces) {
    if (!out || max_ifaces <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);

    int n = std::min((int)g_state.size(), max_ifaces);
    for (int i = 0; i < n; i++) {
        const IfaceState& st = g_state[i];
        spectrum_iface_t& dst = out[i];
        std::memset(&dst, 0, sizeof(dst));
        snprintf(dst.name, sizeof(dst.name), "%s", st.name.c_str());
        snprintf(dst.bandwidth, sizeof(dst.bandwidth), "%s", st.bw.c_str());
        int rows = std::min((int)st.rows.size(), SPECTRUM_MAX_ROWS);
        for (int r = 0; r < rows; r++) dst.rows[r] = st.rows[r];
        dst.row_count = rows;
    }
    return n;
}
