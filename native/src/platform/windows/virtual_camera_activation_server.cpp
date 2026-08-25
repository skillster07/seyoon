#include "vividcam/media_foundation_source.hpp"
#include "vividcam/output_profile.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <unknwn.h>
#include <wrl/client.h>

#include <atomic>
#include <new>
#include <mutex>
#include <string>

namespace {
std::atomic<long> g_locks{0};
std::once_flag g_media_foundation_once;
HRESULT g_media_foundation_status = E_UNEXPECTED;
constexpr CLSID kVividCamSourceClsid{
    0xb3f8e8e4, 0x1c65, 0x4c10, {0x9d, 0xb4, 0xad, 0x2b, 0x78, 0x0a, 0x64, 0x01}};

class SourceActivate final : public IMFActivate {
 public:
  HRESULT Initialize() { return MFCreateAttributes(&attributes_, 8); }

  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == __uuidof(IMFAttributes) ||
        iid == __uuidof(IMFActivate)) {
      *object = static_cast<IMFActivate*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const auto references = --references_;
    if (references == 0) delete this;
    return references;
  }

  STDMETHODIMP ActivateObject(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    std::string error;
    auto source = vividcam::create_media_foundation_virtual_camera_source(
        vividcam::default_profile(vividcam::Platform::Soop), error,
        vividcam::MediaFoundationVirtualCameraSourceMode::SyntheticPattern);
    if (!source.valid()) return E_FAIL;

    auto* media_source = reinterpret_cast<IMFMediaSource*>(source.native_pointer);
    Microsoft::WRL::ComPtr<IMFMediaSourceEx> source_ex;
    HRESULT status = media_source->QueryInterface(IID_PPV_ARGS(&source_ex));
    Microsoft::WRL::ComPtr<IMFAttributes> source_attributes;
    if (SUCCEEDED(status)) status = source_ex->GetSourceAttributes(&source_attributes);
    if (SUCCEEDED(status)) status = attributes_->CopyAllItems(source_attributes.Get());
    if (SUCCEEDED(status)) status = media_source->QueryInterface(iid, object);
    return status;
  }
  STDMETHODIMP ShutdownObject() override { return E_NOTIMPL; }
  STDMETHODIMP DetachObject() override { return E_NOTIMPL; }

  STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override {
    return attributes_->GetItem(key, value);
  }
  STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override {
    return attributes_->GetItemType(key, type);
  }
  STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override {
    return attributes_->CompareItem(key, value, result);
  }
  STDMETHODIMP Compare(IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE match,
                       BOOL* result) override {
    return attributes_->Compare(theirs, match, result);
  }
  STDMETHODIMP GetUINT32(REFGUID key, UINT32* value) override {
    return attributes_->GetUINT32(key, value);
  }
  STDMETHODIMP GetUINT64(REFGUID key, UINT64* value) override {
    return attributes_->GetUINT64(key, value);
  }
  STDMETHODIMP GetDouble(REFGUID key, double* value) override {
    return attributes_->GetDouble(key, value);
  }
  STDMETHODIMP GetGUID(REFGUID key, GUID* value) override {
    return attributes_->GetGUID(key, value);
  }
  STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override {
    return attributes_->GetStringLength(key, length);
  }
  STDMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size,
                         UINT32* length) override {
    return attributes_->GetString(key, value, size, length);
  }
  STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value,
                                  UINT32* length) override {
    return attributes_->GetAllocatedString(key, value, length);
  }
  STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override {
    return attributes_->GetBlobSize(key, size);
  }
  STDMETHODIMP GetBlob(REFGUID key, UINT8* buffer, UINT32 size,
                       UINT32* blob_size) override {
    return attributes_->GetBlob(key, buffer, size, blob_size);
  }
  STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buffer,
                                UINT32* size) override {
    return attributes_->GetAllocatedBlob(key, buffer, size);
  }
  STDMETHODIMP GetUnknown(REFGUID key, REFIID iid, LPVOID* object) override {
    return attributes_->GetUnknown(key, iid, object);
  }
  STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override {
    return attributes_->SetItem(key, value);
  }
  STDMETHODIMP DeleteItem(REFGUID key) override {
    return attributes_->DeleteItem(key);
  }
  STDMETHODIMP DeleteAllItems() override { return attributes_->DeleteAllItems(); }
  STDMETHODIMP SetUINT32(REFGUID key, UINT32 value) override {
    return attributes_->SetUINT32(key, value);
  }
  STDMETHODIMP SetUINT64(REFGUID key, UINT64 value) override {
    return attributes_->SetUINT64(key, value);
  }
  STDMETHODIMP SetDouble(REFGUID key, double value) override {
    return attributes_->SetDouble(key, value);
  }
  STDMETHODIMP SetGUID(REFGUID key, REFGUID value) override {
    return attributes_->SetGUID(key, value);
  }
  STDMETHODIMP SetString(REFGUID key, LPCWSTR value) override {
    return attributes_->SetString(key, value);
  }
  STDMETHODIMP SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) override {
    return attributes_->SetBlob(key, buffer, size);
  }
  STDMETHODIMP SetUnknown(REFGUID key, IUnknown* value) override {
    return attributes_->SetUnknown(key, value);
  }
  STDMETHODIMP LockStore() override { return attributes_->LockStore(); }
  STDMETHODIMP UnlockStore() override { return attributes_->UnlockStore(); }
  STDMETHODIMP GetCount(UINT32* count) override { return attributes_->GetCount(count); }
  STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key,
                              PROPVARIANT* value) override {
    return attributes_->GetItemByIndex(index, key, value);
  }
  STDMETHODIMP CopyAllItems(IMFAttributes* destination) override {
    return attributes_->CopyAllItems(destination);
  }

 private:
  ~SourceActivate() = default;
  std::atomic<ULONG> references_{1};
  Microsoft::WRL::ComPtr<IMFAttributes> attributes_;
};

class SourceFactory final : public IClassFactory {
 public:
  SourceFactory() = default;
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
    *object = static_cast<IClassFactory*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const auto references = --references_;
    if (references == 0) delete this;
    return references;
  }
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    std::call_once(g_media_foundation_once, [] {
      g_media_foundation_status = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    });
    if (FAILED(g_media_foundation_status)) return g_media_foundation_status;
    auto* activation = new (std::nothrow) SourceActivate();
    if (!activation) return E_OUTOFMEMORY;
    HRESULT status = activation->Initialize();
    if (SUCCEEDED(status)) status = activation->QueryInterface(iid, object);
    activation->Release();
    return status;
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) ++g_locks;
    else if (g_locks.load() > 0) --g_locks;
    return S_OK;
  }
 private:
  ~SourceFactory() = default;
  std::atomic<ULONG> references_{1};
};
} // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  if (clsid != kVividCamSourceClsid) return CLASS_E_CLASSNOTAVAILABLE;
  auto* factory = new (std::nothrow) SourceFactory();
  if (!factory) return E_OUTOFMEMORY;
  const auto status = factory->QueryInterface(iid, object);
  factory->Release();
  return status;
}

STDAPI DllCanUnloadNow(void) {
  // Media Foundation can retain the source after releasing the class factory.
  // Keep this in-process server loaded until its host exits so source vtables
  // can never point into an unloaded module.
  return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
