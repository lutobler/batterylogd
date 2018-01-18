/**
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
#include <condition_variable>
#include <memory>
#include <assert.h>

static const std::string kBatterylogdVersion("0.1");
static const char *kUsage =
"Usage: batterylogd [options]\n\n"
"  -h   --help          Print this message and exit.\n"
"  -v   --version       Print version information and exit.\n"
"  -i   --interval      Sampling interval. Defaults to 30 seconds.\n"
"  -b   --battery       Path to a battery in sysfs. Argument can be specified\n"
"                       multiple times. If omitted entirely, automatic detection\n"
"                       will be enabled.\n"
"  -L   --backlight     Add entries for the display backlight to the log.\n"
"                       Argument can be specified multiple times. If omitted\n"
"                       entirely, automatic detection will be enabled.\n"
"  -l   --log           Path to log file. Defaults to $HOME/batterylogd.log.\n\n";

/* standard device paths */
static const std::string kBatterySysfs("/sys/class/power_supply/");
static const std::string kBacklightSysfs("/sys/class/backlight/");

/* defaults */
static const std::string kDefaultLogfile("batterylogd.log");
static const int kDefaultSampleInterval = 60;

/* set by signal handler to indicate the program should terminate */
static volatile std::sig_atomic_t sig_terminated = false;

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

/* a data point that represents a file in the file system */
class FSDataPoint {
private:
    std::string path_;
    std::ifstream file_;
    std::string data_;

public:
    FSDataPoint(std::string path) : path_(path) {}

    bool initialize() {
        file_.open(path_);
        return file_.good();
    }

    void sample() {
        std::getline(file_, data_);
        file_.clear();
        file_.seekg(0, std::ios::beg);
    }

    const std::string& data() const { return data_; }
};

/* abstract container for a collection of data points */
class DataCollection {
protected:
    std::vector<FSDataPoint> data_points_;
    std::string sysfs_path_;
    std::string name_;
    std::string type_;

    const std::string name_from_path(std::string& path) const {
        return std::string(basename((char *) path.c_str()));
    }

public:
    DataCollection(std::string sysfs_path) {
        /* strip a trailing slash from path if needed */
        if (sysfs_path.back() == '/') {
            sysfs_path_ = sysfs_path.substr(0, sysfs_path.size() - 1);
        } else {
            sysfs_path_ = sysfs_path;
        }
        name_ = name_from_path(sysfs_path);
    }

    virtual bool initialize() = 0;

    void sample_all() {
        for (FSDataPoint& dp : data_points_) {
            dp.sample();
        }
    }

    const std::vector<std::string> data_vector() {
        std::vector<std::string> dv;
        dv.reserve(data_points_.size());
        for (FSDataPoint& dp : data_points_) {
            dv.push_back(dp.data());
        }
        return dv;
    }

    const std::string& name() const { return name_; }
    const std::string& type() const { return type_; }
};

/* a data collection that models a battery */
class BatteryDataCollection : public DataCollection {
public:
    BatteryDataCollection(std::string sysfs_path)
        : DataCollection(sysfs_path) {}

    bool initialize() {
        type_ = "battery";
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
        data_points_.reserve(files.size());
        for (std::string& s : files) {
            FSDataPoint fdp(sysfs_path_ + "/" + s);
            if (!fdp.initialize()) {
                return false;
            }
            data_points_.push_back(std::move(fdp));
        }
        return true;
    }
};

class BacklightDataCollection : public DataCollection {
public:
    BacklightDataCollection(std::string sysfs_path)
        : DataCollection(sysfs_path) {}

    bool initialize() {
        type_ = "backlight";
        std::vector<std::string> files { "brightness", "max_brightness" };
        data_points_.reserve(files.size());
        for (std::string& s : files) {
            FSDataPoint fdp(sysfs_path_ + "/" + s);
            if (!fdp.initialize()) {
                return false;
            }
            data_points_.push_back(std::move(fdp));
        }
        return true;
    }
};

class LogBuilder {
private:
    std::vector<std::shared_ptr<DataCollection>> data_collections_;
    std::ofstream log_file_;

public:
    LogBuilder(std::vector<std::shared_ptr<DataCollection>> data_collections,
               std::ofstream log_file)
        : data_collections_(data_collections), log_file_(std::move(log_file)) {}

    void write() {
        for (std::shared_ptr<DataCollection>& dc : data_collections_) {
            std::stringstream ss;
            std::vector<std::string> dv = dc.get()->data_vector();
            std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

            for (std::string& s : dv) {
                ss << "," << s;
            }

            log_file_ << dc.get()->type()
                      << "," << dc.get()->name()
                      << "," << std::put_time(std::localtime(&t), "%FT%TZ")
                      << ss.str() << std::endl;
        }
    }
};

