#include "ProgressMapper.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ChapterXPathResolver.h"
#include "Epub/Section.h"
#include "Epub/SpineSizeLookup.h"
#include "Epub/VisibleTextUtils.h"
#include "Epub/htmlEntities.h"
#include "Utf8.h"

namespace {
int parseIndex(const std::string& xpath, const char* prefix, bool last = false) {
  const size_t prefixLen = strlen(prefix);
  const size_t pos = last ? xpath.rfind(prefix) : xpath.find(prefix);
  if (pos == std::string::npos) return -1;
  const size_t numStart = pos + prefixLen;
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return -1;
  int val = 0;
  for (size_t i = numStart; i < numEnd; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return -1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val;
}

int parseCharOffset(const std::string& xpath) {
  const size_t textPos = xpath.rfind("text()");
  const size_t dotPos = (textPos != std::string::npos) ? xpath.find('.', textPos) : xpath.rfind('.');
  if (dotPos == std::string::npos || dotPos + 1 >= xpath.size()) return 0;
  int val = 0;
  for (size_t i = dotPos + 1; i < xpath.size(); i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return 0;
    val = val * 10 + (xpath[i] - '0');
  }
  return val;
}

// Parse the N from text()[N] in the XPath (1-based; defaults to 1 if absent or 1).
int parseTextNodeIndex(const std::string& xpath) {
  const size_t textPos = xpath.rfind("text()[");
  if (textPos == std::string::npos) return 1;
  const size_t numStart = textPos + 7;  // strlen("text()[")
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return 1;
  int val = 0;
  for (size_t i = numStart; i < numEnd; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return 1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val > 0 ? val : 1;
}

bool isChapterStartXPath(const std::string& xpath) {
  if (xpath.find("/p[") != std::string::npos || xpath.find("/li[") != std::string::npos) {
    return false;
  }

  static constexpr char kDocFragment[] = "/body/DocFragment[";
  const size_t docFragPos = xpath.find(kDocFragment);
  if (docFragPos == std::string::npos) {
    return false;
  }
  const size_t docFragEnd = xpath.find(']', docFragPos + strlen(kDocFragment));
  if (docFragEnd == std::string::npos) {
    return false;
  }
  if (docFragEnd + 1 == xpath.size()) {
    return true;
  }
  if (xpath[docFragEnd + 1] == '.') {
    if (docFragEnd + 2 >= xpath.size()) {
      return false;
    }
    for (size_t i = docFragEnd + 2; i < xpath.size(); i++) {
      if (xpath[i] != '0') return false;
    }
    return true;
  }

  static constexpr char kDocBody[] = "]/body";
  const size_t docBodyPos = xpath.find(kDocBody);
  if (docBodyPos == std::string::npos) {
    return false;
  }
  size_t bodyContentStart = docBodyPos + strlen(kDocBody);
  if (bodyContentStart == xpath.size()) {
    return true;
  }
  if (xpath[bodyContentStart] != '/') {
    return false;
  }
  bodyContentStart++;
  if (bodyContentStart == xpath.size()) {
    return true;
  }

  const size_t dotPos = xpath.rfind('.');
  if (dotPos == std::string::npos || dotPos <= bodyContentStart || dotPos + 1 >= xpath.size()) {
    return false;
  }
  size_t terminalEnd = dotPos;
  static constexpr char kTextNode[] = "/text()";
  const size_t textNodePos = xpath.rfind(kTextNode, dotPos);
  if (textNodePos != std::string::npos && textNodePos >= bodyContentStart) {
    terminalEnd = textNodePos;
  }
  if (xpath.find('/', bodyContentStart) < terminalEnd) {
    return false;
  }

  for (size_t i = dotPos + 1; i < xpath.size(); i++) {
    if (xpath[i] != '0') return false;
  }
  return parseTextNodeIndex(xpath) <= 1;
}

bool isBodyTextXPath(const std::string& xpath) {
  static constexpr char kBodyFrag[] = "/body/DocFragment[";
  const size_t fragPos = xpath.find(kBodyFrag);
  if (fragPos == std::string::npos) return false;
  const size_t afterBracket = xpath.find(']', fragPos + strlen(kBodyFrag));
  if (afterBracket == std::string::npos) return false;
  static constexpr char kBody[] = "/body/";
  if (xpath.compare(afterBracket + 1, strlen(kBody), kBody) != 0) return false;
  const size_t contentPos = afterBracket + 1 + strlen(kBody);
  return xpath.compare(contentPos, strlen("text()"), "text()") == 0;
}

// Parsed representation of one step in the XPath ancestry.
struct XPathStep {
  char tag[12];      // element name, null-terminated
  int siblingIndex;  // 1-based sibling index, or 0 if unspecified (match any)
};

static constexpr int MAX_XPATH_DEPTH = 16;

// Parse the XPath segment between /body/DocFragment[N]/body/ and the terminal position
// into an ordered sequence of steps. Returns step count, 0 on failure.
// Example input: "/body/DocFragment[1]/body/div[1]/ul/li[4]/text()[1].51"
// Fills steps with: {div,1}, {ul,1}, {li,4}
int parseXPathSteps(const std::string& xpath, XPathStep steps[MAX_XPATH_DEPTH]) {
  static const char kBodyFrag[] = "/body/DocFragment[";
  const size_t fragPos = xpath.find(kBodyFrag);
  if (fragPos == std::string::npos) return 0;
  const size_t afterBracket = xpath.find(']', fragPos + strlen(kBodyFrag));
  if (afterBracket == std::string::npos) return 0;
  static const char kBody[] = "/body/";
  if (xpath.compare(afterBracket + 1, strlen(kBody), kBody) != 0) return 0;
  size_t pos = afterBracket + 1 + strlen(kBody);

  size_t stepsEnd = xpath.rfind("/text()");
  if (stepsEnd == std::string::npos) {
    stepsEnd = xpath.rfind('.');
    if (stepsEnd == std::string::npos || stepsEnd <= pos || stepsEnd + 1 >= xpath.size()) return 0;
    for (size_t i = stepsEnd + 1; i < xpath.size(); i++) {
      if (xpath[i] < '0' || xpath[i] > '9') return 0;
    }
  }
  if (stepsEnd <= pos) return 0;

  int count = 0;
  while (pos < stepsEnd && count < MAX_XPATH_DEPTH) {
    const size_t slash = xpath.find('/', pos);
    const size_t segEnd = (slash < stepsEnd) ? slash : stepsEnd;

    XPathStep& step = steps[count];
    const size_t bracket = xpath.find('[', pos);
    const size_t nameEnd = (bracket != std::string::npos && bracket < segEnd) ? bracket : segEnd;
    const size_t nameLen = nameEnd - pos;
    if (nameLen == 0 || nameLen >= sizeof(step.tag)) return 0;
    memcpy(step.tag, xpath.c_str() + pos, nameLen);
    step.tag[nameLen] = '\0';

    if (bracket != std::string::npos && bracket < segEnd) {
      const size_t closeBracket = xpath.find(']', bracket + 1);
      if (closeBracket == std::string::npos || closeBracket > segEnd) return 0;
      int idx = 0;
      for (size_t i = bracket + 1; i < closeBracket; i++) {
        if (xpath[i] < '0' || xpath[i] > '9') return 0;
        idx = idx * 10 + (xpath[i] - '0');
      }
      step.siblingIndex = idx;
    } else {
      step.siblingIndex = 0;
    }

    count++;
    pos = (slash < stepsEnd) ? slash + 1 : stepsEnd;
  }
  return count;
}

class ParagraphStreamer final : public Print {
  size_t bytesWritten = 0;
  bool globalInTag = false;
  bool globalInEntity = false;
  static constexpr size_t MAX_ENTITY_SIZE = 16;
  char entityBuffer[MAX_ENTITY_SIZE] = {};
  size_t entityLen = 0;
  bool prevCR = false;  // last counted visible byte was a CR (XML line-ending normalization)

  // Forward mode: count <p> paragraphs at a byte offset (legacy, used by generateXPath)
  size_t fwdTarget;
  int fwdResult = 0;
  bool fwdCaptured = false;

  // Reverse mode shared state
  int revChar;
  bool revPFound = false;
  bool revDone = false;
  int revVisChars = 0;
  size_t totalVisChars = 0;
  size_t targetVisChars = 0;

  // --- Legacy reverse mode (paragraph index only, no ancestry) ---
  int revParagraph = 0;
  int pCount = 0;
  int paragraphAtMatch = 0;
  int liCount = 0;
  int liCountAtMatch = 0;
  int targetTextNode = 1;
  int currentTextNode = 0;
  int paragraphHtmlDepth = -1;

  // --- Ancestry-aware reverse mode ---
  const XPathStep* steps = nullptr;
  int stepCount = 0;
  int siblingCounters[MAX_XPATH_DEPTH] = {};
  bool insideStep[MAX_XPATH_DEPTH] = {};
  int htmlDepth = 0;
  int bodyHtmlDepth = -1;
  int stepEnteredAtDepth[MAX_XPATH_DEPTH] = {};
  bool relaxFirstStepDepth = false;

  // Tag name accumulation
  enum TagParseState { TAG_IDLE, TAG_IN_NAME, TAG_ATTRS } tagState = TAG_IDLE;
  bool tagIsClose = false;
  char tagName[12] = {};
  int tagNameLen = 0;

  int matchedDepth = 0;

  // Anchor ID capture
  static constexpr int MAX_ANCHOR_ID = 64;
  char capturedAnchorId[MAX_ANCHOR_ID] = {};
  int capturedAnchorIdLen = 0;
  bool capturingAnchorTag = false;
  enum AnchorAttrState {
    ATTR_FIND_NAME,
    ATTR_READ_NAME,
    ATTR_AFTER_NAME,
    ATTR_BEFORE_VALUE,
    ATTR_CAPTURE_D,
    ATTR_CAPTURE_S
  } attrState = ATTR_FIND_NAME;
  uint8_t attrNameLen = 0;
  bool currentAttrIsId = false;
  bool inAttrQuote =
      false;  // true while inside a quoted attribute value (prevents '/' from being treated as self-close)
  char attrQuoteChar = 0;
  uint8_t nonVisibleDepth = 0;
  bool insideBody = false;
  bool targetBodyText = false;

  bool isNonVisibleTag() const { return VisibleTextUtils::isNonVisibleElement(tagName); }

  static bool isAttrWhitespace(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

  static bool isAttrNameChar(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == ':' || c == '.';
  }

  void resetAnchorAttrScan() {
    attrState = ATTR_FIND_NAME;
    attrNameLen = 0;
    currentAttrIsId = false;
  }

  void finishCapturedAnchorId() {
    capturedAnchorId[capturedAnchorIdLen] = '\0';
    capturingAnchorTag = false;
    resetAnchorAttrScan();
  }

  void beginAnchorIdScan() {
    capturingAnchorTag = true;
    resetAnchorAttrScan();
  }

  void endAnchorIdScan() {
    if (capturingAnchorTag) {
      capturedAnchorIdLen = 0;
    }
    capturingAnchorTag = false;
    resetAnchorAttrScan();
  }

  void appendCapturedAnchorId(uint8_t c) {
    if (capturedAnchorIdLen + 1 < MAX_ANCHOR_ID) {
      capturedAnchorId[capturedAnchorIdLen++] = c;
    }
  }

  void scanAnchorAttribute(uint8_t c) {
    switch (attrState) {
      case ATTR_FIND_NAME:
        if (isAttrNameChar(c)) {
          attrState = ATTR_READ_NAME;
          attrNameLen = 1;
          currentAttrIsId = c == 'i';
        }
        break;
      case ATTR_READ_NAME:
        if (isAttrNameChar(c)) {
          if (attrNameLen == 1) {
            currentAttrIsId = currentAttrIsId && c == 'd';
          } else {
            currentAttrIsId = false;
          }
          attrNameLen++;
        } else {
          currentAttrIsId = currentAttrIsId && attrNameLen == 2;
          if (isAttrWhitespace(c)) {
            attrState = ATTR_AFTER_NAME;
          } else if (c == '=') {
            attrState = ATTR_BEFORE_VALUE;
          } else {
            resetAnchorAttrScan();
          }
        }
        break;
      case ATTR_AFTER_NAME:
        if (isAttrWhitespace(c)) {
          break;
        }
        if (c == '=') {
          attrState = ATTR_BEFORE_VALUE;
        } else if (isAttrNameChar(c)) {
          attrState = ATTR_READ_NAME;
          attrNameLen = 1;
          currentAttrIsId = c == 'i';
        } else {
          resetAnchorAttrScan();
        }
        break;
      case ATTR_BEFORE_VALUE:
        if (isAttrWhitespace(c)) {
          break;
        }
        if (currentAttrIsId && c == '"') {
          capturedAnchorIdLen = 0;
          attrState = ATTR_CAPTURE_D;
        } else if (currentAttrIsId && c == '\'') {
          capturedAnchorIdLen = 0;
          attrState = ATTR_CAPTURE_S;
        } else if (c == '"') {
          attrState = ATTR_CAPTURE_D;
        } else if (c == '\'') {
          attrState = ATTR_CAPTURE_S;
        } else {
          resetAnchorAttrScan();
        }
        break;
      case ATTR_CAPTURE_D:
        if (c == '"') {
          if (currentAttrIsId) {
            finishCapturedAnchorId();
          } else {
            resetAnchorAttrScan();
          }
        } else if (currentAttrIsId) {
          appendCapturedAnchorId(c);
        }
        break;
      case ATTR_CAPTURE_S:
        if (c == '\'') {
          if (currentAttrIsId) {
            finishCapturedAnchorId();
          } else {
            resetAnchorAttrScan();
          }
        } else if (currentAttrIsId) {
          appendCapturedAnchorId(c);
        }
        break;
    }
  }

  void onVisibleCodepoint() {
    totalVisChars++;
    if (revPFound && !revDone) {
      // Ancestry mode: count only while inside the fully-matched element and in the target text node.
      // Legacy mode: count only while still inside the matched paragraph and in the target text node.
      const bool inTargetNode =
          (stepCount > 0)
              ? (matchedDepth == stepCount && htmlDepth == stepEnteredAtDepth[stepCount - 1] &&
                 currentTextNode == targetTextNode)
              : (paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth && currentTextNode == targetTextNode);
      if (inTargetNode) {
        revVisChars++;
        if (revVisChars >= revChar) {
          targetVisChars = totalVisChars;
          revDone = true;
        }
      }
    }
  }

  void onVisibleText(const char* text) {
    if (!text) return;
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text);
    while (*ptr != 0) {
      utf8NextCodepoint(&ptr);
      onVisibleCodepoint();
    }
  }

  void flushEntityAsLiteral() {
    for (size_t i = 0; i < entityLen; i++) onVisibleCodepoint();
  }

  void finishEntity() {
    entityBuffer[entityLen] = '\0';
    const char* resolved = lookupHtmlEntity(entityBuffer, entityLen);
    if (resolved)
      onVisibleText(resolved);
    else if (entityLen >= 3 && entityBuffer[1] == '#')
      // Numeric character reference (&#NNN; / &#xHH;): expat -- which builds the page LUT --
      // decodes it to a single codepoint. Count one here too, not the raw "&#NNN" characters.
      onVisibleCodepoint();
    else
      flushEntityAsLiteral();
    globalInEntity = false;
    entityLen = 0;
  }

  void onLegacyP() {
    pCount++;
    if (!revPFound && revParagraph > 0 && pCount >= revParagraph) {
      revPFound = true;
      revVisChars = 0;
      paragraphHtmlDepth = htmlDepth;
      currentTextNode = 1;
      if (revChar <= 0 && targetTextNode <= 1) {
        targetVisChars = totalVisChars;
        revDone = true;
      }
    }
  }

  void onOpenTag() {
    htmlDepth++;

    if (strcasecmp(tagName, "body") == 0) {
      insideBody = true;
      bodyHtmlDepth = htmlDepth;
      if (targetBodyText) {
        revPFound = true;
        paragraphHtmlDepth = htmlDepth;
        currentTextNode = 1;
        if (revChar <= 0 && targetTextNode <= 1) {
          targetVisChars = totalVisChars;
          revDone = true;
        }
      }
      return;
    }
    if (!insideBody) return;

    if (nonVisibleDepth > 0 || isNonVisibleTag()) {
      nonVisibleDepth++;
      return;
    }

    if (stepCount == 0) {
      if (strcasecmp(tagName, "p") == 0) onLegacyP();
      return;
    }

    // Capture a child <a id> inside the fully-matched element even after target char is found.
    if (revPFound && matchedDepth == stepCount && capturedAnchorIdLen == 0 && strcasecmp(tagName, "a") == 0) {
      beginAnchorIdScan();
    }

    if (revDone) return;

    if (strcasecmp(tagName, "p") == 0) pCount++;
    if (strcasecmp(tagName, "li") == 0) liCount++;

    if (matchedDepth < stepCount) {
      const XPathStep& target = steps[matchedDepth];
      if (strcasecmp(tagName, target.tag) == 0) {
        // Count only direct children of the previously matched ancestor step.
        // For step 0 any depth is valid; subsequent steps must be exactly one level deeper.
        const bool atCorrectDepth = (matchedDepth == 0) ? (relaxFirstStepDepth || htmlDepth == bodyHtmlDepth + 1)
                                                        : (htmlDepth == stepEnteredAtDepth[matchedDepth - 1] + 1);
        if (!atCorrectDepth) return;
        siblingCounters[matchedDepth]++;
        if (target.siblingIndex == 0 || siblingCounters[matchedDepth] == target.siblingIndex) {
          insideStep[matchedDepth] = true;
          stepEnteredAtDepth[matchedDepth] = htmlDepth;
          matchedDepth++;
          if (matchedDepth == stepCount) {
            beginAnchorIdScan();
            paragraphAtMatch = pCount;
            liCountAtMatch = liCount;
            revPFound = true;
            capturedAnchorIdLen = 0;
            revVisChars = 0;
            currentTextNode = 1;  // Reset text node counter for this element
            if (revChar <= 0 && targetTextNode <= 1) {
              targetVisChars = totalVisChars;
              revDone = true;
            }
          }
        }
      }
    }
  }

  void onCloseTag() {
    if (strcasecmp(tagName, "body") == 0) {
      insideBody = false;
      if (htmlDepth > 0) htmlDepth--;
      return;
    }
    if (!insideBody) {
      if (htmlDepth > 0) htmlDepth--;
      return;
    }

    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
      if (htmlDepth > 0) htmlDepth--;
      return;
    }

    // Legacy mode: each direct child element closing advances the text node index.
    if (stepCount == 0 && revPFound && !revDone && paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth + 1) {
      currentTextNode++;
      if (currentTextNode == targetTextNode && revChar <= 0) {
        targetVisChars = totalVisChars;
        revDone = true;
      }
    }
    // Legacy mode: stop tracking when the matched paragraph itself closes.
    if (stepCount == 0 && revPFound && !revDone && paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth) {
      revPFound = false;
      paragraphHtmlDepth = -1;
    }

    // Ancestry mode: advance text node when a direct child of the fully-matched element closes.
    if (stepCount > 0 && matchedDepth == stepCount && revPFound && !revDone) {
      const int elementDepth = stepEnteredAtDepth[stepCount - 1];
      if (htmlDepth == elementDepth + 1) {
        currentTextNode++;
        if (currentTextNode == targetTextNode && revChar <= 0) {
          targetVisChars = totalVisChars;
          revDone = true;
        }
      }
    }

    if (stepCount > 0 && matchedDepth > 0) {
      const int step = matchedDepth - 1;
      if (insideStep[step] && htmlDepth == stepEnteredAtDepth[step]) {
        insideStep[step] = false;
        matchedDepth--;
        // If the fully-matched element just closed without finding the target, abort.
        if (matchedDepth < stepCount && revPFound && !revDone) {
          revPFound = false;
        }
        for (int i = matchedDepth + 1; i < stepCount; i++) {
          siblingCounters[i] = 0;
          insideStep[i] = false;
          stepEnteredAtDepth[i] = -1;
        }
      }
    }
    if (htmlDepth > 0) htmlDepth--;
  }

  void processByteInTag(uint8_t c) {
    switch (tagState) {
      case TAG_IDLE:
        if (c == '/') {
          tagIsClose = true;
          tagState = TAG_IN_NAME;
        } else if (c != '!' && c != '?') {
          tagIsClose = false;
          tagName[0] = static_cast<char>(c);
          tagNameLen = 1;
          tagState = TAG_IN_NAME;
        }
        break;
      case TAG_IN_NAME:
        if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
          tagName[tagNameLen] = '\0';
          if (tagNameLen > 0) {
            if (tagIsClose)
              onCloseTag();
            else
              onOpenTag();
            // Self-closing open tag (<br/>). Don't double-fire for close tags (</br/>).
            if (c == '/' && !tagIsClose) onCloseTag();
          }
          tagNameLen = 0;
          tagState = (c == '>') ? TAG_IDLE : TAG_ATTRS;
        } else if (tagNameLen + 1 < static_cast<int>(sizeof(tagName))) {
          tagName[tagNameLen++] = static_cast<char>(c);
        }
        break;
      case TAG_ATTRS:
        // Track quoted attribute values so '/' inside them is not mistaken for self-closing.
        if (!inAttrQuote) {
          if (c == '"' || c == '\'') {
            inAttrQuote = true;
            attrQuoteChar = c;
          }
        } else if (c == attrQuoteChar) {
          inAttrQuote = false;
          attrQuoteChar = 0;
        }
        if (capturingAnchorTag) {
          scanAnchorAttribute(c);
        }
        // Only treat '/' as self-closing when outside a quoted attribute value.
        if (c == '/' && !inAttrQuote) {
          endAnchorIdScan();
          onCloseTag();
        }
        break;
    }
  }

 public:
  explicit ParagraphStreamer(size_t targetByte) : fwdTarget(targetByte), revChar(0) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  ParagraphStreamer(int paragraph, int charOff, int textNodeIdx = 1)
      : fwdTarget(SIZE_MAX), revChar(charOff), revParagraph(paragraph), targetTextNode(textNodeIdx) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  ParagraphStreamer(const XPathStep* xpathSteps, int xpathStepCount, int charOff, int textNodeIdx = 1,
                    bool relaxFirstStep = false)
      : fwdTarget(SIZE_MAX),
        revChar(charOff),
        steps(xpathSteps),
        stepCount(xpathStepCount),
        targetTextNode(textNodeIdx),
        relaxFirstStepDepth(relaxFirstStep) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  ParagraphStreamer(bool resolveBodyText, int charOff, int textNodeIdx)
      : fwdTarget(SIZE_MAX), revChar(charOff), targetTextNode(textNodeIdx), targetBodyText(resolveBodyText) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  size_t write(uint8_t c) override {
    if (!fwdCaptured && bytesWritten >= fwdTarget) {
      fwdResult = pCount;
      fwdCaptured = true;
    }
    bytesWritten++;

    if (globalInEntity) {
      if (entityLen + 1 < MAX_ENTITY_SIZE) {
        entityBuffer[entityLen++] = static_cast<char>(c);
      } else {
        flushEntityAsLiteral();
        globalInEntity = false;
        entityLen = 0;
      }
      if (globalInEntity) {
        if (c == ';') {
          finishEntity();
        } else if (c == '<' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          flushEntityAsLiteral();
          globalInEntity = false;
          entityLen = 0;
        }
      }
      return 1;
    }

    // XML 1.0 §2.11 line-ending normalization: expat (which builds the page LUT) collapses a
    // "\r\n" pair and a lone "\r" to a single "\n". Mirror that so this byte counter -- which the
    // resolved offset is measured against -- stays codepoint-for-codepoint identical. prevCR is
    // set only by the visible-text branch below, so any non-text byte clears it here.
    const bool afterCR = prevCR;
    prevCR = false;

    if (c == '<') {
      globalInTag = true;
      tagState = TAG_IDLE;
      tagNameLen = 0;
      tagIsClose = false;
      capturingAnchorTag = false;
      resetAnchorAttrScan();
      inAttrQuote = false;
      attrQuoteChar = 0;
    } else if (c == '>') {
      if (tagState == TAG_ATTRS) {
        endAnchorIdScan();
      }
      globalInTag = false;
      inAttrQuote = false;
      if (tagState == TAG_IN_NAME && tagNameLen > 0) {
        tagName[tagNameLen] = '\0';
        if (tagIsClose)
          onCloseTag();
        else
          onOpenTag();
        tagNameLen = 0;
      }
      tagState = TAG_IDLE;
    } else if (globalInTag) {
      processByteInTag(c);
    } else if (!insideBody || nonVisibleDepth > 0) {
      // Ignore head/style/script/title text. KOReader XPaths are body-relative, and CSS text
      // should not contribute to intra-spine progress.
    } else {
      if (c == '&') {
        globalInEntity = true;
        entityBuffer[0] = '&';
        entityLen = 1;
      } else if (c == '\n' && afterCR) {
        // Second half of a CRLF: the newline was already counted on the preceding CR.
      } else {
        const bool startsCodepoint = (c & 0xC0) != 0x80;
        if (startsCodepoint) onVisibleCodepoint();
        prevCR = (c == '\r');  // a lone/leading CR is the newline; swallow any '\n' that follows
      }
    }
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) write(buffer[i]);
    return size;
  }

