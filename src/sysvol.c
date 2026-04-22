// sysvol.c -- Windows Core Audio system master-volume query. Kept in its
// own TU so the COM-macro flags below (COBJMACROS/CINTERFACE) don't
// infect the rest of the build.
#define COBJMACROS
#define CINTERFACE
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

// Returns the default render-endpoint master volume in [0..1]. Returns 0.0
// if the endpoint is muted and 1.0 on any failure.
float Sysvol_Get(void) {
    float vol = 1.0f;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int needUninit = SUCCEEDED(hr);
    IMMDeviceEnumerator *enumr = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void**)&enumr))) {
        if (needUninit) CoUninitialize();
        return vol;
    }
    IMMDevice *dev = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eConsole, &dev))) {
        IAudioEndpointVolume *av = NULL;
        if (SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioEndpointVolume,
                                         CLSCTX_ALL, NULL, (void**)&av))) {
            BOOL mute = FALSE;
            IAudioEndpointVolume_GetMute(av, &mute);
            if (mute) {
                vol = 0.0f;
            } else {
                float scalar = 1.0f;
                if (SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(av, &scalar))) {
                    vol = scalar;
                }
            }
            IAudioEndpointVolume_Release(av);
        }
        IMMDevice_Release(dev);
    }
    IMMDeviceEnumerator_Release(enumr);
    if (needUninit) CoUninitialize();
    return vol;
}
