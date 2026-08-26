// clipboard.hpp 的 Windows 实现:Win32 Unicode clipboard + 剪贴板位图读
// (PNG 直取 / CF_DIB 经 WIC 转 PNG)。

#include "platform/clipboard.hpp"

#ifdef _WIN32

#include <windows.h>
#include <wincodec.h>

#include <mutex>
#include <vector>

#include "platform/paths.hpp"  // Utf8ToWide

namespace lubancode::platform {

namespace {

// 全局剪贴板一把锁:会话里可能多处并发想写(/copy、贴图、测试),Win32
// 的 OpenClipboard 本身互斥,但排队重试逻辑不想要,直接锁掉。
std::mutex& ClipboardMutex() {
    static std::mutex m;
    return m;
}

// 剪贴板会话 RAII:Open 成功后无论哪条路退出都 Close,后来者不挨饿。
struct ClipboardSession {
    bool opened = false;
    explicit ClipboardSession() : opened(OpenClipboard(nullptr) != FALSE) {}
    ~ClipboardSession() {
        if (opened) {
            CloseClipboard();
        }
    }
    ClipboardSession(const ClipboardSession&) = delete;
    ClipboardSession& operator=(const ClipboardSession&) = delete;
};

// HGLOBAL 里的字节拷出来(GMEM_MOVEABLE 数据),失败给空。
std::vector<unsigned char> CopyGlobalBytes(HGLOBAL memory) {
    std::vector<unsigned char> out;
    if (memory == nullptr) {
        return out;
    }
    const void* locked = GlobalLock(memory);
    if (locked == nullptr) {
        return out;
    }
    const SIZE_T size = GlobalSize(memory);
    out.resize(static_cast<std::size_t>(size));
    const auto* bytes = static_cast<const unsigned char*>(locked);
    out.assign(bytes, bytes + size);
    GlobalUnlock(memory);
    return out;
}

}  // namespace

bool ClipboardLikelyAvailable() {
    return true;  // Win32 剪贴板开箱即用(远程桌面裁剪另说,失败路径如实报)
}

ClipboardResult CopyTextToClipboard(const std::string& utf8_text, std::string& error_detail) {
    std::lock_guard<std::mutex> lock(ClipboardMutex());
    ClipboardSession session;
    if (!session.opened) {
        error_detail = "OpenClipboard 失败(剪贴板被别的程序占着)";
        return ClipboardResult::Failure;
    }
    if (!EmptyClipboard()) {
        error_detail = "EmptyClipboard 失败";
        return ClipboardResult::Failure;
    }
    const std::wstring wide = Utf8ToWide(utf8_text);
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        error_detail = "GlobalAlloc 失败";
        return ClipboardResult::Failure;
    }
    wchar_t* target = static_cast<wchar_t*>(GlobalLock(memory));
    if (target == nullptr) {
        GlobalFree(memory);
        error_detail = "GlobalLock 失败";
        return ClipboardResult::Failure;
    }
    std::copy(wide.begin(), wide.end(), target);
    target[wide.size()] = L'\0';
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        error_detail = "SetClipboardData 失败";
        return ClipboardResult::Failure;
    }
    // 成功后内存归剪贴板所有,不许再 free。
    return ClipboardResult::Ok;
}

// ---------------------------------------------------------------------------
// 剪贴板位图读(Alt+V 直贴)
// ---------------------------------------------------------------------------

namespace {

// COM 一站式初始化:本线程没起过 COM 就 APARTMENTTHREADDED 起一次;起过
// (别的库先来)就不动。失败返回 false,调用方如实报错。
struct ComScope {
    bool ok = false;
    bool uninit = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) {
            ok = true;
            uninit = hr != S_FALSE;  // S_FALSE = 已初始化,别我们来做反初始化
        }
    }
    ~ComScope() {
        if (ok && uninit) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

template <typename T>
struct ComPtr {
    T* ptr = nullptr;
    ~ComPtr() {
        if (ptr != nullptr) {
            ptr->Release();
        }
    }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** operator&() { return &ptr; }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

// DIB 字节包一层 BITMAPFILEHEADER 拼成完整 BMP 文件(WIC 认得 BMP)。
std::vector<unsigned char> DibToBmpFile(const std::vector<unsigned char>& dib) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) {
        return {};
    }
    const auto* info = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
    const DWORD palette = info->biBitCount <= 8
                              ? (static_cast<DWORD>(sizeof(RGBQUAD)) * (DWORD{1} << info->biBitCount))
                              : 0;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;  // "BM"
    header.bfSize = sizeof(header) + static_cast<DWORD>(dib.size());
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER) + palette;
    std::vector<unsigned char> out;
    out.reserve(sizeof(header) + dib.size());
    const auto* header_bytes = reinterpret_cast<const unsigned char*>(&header);
    out.insert(out.end(), header_bytes, header_bytes + sizeof(header));
    out.insert(out.end(), dib.begin(), dib.end());
    return out;
}

}  // namespace

