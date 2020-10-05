#include "linuxcpuplugin.h"

#include <QFile>

#include <KLocalizedString>

#include <SensorContainer.h>

#ifdef HAVE_SENSORS
#include <sensors/sensors.h>
#endif

#include "linuxcpu.h"

LinuxCpuPluginPrivate::LinuxCpuPluginPrivate(CpuPlugin *q)
    : CpuPluginPrivate(q)
{
    // Parse /proc/cpuinfo for information about cpus
    QFile cpuinfo("/proc/cpuinfo");
    cpuinfo.open(QIODevice::ReadOnly);

    QHash<int, int> numCores;
    for (QByteArray line = cpuinfo.readLine(); !line.isEmpty(); line = cpuinfo.readLine()) {
        unsigned int processor, physicalId, coreId;
        double frequency = 0;
        // Processors are divided by empty lines
        for (; line != "\n";  line = cpuinfo.readLine()) {
            // we are interested in processor number as identifier for /proc/stat, physical id (the
            // cpu the core belongs to) and the number of the core. However with hyperthreading
            // multiple entries will have the same combination of physical id and core id. So we just
            // count up the core number. For mapping temperature both ids are still needed nonetheless.
            const int delim = line.indexOf(":");
            const QByteArray field = line.left(delim).trimmed();
            const QByteArray value = line.mid(delim + 1).trimmed();
            if (field == "processor") {
                processor = value.toInt();
            } else if (field == "physical id") {
                physicalId = value.toInt();
            } else if (field == "core id") {
                coreId = value.toInt();
            } else if (field == "cpu MHz") {
                frequency = value.toDouble();
            }
        }
        const QString name = i18nc("@title", "CPU %1 Core %2", physicalId + 1, ++numCores[physicalId]);
        auto cpu = new LinuxCpuObject(QStringLiteral("cpu%1").arg(processor), name, m_container, frequency);
        m_cpusBySystemIds.insert({physicalId, coreId}, cpu);
    }
    new LinuxAllCpusObject(numCores.keys().size(), numCores.size(), m_container);

    addSensors();
}

void LinuxCpuPluginPrivate::update()
{
    // Parse /proc/stat to get usage values. The format is described at
    // https://www.kernel.org/doc/html/latest/filesystems/proc.html#miscellaneous-kernel-statistics-in-proc-stat
    QFile stat("/proc/stat");
    stat.open(QIODevice::ReadOnly);
    QByteArray line;
    for (QByteArray line = stat.readLine(); !line.isNull(); line = stat.readLine()) {
        auto values = line.split(' ');
        if (!line.startsWith("cpu")) {
            continue;
        }

        unsigned long long user = values[1].toULongLong();
        unsigned long long nice = values[2].toULongLong();
        unsigned long long system = values[3].toULongLong();
        unsigned long long idle = values[4].toULongLong();
        unsigned long long iowait = values[5].toULongLong();
        unsigned long long irq = values[6].toULongLong();
        unsigned long long softirq = values[7].toULongLong();
        unsigned long long steal = values[8].toULongLong();

        // Total values just start with "cpu", single cpus are numbered cpu0, cpu1, ...
        if (line.startsWith("cpu ")) {
            auto cpu  = static_cast<LinuxAllCpusObject*>(m_container->object(QStringLiteral("all")));
            cpu->update(system + irq + softirq, user + nice , iowait + steal, idle);
        } else if (line.startsWith("cpu")) {
            auto cpu = static_cast<LinuxCpuObject*>(m_container->object(line.left(line.indexOf(' '))));
            cpu->update(system + irq + softirq, user + nice , iowait + steal, idle);
        }
    }
}


void LinuxCpuPluginPrivate::addSensors()
{
#ifdef HAVE_SENSORS
    sensors_init(nullptr);
    int number = 0;
    while (const sensors_chip_name * const chipName = sensors_get_detected_chips(nullptr, &number)) {
        char name[100];
        sensors_snprintf_chip_name(name, 100, chipName);
        if (qstrcmp(chipName->prefix, "coretemp") == 0) {
            addSensorsIntel(chipName);
        } else if (qstrcmp(chipName->prefix, "k10temp") == 0) {
            addSensorsAmd(chipName);
        }
    }
#endif
}

// Documentation: https://www.kernel.org/doc/html/latest/hwmon/coretemp.html
void LinuxCpuPluginPrivate::addSensorsIntel(const sensors_chip_name * const chipName)
{
#ifdef HAVE_SENSORS
    int featureNumber = 0;
    QHash<unsigned int,  sensors_feature const *> coreFeatures;
    int physicalId = -1;
    while (sensors_feature const * feature = sensors_get_features(chipName, &featureNumber)) {
        if (feature->type != SENSORS_FEATURE_TEMP) {
            continue;
        }
        char * sensorLabel = sensors_get_label(chipName, feature);
        unsigned int coreId;
        // First try to see if it's a core temperature because we should have more of those
        if (std::sscanf(sensorLabel, "Core %d", &coreId) != 0) {
            coreFeatures.insert(coreId, feature);
        } else {
            std::sscanf(sensorLabel, "Package id %d", &physicalId);
        }
        free(sensorLabel);
    }
    if (physicalId == -1) {
        return;
    }
    for (auto feature = coreFeatures.cbegin(); feature != coreFeatures.cend(); ++feature) {
        if (m_cpusBySystemIds.contains({physicalId, feature.key()})) {
            // When the cpu has hyperthreading we display multiple cores for each physical core.
            // Naturally they share the same temperature sensor and have the same coreId.
            auto cpu_range = m_cpusBySystemIds.equal_range({physicalId, feature.key()});
            for (auto cpu_it = cpu_range.first; cpu_it != cpu_range.second; ++cpu_it) {
                (*cpu_it)->setTemperatureSensor(chipName, feature.value());
            }
        }
    }
#endif
}

void LinuxCpuPluginPrivate::addSensorsAmd(const sensors_chip_name * const chipName)
{
    // All Processors should have the Tctl pseudo temperature as temp1. Newer ones have the real die
    // temperature Tdie as temp2. Some of those have temperatures for each core complex die (CCD) as
    // temp3-6 or temp3-10 depending on the number of CCDS.
    // https://www.kernel.org/doc/html/latest/hwmon/k10temp.html
    int featureNumber = 0;
    sensors_feature const * tctl = nullptr;
    sensors_feature const * tdie = nullptr;
    sensors_feature const * tccd[8] = {nullptr};
    while (sensors_feature const * feature = sensors_get_features(chipName, &featureNumber)) {
        const QByteArray name (feature->name);
        if (feature->type != SENSORS_FEATURE_TEMP || !name.startsWith("temp")) {
            continue;
        }
        // For temps 1 and 2 we can't just go by the number because in  kernels older than 5.7 they
        // are the wrong way around, so we have to compare labels.
        // https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=b02c6857389da66b09e447103bdb247ccd182456
        char * label = sensors_get_label(chipName, feature);
        if (qstrcmp(label, "Tctl") == 0) {
            tctl = feature;
        }
        else if (qstrcmp(label, "Tdie") == 0) {
            tdie = feature;
        } else {
            tccd[name.mid(4).toUInt()] = feature;
        }
        free(label);
    }
    // TODO How to map CCD temperatures to cores?

    auto setSingleSensor = [this, chipName] (const sensors_feature * const feature) {
        for (auto &cpu : m_cpusBySystemIds) {
            cpu->setTemperatureSensor(chipName, feature);
        }
    };
    if (tdie) {
        setSingleSensor(tdie);
    } else if (tctl) {
        setSingleSensor(tctl);
    }
}

