#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <optional>
#include <filesystem>

#include "spdlog/spdlog.h"

extern "C" {
#include "osd.h"
}

#include "os_mon.hpp"


/**
 * @brief CPU load percentage monitor
 *
 * Currently returns total CPU load percentage and also separately IO-wait part from total (which
 * is somewhat important to monitor DVR-disk and network IO capacity).
 *
 * It reads data from the 1st line of /proc/stat (see `man proc_stat`)
 */
class CPUSensor : public ISensor {
public:
    explicit CPUSensor() = default;
    virtual ~CPUSensor() = default;

    static void detect(std::vector<std::shared_ptr<ISensor>> sensors) {
		if (std::filesystem::exists(std::filesystem::path("/proc/stat"))) {
			spdlog::debug("Detected CPU load sensor");
			sensors.push_back(std::make_shared<CPUSensor>());
		}
	}

    void run() override {
        std::ifstream stat_file("/proc/stat");
        std::string line;

        if (!stat_file.is_open()) {
            spdlog::error("Error opening /proc/stat");
            return;
        }

        // Read the first line only, because it contains the aggregate CPU data
        // We can read load per-core, but I doubt anyone cares; if you care, then keep reading
        // line-by-line as long as line starts with `cpuN`. I guess add CPU number as tag then?
        std::getline(stat_file, line);
        std::optional<CpuLoad> cpu_load = calculate_cpu_load(line);
        if (cpu_load.has_value()) {
            void *batch = osd_batch_init(3);
            osd_add_uint_fact(batch, "os_mon.cpu.load_total", NULL, 0,
                                cpu_load->total_load);
            osd_add_uint_fact(batch, "os_mon.cpu.load_iowait", NULL, 0,
                                cpu_load->io_wait_load);
            osd_publish_batch(batch);
        }
    }

    bool is_valid() override {
      return true;
    }

protected:
    struct CpuLoad {
        double total_load;  // Overall CPU load percentage
        double io_wait_load; // I/O wait percentage
    };

    std::optional<CpuLoad> calculate_cpu_load(const std::string &line) {
        // line looks like
        // cpu  7018539 25772 1876671 98010101 163631 0 72991 0 0 0
        //      ^ user  ^ nice
        //                    ^ system
        //                            ^ idle   ^ IO-wait
        //                                            ^ Hardware IRQ
        //                                              ^ Soft IRQ
        //                                                    ^ ^ ^ only for virtual environments
        std::istringstream ss(line);
        std::string cpuLabel;
        std::vector<long> times;
        long point, utime, nice, stime, idle, iowait;

        ss >> cpuLabel; // Read the label (e.g., "cpu")
        while (ss >> point) {
            times.push_back(point);
        }

        // Extract necessary values (assuming the order is consistent)
        if (times.size() < 5) {
            spdlog::error("Unexpected data from /proc/stat");
            return std::nullopt;
        }

        utime = times[0];
        nice = times[1];
        stime = times[2];
        idle = times[3];
        iowait = times[4];

        long total_time = utime + nice + stime + idle + iowait;
        long active_time = utime + nice + stime;

        // Calculate CPU load percentage
        double total_load = (active_time * 100.0) / total_time;
        double io_wait_load = (iowait * 100.0) / total_time;

        return CpuLoad{ total_load, io_wait_load };
    }
};


/**
 * @brief The Linux hwmon power sensor interface for reading voltage, current, and power data.
 * 
 * Currently only i2c INA226 sensor is implemented, other sensors can be added later, Linux hwmon
 * provides good enough abstraction.
 * See https://www.kernel.org/doc/html/latest/hwmon/ina2xx.html
 * How to add one to your SBC:
 * https://seriyps.com/blog/2025/09/25/pair-ina226-and-raspberry-pi-ina2xx-kernel-driver/
 *
 * It reads data from /sys/class/hwmon/{hwmonId}/:
 * - curr1_input - the current value in mA
 * - in1_input - voltage in mV
 * - power1_input - power in uW
 *
 * All 3 published as 3 separate facts tagged with hwmon_id.
 */
class PowerSensor : public ISensor {
public:
    enum Sensor { SENSOR_INA226 };