  int paragraphCount() const { return fwdCaptured ? fwdResult : pCount; }
  int getParagraphAtMatch() const { return paragraphAtMatch; }
  int getListItemAtMatch() const { return liCountAtMatch; }
  const char* getCapturedAnchorId() const { return capturedAnchorIdLen > 0 ? capturedAnchorId : nullptr; }
  size_t totalBytes() const { return bytesWritten; }
  bool found() const { return revDone; }
  size_t getTotalVisChars() const { return totalVisChars; }
  size_t getTargetVisChars() const { return targetVisChars; }
  float progress() const {
    return totalVisChars > 0 ? static_cast<float>(targetVisChars) / static_cast<float>(totalVisChars) : 0.0f;
  }
};

bool streamSpine(const std::shared_ptr<Epub>& epub, int spineIndex, ParagraphStreamer& s) {
  const auto href = epub->getSpineItem(spineIndex).href;
  return !href.empty() && epub->readItemContentsToStream(href, s, 1024);
}
}  // namespace

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  KOReaderPosition result;
  float intra =
      (pos.totalPages > 1) ? static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages - 1) : 0.0f;
  result.percentage = epub->calculateProgress(pos.spineIndex, intra);
  // The page's exact content offset first: the same text on every device.
  if (pos.hasVisibleTextOffset) {
    result.xpath = ChapterXPathResolver::findXPathForVisibleTextOffset(epub, pos.spineIndex, pos.visibleTextOffset);
  }
  // Progress-based XPath correctly handles both <p> and <li> positions.
  if (result.xpath.empty()) {
    result.xpath = ChapterXPathResolver::findXPathForProgress(epub, pos.spineIndex, intra);
  }
  // Fall back to paragraph-index lookup when progress-based resolution fails.
  if (result.xpath.empty() && pos.hasParagraphIndex && pos.paragraphIndex > 0) {
    result.xpath = ChapterXPathResolver::findXPathForParagraph(epub, pos.spineIndex, pos.paragraphIndex);
  }
  if (result.xpath.empty()) {
    LOG_INF("PM", "No structural XPath for spine %d; using a paragraph-count guess", pos.spineIndex);
    result.xpath = generateXPath(epub, pos.spineIndex, intra);
    result.structural = false;
  }
  LOG_DBG("PM", "-> KO: spine=%d page=%d/%d %.2f%% %s", pos.spineIndex, pos.pageNumber, pos.totalPages,
          result.percentage * 100, result.xpath.c_str());
  return result;
}

