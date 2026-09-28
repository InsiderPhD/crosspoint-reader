#include "VoidTagCloser.h"

#include <cctype>
#include <cstring>

namespace {

bool isNameStart(const uint8_t c) { return std::isalpha(c) || c == '_' || c == ':'; }

bool isNameChar(const uint8_t c) { return std::isalnum(c) || c == '-' || c == '_' || c == ':' || c == '.'; }

}  // namespace

bool VoidTagCloser::emit(const uint8_t* data, const size_t len) {
  if (len > 0 && out.write(data, len) != len) ok = false;
  return ok;
}

bool VoidTagCloser::isVoidName() const {
  // HTML void elements: legal without a closing tag in HTML, but must be
  // self-closed to be well-formed XML.
  static constexpr const char* VOID_ELEMENTS[] = {"area",  "base", "br",   "col",   "embed",  "hr",    "img",
                                                  "input", "link", "meta", "param", "source", "track", "wbr"};
  if (nameLen > MAX_NAME) return false;
  for (const char* v : VOID_ELEMENTS) {
    if (strlen(v) == nameLen && strncmp(v, name, nameLen) == 0) return true;
  }
  return false;
}

size_t VoidTagCloser::write(const uint8_t* buf, const size_t size) {
  // Bytes buf[runStart, i) pass through unchanged and are written in one call
  // whenever the filter needs to insert, hold or drop something.
  size_t runStart = 0;
  const auto flushRun = [&](const size_t end) {
    if (end > runStart) emit(buf + runStart, end - runStart);
    runStart = end;
  };
  // Held bytes always precede buf[runStart], so releasing them before the run
  // continues keeps the output in order.
  const auto releaseHeld = [&]() {
    if (heldLen > 0) emit(held, heldLen);
    heldLen = 0;
  };

  for (size_t i = 0; i < size; i++) {
    const uint8_t c = buf[i];
    switch (state) {
      case State::Text:
        if (c == '<') state = State::TagOpen;
        break;

      case State::TagOpen:
        closing = false;
        nameLen = 0;
        if (c == '/') {
          // Hold "</" until the name says whether this closer is dropped. The
          // '<' is either the previous byte of this run or already held (it
          // ended the previous chunk).
          if (heldLen == 0) {
            flushRun(i - 1);
            held[heldLen++] = '<';
          }
          held[heldLen++] = '/';
          runStart = i + 1;
          closing = true;
          state = State::TagName;
          break;
        }
        releaseHeld();
        if (isNameStart(c)) {
          name[nameLen++] = static_cast<char>(std::tolower(c));
          state = State::TagName;
        } else if (c == '!') {
          state = State::Bang;
        } else if (c == '?') {
          markupRun = 0;
          state = State::Pi;
        } else {
          state = c == '<' ? State::TagOpen : State::Text;  // a bare '<' in text
        }
        break;

      case State::TagName:
        if (isNameChar(c)) {
          if (nameLen < MAX_NAME) {
            name[nameLen++] = static_cast<char>(std::tolower(c));
          } else {
            nameLen = MAX_NAME + 1;  // too long for any void element
          }
          if (heldLen > 0) {
            if (nameLen <= MAX_NAME && heldLen < MAX_HELD) {
              held[heldLen++] = c;
              runStart = i + 1;
            } else {
              releaseHeld();  // cannot be a void closer; c joins the run
            }
          }
          break;
        }
        if (closing && isVoidName()) {
          // "</br>", "</img>": no XML equivalent once the opener is
          // self-closed. Discard the held prefix and everything up to '>'.
          heldLen = 0;
          runStart = i + 1;
          state = c == '>' ? State::Text : State::DropCloser;
          break;
        }
        releaseHeld();
        prevSignificant = 0;
        state = State::TagBody;
        [[fallthrough]];

      case State::TagBody:
        if (c == '"' || c == '\'') {
          quote = c;
          state = State::TagQuoted;
        } else if (c == '>') {
          if (!closing && prevSignificant != '/' && isVoidName()) {
            flushRun(i);
            static constexpr uint8_t SLASH = '/';
            emit(&SLASH, 1);
          }
          state = State::Text;
        } else if (!std::isspace(c)) {
          prevSignificant = c;
        }
        break;

      case State::TagQuoted:
        if (c == quote) {
          prevSignificant = c;
          state = State::TagBody;
        }
        break;

      case State::DropCloser:
        runStart = i + 1;
        if (c == '>') state = State::Text;
        break;

      case State::Bang:
        markupRun = 0;
        if (c == '-') {
          state = State::BangDash;
        } else if (c == '[') {
          state = State::CData;
        } else {
          state = c == '>' ? State::Text : State::Decl;
        }
        break;

      case State::BangDash:
        if (c == '-') {
          markupRun = 0;
          state = State::Comment;
        } else {
          state = c == '>' ? State::Text : State::Decl;
        }
        break;

      case State::Comment:
      case State::CData: {
        const uint8_t closer = state == State::Comment ? '-' : ']';
        if (c == '>' && markupRun >= 2) {
          state = State::Text;
        } else if (c == closer) {
          if (markupRun < 2) markupRun++;
        } else {
          markupRun = 0;
        }
        break;
      }

      case State::Decl:
        if (c == '>') state = State::Text;
        break;

      case State::Pi:
        if (c == '>' && markupRun) state = State::Text;
        markupRun = c == '?';
        break;
    }
  }

  // A chunk that ends on '<' holds it back: if the next chunk opens with '/',
  // the whole "</" may yet be dropped.
  if (state == State::TagOpen && heldLen == 0 && size > runStart) {
    flushRun(size - 1);
    held[heldLen++] = '<';
    runStart = size;
  }
  flushRun(size);
  return ok ? size : 0;
}

bool VoidTagCloser::finish() {
  if (heldLen > 0) emit(held, heldLen);
  heldLen = 0;
  return ok;
}
