#include <gtest/gtest.h>
#include "diagram_view.h"
#include "test_helpers.h"
#include "wic_util.h"
#include <array>
#include <filesystem>
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

    bool WritePng(const std::filesystem::path& path, UINT width, UINT height)
    {
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(wic_->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
            FAILED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
            FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(width, height)) ||
            FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA) {
            return false;
        }
        std::vector<BYTE> row(width * 4);
        for (UINT x = 0; x < width; ++x) {
            row[x * 4 + 2] = 255;
            row[x * 4 + 3] = 128;
        }
        for (UINT y = 0; y < height; ++y) {
            if (FAILED(frame->WritePixels(1, width * 4, width * 4, row.data()))) {
                return false;
            }
        }
        return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    ScopedTempDir temp_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
};

TEST_F(DiagramViewDecodeTest, OwnsDecodedPixelsAndPreservesTransparencyAfterFileRemoval)
{
    const auto path = temp_.path() / L"image.png";
    ASSERT_TRUE(WritePng(path, 2, 1));
    const auto image = diagram_view::DecodeImage(wic_.Get(), path.wstring(), 16384);
    ASSERT_TRUE(image);
    EXPECT_EQ(image->pixel_width, 2u);
    EXPECT_EQ(image->pixel_height, 1u);
    ASSERT_TRUE(std::filesystem::remove(path));
    std::array<BYTE, 8> pixels{};
    ASSERT_HRESULT_SUCCEEDED(image->bitmap->CopyPixels(nullptr, 8, 8, pixels.data()));
    EXPECT_EQ(pixels, (std::array<BYTE, 8>{ 0, 0, 128, 128, 0, 0, 128, 128 }));
}

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
        const auto image = diagram_view::DecodeImage(wic_.Get(), path.wstring(), c.gpu_limit);
        ASSERT_TRUE(image);
        EXPECT_EQ(image->pixel_width, c.width);
        EXPECT_EQ(image->pixel_height, c.height);
        UINT width = 0, height = 0;
        ASSERT_HRESULT_SUCCEEDED(image->bitmap->GetSize(&width, &height));
        EXPECT_EQ(width, c.decoded_width);
        EXPECT_EQ(height, c.decoded_height);
    }
}

TEST_F(DiagramViewDecodeTest, RejectsOversizeMissingAndInvalidFiles)
{
    const auto path = temp_.path() / L"large-file.png";
    ASSERT_TRUE(WritePng(path, 2, 1));
    std::filesystem::resize_file(path, 256ull * 1024 * 1024 + 1);
    EXPECT_FALSE(diagram_view::DecodeImage(wic_.Get(), path.wstring(), 16384));
    EXPECT_FALSE(diagram_view::DecodeImage(wic_.Get(), (temp_.path() / L"missing.png").wstring(), 16384));
    const auto corrupt = temp_.Write(L"corrupt.png", std::string_view("\x89PNG\r\n\x1a\n", 8));
    EXPECT_FALSE(diagram_view::DecodeImage(wic_.Get(), corrupt.wstring(), 16384));
    const auto invalid = temp_.Write(L"text.png", "not an image");
    EXPECT_FALSE(diagram_view::DecodeImage(wic_.Get(), invalid.wstring(), 16384));
}

TEST_F(DiagramViewDecodeTest, RejectsNetworkAndDevicePaths)
{
    for (const auto* path : { L"\\\\host\\share\\a.png", L"//host/share/a.png", L"\\\\?\\C:\\a.png", L"\\\\.\\pipe\\x", L"\\??\\C:\\a.png" }) {
        EXPECT_FALSE(diagram_view::DecodeImage(wic_.Get(), path, 16384));
    }
}
