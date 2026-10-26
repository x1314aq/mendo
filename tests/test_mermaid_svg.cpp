#include <gtest/gtest.h>
#include "mermaid_svg.h"
#include "mermaid_theme.h"
#include "merman_abi.h"
#include <lunasvg.h>
#include <string>

TEST(MermaidSvg, UnsupportedSelectorsDoNotDiscardLaterRules)
{
    const auto svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
        <style>
            .node path:nth-of-type(2){stroke:red;}
            .node rect{fill:#ffffff;stroke:#000000;}
            .node path:nth-of-type(2),.flowchart-link{fill:none;stroke:#000000;}
            rect{fill:red;}
            .label text{text-anchor:middle;fill:#000000;}
        </style>
        <g class="node"><rect width="100" height="20"/></g>
        <path class="flowchart-link" d="M 10 30 L 50 50 L 90 30"/>
        <g class="label"><text x="50" y="15">Test</text></g>
    </svg>)";
    auto document = LoadMermaidSvg(svg);
    ASSERT_NE(document, nullptr);
    EXPECT_EQ(document->querySelectorAll("rect").front().getAttribute("fill"), "#ffffff");
    EXPECT_EQ(document->querySelectorAll(".flowchart-link").front().getAttribute("fill"), "none");
    EXPECT_EQ(document->querySelectorAll("text").front().getAttribute("text-anchor"), "middle");
    auto bitmap = document->renderToBitmap();
    ASSERT_FALSE(bitmap.isNull());
    const auto* pixels = reinterpret_cast<const uint32_t*>(bitmap.data());
    EXPECT_EQ(pixels[10 * (bitmap.stride() / 4) + 10], 0xffffffffu);
    EXPECT_EQ(pixels[35 * (bitmap.stride() / 4) + 50], 0u);
}

class MermaidFlowchartSvgTest : public ::testing::TestWithParam<std::string_view> {};

TEST_P(MermaidFlowchartSvgTest, ThemeKeepsNodesTextAndEdgesStyledInBothModes)
{
    constexpr auto kCode = R"(flowchart TB
    API[NIXL 兼容 Python / C++ API]
    AG[NIXL Agent core]
    MD[Metadata manager / SerDes]
    DL[Descriptor / registered sections]
    XR[Async request / handle / telemetry]
    NT[Notification / progress / plugin manager]
    BE[AGIC backend：兼容 UCX SB API]
    CP[复用 UCX 连接、rkey、RMA 与通知]
    MP[新增 AGIC memory provider]
    MR[Runtime allocation / DMA-BUF / peer-memory]
    NIC[RDMA NIC / provider]
    API --> AG
    AG --> MD
    AG --> DL
    AG --> XR
    AG --> NT
    MD --> BE
    DL --> BE
    XR --> BE
    NT --> BE
    BE --> CP
    BE --> MP
    MP --> MR
    MP --> CP
    CP --> NIC)";
    MermanAbi merman;
    std::string error;
    ASSERT_TRUE(merman.Initialize(error)) << error;
    for (const bool dark_mode : { false, true }) {
        SCOPED_TRACE(dark_mode);
        std::string svg;
        ASSERT_TRUE(merman.RenderSvg(kCode, 1500.0f, mermaid_theme::Resolve(GetParam(), dark_mode), svg, error)) << error;
        auto document = LoadMermaidSvg(svg);
        ASSERT_NE(document, nullptr);
        const auto nodes = document->querySelectorAll(".node rect");
        ASSERT_EQ(nodes.size(), 11u);
        for (const auto& node : nodes) {
            EXPECT_FALSE(node.getAttribute("fill").empty());
            EXPECT_FALSE(node.getAttribute("stroke").empty());
        }
        const auto links = document->querySelectorAll(".flowchart-link");
        ASSERT_EQ(links.size(), 14u);
        for (const auto& link : links) {
            EXPECT_EQ(link.getAttribute("fill"), "none");
            EXPECT_FALSE(link.getAttribute("stroke").empty());
        }
        for (const auto& text : document->querySelectorAll(".node .label text")) {
            EXPECT_EQ(text.getAttribute("text-anchor"), "middle");
            EXPECT_FALSE(text.getAttribute("fill").empty());
        }
        EXPECT_FALSE(document->renderToBitmap().isNull());
    }
}

INSTANTIATE_TEST_SUITE_P(AllThemes, MermaidFlowchartSvgTest, ::testing::Values(
    "default", "base", "dark", "forest", "neutral", "neo", "neo-dark",
    "redux", "redux-dark", "redux-color", "redux-dark-color"));

class MermaidSequenceSvgTest : public ::testing::TestWithParam<std::string_view> {};

TEST_P(MermaidSequenceSvgTest, LongParticipantLabelsFitTopAndBottomBoxes)
{
    constexpr auto kCode = R"(sequenceDiagram
    autonumber
    participant R as Proxy
    participant PS as P Scheduler
    participant PW as P AixlConnector Worker
    participant PK as P DB500 KV / Exchange Buffer
    participant DS as D Scheduler
    participant DW as D AixlConnector Worker
    participant DK as D DB500 KV
    R->>PS: prefill，do_remote_decode
    PS->>PW: 调度 prefill，可分 chunk 计算
    PW->>PK: KV 写入；按 mode 固定源或 pack 快照
    PK-->>PW: 数据 ready fence
    PW-->>PS: 可交接 token/blocks 状态
    PS->>PS: 保留源数据，建立 lease
    PS-->>R: kv_transfer_params / 远端请求身份
    R->>DS: decode，do_remote_prefill
    DS->>DS: 计算本地 prefix，分配目标 block
    DS->>DW: local blocks + remote meta
    DW->>PW: 兼容握手与读取授权
    PW-->>DW: 当前 source / lease / region 确认
    DW->>DW: 构建 block/tile transfer plan
    DW->>PK: AIXL READ 到本地 KV 或 Exchange Buffer
    PK-->>DW: 数据返回到D目标，尚需本地completion确认
    DW->>DW: 等待本地READ/flush完成
    DW-->>PW: 经backend传递读完成notification，P源可按reader引用释放
    PW-->>PS: 所有源 reader 完成，可释放
    DW->>DK: 必要的 unpack/scatter/visibility
    DK-->>DW: 完整 KV 可消费
    DW-->>DS: finished_recving
    DS->>DW: 调度 decode
    DS-->>R: 返回生成结果)";
    MermanAbi merman;
    std::string error;
    ASSERT_TRUE(merman.Initialize(error)) << error;
    for (const bool dark_mode : { false, true }) {
        SCOPED_TRACE(dark_mode);
        std::string svg;
        ASSERT_TRUE(merman.RenderSvg(kCode, 2000.0f, mermaid_theme::Resolve(GetParam(), dark_mode), svg, error)) << error;
        auto document = LoadMermaidSvg(svg);
        ASSERT_NE(document, nullptr);
        document->applyStyleSheet("text{font-family:\"Microsoft YaHei UI\",\"Segoe UI\",sans-serif;font-size:14px;}");
        document->updateLayout();
        const auto actors = document->querySelectorAll("rect.actor");
        const auto texts = document->querySelectorAll("text.actor");
        ASSERT_EQ(actors.size(), 14u);
        ASSERT_EQ(texts.size(), actors.size());
        for (size_t i = 0; i < texts.size(); ++i) {
            SCOPED_TRACE(i);
            const auto text = texts[i].getBoundingBox();
            const float x = std::stof(actors[i].getAttribute("x"));
            const float width = std::stof(actors[i].getAttribute("width"));
            EXPECT_GE(text.x, x + 5.0f);
            EXPECT_LE(text.x + text.w, x + width - 5.0f);
        }
        EXPECT_FALSE(document->renderToBitmap().isNull());
    }
}

INSTANTIATE_TEST_SUITE_P(AllThemes, MermaidSequenceSvgTest, ::testing::Values(
    "default", "base", "dark", "forest", "neutral", "neo", "neo-dark",
    "redux", "redux-dark", "redux-color", "redux-dark-color"));