template <class T>
class DeviceDetector {
private:
    std::string base_path_;
    std::string detect_file_;
    std::string detect_string_;

    std::vector<std::shared_ptr<DataCollection>> auto_detect_() {
        std::vector<std::shared_ptr<DataCollection>> detected;
        DIR *dir;
        struct dirent *ent;

        /* open the base path */
        if ((dir = opendir(base_path_.c_str())) != nullptr) {

            /* loop through the contents of the base path */
            while ((ent = readdir(dir)) != nullptr) {
                std::ifstream f_detect;
                std::string path = base_path_ + ent->d_name;
                f_detect.open(path + "/" + detect_file_);

                /* read and check the detection file */
                if (f_detect.good()) {
                    std::string f_content;
                    std::getline(f_detect, f_content);
                    if (f_content == detect_string_) {
                        std::shared_ptr<DataCollection> dc_ptr(new T(path));
                        if (dc_ptr->initialize()) {
                            detected.push_back(dc_ptr);
                        }
                    }
                }
            }
            closedir(dir);
        }
        return detected;
    }

public:
    DeviceDetector(std::string base_path, std::string detect_file, std::string detect_string)
        : base_path_(base_path), detect_file_(detect_file), detect_string_(detect_string) {
            assert(base_path_.back() == '/');
        }

    /*
     * Construct data collection from command line and perform auto-detection
     * if none are given.
     */
    bool detect_from_cmdline(std::vector<std::shared_ptr<DataCollection>>& data_collections,
                             std::vector<std::string>& args) {

        /* no cmdline arguments given, auto-detect */
        std::vector<std::shared_ptr<DataCollection>> detected;
        if (args.size() == 0) {
            detected = auto_detect_();
            if (detected.size() == 0) {
                std::cerr << "No device in " << base_path_
                          << " found. Provide -b argument." << std::endl;
                return false;
            }

        /* use cmdline arguements */
        } else {
            for (std::string& s : args) {
                std::shared_ptr<DataCollection> t_ptr(new T(s));
                if (t_ptr->initialize()) {
                    detected.push_back(t_ptr);
                }
            }
        }

        /* log configured batteries to stdout */
        for (std::shared_ptr<DataCollection>& dc : detected) {
            std::cout << "Added device " << dc.get()->name() << std::endl;
        }

        /* merge vectors */
        data_collections.reserve(data_collections.size() + detected.size());
        data_collections.insert(data_collections.end(), detected.begin(), detected.end());

        return true;
    }
};

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
    std::vector<std::string> cmdline_backlights;
    std::string cmdline_logfile;
    int cmdline_interval = 0;

    /* parse cmdline arguemnts */
    int c;
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
            std::cout << "batterylogd: version " << kBatterylogdVersion << std::endl;
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
            cmdline_backlights.push_back(std::string(optarg));
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

    /* data collections to observe, i.e. batteries and backlights */
    std::vector<std::shared_ptr<DataCollection>> data_collections;

    DeviceDetector<BatteryDataCollection> battery_detector(kBatterySysfs, "type", "Battery");
    DeviceDetector<BacklightDataCollection> backlight_detector(kBacklightSysfs, "type", "raw");
    if (!battery_detector.detect_from_cmdline(data_collections, cmdline_batteries)) {
        std::exit(EXIT_FAILURE);
    }
    if (!backlight_detector.detect_from_cmdline(data_collections, cmdline_backlights)){
        std::exit(EXIT_FAILURE);
    }

    /* initialize log file */
    if (cmdline_logfile.empty()) {
        cmdline_logfile = std::string(std::getenv("HOME")) + "/" + kDefaultLogfile;
    }

    std::ofstream log_file;
    log_file.open(cmdline_logfile, std::ios_base::app);
    if (!log_file.good()) {
        std::cerr << "Could not open log file" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    std::cout << "Log file: " << cmdline_logfile << std::endl;
    LogBuilder log_builder(data_collections, std::move(log_file));

    /* register signal handlers */
    if (signal(SIGINT, signal_handler) == SIG_ERR ||
        signal(SIGTERM, signal_handler) == SIG_ERR) {
        std::cerr << "Failed to regiser signal handler" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    /* set sample interval */
    int sample_interval = (cmdline_interval == 0) ? kDefaultSampleInterval
                                                  : cmdline_interval;

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
            for (std::shared_ptr<DataCollection>& dc : data_collections) {
                dc.get()->sample_all();
            }
            log_builder.write();

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

