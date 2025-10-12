#ifndef OS_MON_H
#define OS_MON_H

#include <memory>
#include <vector>

class ISensor {
public:
    virtual ~ISensor() = default;
    virtual void run() = 0;
    virtual bool is_valid() = 0;
};

// Forward declaration
// class OsSensors;

class OsSensors {
public:
    void addCPU();
    void addPower(const std::string &sensor_name, const std::string &hwmon_id);
    void addTemperature(const std::string &thermal_zone);

    std::size_t size();

    void run();

private:
    std::vector<std::shared_ptr<ISensor>> sensors;
};

#endif
