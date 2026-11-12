#include "netplay/game_layout.h"

#include "display_settings.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <aurora/imgui.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>

namespace GameLayout {

class Font {
public:
    struct Glyph {
        int8_t left = 0;
        uint8_t glyphWidth = 0;
        int8_t charWidth = 0;
        uint16_t sheet = 0;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    };
    bool ok = false;
    int8_t linefeed = 0;
    uint8_t height = 0;
    uint8_t width = 0;
    uint8_t ascent = 0;
    uint8_t cellWidth = 0;
    uint8_t cellHeight = 0;
    uint16_t defaultIndex = 0;
    std::vector<uint64_t> sheets;  // ImTextureID per sheet
    std::vector<Glyph> glyphs;     // by glyph index
    std::unordered_map<uint16_t, uint16_t> map;  // code -> glyph index

    const Glyph* Find(char16_t code) const {
        const auto it = map.find(static_cast<uint16_t>(code));
        const uint16_t index = it != map.end() ? it->second : defaultIndex;
        return index < glyphs.size() ? &glyphs[index] : nullptr;
    }
};

namespace {

// ---------------------------------------------------------------- files
uint16_t BE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t BE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}
float BEF(const uint8_t* p) {
    const uint32_t v = BE32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// Straight from the extracted game's files/ folder (the disc root): this also works before the
// game has set up its own disc access, which the boot screen needs.
bool ReadDiscFile(const std::string& dvdPath, std::vector<uint8_t>& out) {
    const std::filesystem::path root = RuntimeConfigFile::ResolvedDvdRoot();
    if (root.empty()) {
        return false;
    }
    std::string relative = dvdPath;
    while (!relative.empty() && relative.front() == '/') {
        relative.erase(relative.begin());
    }
    std::ifstream in(root / "files" / std::filesystem::u8path(relative), std::ios::binary);
    if (!in) {
        RT_LOGF(RT_TAG_RUNTIME, "netplay ui: %s is not in the game folder\n", dvdPath.c_str());
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

// U8 archive: name -> (offset, size) into the archive bytes.
std::map<std::string, std::pair<uint32_t, uint32_t>> ListU8(const std::vector<uint8_t>& data) {
    std::map<std::string, std::pair<uint32_t, uint32_t>> files;
    if (data.size() < 0x20 || BE32(data.data()) != 0x55AA382Du) {
        return files;
    }
    const size_t rootOffset = BE32(&data[4]);
    if (rootOffset > data.size() || data.size() - rootOffset < 12) {
        return files;
    }
    const uint32_t total = BE32(&data[rootOffset + 8]);
    if (total == 0 || total > (data.size() - rootOffset) / 12) {
        return files;
    }
    const size_t stringTable = rootOffset + static_cast<size_t>(total) * 12;
    const auto name = [&](uint32_t offset) {
        std::string s;
        if (offset >= data.size() - stringTable) return s;
        for (size_t i = stringTable + offset; i < data.size() && data[i] != 0; ++i) {
            s.push_back(static_cast<char>(data[i]));
        }
        return s;
    };
    std::vector<std::pair<std::string, uint32_t>> dirs{{"", total}};
    for (size_t i = 1; i < total; ++i) {
        while (dirs.size() > 1 && i >= dirs.back().second) {
            dirs.pop_back();
        }
        const uint8_t* node = &data[rootOffset + i * 12];
        const uint32_t typeName = BE32(node);
        const std::string nodeName = name(typeName & 0xFFFFFFu);
        std::string path;
        for (const auto& d : dirs) {
            if (!d.first.empty()) {
                path += d.first + "/";
            }
        }
        path += nodeName;
        if ((typeName >> 24) == 1) {
            dirs.emplace_back(nodeName, BE32(node + 8));
        } else {
            const uint32_t offset = BE32(node + 4), size = BE32(node + 8);
            if (offset > data.size() || size > data.size() - offset) return {};
            files[path] = {offset, size};
        }
    }
    return files;
}

// ---------------------------------------------------------------- GX texture decoding
uint8_t Exp3(uint32_t v) { return static_cast<uint8_t>((v << 5) | (v << 2) | (v >> 1)); }
uint8_t Exp4(uint32_t v) { return static_cast<uint8_t>((v << 4) | v); }
uint8_t Exp5(uint32_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
uint8_t Exp6(uint32_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }

void Rgb565(uint16_t c, uint8_t* o) {
    o[0] = Exp5(c >> 11);
    o[1] = Exp6((c >> 5) & 63);
    o[2] = Exp5(c & 31);
    o[3] = 255;
}
void Rgb5a3(uint16_t c, uint8_t* o) {
    if (c & 0x8000) {
        o[0] = Exp5((c >> 10) & 31);
        o[1] = Exp5((c >> 5) & 31);
        o[2] = Exp5(c & 31);
        o[3] = 255;
    } else {
        o[3] = Exp3((c >> 12) & 7);
        o[0] = Exp4((c >> 8) & 15);
        o[1] = Exp4((c >> 4) & 15);
        o[2] = Exp4(c & 15);
    }
}
void Ia8(uint16_t c, uint8_t* o) {
    o[0] = o[1] = o[2] = static_cast<uint8_t>(c & 0xFF);
    o[3] = static_cast<uint8_t>(c >> 8);
}

struct FormatInfo {
    uint32_t bw, bh, bits;
};
bool Info(uint32_t format, FormatInfo& info) {
    switch (format) {
    case 0: info = {8, 8, 4}; return true;    // I4
    case 1: info = {8, 4, 8}; return true;    // I8
    case 2: info = {8, 4, 8}; return true;    // IA4
    case 3: info = {4, 4, 16}; return true;   // IA8
    case 4: info = {4, 4, 16}; return true;   // RGB565
    case 5: info = {4, 4, 16}; return true;   // RGB5A3
    case 6: info = {4, 4, 32}; return true;   // RGBA8
    case 8: info = {8, 8, 4}; return true;    // C4
    case 9: info = {8, 4, 8}; return true;    // C8
    case 10: info = {4, 4, 16}; return true;  // C14X2
    case 14: info = {8, 8, 4}; return true;   // CMPR
    default: return false;
    }
}

uint64_t EncodedSize(uint32_t format, uint32_t width, uint32_t height) {
    FormatInfo info{};
    if (!Info(format, info)) {
        return 0;
    }
    const uint64_t w = (uint64_t(width) + info.bw - 1) / info.bw * info.bw;
    const uint64_t h = (uint64_t(height) + info.bh - 1) / info.bh * info.bh;
    return w * h * info.bits / 8;
}

void PaletteColor(const uint8_t* palette, uint32_t paletteFormat, uint32_t index, uint8_t* o) {
    const uint16_t c = BE16(palette + index * 2);
    if (paletteFormat == 0) Ia8(c, o);
    else if (paletteFormat == 1) Rgb565(c, o);
    else Rgb5a3(c, o);
}

bool DecodeTexture(const uint8_t* src, size_t size, uint32_t format, uint32_t width, uint32_t height,
                   std::vector<uint8_t>& rgba, const uint8_t* palette = nullptr, uint32_t paletteFormat = 0) {
    FormatInfo info{};
    if (!Info(format, info) || EncodedSize(format, width, height) > size || width == 0 || height == 0) {
        return false;
    }
    if ((format == 8 || format == 9 || format == 10) && palette == nullptr) {
        return false;
    }
    const uint64_t pixelsSize = uint64_t(width) * height * 4;
    if (pixelsSize > rgba.max_size()) return false;
    rgba.assign(static_cast<size_t>(pixelsSize), 0);
    const uint32_t blocksX = (width + info.bw - 1) / info.bw;
    const uint32_t blocksY = (height + info.bh - 1) / info.bh;
    const size_t blockBytes = info.bw * info.bh * info.bits / 8;
    const uint8_t* block = src;
    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx, block += blockBytes) {
            if (format == 14) {
                // CMPR: 2x2 DXT1 sub-blocks with big-endian colours.
                for (uint32_t sub = 0; sub < 4; ++sub) {
                    const uint8_t* s = block + sub * 8;
                    const uint16_t c0 = BE16(s), c1 = BE16(s + 2);
                    uint8_t pal[4][4];
                    Rgb565(c0, pal[0]);
                    Rgb565(c1, pal[1]);
                    if (c0 > c1) {
                        for (int k = 0; k < 3; ++k) {
                            pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
                            pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
                        }
                        pal[2][3] = pal[3][3] = 255;
                    } else {
                        for (int k = 0; k < 3; ++k) {
                            pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
                            pal[3][k] = 0;
                        }
                        pal[2][3] = 255;
                        pal[3][3] = 0;
                    }
                    for (uint32_t y = 0; y < 4; ++y) {
                        const uint8_t row = s[4 + y];
                        for (uint32_t x = 0; x < 4; ++x) {
                            const uint32_t px = bx * 8 + (sub & 1) * 4 + x;
                            const uint32_t py = by * 8 + (sub >> 1) * 4 + y;
                            if (px < width && py < height) {
                                std::memcpy(&rgba[(static_cast<size_t>(py) * width + px) * 4],
                                            pal[(row >> (6 - x * 2)) & 3], 4);
                            }
                        }
                    }
                }
                continue;
            }
            for (uint32_t y = 0; y < info.bh; ++y) {
                for (uint32_t x = 0; x < info.bw; ++x) {
                    const uint32_t px = bx * info.bw + x;
                    const uint32_t py = by * info.bh + y;
                    uint8_t o[4] = {0, 0, 0, 0};
                    const uint32_t i = y * info.bw + x;
                    switch (format) {
                    case 0: {
                        const uint8_t b = block[i / 2];
                        o[0] = o[1] = o[2] = o[3] = Exp4((i & 1) ? (b & 15) : (b >> 4));
                        break;
                    }
                    case 1: o[0] = o[1] = o[2] = o[3] = block[i]; break;
                    case 2:
                        o[0] = o[1] = o[2] = Exp4(block[i] & 15);
                        o[3] = Exp4(block[i] >> 4);
                        break;
                    case 3: Ia8(BE16(block + i * 2), o); break;
                    case 4: Rgb565(BE16(block + i * 2), o); break;
                    case 5: Rgb5a3(BE16(block + i * 2), o); break;
                    case 6:
                        o[3] = block[i * 2];
                        o[0] = block[i * 2 + 1];
                        o[1] = block[32 + i * 2];
                        o[2] = block[32 + i * 2 + 1];
                        break;
                    case 8: {
                        const uint8_t b = block[i / 2];
                        PaletteColor(palette, paletteFormat, (i & 1) ? (b & 15) : (b >> 4), o);
                        break;
                    }
                    case 9: PaletteColor(palette, paletteFormat, block[i], o); break;
                    case 10: PaletteColor(palette, paletteFormat, BE16(block + i * 2) & 0x3FFF, o); break;
                    default: break;
                    }
                    if (px < width && py < height) {
                        std::memcpy(&rgba[(static_cast<size_t>(py) * width + px) * 4], o, 4);
                    }
                }
            }
        }
    }
    return true;
}

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

uint64_t Upload(const Image& image) {
    return static_cast<uint64_t>(aurora_imgui_add_texture(image.width, image.height, image.rgba.data()));
}

// ---------------------------------------------------------------- colours
struct Rgba {
    float r = 1, g = 1, b = 1, a = 1;
};
Rgba FromRgba8(uint32_t c) {
    return {((c >> 24) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f};
}
ImU32 ToImU32(const Rgba& c) {
    const auto q = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(q(c.r), q(c.g), q(c.b), q(c.a));
}

// ---------------------------------------------------------------- caches
std::mutex g_cacheMutex;
std::unordered_map<std::string, std::shared_ptr<Font>> g_fonts;

} // namespace

// ---------------------------------------------------------------- fonts
std::shared_ptr<Font> LoadFont(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        if (const auto it = g_fonts.find(name); it != g_fonts.end()) {
            return it->second;
        }
    }
    auto font = std::make_shared<Font>();
    std::vector<uint8_t> data;
    font->ok = [&] {
        if (!ReadDiscFile(name, data) || data.size() < 16 || BE32(data.data()) != 0x52464E54u) return false;
        // Links point to block bodies, after their eight-byte tag/size headers.
        const auto blockSize = [&](uint32_t body, uint32_t tag, size_t minimum) -> size_t {
            if (body < 8 || body > data.size() || minimum > data.size() - body) return 0;
            const uint32_t size = BE32(data.data() + body - 4);
            if (BE32(data.data() + body - 8) != tag || size < 8 + minimum ||
                size > data.size() - (body - 8)) return 0;
            return size - 8;
        };
        const uint32_t finf = BE16(data.data() + 12) + 8u;
        if (!blockSize(finf, 0x46494E46u, 24)) return false;
        const uint8_t* f = data.data() + finf;
        font->linefeed = static_cast<int8_t>(f[1]);
        font->defaultIndex = BE16(f + 2);
        const uint32_t tglp = BE32(f + 8);
        uint32_t cwdh = BE32(f + 12), cmap = BE32(f + 16);
        font->height = f[20];
        font->width = f[21];
        font->ascent = f[22];
        const size_t textureSize = blockSize(tglp, 0x54474C50u, 24);
        if (!textureSize) return false;
        const uint8_t* t = data.data() + tglp;
        font->cellWidth = t[0];
        font->cellHeight = t[1];
        const uint32_t sheetSize = BE32(t + 4);
        const uint16_t sheetCount = BE16(t + 8), sheetFormat = BE16(t + 10);
        const uint16_t perRow = BE16(t + 12), perColumn = BE16(t + 14);
        const uint16_t sheetWidth = BE16(t + 16), sheetHeight = BE16(t + 18);
        const uint32_t sheetData = BE32(t + 20);
        if (!sheetSize || !sheetCount || !perRow || !perColumn || !sheetWidth || !sheetHeight ||
            sheetData < tglp || sheetData - tglp > textureSize ||
            sheetCount > (textureSize - (sheetData - tglp)) / sheetSize) return false;
        std::vector<Image> sheets;
        for (uint16_t i = 0; i < sheetCount; ++i) {
            const size_t at = sheetData + static_cast<size_t>(i) * sheetSize;
            Image sheet;
            sheet.width = sheetWidth;
            sheet.height = sheetHeight;
            if (!DecodeTexture(data.data() + at, sheetSize, sheetFormat, sheetWidth, sheetHeight, sheet.rgba)) return false;
            sheets.push_back(std::move(sheet));
        }
        const uint32_t perSheet = static_cast<uint32_t>(perRow) * perColumn;
        std::unordered_set<uint32_t> visited;
        while (cwdh != 0) {
            const size_t size = blockSize(cwdh, 0x43574448u, 8);
            if (!size || !visited.insert(cwdh).second) return false;
            const uint8_t* w = data.data() + cwdh;
            const uint16_t first = BE16(w), last = BE16(w + 2);
            if (last < first || (uint32_t(last) - first + 1) > (size - 8) / 3) return false;
            if (last >= font->glyphs.size()) font->glyphs.resize(uint32_t(last) + 1);
            for (uint32_t index = first; index <= last; ++index) {
                Font::Glyph& g = font->glyphs[index];
                const uint8_t* e = w + 8 + (index - first) * 3;
                g.left = static_cast<int8_t>(e[0]);
                g.glyphWidth = e[1];
                g.charWidth = static_cast<int8_t>(e[2]);
            }
            cwdh = BE32(w + 4);
        }
        for (uint32_t index = 0; index < font->glyphs.size(); ++index) {
            Font::Glyph& g = font->glyphs[index];
            g.sheet = static_cast<uint16_t>(index / perSheet);
            const uint32_t cell = index % perSheet;
            const float x = static_cast<float>((cell % perRow) * (font->cellWidth + 1));
            const float y = static_cast<float>((cell / perRow) * (font->cellHeight + 1));
            g.u0 = x / sheetWidth;
            g.v0 = y / sheetHeight;
            g.u1 = (x + g.glyphWidth) / sheetWidth;
            g.v1 = (y + font->cellHeight) / sheetHeight;
        }
        visited.clear();
        while (cmap != 0) {
            const size_t size = blockSize(cmap, 0x434D4150u, 14);
            if (!size || !visited.insert(cmap).second) return false;
            const uint8_t* m = data.data() + cmap;
            const uint16_t begin = BE16(m), end = BE16(m + 2), method = BE16(m + 4);
            if (end < begin) return false;
            const uint8_t* body = m + 12;
            if (method == 0) {
                const uint16_t offset = BE16(body);
                for (uint32_t code = begin; code <= end; ++code)
                    font->map[static_cast<uint16_t>(code)] = static_cast<uint16_t>(code - begin + offset);
            } else if (method == 1) {
                if ((uint32_t(end) - begin + 1) > (size - 12) / 2) return false;
                for (uint32_t code = begin; code <= end; ++code) {
                    const uint16_t index = BE16(body + (code - begin) * 2);
                    if (index != 0xFFFF) font->map[static_cast<uint16_t>(code)] = index;
                }
            } else if (method == 2) {
                const uint16_t count = BE16(body);
                if (count > (size - 14) / 4) return false;
                for (uint16_t i = 0; i < count; ++i)
                    font->map[BE16(body + 2 + i * 4)] = BE16(body + 4 + i * 4);
            } else {
                return false;
            }
            cmap = BE32(m + 8);
        }
        if (font->glyphs.empty() || font->map.empty()) return false;
        for (const Image& sheet : sheets) font->sheets.push_back(Upload(sheet));
        return true;
    }();
    if (!font->ok) {
        RT_LOGF(RT_TAG_RUNTIME, "netplay ui: could not load the game font %s\n", name.c_str());
    }
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_fonts[name] = font;
    return font;
}

namespace {

struct TextLine {
    std::u16string text;
    float width = 0;
};

std::vector<TextLine> SplitLines(const Font& font, const std::u16string& text, float scaleX, float charSpace) {
    std::vector<TextLine> lines(1);
    for (char16_t c : text) {
        if (c == u'\n') {
            lines.emplace_back();
            continue;
        }
        const Font::Glyph* g = font.Find(c);
        if (g == nullptr) {
            continue;
        }
        if (!lines.back().text.empty()) {
            lines.back().width += charSpace;
        }
        lines.back().text.push_back(c);
        lines.back().width += g->charWidth * scaleX;
    }
    return lines;
}

void PushQuad(ImDrawList* list, uint64_t texture, const ImVec2 p[4], const ImVec2 uv[4], const ImU32 col[4]) {
    list->PushTextureID(static_cast<ImTextureID>(texture));
    list->PrimReserve(6, 4);
    const ImDrawIdx base = static_cast<ImDrawIdx>(list->_VtxCurrentIdx);
    list->PrimWriteIdx(base);
    list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
    list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    list->PrimWriteIdx(base);
    list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
    for (int i = 0; i < 4; ++i) {
        list->PrimWriteVtx(p[i], uv[i], col[i]);
    }
    list->PopTextureID();
}

// Layout-space affine transform: x' = a*x + b*y + tx, y' = c*x + d*y + ty.
struct Mtx {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};
ImVec2 ToScreen(const View& view, const Mtx& m, float x, float y) {
    const float lx = m.a * x + m.b * y + m.tx;
    const float ly = m.c * x + m.d * y + m.ty;
    return ImVec2(view.centerX + lx * view.scale, view.centerY - ly * view.scale);
}

void DrawTextLines(ImDrawList* list, const View& view, const Mtx& m, const Font& font, const std::u16string& text,
                   float rectX0, float rectY0, float rectW, float rectH, int textPosition, int alignment, float fontW,
                   float fontH, float charSpace, float lineSpace, const Rgba& top, const Rgba& bottom) {
    if (!font.ok || font.width == 0 || font.height == 0) {
        return;
    }
    const float scaleX = fontW / font.width;
    const float scaleY = fontH / font.height;
    const float lineHeight = font.linefeed * scaleY + lineSpace;
    const auto lines = SplitLines(font, text, scaleX, charSpace);
    float blockWidth = 0;
    for (const auto& line : lines) {
        blockWidth = std::max(blockWidth, line.width);
    }
    const float blockHeight = lineHeight * (lines.size() - 1) + font.cellHeight * scaleY;
    const int posH = textPosition % 3;
    const int posV = textPosition / 3;
    const float blockX = rectX0 + (posH == 0 ? 0.0f : posH == 1 ? (rectW - blockWidth) / 2 : rectW - blockWidth);
    float y = rectY0 - (posV == 0 ? 0.0f : posV == 1 ? (rectH - blockHeight) / 2 : rectH - blockHeight);
    const ImU32 topCol = ToImU32(top), bottomCol = ToImU32(bottom);
    for (const auto& line : lines) {
        const int align = alignment == 0 ? posH : alignment - 1;
        float x = blockX + (align == 0 ? 0.0f : align == 1 ? (blockWidth - line.width) / 2 : blockWidth - line.width);
        for (char16_t c : line.text) {
            const Font::Glyph* g = font.Find(c);
            if (g == nullptr || g->sheet >= font.sheets.size()) {
                continue;
            }
            const float gx = x + g->left * scaleX;
            const float gw = g->glyphWidth * scaleX;
            const float gh = font.cellHeight * scaleY;
            const ImVec2 p[4] = {ToScreen(view, m, gx, y), ToScreen(view, m, gx + gw, y), ToScreen(view, m, gx + gw, y - gh),
                                 ToScreen(view, m, gx, y - gh)};
            const ImVec2 uv[4] = {ImVec2(g->u0, g->v0), ImVec2(g->u1, g->v0), ImVec2(g->u1, g->v1), ImVec2(g->u0, g->v1)};
            const ImU32 col[4] = {topCol, topCol, bottomCol, bottomCol};
            PushQuad(list, font.sheets[g->sheet], p, uv, col);
            x += g->charWidth * scaleX + charSpace;
        }
        y -= lineHeight;
    }
}

} // namespace

std::vector<std::u16string> WrapText(const Font& font, const std::u16string& text, float fontSize, float maxWidth) {
    std::vector<std::u16string> lines;
    std::u16string line;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(u' ', pos);
        if (end == std::u16string::npos) {
            end = text.size();
        }
        const std::u16string word = text.substr(pos, end - pos);
        const std::u16string candidate = line.empty() ? word : line + u' ' + word;
        if (!line.empty() && MeasureText(font, candidate, fontSize) > maxWidth) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        pos = end + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

float MeasureText(const Font& font, const std::u16string& text, float fontSize) {
    if (!font.ok || font.width == 0) {
        return 0;
    }
    float widest = 0;
    for (const auto& line : SplitLines(font, text, fontSize / font.width, 0)) {
        widest = std::max(widest, line.width);
    }
    return widest;
}

float LineHeight(const Font& font, float fontSize) {
    return font.height == 0 ? 0 : font.linefeed * fontSize / font.height;
}

void DrawText(ImDrawList* list, const View& view, const Font& font, const std::u16string& text, float x, float y,
              float fontSize, uint32_t topColor, uint32_t bottomColor, float alpha, int originH, int originV) {
    Rgba top = FromRgba8(topColor), bottom = FromRgba8(bottomColor);
    top.a *= alpha;
    bottom.a *= alpha;
    const float w = MeasureText(font, text, fontSize);
    const float h = font.height == 0 ? 0 : font.cellHeight * fontSize / font.height;
    const float x0 = x - (originH == 0 ? 0 : originH == 1 ? w / 2 : w);
    const float y0 = y + (originV == 0 ? 0 : originV == 1 ? h / 2 : h);
    DrawTextLines(list, view, Mtx{}, font, text, x0, y0, w, h, 0, 0, fontSize, fontSize, 0, 0, top, bottom);
}

// ---------------------------------------------------------------- layouts
struct Layout::Pane {
    std::string name;
    float tx = 0, ty = 0, sx = 1, sy = 1, w = 0, h = 0;
};

Layout::Layout() = default;
Layout::~Layout() = default;

bool FontReady(const Font* font) {
    return font != nullptr && font->ok;
}

bool Layout::Load(const std::string& arcPath, const std::string& brlytName) {
    m_loaded = false;
    m_byName.clear();
    m_panes.clear();
    std::vector<uint8_t> arc;
    if (!ReadDiscFile(arcPath, arc)) {
        return false;
    }
    const auto files = ListU8(arc);
    const uint8_t* lyt = nullptr;
    size_t lytSize = 0;
    for (const auto& [path, where] : files) {
        if (path.size() > 6 && path.compare(path.size() - 6, 6, ".brlyt") == 0 &&
            (brlytName.empty() || path.find(brlytName) != std::string::npos)) {
            lyt = arc.data() + where.first;
            lytSize = where.second;
            break;
        }
    }
    if (lyt == nullptr || lytSize < 16 || BE32(lyt) != 0x524C5954u) {  // RLYT
        return false;
    }
    std::vector<Pane*> stack;
    Pane* last = nullptr;
    const uint16_t headerSize = BE16(lyt + 12);
    const uint16_t sections = BE16(lyt + 14);
    if (headerSize < 16 || headerSize > lytSize) return false;
    size_t pos = headerSize;
    for (uint16_t s = 0; s < sections; ++s) {
        if (lytSize - pos < 8) return false;
        const uint8_t* sec = lyt + pos;
        const uint32_t kind = BE32(sec);
        const uint32_t secSize = BE32(sec + 4);
        if (secSize < 8 || secSize > lytSize - pos) {
            return false;
        }
        const uint8_t* body = sec + 8;
        switch (kind) {
        case 0x70616E31u:    // pan1
        case 0x70696331u:    // pic1
        case 0x74787431u:    // txt1
        case 0x776E6431u:    // wnd1
        case 0x626E6431u: {  // bnd1
            if (secSize < 8 + 68) {
                return false;
            }
            auto pane = std::make_unique<Pane>();
            pane->name.assign(reinterpret_cast<const char*>(body + 4), strnlen(reinterpret_cast<const char*>(body + 4), 16));
            pane->tx = BEF(body + 28);
            pane->ty = BEF(body + 32);
            pane->sx = BEF(body + 52);
            pane->sy = BEF(body + 56);
            pane->w = BEF(body + 60);
            pane->h = BEF(body + 64);
            last = pane.get();
            if (!stack.empty()) {
                // Accumulate translation and scale once; diagnostic rectangles ignore rotation.
                const Pane& parent = *stack.back();
                pane->tx = parent.tx + pane->tx * parent.sx;
                pane->ty = parent.ty + pane->ty * parent.sy;
                pane->sx *= parent.sx;
                pane->sy *= parent.sy;
            }
            if (!pane->name.empty()) {
                m_byName.emplace(pane->name, pane.get());
            }
            m_panes.push_back(std::move(pane));
            break;
        }
        case 0x70617331u:  // pas1
            if (last != nullptr) {
                stack.push_back(last);
            }
            break;
        case 0x70616531u:  // pae1
            if (!stack.empty()) {
                stack.pop_back();
            }
            break;
        default:
            break;
        }
        pos += secSize;
    }
    m_loaded = !m_panes.empty();
    if (!m_loaded) {
        RT_LOGF(RT_TAG_RUNTIME, "netplay ui: no panes in %s\n", arcPath.c_str());
    }
    return m_loaded;
}

bool Layout::PaneWorldRect(const std::string& paneName, float& x, float& y, float& w, float& h) const {
    const auto it = m_byName.find(paneName);
    if (!m_loaded || it == m_byName.end()) {
        return false;
    }
    const Pane& pane = *it->second;
    x = pane.tx;
    y = pane.ty;
    w = pane.w * pane.sx;
    h = pane.h * pane.sy;
    return true;
}

std::vector<std::string> Layout::PaneNames() const {
    if (!m_loaded) return {};
    std::vector<std::string> names;
    for (const auto& pane : m_panes) {
        names.push_back(pane->name);
    }
    return names;
}

View GameView() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    View view;
    float w = display.x, h = display.y;
    const auto settings = DisplaySettings::Current();
    const bool wide = DisplaySettings::GameWidescreen();
    float aspect = 0.0f;  // 0 = fills the window
    if (!wide || settings.aspect == DisplaySettings::kAspect4x3) {
        aspect = 4.0f / 3.0f;
    } else if (settings.aspect == DisplaySettings::kAspectCinematic) {
        aspect = DisplaySettings::kRatioCinematic;
    } else if (settings.aspect == DisplaySettings::kAspect16x9 || w / std::max(h, 1.0f) <= 16.0f / 9.0f) {
        aspect = 16.0f / 9.0f;
    }
    if (aspect > 0.0f) {
        if (w / std::max(h, 1.0f) > aspect) {
            w = h * aspect;
        } else {
            h = w / aspect;
        }
    }
    view.centerX = display.x / 2;
    view.centerY = display.y / 2;
    view.scale = h / 456.0f;
    return view;
}

std::u16string Utf16(const std::string& utf8) {
    std::u16string out;
    for (size_t i = 0; i < utf8.size();) {
        const uint8_t c = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 6) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c >> 4) == 14) {
            cp = c & 0x0F;
            extra = 2;
        } else {
            cp = c & 0x07;
            extra = 3;
        }
        ++i;
        for (int k = 0; k < extra && i < utf8.size(); ++k, ++i) {
            cp = (cp << 6) | (static_cast<uint8_t>(utf8[i]) & 0x3F);
        }
        out.push_back(cp < 0x10000 ? static_cast<char16_t>(cp) : u'?');
    }
    return out;
}

} // namespace GameLayout
