#include "PluginInfoActivity.h"

#if CROSSPOINT_SD_PLUGINS

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

namespace {
// A README is read whole into one std::string (then split into lines), so cap
// it well below anything that would matter on the PSRAM boards that build
// plugins; a larger file is skipped rather than truncated mid-sentence.
constexpr size_t MAX_README_SIZE = 24 * 1024;
constexpr int LINE_GAP = 4;  // air between wrapped lines, as the error screens use

// A very light markdown flattener for e-ink: drops heading/quote/list/emphasis
// markers and code fences, leaving readable plain text. No renderer needed.
std::string stripMarkdown(const std::string& line) {
  std::string s = line;
  size_t start = 0;
  while (start < s.size() && (s[start] == '#' || s[start] == '>' || s[start] == ' ')) start++;
  s = s.substr(start);
  if (s.rfind("- ", 0) == 0 || s.rfind("* ", 0) == 0) s = "\xE2\x80\xA2 " + s.substr(2);  // bullet
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c == '*' || c == '_' || c == '`') continue;  // inline emphasis / code
    out += c;
  }
  return out;
}
}  // namespace

void PluginInfoActivity::onEnter() {
  Activity::onEnter();
  topLine = 0;
  wrappedWidth = -1;
  loadParagraphs();
  requestUpdate();
}

// Read raw text into `paragraphs` without any measurement — safe to run in
// onEnter (fonts/orientation may not be settled until the render task runs).
void PluginInfoActivity::loadParagraphs() {
  paragraphs.clear();
  if (!plugin.description.empty()) {
    paragraphs.push_back(plugin.description);
    paragraphs.emplace_back("");
  }
  if (plugin.readmePath.empty()) {
    // Nothing else to show: say how a browser plugin is used.
    if (plugin.manifestPath.empty()) paragraphs.emplace_back(tr(STR_PLUGIN_WEB_ONLY_HINT));
    return;
  }
  FsFile file;
  if (!Storage.openFileForRead("PINFO", plugin.readmePath, file)) return;
  const size_t size = file.fileSize();
  if (size == 0 || size > MAX_README_SIZE) {
    LOG_INF("PINFO", "README skipped: %u bytes (cap %u)", (unsigned)size, (unsigned)MAX_README_SIZE);
    file.close();
    return;
  }
  std::string raw;
  raw.resize(size);
  const int got = file.read(raw.data(), size);
  file.close();
  if (got != static_cast<int>(size)) {
    LOG_ERR("PINFO", "README short read: %d of %u", got, (unsigned)size);
    return;
  }

  paragraphs.reserve(paragraphs.size() + raw.size() / 40);  // rough average line length
  size_t pos = 0;
  while (pos <= raw.size()) {
    const size_t nl = raw.find('\n', pos);
    std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("```", 0) != 0) paragraphs.push_back(std::move(line));  // drop code-fence markers
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
}

// Word-wrap `paragraphs` into `lines` at the current content width, measured
// with the UI font the screen draws with. Runs on the render task, where the
// width and font metrics are final.
void PluginInfoActivity::ensureWrapped() {
  const int sidePadding = UITheme::getInstance().getMetrics().contentSidePadding;
  const int wrapWidth = renderer.getScreenWidth() - sidePadding * 2;
  if (wrappedWidth == wrapWidth) return;  // already wrapped for this width
  wrappedWidth = wrapWidth;
  lines.clear();
  lines.reserve(paragraphs.size() * 2);  // rough average wrapped lines per paragraph
  for (const std::string& raw : paragraphs) {
    const std::string text = stripMarkdown(raw);
    if (text.empty()) {
      lines.emplace_back("");
      continue;
    }
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
      const size_t sp = text.find(' ', i);
      std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
      // Hard-break a single word wider than the line (e.g. a long URL).
      while (renderer.getTextWidth(UI_10_FONT_ID, word.c_str()) > wrapWidth && word.size() > 1) {
        size_t cut = word.size();
        while (cut > 1 && renderer.getTextWidth(UI_10_FONT_ID, word.substr(0, cut).c_str()) > wrapWidth) cut--;
        if (!cur.empty()) {
          lines.push_back(cur);
          cur.clear();
        }
        lines.push_back(word.substr(0, cut));
        word = word.substr(cut);
      }
      const std::string trial = cur.empty() ? word : cur + " " + word;
      if (renderer.getTextWidth(UI_10_FONT_ID, trial.c_str()) > wrapWidth && !cur.empty()) {
        lines.push_back(cur);
        cur = word;
      } else {
        cur = trial;
      }
      if (sp == std::string::npos) break;
      i = sp + 1;
    }
    lines.push_back(cur);
  }
}

void PluginInfoActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int rows = visibleRows;
  const int maxTop = std::max(0, static_cast<int>(lines.size()) - rows);
  const auto scroll = [&](int delta) {
    const int next = std::min(std::max(0, topLine + delta), maxTop);
    if (next != topLine) {
      topLine = next;
      requestUpdate();
    }
  };
  // Full Touch: a vertical swipe pages the text, content-drag sense.
  if (const int delta = TouchListNav::pageSwipeDelta(mappedInput)) {
    scroll(delta * rows);
    return;
  }
  buttonNavigator.onNextRelease([&] { scroll(rows); });
  buttonNavigator.onPreviousRelease([&] { scroll(-rows); });
}

void PluginInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();
  ensureWrapped();  // wrap now that width + fonts are final

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, plugin.title.c_str());

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + LINE_GAP;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  visibleRows = std::max(1, (bottom - top) / lineH);
  int y = top;
  for (int i = 0; i < visibleRows && topLine + i < static_cast<int>(lines.size()); i++) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, lines[topLine + i].c_str());
    y += lineH;
  }

  const bool canScroll = static_cast<int>(lines.size()) > visibleRows;
  const char* up = canScroll ? tr(STR_DIR_UP) : "";
  const char* down = canScroll ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

#endif  // CROSSPOINT_SD_PLUGINS
