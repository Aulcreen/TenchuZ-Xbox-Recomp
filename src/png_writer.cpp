// tenchuz - PNG files through the Windows Imaging Component.

#include "png_writer.h"

#include <vector>

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

namespace tenchuz {

bool WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint8_t* rgba,
              uint32_t pitch) {
  using Microsoft::WRL::ComPtr;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    // 24-bit BGR rows for the encoder.
    std::vector<uint8_t> bgr(size_t(width) * 3 * height);
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* src = rgba + size_t(y) * pitch;
      uint8_t* dst = bgr.data() + size_t(y) * width * 3;
      for (uint32_t x = 0; x < width; ++x) {
        dst[3 * x] = src[4 * x + 2];
        dst[3 * x + 1] = src[4 * x + 1];
        dst[3 * x + 2] = src[4 * x];
      }
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) &&
         SUCCEEDED(factory->CreateStream(&stream)) &&
         SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
         SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
         format == GUID_WICPixelFormat24bppBGR &&
         SUCCEEDED(frame->WritePixels(height, width * 3, UINT(bgr.size()), bgr.data())) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  }
  if (SUCCEEDED(init)) {
    CoUninitialize();
  }
  return ok;
}

}  // namespace tenchuz