std::optional<std::vector<unsigned char>> ReadClipboardImagePng(std::size_t max_bytes,
                                                                std::string& error) {
    std::lock_guard<std::mutex> lock(ClipboardMutex());
    ClipboardSession session;
    if (!session.opened) {
        error = "OpenClipboard 失败";
        return std::nullopt;
    }

    // 第一优先:现成的 "PNG" 注册格式(浏览器/编辑器复制图片常带,免转码)。
    const UINT png_format = RegisterClipboardFormatW(L"PNG");
    if (png_format != 0 && IsClipboardFormatAvailable(png_format)) {
        std::vector<unsigned char> png = CopyGlobalBytes(GetClipboardData(png_format));
        if (!png.empty()) {
            if (png.size() > max_bytes) {
                error = "剪贴板图片超过大小上限";
                return std::nullopt;
            }
            return png;
        }
    }
    // 第二路:CF_DIB -> 包 BMP 头 -> WIC 解码 -> PNG 编码。
    if (!IsClipboardFormatAvailable(CF_DIB)) {
        error = "剪贴板里没有图片";
        return std::nullopt;
    }
    const std::vector<unsigned char> dib = CopyGlobalBytes(GetClipboardData(CF_DIB));
    const std::vector<unsigned char> bmp = DibToBmpFile(dib);
    if (bmp.empty()) {
        error = "剪贴板位图不完整";
        return std::nullopt;
    }

    ComScope com;
    if (!com.ok) {
        error = "COM 初始化失败";
        return std::nullopt;
    }
    // HGLOBAL 内存流喂 BMP 进去。
    HGLOBAL bmp_memory = GlobalAlloc(GMEM_MOVEABLE, bmp.size());
    if (bmp_memory == nullptr) {
        error = "GlobalAlloc 失败";
        return std::nullopt;
    }
    {
        void* target = GlobalLock(bmp_memory);
        if (target == nullptr) {
            GlobalFree(bmp_memory);
            error = "GlobalLock 失败";
            return std::nullopt;
        }
        std::copy(bmp.begin(), bmp.end(), static_cast<unsigned char*>(target));
        GlobalUnlock(bmp_memory);
    }
    ComPtr<IStream> bmp_stream;
    if (FAILED(CreateStreamOnHGlobal(bmp_memory, /*fDeleteOnRelease=*/TRUE, &bmp_stream))) {
        GlobalFree(bmp_memory);  // 流没建起来,释放责任还在我们手里
        error = "建内存流失败";
        return std::nullopt;
    }
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        error = "WIC 工厂起不来";
        return std::nullopt;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(bmp_stream.ptr, nullptr, WICDecodeMetadataCacheOnDemand,
                                                &decoder))) {
        error = "剪贴板位图解不开";
        return std::nullopt;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) {
        error = "剪贴板位图没有帧";
        return std::nullopt;
    }
    // PNG 编码进内存流,读出来。
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
        error = "PNG 编码器起不来";
        return std::nullopt;
    }
    ComPtr<IStream> png_stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &png_stream)) || !png_stream) {
        error = "建输出流失败";
        return std::nullopt;
    }
    if (FAILED(encoder->Initialize(png_stream.ptr, WICBitmapEncoderNoCache))) {
        error = "PNG 编码器初始化失败";
        return std::nullopt;
    }
    ComPtr<IWICBitmapFrameEncode> out_frame;
    ComPtr<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(&out_frame, &options)) || !out_frame) {
        error = "PNG 编码帧建不起来";
        return std::nullopt;
    }
    if (FAILED(out_frame->Initialize(options.ptr))) {
        error = "PNG 编码帧初始化失败";
        return std::nullopt;
    }
    if (FAILED(out_frame->WriteSource(frame.ptr, nullptr))) {
        error = "PNG 编码失败";
        return std::nullopt;
    }
    if (FAILED(out_frame->Commit()) || FAILED(encoder->Commit())) {
        error = "PNG 编码提交失败";
        return std::nullopt;
    }
    HGLOBAL png_memory = nullptr;
    if (FAILED(GetHGlobalFromStream(png_stream.ptr, &png_memory)) || png_memory == nullptr) {
        error = "读回 PNG 失败";
        return std::nullopt;
    }
    std::vector<unsigned char> png = CopyGlobalBytes(png_memory);
    if (png.empty()) {
        error = "PNG 内容为空";
        return std::nullopt;
    }
    if (png.size() > max_bytes) {
        error = "剪贴板图片转 PNG 后超过大小上限";
        return std::nullopt;
    }
    return png;
}

bool ClipboardHasImage() {
    std::lock_guard<std::mutex> lock(ClipboardMutex());
    // 只查格式可用性,不搬数据:没图的日常粘贴(高频)这一下就完,不进
    // WIC。锁被占就当没图,让文本路自己再试一遍。
    ClipboardSession session;
    if (!session.opened) {
        return false;
    }
    const UINT png_format = RegisterClipboardFormatW(L"PNG");
    return (png_format != 0 && IsClipboardFormatAvailable(png_format) != FALSE) ||
           IsClipboardFormatAvailable(CF_DIB) != FALSE;
}

std::optional<std::string> ReadClipboardTextUtf8(std::string& error) {
    // VS Code 这类编辑器刚把正文写进剪贴板时,别的进程偶尔还攥着锁(与
    // console_win 的 ReadClipboardText 同一个坑),略试几次便走。
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(nullptr) != FALSE) {
            const HANDLE data = GetClipboardData(CF_UNICODETEXT);
            if (data == nullptr) {
                CloseClipboard();
                error = "剪贴板里没有文本";
                return std::nullopt;
            }
            const auto* chars = static_cast<const wchar_t*>(GlobalLock(data));
            if (chars == nullptr) {
                CloseClipboard();
                error = "GlobalLock 失败";
                return std::nullopt;
            }
            const std::size_t capacity = GlobalSize(data) / sizeof(wchar_t);
            std::size_t length = 0;
            while (length < capacity && chars[length] != L'\0') {
                ++length;
            }
            std::wstring text(chars, length);
            GlobalUnlock(data);
            CloseClipboard();
            if (text.empty()) {
                error = "剪贴板里没有文本";
                return std::nullopt;
            }
            return WideToUtf8(text);
        }
        Sleep(5);
    }
    error = "OpenClipboard 失败(剪贴板被别的程序占着)";
    return std::nullopt;
}

}  // namespace lubancode::platform

#endif  // _WIN32
