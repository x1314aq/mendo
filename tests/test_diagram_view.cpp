#include <gtest/gtest.h>
#include "diagram_view.h"
#include "test_helpers.h"
#include "wic_util.h"
#include <d2d1_3.h>
#include <d3d11.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

// ---- DiagramView の倍率 ----

using diagram_view::FitZoom;
using diagram_view::NextZoom;

// 全体表示は余白を除いた窓に収め、等倍より大きくしない。潰した窓では最小の倍率で止める。
TEST(DiagramViewZoom, FitsInsidePaddingWithoutEnlarging)
{
    EXPECT_FLOAT_EQ(FitZoom(1000.0f, 800.0f, 200.0f, 100.0f), 1.0f);
    EXPECT_FLOAT_EQ(FitZoom(432.0f, 1000.0f, 800.0f, 100.0f), 0.5f);
    EXPECT_FLOAT_EQ(FitZoom(1000.0f, 232.0f, 100.0f, 400.0f), 0.5f);
    EXPECT_FLOAT_EQ(FitZoom(10.0f, 10.0f, 1000.0f, 1000.0f), 0.05f);
}

// ボタンの止まる位置は表の値と全体表示の倍率。全体表示のすぐ隣の値は飛ばす。
TEST(DiagramViewZoom, StepsThroughStopsIncludingFit)
{
    EXPECT_FLOAT_EQ(*NextZoom(1.0f, 1.0f, 1), 1.25f);
    EXPECT_FLOAT_EQ(*NextZoom(1.0f, 1.0f, -1), 0.75f);
    // 全体表示 0.7 は止まる位置になり、すぐ隣の 0.67 と 0.75 は飛ばす
    EXPECT_FLOAT_EQ(*NextZoom(0.7f, 0.7f, 1), 1.0f);
    EXPECT_FLOAT_EQ(*NextZoom(0.7f, 0.7f, -1), 0.5f);
    EXPECT_FLOAT_EQ(*NextZoom(1.0f, 0.7f, -1), 0.7f);
    EXPECT_FALSE(NextZoom(8.0f, 1.0f, 1));
    EXPECT_FALSE(NextZoom(0.05f, 0.05f, -1));
}

// Ctrl+ホイールは 1 ノッチ 1 段。触覚板の半ノッチは貯めて 1 段にし、向きが変わったら端数を捨てる。
TEST(DiagramViewZoom, WheelAccumulatesPartialNotchesPerDirection)
{
    using diagram_view::WheelZoom;
    WheelZoom wheel;
    EXPECT_EQ(wheel.Accumulate(WHEEL_DELTA), 1);
    EXPECT_EQ(wheel.Accumulate(-WHEEL_DELTA), -1);
    // 2 ノッチまとめて届いたら 2 段。
    EXPECT_EQ(wheel.Accumulate(WHEEL_DELTA * 2), 2);

    // 半分ずつで 1 段。
    WheelZoom touchpad;
    EXPECT_EQ(touchpad.Accumulate(WHEEL_DELTA / 2), 0);
    EXPECT_EQ(touchpad.Accumulate(WHEEL_DELTA / 2), 1);
    // 逆向きに半分回したら、溜めていた分は捨てて段は動かない。
    WheelZoom reversing;
    EXPECT_EQ(reversing.Accumulate(WHEEL_DELTA / 2), 0);
    EXPECT_EQ(reversing.Accumulate(-WHEEL_DELTA / 2), 0);
    EXPECT_EQ(reversing.Accumulate(-WHEEL_DELTA / 2), -1);
}

TEST(DiagramViewZoom, WheelKeepsRemainderAcrossWholeAndZeroDeltas)
{
    diagram_view::WheelZoom wheel;
    EXPECT_EQ(wheel.Accumulate(30), 0);
    EXPECT_EQ(wheel.Accumulate(0), 0);
    EXPECT_EQ(wheel.Accumulate(240), 2);
    EXPECT_EQ(wheel.Accumulate(90), 1);
    EXPECT_EQ(wheel.Accumulate(-150), -1);
    EXPECT_EQ(wheel.Accumulate(0), 0);
    EXPECT_EQ(wheel.Accumulate(-90), -1);
}