CrossPointPosition ProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                int currentSpineIndex, int totalPagesInCurrentSpine) {
  CrossPointPosition result{};
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return result;

  const int spineCount = epub->getSpineItemsCount();
  const float clampedPercentage = std::max(0.0f, std::min(1.0f, koPos.percentage));
  const size_t targetBytes = static_cast<size_t>(static_cast<float>(bookSize) * clampedPercentage);

  const int docFrag = parseIndex(koPos.xpath, "/body/DocFragment[");
  const int xpathP = parseIndex(koPos.xpath, "/p[", true);
  const int xpathChar = parseCharOffset(koPos.xpath);
  const int xpathTextNode = parseTextNodeIndex(koPos.xpath);
  const int xpathSpine = (docFrag >= 1) ? (docFrag - 1) : -1;

  XPathStep xpathSteps[MAX_XPATH_DEPTH];
  const int xpathStepCount = parseXPathSteps(koPos.xpath, xpathSteps);
  // Use ancestry mode whenever the XPath has a structured path (always more accurate than global counting).
  const bool useAncestry = xpathStepCount > 0;
  const bool useBodyText = !useAncestry && isBodyTextXPath(koPos.xpath);

  if (xpathSpine >= 0 && xpathSpine < spineCount) {
    result.spineIndex = xpathSpine;
    result.hasResolvedSpineIndex = true;
  } else {
    // First spine item whose end offset reaches the target. Cumulative sizes are
    // nondecreasing, so this is the item the old front-to-back walk stopped at,
    // found in O(log n) book.bin lookups instead of one per preceding chapter.
    const int found = findSpineForSize(spineCount, targetBytes,
                                       [&epub](int index) { return epub->getCumulativeSpineItemSize(index); });
    // The walk left spineIndex at 0 when no item reached the target (float
    // rounding can push targetBytes past the book size); the lookup answers
    // "last item" there, so keep the old result for that case.
    if (found >= 0 && epub->getCumulativeSpineItemSize(found) >= targetBytes) {
      result.spineIndex = found;
    }
  }
  if (result.spineIndex >= spineCount) return result;

  const size_t prevCum = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
  const size_t spineSize = epub->getCumulativeSpineItemSize(result.spineIndex) - prevCum;

  if (result.spineIndex == currentSpineIndex && totalPagesInCurrentSpine > 0) {
    result.totalPages = totalPagesInCurrentSpine;
  } else if (currentSpineIndex >= 0 && currentSpineIndex < spineCount && totalPagesInCurrentSpine > 0) {
    const size_t pc = (currentSpineIndex > 0) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    const size_t cs = epub->getCumulativeSpineItemSize(currentSpineIndex) - pc;
    if (cs > 0)
      result.totalPages = std::max(
          1, static_cast<int>(totalPagesInCurrentSpine * static_cast<float>(spineSize) / static_cast<float>(cs)));
  }
  if (spineSize == 0 || result.totalPages == 0) return result;

  float intra = 0.0f;
  bool resolvedIntra = false;
  // A resolved XPath gives the exact content offset (counted the way the page
  // parser counts, so it matches the section's page-start LUT) and the
  // intra-chapter fraction used until the caller refines against the layout.
  const auto applyResolved = [&](const ParagraphStreamer& s) {
    result.visibleTextOffset =
        static_cast<uint32_t>(std::min<size_t>(s.getTargetVisChars(), static_cast<size_t>(UINT32_MAX)));
    result.hasVisibleTextOffset = true;
    intra = s.progress();
    resolvedIntra = true;
  };
  if (useAncestry) {
    const auto applyResolvedXPath = [&](const ParagraphStreamer& s) {
      applyResolved(s);
      const int pAtMatch = s.getParagraphAtMatch();
      if (pAtMatch > 0) {
        result.paragraphIndex = static_cast<uint16_t>(pAtMatch);
        result.hasParagraphIndex = true;
      }
      if (xpathStepCount > 0 && strcasecmp(xpathSteps[xpathStepCount - 1].tag, "li") == 0) {
        const int liAtMatch = s.getListItemAtMatch();
        if (liAtMatch > 0) {
          result.liIndex = static_cast<uint16_t>(liAtMatch);
          result.hasLiIndex = true;
        }
      }
      const char* anchorId = s.getCapturedAnchorId();
      if (anchorId) {
        strncpy(result.xpathAnchorId, anchorId, sizeof(result.xpathAnchorId) - 1);
      }
      LOG_DBG("PM", "XPath ancestry(%s[%d])/text()[%d]+%d -> offset %u, %.1f%% (total=%zu p~%d li~%d anchor=%s)",
              xpathSteps[xpathStepCount - 1].tag, xpathSteps[xpathStepCount - 1].siblingIndex, xpathTextNode, xpathChar,
              result.visibleTextOffset, intra * 100, s.getTotalVisChars(), pAtMatch,
              result.hasLiIndex ? static_cast<int>(result.liIndex) : 0, anchorId ? anchorId : "none");
    };

    ParagraphStreamer strict(xpathSteps, xpathStepCount, xpathChar, xpathTextNode);
    if (streamSpine(epub, result.spineIndex, strict) && strict.found()) {
      applyResolvedXPath(strict);
    } else {
      // Some KOReader producers omit an unindexed wrapper from the ancestry.
      // Retry only after the structurally exact path fails, allowing the first
      // step at any body depth.
      ParagraphStreamer relaxed(xpathSteps, xpathStepCount, xpathChar, xpathTextNode, true);
      if (streamSpine(epub, result.spineIndex, relaxed) && relaxed.found()) {
        applyResolvedXPath(relaxed);
      }
    }
  } else if (useBodyText) {
    ParagraphStreamer s(true, xpathChar, xpathTextNode);
    if (streamSpine(epub, result.spineIndex, s) && s.found()) {
      applyResolved(s);
      LOG_DBG("PM", "XPath body/text()[%d]+%d -> offset=%u", xpathTextNode, xpathChar, result.visibleTextOffset);
    }
  } else if (xpathP > 0) {
    ParagraphStreamer s(xpathP, xpathChar, xpathTextNode);
    if (streamSpine(epub, result.spineIndex, s) && s.found()) {
      applyResolved(s);
      LOG_DBG("PM", "XPath p[%d]/text()[%d]+%d -> offset %u, %.1f%% (total=%zu)", xpathP, xpathTextNode, xpathChar,
              result.visibleTextOffset, intra * 100, s.getTotalVisChars());
    }
  }
  if (!resolvedIntra && xpathSpine >= 0 && xpathSpine < spineCount && isChapterStartXPath(koPos.xpath)) {
    // Only when no intra-chapter offset resolved above: a resolved deep position
    // (e.g. body/div[3]/text().0) must not be clobbered to page 0.
    intra = 0.0f;
    resolvedIntra = true;
    result.visibleTextOffset = 0;
    result.hasVisibleTextOffset = true;
    LOG_DBG("PM", "Chapter-start XPath %s -> spine=%d page start", koPos.xpath.c_str(), result.spineIndex);
  }
  if (!resolvedIntra) {
    const size_t bytesIn = (targetBytes > prevCum) ? (targetBytes - prevCum) : 0;
    intra = std::max(0.0f, std::min(1.0f, static_cast<float>(bytesIn) / static_cast<float>(spineSize)));
  }

  result.pageNumber = std::max(
      0, std::min(static_cast<int>(intra * static_cast<float>(result.totalPages - 1) + 0.5f), result.totalPages - 1));
  LOG_DBG("PM", "<- KO: %.2f%% %s -> spine=%d page=%d/%d", koPos.percentage * 100, koPos.xpath.c_str(),
          result.spineIndex, result.pageNumber, result.totalPages);
  return result;
}