    explicit PowerSensor(const std::string &sensor_type,
						 const std::string &hwmon_id)
        : is_valid_sensor(false), hwmon_id(hwmon_id) {
        hwmon_path = std::filesystem::path("/sys/class/hwmon/") / hwmon_id / ""; // Construct the full path
        if (sensor_type == "ina226") {
            sensor = Sensor::SENSOR_INA226;
            is_valid_sensor = validate_sensor();
            if (!is_valid_sensor) {
                spdlog::error("The power sensor {} is either not available or its 'name' does not "
                              "match the expected sensor type! Sensor is disabled.", hwmon_id);
            }
        } else {
            is_valid_sensor = false;
        }
    };
    virtual ~PowerSensor() = default;

	static void detect(std::vector<std::shared_ptr<ISensor>> sensors) {
		// list all /sys/class/hwmon/hwmonN, make sure they have `name` file that contains
		// the name of one of the supported sensors
		std::string path = "/sys/class/hwmon";
		std::string prefix = "hwmon";
		for (const auto& entry : std::filesystem::directory_iterator(path)) {
			std::string filename = entry.path().filename().string();
			if (entry.is_directory() &&
				filename.compare(0, prefix.size(), prefix) == 0 &&
				std::filesystem::exists(entry.path() / "name")) {
				// Check if the "name" file contains known sensor name
				std::ifstream name_file(entry.path() / "name");
				std::string content;

				std::getline(name_file, content);

				if (content == "ina226") {
					spdlog::debug("Detected power sensor {}", content);
					sensors.push_back(std::make_shared<PowerSensor>(filename, "ina226"));
				}
			}
		}
	}

    void run() override {
        if (!is_valid_sensor) {
            return;
        }
        std::optional<Power> power = fetch_power();
        if (power.has_value()) {
            void *batch = osd_batch_init(3);
            osd_tag tags[1];
            strcpy(tags[0].key, "sensor");
            strcpy(tags[0].val, hwmon_id.c_str());

            osd_add_int_fact(batch, "os_mon.power.voltage", tags, 1, power->voltage);
            osd_add_int_fact(batch, "os_mon.power.current", tags, 1, power->current);
            osd_add_int_fact(batch, "os_mon.power.power", tags, 1, power->power);
            osd_publish_batch(batch);
        }
    };

    bool is_valid() override {
      return is_valid_sensor;
    }

protected:
    bool is_valid_sensor;
    Sensor sensor;
    std::filesystem::path hwmon_path;
    std::string hwmon_id;
    struct Power {
        long voltage;  // Voltage in millivolts (mV)
        long current;  // Current in milliamperes (mA)
        long power;    // Power in microwatts (uW)
    };

    bool validate_sensor() {
        std::ifstream name_file(hwmon_path / "name");
        if (!name_file.is_open()) {
            spdlog::warn("Unable to open power sensor 'name' file: {}", (hwmon_path / "name").string());
            return false;
        }

        std::string sensor_name;
        std::getline(name_file, sensor_name);
        if (sensor == Sensor::SENSOR_INA226) {
          return sensor_name == "ina226";
        } else {
            return false;
        }
    }

    std::optional<long> read_value(const std::filesystem::path& filename) {
        std::ifstream file(hwmon_path / filename);
        if (!file.is_open()) {
            spdlog::error("Unable to open power sensor file: {}", (hwmon_path / filename).string());
            return std::nullopt;
        }

        std::string value;
        std::getline(file, value);

        try {
            return std::stoll(value); // Convert string to integer
        } catch (...) {
            spdlog::error("Unexpected value '{}' in file: {}", value, filename.string());
            return std::nullopt;
        }
    }

    std::optional<Power> fetch_power() {
        Power data;

        auto current_opt = read_value("curr1_input");
        auto voltage_opt = read_value("in1_input");
        auto power_opt = read_value("power1_input");

        if (current_opt.has_value() && voltage_opt.has_value() && power_opt.has_value()) {
            data.current = *current_opt;
            data.voltage = *voltage_opt;
            data.power = *power_opt;
            return data;
        } else {
            spdlog::error("Failed to read power sensor data: One or more values are missing");
            return std::nullopt;
        }
    }
};

/**
 * @brief Represents a temperature sensor interface for reading temperature data from SysFS.
 * 
 * The TemperatureSensor class provides methods to initialize the sensor using the 
 * thermal zone string and read the temperature values from sysfs.
 * It reads files in /sys/class/thermal/{thermal_zone}:
 * - temp - temperature in millidegrees C (published as value)
 * - type - the name of the sensor (added as `name` tag)
 */
