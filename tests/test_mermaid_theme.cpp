#include <gtest/gtest.h>
#include "config_service.h"
#include "mermaid_theme.h"
#include "test_helpers.h"
#include <string>

class MermaidThemeTest : public TempDirTestBase {
protected:
    ConfigService config_;

    void SetUp() override
    {
        config_.SetConfigDirOverride(temp_dir_);
    }
};

TEST_F(MermaidThemeTest, LoadsSupportedThemesAndRestoresConfiguredThemeAfterDarkMode)
{
    struct Case {
        std::string_view configured;
        std::string_view dark;
    };
    constexpr Case kCases[] = {
        { "default", "dark" },
        { "base", "dark" },
        { "dark", "dark" },
        { "forest", "dark" },
        { "neutral", "dark" },
        { "neo", "neo-dark" },
        { "neo-dark", "neo-dark" },
        { "redux", "redux-dark" },
        { "redux-dark", "redux-dark" },
        { "redux-color", "redux-dark-color" },
        { "redux-dark-color", "redux-dark-color" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.configured);
        WriteTempFile(L"settings.ini", std::string{ "[Mermaid]\nTheme=" } + std::string{ c.configured } + "\n");
        config_.Load();
        const auto theme = config_.LoadMermaidTheme();
        EXPECT_EQ(theme, c.configured);
        EXPECT_EQ(mermaid_theme::Resolve(theme, false), c.configured);
        EXPECT_EQ(mermaid_theme::Resolve(theme, true), c.dark);
        EXPECT_EQ(mermaid_theme::Resolve(theme, false), c.configured);
        EXPECT_EQ(config_.LoadMermaidTheme(), c.configured);
    }
}

TEST_F(MermaidThemeTest, MissingOrInvalidThemeFallsBackToDefault)
{
    EXPECT_EQ(config_.LoadMermaidTheme(), "default");
    for (const std::string_view theme : { "", "unknown", "Default", "neo-light", "redux-dark-colour", "\"dark\"" }) {
        SCOPED_TRACE(theme);
        WriteTempFile(L"settings.ini", std::string{ "[Mermaid]\nTheme=" } + std::string{ theme } + "\n");
        config_.Load();
        EXPECT_EQ(config_.LoadMermaidTheme(), "default");
        EXPECT_EQ(mermaid_theme::Resolve(config_.LoadMermaidTheme(), true), "dark");
    }
}

TEST_F(MermaidThemeTest, SavesConfiguredThemeAcrossDarkModeAndRestart)
{
    config_.SaveMermaidTheme("redux-color");
    config_.SaveBool("View", "DarkMode", true);
    config_.Flush();
    ConfigService fresh;
    fresh.SetConfigDirOverride(temp_dir_);
    fresh.Load();
    EXPECT_EQ(fresh.LoadMermaidTheme(), "redux-color");
    EXPECT_EQ(mermaid_theme::Resolve(fresh.LoadMermaidTheme(), fresh.LoadBool("View", "DarkMode", false)), "redux-dark-color");
    fresh.SaveBool("View", "DarkMode", false);
    EXPECT_EQ(mermaid_theme::Resolve(fresh.LoadMermaidTheme(), false), "redux-color");
}

TEST_F(MermaidThemeTest, PersistsDefaultForMissingOrInvalidTheme)
{
    config_.SaveMermaidTheme(config_.LoadMermaidTheme());
    config_.Flush();
    config_.Load();
    EXPECT_EQ(config_.LoadWString("Mermaid", "Theme"), L"default");

    WriteTempFile(L"settings.ini", "[Mermaid]\nTheme=invalid\n");
    config_.Load();
    config_.SaveMermaidTheme(config_.LoadMermaidTheme());
    config_.Flush();
    config_.Load();
    EXPECT_EQ(config_.LoadWString("Mermaid", "Theme"), L"default");
}