void ProgressMapper::refinePageFromLayout(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer,
                                          CrossPointPosition& pos) {
  if (!pos.hasVisibleTextOffset && !pos.hasLiIndex && pos.xpathAnchorId[0] == '\0' && !pos.hasParagraphIndex) return;
  Section tempSection(epub, pos.spineIndex, renderer);

  if (pos.hasVisibleTextOffset) {
    // An image-only page shares its offset with the text page after it; a
    // chapter-start or image anchor wants the first of them.
    const bool preferFirst = pos.visibleTextOffset == 0;
    if (const auto offsetPage = tempSection.getPageForVisibleTextOffset(pos.visibleTextOffset, preferFirst)) {
      pos.pageNumber = *offsetPage;
      pos.totalPages = std::max(pos.totalPages, pos.pageNumber + 1);
      pos.hasMappedPage = true;
      LOG_DBG("PM", "Content offset %u -> page %d", pos.visibleTextOffset, pos.pageNumber);
      return;
    }
    LOG_DBG("PM", "Content offset %u not in section cache", pos.visibleTextOffset);
  }

  if (pos.hasLiIndex) {
    if (const auto liPage = tempSection.getPageForListItemIndex(pos.liIndex)) {
      LOG_DBG("PM", "Li index %u -> page %d (was %d)", pos.liIndex, *liPage, pos.pageNumber);
      pos.pageNumber = *liPage;
      pos.hasMappedPage = true;
      return;
    }
    LOG_DBG("PM", "Li index %u not found in section LUT", pos.liIndex);
  }
  if (pos.xpathAnchorId[0] != '\0') {
    if (const auto anchorPage = tempSection.getPageForAnchor(std::string(pos.xpathAnchorId))) {
      LOG_DBG("PM", "Anchor '%s' -> page %d (was %d)", pos.xpathAnchorId, *anchorPage, pos.pageNumber);
      pos.pageNumber = *anchorPage;
      pos.hasMappedPage = true;
      return;
    }
    LOG_DBG("PM", "Anchor '%s' not found in section cache", pos.xpathAnchorId);
  }
  if (pos.hasParagraphIndex) {
    const auto paragraphPage = tempSection.getPageForParagraphIndex(pos.paragraphIndex);
    const auto nextParagraphPage = tempSection.getPageForParagraphIndex(pos.paragraphIndex + 1);
    if (!paragraphPage.has_value()) {
      LOG_DBG("PM", "Paragraph %u not found in section LUT", pos.paragraphIndex);
      return;
    }
    int refinedPage = std::max(pos.pageNumber, static_cast<int>(*paragraphPage));
    if (nextParagraphPage.has_value()) {
      const int lutSpan = static_cast<int>(*nextParagraphPage) - static_cast<int>(*paragraphPage);
      // Only cap when the LUT span is >1. A span of 1 means the LUT granularity is too
      // coarse to trust over the intra-spine position (e.g. a stale cache where the paragraph
      // occupies different pages than at build time).
      if (lutSpan > 1 && refinedPage >= static_cast<int>(*nextParagraphPage)) {
        refinedPage = static_cast<int>(*nextParagraphPage) - 1;
      }
    }
    LOG_DBG("PM", "Paragraph %u -> LUT page %d, nextPara page %d, intra page %d, using %d", pos.paragraphIndex,
            *paragraphPage, nextParagraphPage.has_value() ? static_cast<int>(*nextParagraphPage) : -1, pos.pageNumber,
            refinedPage);
    pos.pageNumber = refinedPage;
    pos.hasMappedPage = true;
  }
}

