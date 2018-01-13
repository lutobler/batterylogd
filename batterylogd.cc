/*
 * Copyright (c) 2017, Lukas Tobler
 * GNU General Public License v3.0
 * (see LICENSE or https://www.gnu.org/licenses/gpl-3.0.txt)
 */

#include <getopt.h>
#include <iostream>
#include <iomanip>
#include <ios>
#include <sstream>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>
#include <dirent.h>
#include <libgen.h>
#include <ctime>
#include <mutex>
#include <condition_variable>

static const std::string kBatterylogdVersion("0.1");
static const char *kUsage =
"Usage: batterylogd [options]\n\n"
"  -h   --help          Print this message and exit.\n"
"  -v   --version       Print version information and exit.\n"
"  -i   --interval      Sampling interval. Defaults to 30 seconds.\n"
"  -b   --battery       Path to a battery in sysfs. Argument can be specified\n"
"                       multiple times. If omitted entirely, automatic battery\n"
"                       detection will be enabled.\n"
"  -L   --backlight     Add entries for the display backlight to the log.\n"
"                       Fields for 'max_brightness' and 'current_brightness'\n"
"                       will be appended to the logged battery data.\n"
"  -l   --log           Path to log file. Defaults to $HOME/batterylogd.log.\n\n";

static const std::string kDefaultLogfile("batterylogd.log");
static const std::string kBatterySysfs("/sys/class/power_supply/");
static const int kDefaultSampleInterval = 60;

/* set by signal handler to indicate the program should terminate */
static volatile std::sig_atomic_t sig_terminated = false;

/* strip a trailing slash from a string if needed */
static std::string strip_traling_slash(std::string str) {
    if (str.back() == '/') {
        return str.substr(0, str.size() - 1);
    }
    return str;
}

/* a timer that can be interrupted prematurely */
struct KillableTimer {
private:
    std::condition_variable cv;
    std::mutex m;
    bool terminate = false;

public:
    /* return false if killed */
    template<class R, class P>
    bool wait_for(std::chrono::duration<R,P> const& time ) {
        std::unique_lock<std::mutex> lock(m);
        return !cv.wait_for(lock, time, [&] { return terminate; });
    }

    void kill() {
        std::unique_lock<std::mutex> lock(m);
        terminate = true;
        cv.notify_all();
    }
};

struct DisplayBacklightStat {
public:
    std::string path;
    std::ifstream f_brightness;
    std::ifstream f_max_brightness;

    std::string brightness;
    std::string max_brightness;

    DisplayBacklightStat(std::string& path_) {
        path = strip_traling_slash(path_);
    }

    ~DisplayBacklightStat() {
        f_brightness.close();
        f_max_brightness.close();
    }

    void sample() {
        std::getline(f_brightness, brightness);
        std::getline(f_max_brightness, max_brightness);

        /* getline will put the file cursor to the end so we reset it */
        f_brightness.clear();
        f_brightness.seekg(0, std::ios::beg);
        f_max_brightness.clear();
        f_max_brightness.seekg(0, std::ios::beg);
    }

    bool initialize() {
        f_brightness.open(path + "/brightness");
        f_max_brightness.open(path + "/max_brightness");
        return f_brightness && f_max_brightness;
    }

    std::string build_log_string() {
        return brightness + "," + max_brightness;
    }
};

struct BatteryStat {
public:
    std::string path;
    std::string data;

    BatteryStat(std::string& base_path, std::string& fname) {
        path = strip_traling_slash(base_path) + "/" + fname;
    }

    void sample() {
        std::ifstream file;
        file.open(path);
        if (file) {
            std::getline(file, data);
        }
        file.close();
    }
};

struct Battery {
private:
    bool is_battery_dir(std::string& path) {
        std::ifstream f_type;
        f_type.open(path + "/type");
        if (!f_type) {
            return false;
        }
        std::string file_content;
        std::getline(f_type, file_content);
        f_type.close();
        return file_content == "Battery";
    }

public:
    std::string sysfs_path;
    std::string name;
    std::vector<BatteryStat> battery_stats;

    Battery(std::string& sysfs_path_) {
        sysfs_path = strip_traling_slash(sysfs_path_);
        name = basename((char *) sysfs_path.c_str());
        std::vector<std::string> files {
            "capacity",
            "cycle_count",
            "energy_full",
            "energy_full_design",
            "energy_now",
            "power_now",
            "present",
            "status",
            "voltage_min_design",
            "voltage_now",
        };
        for (std::string& s : files) {
            battery_stats.push_back(BatteryStat(sysfs_path, s));
        }
    }

    bool initialize() {
        return (is_battery_dir(sysfs_path));
    }

    void sample_all() {
        for (BatteryStat& b : battery_stats) {
            b.sample();
        }
    }

    std::string build_log_string() {
        std::stringstream ss;
        std::string line;
        std::time_t t = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
        ss << std::put_time(std::localtime(&t), "%FT%TZ");

        line = name + "," + ss.str() + ",";
        for (BatteryStat& b : battery_stats) {
            line += b.data + ",";
        }
        return line;
    }

    std::vector<std::string> data_vector() {
        std::vector<std::string> v(battery_stats.size());
        for (BatteryStat& b : battery_stats) {
            v.push_back(b.data);
        }
        return v;
    }
};