class TemperatureSensor : public ISensor {
public:
    explicit TemperatureSensor(const std::string &thermal_zone)
        : thermal_zone(thermal_zone), is_valid_sensor(false) {
        thermal_path = std::filesystem::path("/sys/class/thermal/") / thermal_zone / "temp";
        is_valid_sensor = read_sensor_name();
        if (!is_valid_sensor) {
          spdlog::error("The temperature sensor {} is not available! Sensor is disabled.",
                        thermal_zone);
        }
    }

    static void detect(std::vector<std::shared_ptr<ISensor>> sensors) {
		// list all /sys/class/thermal/thermal_zoneN, make sure they have `temp` and `type` files
		std::string path = "/sys/class/thermal";
		std::string prefix = "thermal_zone";
		for (const auto& entry : std::filesystem::directory_iterator(path)) {
			std::string filename = entry.path().filename().string();
			if (entry.is_directory() &&
				filename.compare(0, prefix.size(), prefix) == 0 &&
				std::filesystem::exists(entry.path() / "temp") &&
				std::filesystem::exists(entry.path() / "type")) {
				spdlog::debug("Detected temperature sensor {}", filename);
				sensors.push_back(std::make_shared<TemperatureSensor>(filename));
			}
		}
	}

    void run() override {
        if (!is_valid_sensor) {
            return; // Early exit if not a valid sensor
        }

        auto tempOpt = read_temperature();
        if (tempOpt.has_value()) {
            osd_tag tags[2];
            strcpy(tags[0].key, "sensor");
            strcpy(tags[0].val, thermal_zone.c_str());
            strcpy(tags[1].key, "name");
            strcpy(tags[1].val, sensor_name.c_str());
            osd_publish_int_fact("os_mon.temperature", tags, 2, *tempOpt); // millidegrees C
        } else {
            spdlog::error("Temperature data for {} could not be read.", thermal_zone);
        }
    }

    bool is_valid() override {
      return is_valid_sensor;
    }

private:
    std::string thermal_zone;
    std::filesystem::path thermal_path;   // Path to the thermal zone
    std::string sensor_name;              // Name of the sensor
    bool is_valid_sensor;                 // Flag to indicate if the sensor is valid

    std::optional<long> read_temperature() {
        std::ifstream file(thermal_path);
        if (!file.is_open()) {
            spdlog::warn("Unable to open file: {}", thermal_path.string());
            return std::nullopt; // Return nullopt if the file cannot be opened
        }

        std::string value;
        std::getline(file, value);

        try {
            return std::stoll(value);
        } catch (...) {
            spdlog::error("Failed to convert value from file: {}", thermal_path.string());
            return std::nullopt;
        }
    }

    bool read_sensor_name() {
        auto type_path = std::filesystem::path("/sys/class/thermal/") / thermal_zone / "type";
        std::ifstream file(type_path);
        if (!file.is_open()) {
            spdlog::warn("Unable to open thermal sensor 'type' file: {}", type_path.string());
            return false; // Return false if the file cannot be opened
        }

        std::getline(file, sensor_name);
        return true; // Successful read
    }

};


void OsSensors::addCPU() {
    sensors.push_back(std::make_shared<CPUSensor>());
}

std::size_t OsSensors::discoverCPU() {
	std::size_t before = sensors.size();
	CPUSensor::detect(sensors);
	return sensors.size() - before;
}

void OsSensors::addPower(const std::string &sensor_name, const std::string &hwmon_id) {
    sensors.push_back(std::make_shared<PowerSensor>(sensor_name, hwmon_id));
}

std::size_t OsSensors::discoverPower() {
	std::size_t before = sensors.size();
	PowerSensor::detect(sensors);
	return sensors.size() - before;
}

void OsSensors::addTemperature(const std::string &thermal_zone) {
    sensors.push_back(std::make_shared<TemperatureSensor>(thermal_zone));
}

std::size_t OsSensors::discoverTemperature() {
	std::size_t before = sensors.size();
	TemperatureSensor::detect(sensors);
	return sensors.size() - before;
}

std::size_t OsSensors::autodiscover() {
	return discoverCPU() + discoverPower() + discoverTemperature();
}

void OsSensors::run() {
    for (const auto& sensor : sensors) {
        sensor->run();  // Calls the run method of each sensor
    }
}

std::size_t OsSensors::size() {
    return sensors.size();
}