KOReaderRichPosition ProgressMapper::toRichPosition(const CrossPointPosition& pos, const KOReaderPosition& koPos) {
  KOReaderRichPosition rich;
  const float pct = std::max(0.0f, std::min(1.0f, koPos.percentage));
  rich.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
  rich.spineIndex = static_cast<uint16_t>(std::max(0, pos.spineIndex));
  rich.pageNumber = static_cast<uint16_t>(std::max(0, pos.pageNumber));
  rich.totalPages = static_cast<uint16_t>(pos.totalPages > 0 ? pos.totalPages : 1);
  if (pos.hasParagraphIndex && pos.paragraphIndex > 0) rich.paragraphIndex = pos.paragraphIndex;
  rich.xpath = koPos.xpath;
  return rich;
}

std::optional<CrossPointPosition> ProgressMapper::fromRichPosition(const std::shared_ptr<Epub>& epub,
                                                                   const KOReaderRichPosition& rich) {
  const int spineCount = epub->getSpineItemsCount();
  if (static_cast<int>(rich.spineIndex) >= spineCount) {
    LOG_DBG("PM", "Rich position spine %u out of range (%d spine items)", rich.spineIndex, spineCount);
    return std::nullopt;
  }

  CrossPointPosition result{};
  result.spineIndex = rich.spineIndex;
  result.hasResolvedSpineIndex = true;

  const int cachedCount =
      Section::readCachedPageCount(epub->getCachePath() + "/sections/" + std::to_string(result.spineIndex) + ".bin");
  if (cachedCount <= 0) {
    // No local layout for the target spine yet; the percentage mapping handles
    // density estimation better than a blind copy of remote pages.
    LOG_DBG("PM", "Rich position spine %u has no cached page count", rich.spineIndex);
    return std::nullopt;
  }
  result.totalPages = cachedCount;

  const int remotePages = rich.totalPages > 0 ? rich.totalPages : 1;
  if (result.totalPages == remotePages) {
    // Identical layout (same render settings): the page transfers losslessly.
    result.pageNumber = std::min<int>(rich.pageNumber, result.totalPages - 1);
    result.hasMappedPage = true;
    LOG_DBG("PM", "Rich position exact: spine=%d page=%d/%d", result.spineIndex, result.pageNumber, result.totalPages);
    return result;
  }

  // Layout differs; the paragraph LUT is the most accurate anchor left.
  if (rich.paragraphIndex.has_value()) {
    result.paragraphIndex = *rich.paragraphIndex;
    result.hasParagraphIndex = true;
  }
  const float intra =
      (remotePages > 1) ? static_cast<float>(rich.pageNumber) / static_cast<float>(remotePages - 1) : 0.0f;
  result.pageNumber = std::max(
      0, std::min(static_cast<int>(intra * static_cast<float>(result.totalPages - 1) + 0.5f), result.totalPages - 1));
  LOG_DBG("PM", "Rich position scaled: spine=%d remote %u/%d -> page=%d/%d", result.spineIndex, rich.pageNumber,
          remotePages, result.pageNumber, result.totalPages);
  return result;
}

std::string ProgressMapper::generateXPath(const std::shared_ptr<Epub>& epub, int spineIndex, float intra) {
  const std::string base = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  if (intra <= 0.0f) return base;

  size_t spineSize = 0;
  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty() || !epub->getItemSize(href, &spineSize) || spineSize == 0) return base;

  ParagraphStreamer s(static_cast<size_t>(spineSize * std::min(intra, 1.0f)));
  if (!streamSpine(epub, spineIndex, s)) return base;

  const int p = s.paragraphCount();
  return (p > 0) ? base + "/p[" + std::to_string(p) + "]" : base;
}