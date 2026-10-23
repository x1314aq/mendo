# 混合排版示例 · Mixed Script Rendering

mendo 在同一个段落里同时处理中文和 English：正文使用 **Microsoft YaHei UI**，其中 ASCII 区间切到 **Segoe UI**，让 DirectWrite 的字宽与字面保持一致，避免中文标点与英文单词之间出现忽宽忽窄的缝隙。

> 中文里的英文技术名词（`ID2D1DeviceContext`、DirectWrite、Direct2D）保持拉丁字形，其余字符交给中文字体渲染。

## 为什么需要混合字体

- **字宽一致** — 英文单词使用 Segoe UI 的自然宽度，不会被中文字体的全角拉丁字形撑开。
- **行内代码** — `void Renderer::ApplyZoomFromBase(const Theme& base, float zoom)` 使用 Cascadia Mono，等宽对齐。
- **标点挤压** — 中文句号、逗号与英文缩写相邻时，间距按各自的字体度量计算。

## 代码块

```cpp
// 缩放只作用于正文，侧栏的尺寸与布局保持不变。
void Theme::ApplyZoom(float new_zoom) noexcept
{
    const float ratio = new_zoom / zoom;
    zoom = new_zoom;
    for (float Theme::* field : kScalableThemeFields) {
        this->*field *= ratio;
    }
}
```

| 项目 | 实现 | 说明 |
|---|---|---|
| 正文 | Microsoft YaHei UI | 简体中文与全角标点 |
| 拉丁 | Segoe UI | 行内 ASCII 区间 |
| 等宽 | Cascadia Mono | 代码块与行内代码 |

内存占用 63–84 MB，启动耗时约 40 ms，缩放步进 0.25x–5.00x。
