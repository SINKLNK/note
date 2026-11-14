#pragma once

#include <QString>
#include <QVector>

struct IMMDeviceEnumerator;
struct IAudioEndpointVolume;

// Default audio endpoints and the master volume of the default output, via
// Core Audio. COM objects are apartment-bound, so this lives on the GUI thread
// and is polled from there rather than shared with the monitor thread.
class AudioDevices
{
public:
    enum class Flow { Output, Input };

    struct Device
    {
        QString id;
        QString name;
        bool operator==(const Device &other) const = default;
    };

    AudioDevices();
    ~AudioDevices();
    AudioDevices(const AudioDevices &) = delete;
    AudioDevices &operator=(const AudioDevices &) = delete;

    QVector<Device> devices(Flow flow) const;
    QString defaultDeviceId(Flow flow) const;
    // Makes the endpoint the default for every role, the way the Windows sound
    // settings do. There is no documented API for this; see the .cpp.
    bool setDefaultDevice(const QString &id) const;

    // Master volume of the default output, 0..100; -1 when there is none.
    int volume();
    void setVolume(int percent);

private:
    bool m_ownsCom = false;
    IMMDeviceEnumerator *m_enumerator = nullptr;
    IAudioEndpointVolume *m_volume = nullptr;
    QString m_volumeDeviceId;

    IAudioEndpointVolume *defaultOutputVolume();
};
