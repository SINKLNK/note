#include "AudioDevices.h"

#ifdef Q_OS_WIN
#include <Windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
// Undocumented, but the interface the Windows sound settings use to switch the
// default endpoint, and unchanged since Windows 7; EarTrumpet and SoundSwitch
// rely on it too. Only SetDefaultEndpoint is called, so the other methods only
// need to be there to keep the vtable slots in place.
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

class DECLSPEC_UUID("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;

EDataFlow toDataFlow(AudioDevices::Flow flow)
{
    return flow == AudioDevices::Flow::Output ? eRender : eCapture;
}

QString deviceId(IMMDevice *device)
{
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) {
        return {};
    }
    const QString result = QString::fromWCharArray(id);
    CoTaskMemFree(id);
    return result;
}

QString friendlyName(IMMDevice *device)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    QString result;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        result = QString::fromWCharArray(value.pwszVal);
    }
    PropVariantClear(&value);
    return result;
}
} // namespace

AudioDevices::AudioDevices()
{
    // Qt has normally made the GUI thread an STA already; then this is S_FALSE
    // and still has to be balanced. RPC_E_CHANGED_MODE means someone chose
    // MTA, which works just as well and is not ours to undo.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_ownsCom = SUCCEEDED(init);

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&m_enumerator)))) {
        m_enumerator = nullptr;
    }
}

AudioDevices::~AudioDevices()
{
    if (m_volume) {
        m_volume->Release();
    }
    if (m_enumerator) {
        m_enumerator->Release();
    }
    if (m_ownsCom) {
        CoUninitialize();
    }
}

QVector<AudioDevices::Device> AudioDevices::devices(Flow flow) const
{
    QVector<Device> result;
    ComPtr<IMMDeviceCollection> collection;
    if (!m_enumerator
        || FAILED(m_enumerator->EnumAudioEndpoints(toDataFlow(flow), DEVICE_STATE_ACTIVE, &collection))) {
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, &device))) {
            result.append({deviceId(device.Get()), friendlyName(device.Get())});
        }
    }
    return result;
}

QString AudioDevices::defaultDeviceId(Flow flow) const
{
    ComPtr<IMMDevice> device;
    if (!m_enumerator || FAILED(m_enumerator->GetDefaultAudioEndpoint(toDataFlow(flow), eConsole, &device))) {
        return {};
    }
    return deviceId(device.Get());
}

bool AudioDevices::setDefaultDevice(const QString &id) const
{
    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy)))) {
        return false;
    }

    const std::wstring wideId = id.toStdWString();
    bool ok = true;
    for (const ERole role : {eConsole, eMultimedia, eCommunications}) {
        ok = SUCCEEDED(policy->SetDefaultEndpoint(wideId.c_str(), role)) && ok;
    }
    return ok;
}

IAudioEndpointVolume *AudioDevices::defaultOutputVolume()
{
    // The interface belongs to one endpoint; follow the default when it moves.
    const QString id = defaultDeviceId(Flow::Output);
    if (id != m_volumeDeviceId || !m_volume) {
        if (m_volume) {
            m_volume->Release();
            m_volume = nullptr;
        }
        m_volumeDeviceId = id;

        ComPtr<IMMDevice> device;
        if (!id.isEmpty() && m_enumerator
            && SUCCEEDED(m_enumerator->GetDevice(reinterpret_cast<LPCWSTR>(id.utf16()), &device))) {
            device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void **>(&m_volume));
        }
    }
    return m_volume;
}

int AudioDevices::volume()
{
    IAudioEndpointVolume *endpoint = defaultOutputVolume();
    float level = 0.0f;
    if (!endpoint || FAILED(endpoint->GetMasterVolumeLevelScalar(&level))) {
        return -1;
    }
    return qRound(level * 100.0f);
}

void AudioDevices::setVolume(int percent)
{
    if (IAudioEndpointVolume *endpoint = defaultOutputVolume()) {
        endpoint->SetMasterVolumeLevelScalar(qBound(0, percent, 100) / 100.0f, nullptr);
    }
}

#else

AudioDevices::AudioDevices() = default;
AudioDevices::~AudioDevices() = default;
QVector<AudioDevices::Device> AudioDevices::devices(Flow) const { return {}; }
QString AudioDevices::defaultDeviceId(Flow) const { return {}; }
bool AudioDevices::setDefaultDevice(const QString &) const { return false; }
int AudioDevices::volume() { return -1; }
void AudioDevices::setVolume(int) {}

#endif
