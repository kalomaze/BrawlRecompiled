#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawList;

// Font rendering for Brawl's host overlays and layout inspection for diagnostics.
// Assets are read from the player's extracted disc.
namespace GameLayout {

// Where the game picture is on screen, in ImGui coordinates: layout units are mapped with the
// layout origin at `centerX/centerY` and `scale` pixels per unit (y up in layout space).
struct View {
    float centerX = 0.0f;
    float centerY = 0.0f;
    float scale = 1.0f;
};
// The game picture's rectangle for the current display mode (4:3 or 16:9 pillar/letterbox,
// stretched for Fill), with 456 layout units across its height like the game's own layouts.
View GameView();

class Font;

class Layout {
public:
    Layout();
    ~Layout();
    Layout(const Layout&) = delete;
    Layout& operator=(const Layout&) = delete;

    // `arcPath` is a disc path such as "/Layout/yesnoWindow/yesnoWindow.arc"; the archive's first
    // .brlyt is used unless `brlytName` names one.
    bool Load(const std::string& arcPath, const std::string& brlytName = {});

    // Where a pane sits in the whole layout (its centre for a centred origin), as authored.
    bool PaneWorldRect(const std::string& paneName, float& x, float& y, float& w, float& h) const;
    std::vector<std::string> PaneNames() const;

    struct Pane;

private:
    bool m_loaded = false;
    std::vector<std::unique_ptr<Pane>> m_panes;  // m_panes[0] is the root
    std::unordered_map<std::string, const Pane*> m_byName;
};

// Load a font from the extracted disc, such as "/system/font/font_hira.brfnt".
std::shared_ptr<Font> LoadFont(const std::string& name);
bool FontReady(const Font* font);
float MeasureText(const Font& font, const std::u16string& text, float fontSize);
// Breaks `text` at spaces into lines no wider than `maxWidth` layout units.
std::vector<std::u16string> WrapText(const Font& font, const std::u16string& text, float fontSize, float maxWidth);
// The height of one line of `font` at `fontSize`, in layout units.
float LineHeight(const Font& font, float fontSize);
void DrawText(ImDrawList* list, const View& view, const Font& font, const std::u16string& text, float x, float y,
              float fontSize, uint32_t topColor, uint32_t bottomColor, float alpha, int originH = 1, int originV = 1);

std::u16string Utf16(const std::string& utf8);

} // namespace GameLayout