/* try to find batteries in /sys/class/power_supply/ */
static void detect_batteries(std::vector<Battery>& batteries) {
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(kBatterySysfs.c_str())) != nullptr) {
        while ((ent = readdir(dir)) != nullptr) {
            std::ifstream f_type;
            std::string path = kBatterySysfs + ent->d_name;
            Battery battery(path);
            if (battery.initialize()) {
                batteries.push_back(battery);
                std::cout << "Added battery " << battery.name
                          << std::endl;
            }
        }
    }
    closedir(dir);
}

static void signal_handler(int signo) {
    (void) signo;
    sig_terminated = true;
}

int main(int argc, char **argv) {
    /* cmdline arguments definiton */
    const char *optstring = "hvi:b:L:l:";
    const struct option long_options[] = {
        { "help",       no_argument,        NULL,   'h' },
        { "version",    no_argument,        NULL,   'v' },
        { "interval",   required_argument,  NULL,   'i' },
        { "battery",    required_argument,  NULL,   'b' },
        { "backlight",  required_argument,  NULL,   'L' },
        { "log",        required_argument,  NULL,   'l' },
        { 0,            0,                  0,      0 }
    };

    std::vector<std::string> cmdline_batteries;
    std::string cmdline_backlight;
    bool backlight_logging_enabled = false;
    std::string cmdline_logfile;
    int cmdline_interval = 0;
    int c;

    /* parse cmdline arguemnts */
    while (true) {
        int option_index = 0;
        c = getopt_long(argc, argv, optstring, long_options, &option_index);

        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            std::cout << kUsage;
            std::exit(EXIT_SUCCESS);
            break;
        case 'v':
            std::cout << "batterylogd: version "
                      << kBatterylogdVersion
                      << std::endl;
            std::exit(EXIT_SUCCESS);
            break;
        case 'i':
            cmdline_interval = std::stoi(optarg);
            if (cmdline_interval <= 0) {
                std::cerr << "Invalid interval given" << std::endl;
                std::exit(EXIT_FAILURE);
            }
            break;
        case 'b':
            cmdline_batteries.push_back(std::string(optarg));
            break;
        case 'L':
            cmdline_backlight = std::string(optarg);
            backlight_logging_enabled = true;
            break;
        case 'l':
            cmdline_logfile = std::string(optarg);
            break;
        case '?':
        default:
            std::cout << kUsage;
            std::exit(EXIT_FAILURE);
            break;
        }
    }

    /* set sample interval */
    int sample_interval;
    if (cmdline_interval == 0) {
        sample_interval = kDefaultSampleInterval;
    } else {
        sample_interval = cmdline_interval;
    }

    /* initialize batteries */
    std::vector<Battery> batteries;
    if (cmdline_batteries.size() == 0) {
        detect_batteries(batteries);
        if (batteries.size() == 0) {
            std::cerr << "No batteries found. Provide -b argument."
                      << std::endl;
            std::exit(EXIT_FAILURE);
        }
    } else {
        for (std::string& s : cmdline_batteries) {
            Battery battery(s);
            if (battery.initialize()) {
                batteries.push_back(battery);
                std::cout << "Added battery " << battery.name << std::endl;
            }
        }
    }

    /* initialize backlight */
    DisplayBacklightStat backlight(cmdline_backlight);
    if (backlight_logging_enabled) {
        if (!backlight.initialize()) {
            std::cout << "Invalid backlight given" << std::endl;
            std::exit(EXIT_FAILURE);
        }
        std::cout << "Added backlight " << cmdline_backlight << std::endl;
    }

    /* initialize log file */
    std::ofstream log_file;
    if (cmdline_logfile.empty()) {
        cmdline_logfile = std::string(std::getenv("HOME"))
                          + "/" + kDefaultLogfile;
    }
    std::cout << "Log file: " << cmdline_logfile << std::endl;

    log_file.open(cmdline_logfile);
    if (!log_file) {
        std::cerr << "Could not open log file" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    /* register signal handlers */
    if (signal(SIGINT, signal_handler) == SIG_ERR ||
        signal(SIGTERM, signal_handler) == SIG_ERR) {
        std::cerr << "Failed to regiser signal handler" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    /*
     * Use a signal handler thread because operations on condition variables
     * are not async-safe.
     */
    KillableTimer timer;
    std::thread sig_handler([&] (void) {
        std::chrono::seconds interval(1);
        while (!sig_terminated) {
            std::this_thread::sleep_for(interval);
        }
        timer.kill();
    });

    /* main loop thread */
    std::thread main_loop([&] (void) {
        std::chrono::seconds interval(sample_interval);
        while (true) {
            for (Battery& b : batteries) {
                b.sample_all();
                std::string log_line = b.build_log_string();
                if (backlight_logging_enabled) {
                    backlight.sample();
                    log_line += backlight.build_log_string();
                }
                log_file << log_line << std::endl;
            }

            if (!timer.wait_for(interval)) {
                break;
            }
        }
    });

    /* start the threads */
    sig_handler.detach();
    main_loop.join();

    std::cout << "Shutting down batterylogd ..." << std::endl;
    log_file.close();
    return 0;
}