// 画像は 1 px を物理 1 px で描く。DPI の倍率で割った値が DIP の表示寸法になる。
TEST(DiagramViewImage, NaturalSizeDividesByDpiScale)
{
    const D2D1_SIZE_F size = diagram_view::ImageDipSize(1920, 1080, 1.5f);
    EXPECT_FLOAT_EQ(size.width, 1280.0f);
    EXPECT_FLOAT_EQ(size.height, 720.0f);
    // 等倍の装置では原寸のまま。
    const D2D1_SIZE_F same = diagram_view::ImageDipSize(800, 600, 1.0f);
    EXPECT_FLOAT_EQ(same.width, 800.0f);
    EXPECT_FLOAT_EQ(same.height, 600.0f);
}

class DiagramViewDecodeTest : public ComApartmentTest {
protected:
    void SetUp() override
    {
        wic_ = wic_util::CreateWicFactory("DiagramViewDecodeTest");
        ASSERT_NE(wic_, nullptr);
    }

    // container で書く。opaque なら 24bppBGR (JPEG などアルファを持てない形式)、そうでなければ 32bppBGRA。
    bool WriteImage(const std::filesystem::path& path, const GUID& container, UINT width, UINT height, bool opaque)
    {
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
        if (FAILED(wic_->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
            FAILED(wic_->CreateEncoder(container, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
            FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(width, height)) || FAILED(frame->SetResolution(96.0, 96.0)) ||
            FAILED(frame->SetPixelFormat(&format)) || format != (opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA)) {
            return false;
        }
        const UINT bpp = opaque ? 3u : 4u;
        std::vector<BYTE> row(width * bpp);
        for (UINT x = 0; x < width; ++x) {
            row[x * bpp + 2] = 255;
            if (!opaque) {
                row[x * bpp + 3] = 128;
            }
        }
        for (UINT y = 0; y < height; ++y) {
            if (FAILED(frame->WritePixels(1, width * bpp, width * bpp, row.data()))) {
                return false;
            }
        }
        return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    bool WritePng(const std::filesystem::path& path, UINT width, UINT height)
    {
        return WriteImage(path, GUID_ContainerFormatPng, width, height, false);
    }

    static std::vector<BYTE> ReadFileBytes(const std::filesystem::path& path)
    {
        std::ifstream f(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
    }

    static bool WriteFileBytes(const std::filesystem::path& path, std::span<const BYTE> bytes)
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return f.good();
    }

    // PNG の最初の type のチャンクの先頭位置。無ければ SIZE_MAX。
    static size_t FindChunk(std::span<const BYTE> bytes, std::string_view type)
    {
        for (size_t offset = 8; offset + 12 <= bytes.size();) {
            const uint32_t length = (static_cast<uint32_t>(bytes[offset]) << 24) | (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
                                    (static_cast<uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
            if (std::memcmp(bytes.data() + offset + 4, type.data(), type.size()) == 0) {
                return offset;
            }
            offset += static_cast<size_t>(length) + 12;
        }
        return SIZE_MAX;
    }

    ScopedTempDir temp_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
};

// ファイルを消しても、読んだ中身から列を作り直せる (image source を作り直すときに使う)。
TEST_F(DiagramViewDecodeTest, LoadImageFileKeepsBytesAfterFileRemoval)
{
    const auto path = temp_.path() / L"image.png";
    ASSERT_TRUE(WritePng(path, 2, 1));
    auto loaded = diagram_view::LoadImageFile(wic_.Get(), path.wstring(), 8192, 16384);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->image.pixel_width, 2u);
    EXPECT_EQ(loaded->image.pixel_height, 1u);
    ASSERT_TRUE(std::filesystem::remove(path));
    // 列はファイルではなく bytes を参照しているので、まだ画素を読める。
    std::array<BYTE, 8> pixels{};
    ASSERT_HRESULT_SUCCEEDED(loaded->image.source->CopyPixels(nullptr, 8, 8, pixels.data()));
    EXPECT_EQ(pixels, (std::array<BYTE, 8>{ 0, 0, 128, 128, 0, 0, 128, 128 }));
    // 変換し直しても同じ画素になる。
    const auto decoded = wic_util::DecodeImageFileFromMemory(wic_.Get(), std::as_const(loaded->bytes));
    ASSERT_TRUE(decoded);
    pixels = {};
    ASSERT_HRESULT_SUCCEEDED(decoded->converter->CopyPixels(nullptr, 8, 8, pixels.data()));
    EXPECT_EQ(pixels, (std::array<BYTE, 8>{ 0, 0, 128, 128, 0, 0, 128, 128 }));
}

// 原寸は残したまま、列だけを上限まで縮める。
TEST_F(DiagramViewDecodeTest, CapsDecodedResolutionWithoutChangingNaturalSize)
{
    struct Case {
        UINT width, height, gpu_limit, decoded_width, decoded_height;
    };
    constexpr Case cases[]{
        { 10000, 10, 16384, 8192, 8 },
        { 10, 10000, 16384, 8, 8192 },
        { 10000, 10, 1024, 1024, 1 },
        { 10000, 10, 0, 8192, 8 },
    };
    const auto path = temp_.path() / L"large.png";
    for (const auto& c : cases) {
        SCOPED_TRACE(c.gpu_limit);
        ASSERT_TRUE(WritePng(path, c.width, c.height));
        auto loaded = diagram_view::LoadImageFile(wic_.Get(), path.wstring(), 8192, c.gpu_limit);
        ASSERT_TRUE(loaded);
        EXPECT_EQ(loaded->image.pixel_width, c.width);
        EXPECT_EQ(loaded->image.pixel_height, c.height);
        UINT width = 0, height = 0;
        ASSERT_HRESULT_SUCCEEDED(loaded->image.source->GetSize(&width, &height));
        EXPECT_EQ(width, c.decoded_width);
        EXPECT_EQ(height, c.decoded_height);
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
        ASSERT_HRESULT_SUCCEEDED(loaded->image.source->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
        for (size_t i = 0; i < pixels.size(); i += 4) {
            ASSERT_EQ(pixels[i], 0);
            ASSERT_EQ(pixels[i + 1], 0);
            ASSERT_EQ(pixels[i + 2], 128);
            ASSERT_EQ(pixels[i + 3], 128);
        }
    }
}

TEST_F(DiagramViewDecodeTest, RejectsOversizeMissingAndInvalidFiles)
{
    const auto path = temp_.path() / L"large-file.png";
    ASSERT_TRUE(WritePng(path, 2, 1));
    std::filesystem::resize_file(path, 256ull * 1024 * 1024 + 1);
    EXPECT_FALSE(diagram_view::LoadImageFile(wic_.Get(), path.wstring(), 8192, 16384));
    EXPECT_FALSE(diagram_view::LoadImageFile(wic_.Get(), (temp_.path() / L"missing.png").wstring(), 8192, 16384));
    const auto corrupt = temp_.Write(L"corrupt.png", std::string_view("\x89PNG\r\n\x1a\n", 8));
    EXPECT_FALSE(diagram_view::LoadImageFile(wic_.Get(), corrupt.wstring(), 8192, 16384));
    const auto invalid = temp_.Write(L"text.png", "not an image");
    EXPECT_FALSE(diagram_view::LoadImageFile(wic_.Get(), invalid.wstring(), 8192, 16384));
}

TEST_F(DiagramViewDecodeTest, RejectsNetworkAndDevicePaths)
{
    for (const auto* path : { L"\\\\host\\share\\a.png", L"//host/share/a.png", L"\\\\?\\C:\\a.png", L"\\\\.\\pipe\\x", L"\\??\\C:\\a.png" }) {
        EXPECT_FALSE(diagram_view::LoadImageFile(wic_.Get(), path, 8192, 16384));
    }
}

class DiagramViewImageSourceTest : public DiagramViewDecodeTest {
protected:
    static HRESULT CreateContext(Microsoft::WRL::ComPtr<ID2D1DeviceContext2>& out)
    {
        Microsoft::WRL::ComPtr<ID3D11Device> d3d;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        if (FAILED(hr)) {
            return hr;
        }
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        Microsoft::WRL::ComPtr<ID2D1Factory1> factory;
        Microsoft::WRL::ComPtr<ID2D1Device> device;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
        if (FAILED(hr = d3d.As(&dxgi)) ||
            FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
            FAILED(hr = factory->CreateDevice(dxgi.Get(), &device)) ||
            FAILED(hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) {
            return hr;
        }
        return dc.As(&out);
    }

    static HRESULT DrawPixels(ID2D1DeviceContext* dc, ID2D1Image* image, std::array<BYTE, 8>& pixels)
    {
        const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        const auto target_props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96.0f, 96.0f);
        const auto read_props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96.0f, 96.0f);
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> readback;
        HRESULT hr = dc->CreateBitmap(D2D1::SizeU(2, 1), nullptr, 0, target_props, &target);
        if (FAILED(hr)) {
            return hr;
        }
        dc->SetTarget(target.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0.0f));
        dc->DrawImage(image, nullptr, nullptr, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        hr = dc->EndDraw();
        dc->SetTarget(nullptr);
        if (FAILED(hr) || FAILED(hr = dc->CreateBitmap(D2D1::SizeU(2, 1), nullptr, 0, read_props, &readback)) ||
            FAILED(hr = readback->CopyFromBitmap(nullptr, target.Get(), nullptr))) {
            return hr;
        }
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(hr = readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
            return hr;
        }
        std::memcpy(pixels.data(), mapped.bits, pixels.size());
        return readback->Unmap();
    }
};

TEST_F(DiagramViewImageSourceTest, DrawsAfterReleasingApplicationWicReference)
{
    const auto path = temp_.path() / L"image.png";
    ASSERT_TRUE(WritePng(path, 2, 1));
    auto loaded = diagram_view::LoadImageFile(wic_.Get(), path.wstring(), 8192, 16384);
    ASSERT_TRUE(loaded);
    Microsoft::WRL::ComPtr<ID2D1DeviceContext2> dc;
    ASSERT_HRESULT_SUCCEEDED(CreateContext(dc));
    auto source = diagram_view::CreateImageSource(dc.Get(), loaded->image.source.Get());
    ASSERT_NE(source, nullptr);
    loaded->image.source.Reset();
    ASSERT_TRUE(std::filesystem::remove(path));
    ASSERT_HRESULT_SUCCEEDED(source->TrimCache(nullptr));
    std::array<BYTE, 8> pixels{};
    ASSERT_HRESULT_SUCCEEDED(DrawPixels(dc.Get(), source.Get(), pixels));
    EXPECT_EQ(pixels, (std::array<BYTE, 8>{ 0, 0, 128, 128, 0, 0, 128, 128 }));
}

TEST_F(DiagramViewImageSourceTest, RebuildsOnNewDeviceFromSnapshotAfterFileRemoval)
{
    const auto path = temp_.path() / L"image.png";
    ASSERT_TRUE(WritePng(path, 2, 1));
    auto loaded = diagram_view::LoadImageFile(wic_.Get(), path.wstring(), 8192, 16384);
    ASSERT_TRUE(loaded);
    ByteBuffer bytes = std::move(loaded->bytes);
    loaded.reset();
    ASSERT_TRUE(std::filesystem::remove(path));
    for (int generation = 0; generation < 2; ++generation) {
        SCOPED_TRACE(generation);
        Microsoft::WRL::ComPtr<ID2D1DeviceContext2> dc;
        ASSERT_HRESULT_SUCCEEDED(CreateContext(dc));
        auto decoded = diagram_view::DecodeImage(wic_.Get(), std::as_const(bytes), 8192, 16384);
        ASSERT_TRUE(decoded);
        auto source = diagram_view::CreateImageSource(dc.Get(), decoded->source.Get());
        ASSERT_NE(source, nullptr);
        decoded.reset();
        std::array<BYTE, 8> pixels{};
        ASSERT_HRESULT_SUCCEEDED(DrawPixels(dc.Get(), source.Get(), pixels));
        EXPECT_EQ(pixels, (std::array<BYTE, 8>{ 0, 0, 128, 128, 0, 0, 128, 128 }));
    }
}

// 途中で切れたファイルは、ヘッダ (寸法) が読めても画素を読めないので受け付けない。
// PNG は IDAT の手前、JPEG は走査データの途中で切る。
TEST_F(DiagramViewImageSourceTest, RejectsTruncatedImageFile)
{
    // 切る前は読める。切った後だけが読めないことを、同じ名前で確かめる。
    const auto png = temp_.path() / L"cut.png";
    ASSERT_TRUE(WritePng(png, 64, 64));
    ASSERT_TRUE(diagram_view::LoadImageFile(wic_.Get(), png.wstring(), 8192, 16384));
    std::vector<BYTE> png_bytes = ReadFileBytes(png);
    ASSERT_FALSE(png_bytes.empty());
    const size_t idat = FindChunk(png_bytes, "IDAT");
    ASSERT_NE(idat, SIZE_MAX);
    png_bytes.resize(idat);
    ASSERT_TRUE(WriteFileBytes(png, png_bytes));
    EXPECT_FALSE(diagram_view::LoadImageFile(wic_.Get(), png.wstring(), 8192, 16384));

    const auto jpeg = temp_.path() / L"cut.jpg";
    ASSERT_TRUE(WriteImage(jpeg, GUID_ContainerFormatJpeg, 200, 200, true));
    ASSERT_TRUE(diagram_view::LoadImageFile(wic_.Get(), jpeg.wstring(), 8192, 16384));
    std::vector<BYTE> jpeg_bytes = ReadFileBytes(jpeg);
    ASSERT_FALSE(jpeg_bytes.empty());
    jpeg_bytes.resize(jpeg_bytes.size() / 4);
    ASSERT_TRUE(WriteFileBytes(jpeg, jpeg_bytes));
    EXPECT_FALSE(diagram_view::LoadImageFile(wic_.Get(), jpeg.wstring(), 8192, 16384));
}

// アルファを持たないフレーム (JPEG) は形式変換より先に縮める。デコーダ側の縮小デコードが効くので、
// 原寸の PBGRA に広げてから縮めずに済む。並びが逆になると、大きな JPEG で原寸分のメモリを使う。
TEST_F(DiagramViewImageSourceTest, ScalesOpaqueFrameBeforeConversion)
{
    const auto jpeg = temp_.path() / L"scale.jpg";
    ASSERT_TRUE(WriteImage(jpeg, GUID_ContainerFormatJpeg, 200, 200, true));
    const auto jpeg_bytes = ReadFileBytes(jpeg);
    const auto jpeg_decoded = wic_util::DecodeImageFileFromMemory(wic_.Get(), jpeg_bytes);
    ASSERT_TRUE(jpeg_decoded);
    EXPECT_TRUE(jpeg_decoded->opaque_frame) << "JPEG はアルファを持たない";

    const auto jpeg_scaled = diagram_view::DecodeImage(wic_.Get(), jpeg_bytes, 100, 16384);
    ASSERT_TRUE(jpeg_scaled);
    // 原寸は残したまま、列だけが上限まで縮む。
    EXPECT_EQ(jpeg_scaled->pixel_width, 200u);
    EXPECT_EQ(jpeg_scaled->pixel_height, 200u);
    UINT width = 0;
    UINT height = 0;
    ASSERT_HRESULT_SUCCEEDED(jpeg_scaled->source->GetSize(&width, &height));
    EXPECT_EQ(width, 100u);
    EXPECT_EQ(height, 100u);
    WICPixelFormatGUID format{};
    ASSERT_HRESULT_SUCCEEDED(jpeg_scaled->source->GetPixelFormat(&format));
    EXPECT_TRUE(format == GUID_WICPixelFormat32bppPBGRA);
    // 縮小はデコーダのフレームに掛かっている。表に出る列は「縮小 → 変換」の順なので、
    // 一番外側が形式変換になる (逆順なら外側は scaler になる)。
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    EXPECT_TRUE(SUCCEEDED(jpeg_scaled->source.As(&converter))) << "アンチエイリアスのため変換が外側に来ていない";
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    EXPECT_FALSE(SUCCEEDED(jpeg_scaled->source.As(&scaler))) << "変換の内側で縮小していない";

    // アルファ付きは straight alpha のまま縮めると縁がにじむので、変換してから縮める。
    const auto png = temp_.path() / L"scale.png";
    ASSERT_TRUE(WritePng(png, 200, 200));
    const auto png_bytes = ReadFileBytes(png);
    const auto png_decoded = wic_util::DecodeImageFileFromMemory(wic_.Get(), png_bytes);
    ASSERT_TRUE(png_decoded);
    EXPECT_FALSE(png_decoded->opaque_frame);

    const auto png_scaled = diagram_view::DecodeImage(wic_.Get(), png_bytes, 100, 16384);
    ASSERT_TRUE(png_scaled);
    Microsoft::WRL::ComPtr<IWICBitmapScaler> png_scaler;
    EXPECT_TRUE(SUCCEEDED(png_scaled->source.As(&png_scaler))) << "変換の後に縮小していない";
    Microsoft::WRL::ComPtr<IWICFormatConverter> png_converter;
    EXPECT_FALSE(SUCCEEDED(png_scaled->source.As(&png_converter))) << "縮小の外側で変換していない";
}
