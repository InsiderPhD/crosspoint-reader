#include "SleepThemeExpr.h"

#include <Logging.h>
#include <Memory.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "SleepThemeText.h"
#include "SleepThemeVars.h"

namespace SleepThemeExpr {

namespace {

// Strings made while evaluating (literals, concatenations, function results)
// live in one heap arena per expression: Val stays small, so the recursion
// below costs little stack on the render task.
constexpr size_t ARENA_SIZE = 512;
// Bounds the recursion (each level is a few small frames).
constexpr int MAX_DEPTH = 32;

enum class Type : uint8_t { Empty, Num, Str };

struct Val {
  Type type = Type::Empty;
  double num = 0;
  uint16_t off = 0;
  uint16_t len = 0;
};

bool isNameStart(const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isNameChar(const char c) { return isNameStart(c) || (c >= '0' && c <= '9') || c == '.'; }

// Whole numbers print without a fraction; others to two places, trailing
// zeros dropped ("2.5", not "2.50").
void formatNum(const double d, char* out, const size_t outSize) {
  if (std::fabs(d) < 1e15 && d == std::floor(d)) {
    snprintf(out, outSize, "%lld", static_cast<long long>(d));
    return;
  }
  snprintf(out, outSize, "%.2f", d);
  char* dot = strchr(out, '.');
  if (dot == nullptr) return;
  char* e = out + strlen(out) - 1;
  while (e > dot && *e == '0') *e-- = '\0';
  if (e == dot) *e = '\0';
}

class Evaluator {
 public:
  Evaluator(const char* src, const size_t len, char* arena) : p(src), end(src + len), arena(arena) {}

  bool run(char* out, const size_t outSize) {
    const Val v = ternary();
    skip();
    if (ok && p != end) fail("unexpected text");
    if (!ok) return false;
    const Val s = toStr(v);
    snprintf(out, outSize, "%s", ok ? arena + s.off : "");
    return ok;
  }

  const char* error() const { return err; }
  size_t errorAt(const char* start) const { return static_cast<size_t>(errPos - start); }

 private:
  struct Depth {
    explicit Depth(Evaluator& e) : ev(e) {
      if (++ev.depth > MAX_DEPTH) ev.fail("too deeply nested");
    }
    ~Depth() { --ev.depth; }
    Evaluator& ev;
  };

  void fail(const char* message) {
    if (!ok) return;
    ok = false;
    err = message;
    errPos = p;
  }
  void skip() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }
  bool eat(const char* token) {
    skip();
    const size_t n = strlen(token);
    if (static_cast<size_t>(end - p) >= n && strncmp(p, token, n) == 0) {
      p += n;
      return true;
    }
    return false;
  }

  Val num(const double d) const {
    Val v;
    v.type = Type::Num;
    v.num = d;
    return v;
  }
  Val str(const char* s, const size_t n) {
    Val v;
    v.type = Type::Str;
    if (used + n + 1 > ARENA_SIZE) {
      fail("text too long");
      return v;
    }
    memcpy(arena + used, s, n);
    arena[used + n] = '\0';
    v.off = static_cast<uint16_t>(used);
    v.len = static_cast<uint16_t>(n);
    used += n + 1;
    return v;
  }
  const char* text(const Val& v) const { return arena + v.off; }

  Val toStr(const Val& v) {
    if (v.type == Type::Str) return v;
    if (v.type == Type::Empty) return str("", 0);
    char buf[32];
    formatNum(v.num, buf, sizeof(buf));
    return str(buf, strlen(buf));
  }
  static bool truthy(const Val& v) {
    if (v.type == Type::Num) return v.num != 0;
    if (v.type == Type::Str) return v.len > 0;
    return false;
  }
  // A number, or a string that is entirely one ("12").
  bool toNum(const Val& v, double& out) const {
    if (v.type == Type::Num) {
      out = v.num;
      return true;
    }
    if (v.type != Type::Str || v.len == 0) return false;
    char* stop = nullptr;
    out = strtod(text(v), &stop);
    return stop != nullptr && *stop == '\0';
  }

  Val ternary() {
    Depth d(*this);
    const Val cond = binary(1);
    if (!ok || !eat("?")) return cond;
    const Val a = ternary();
    if (!eat(":")) {
      fail("expected ':'");
      return a;
    }
    const Val b = ternary();
    return truthy(cond) ? a : b;
  }

  // Operators longest-first where they share a prefix; PREC holds each one's
  // precedence, loosest first.
  int matchOp(int& len) const {
    static constexpr const char* OPS[] = {"||", "&&", "==", "!=", "<=", ">=", "<", ">", "+", "-", "*", "/", "%"};
    for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
      const size_t n = strlen(OPS[i]);
      if (static_cast<size_t>(end - p) >= n && strncmp(p, OPS[i], n) == 0) {
        len = static_cast<int>(n);
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  int precOf(const int op) const {
    static constexpr int PREC[] = {1, 2, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 6};
    return PREC[op];
  }

  Val binary(const int minPrec) {
    Depth d(*this);
    Val lhs = unary();
    while (ok) {
      skip();
      int len = 0;
      const int op = matchOp(len);
      if (op < 0 || precOf(op) < minPrec) break;
      p += len;
      const Val rhs = binary(precOf(op) + 1);
      lhs = apply(op, lhs, rhs);
    }
    return lhs;
  }

  Val apply(const int op, const Val& a, const Val& b) {
    switch (op) {
      case 0:  // ||
        return truthy(a) ? a : b;
      case 1:  // &&
        return truthy(a) ? b : a;
      case 2:    // ==
      case 3:    // !=
      case 4:    // <=
      case 5:    // >=
      case 6:    // <
      case 7: {  // >
        bool result = false;
        if (a.type == Type::Empty || b.type == Type::Empty) {
          // Only "is it empty" questions make sense with a missing value.
          const bool both = a.type == b.type;
          result = op == 2 ? both : op == 3 ? !both : false;
          return num(result ? 1 : 0);
        }
        double x = 0, y = 0;
        int cmp = 0;
        if (toNum(a, x) && toNum(b, y)) {
          cmp = x < y ? -1 : (x > y ? 1 : 0);
        } else {
          const Val sa = toStr(a), sb = toStr(b);
          cmp = strcmp(text(sa), text(sb));
        }
        switch (op) {
          case 2:
            result = cmp == 0;
            break;
          case 3:
            result = cmp != 0;
            break;
          case 4:
            result = cmp <= 0;
            break;
          case 5:
            result = cmp >= 0;
            break;
          case 6:
            result = cmp < 0;
            break;
          default:
            result = cmp > 0;
            break;
        }
        return num(result ? 1 : 0);
      }
      case 8: {  // +: joins text when either side is text
        if (a.type == Type::Str || b.type == Type::Str) {
          const Val sa = toStr(a), sb = toStr(b);
          if (!ok) return Val{};
          const size_t n = sa.len + sb.len;
          if (used + n + 1 > ARENA_SIZE) {
            fail("text too long");
            return Val{};
          }
          Val v;
          v.type = Type::Str;
          v.off = static_cast<uint16_t>(used);
          v.len = static_cast<uint16_t>(n);
          memcpy(arena + used, text(sa), sa.len);
          memcpy(arena + used + sa.len, text(sb), sb.len);
          arena[used + n] = '\0';
          used += n + 1;
          return v;
        }
        break;
      }
      default:
        break;
    }
    // Arithmetic: a missing value (or one that isn't a number) gives empty.
    double x = 0, y = 0;
    if (!toNum(a, x) || !toNum(b, y)) return Val{};
    switch (op) {
      case 8:
        return num(x + y);
      case 9:
        return num(x - y);
      case 10:
        return num(x * y);
      case 11:
        return y == 0 ? Val{} : num(x / y);
      default:
        return y == 0 ? Val{} : num(std::fmod(x, y));
    }
  }

  Val unary() {
    Depth d(*this);
    if (eat("!")) {
      const Val v = unary();
      return num(truthy(v) ? 0 : 1);
    }
    if (eat("-")) {
      const Val v = unary();
      double x = 0;
      return toNum(v, x) ? num(-x) : Val{};
    }
    return primary();
  }

  Val primary() {
    skip();
    if (p >= end) {
      fail("expected a value");
      return Val{};
    }
    if (eat("(")) {
      const Val v = ternary();
      if (!eat(")")) fail("expected ')'");
      return v;
    }
    if (*p == '"' || *p == '\'') return stringLiteral();
    if ((*p >= '0' && *p <= '9') || *p == '.') {
      char* stop = nullptr;
      const double d = strtod(p, &stop);
      if (stop == p || stop > end) {
        fail("bad number");
        return Val{};
      }
      p = stop;
      return num(d);
    }
    if (!isNameStart(*p)) {
      fail("unexpected character");
      return Val{};
    }
    const char* name = p;
    while (p < end && isNameChar(*p)) p++;
    const size_t nameLen = static_cast<size_t>(p - name);
    skip();
    if (p < end && *p == '(') return call(name, nameLen);
    if (nameLen == 4 && strncmp(name, "true", 4) == 0) return num(1);
    if (nameLen == 5 && strncmp(name, "false", 5) == 0) return num(0);
    return variable(name, nameLen);
  }

  Val stringLiteral() {
    const char quote = *p++;
    const size_t start = used;
    Val v;
    v.type = Type::Str;
    v.off = static_cast<uint16_t>(start);
    while (p < end && *p != quote) {
      char c = *p++;
      if (c == '\\' && p < end) c = *p++;
      if (used + 2 > ARENA_SIZE) {
        fail("text too long");
        return Val{};
      }
      arena[used++] = c;
    }
    if (p >= end) {
      fail("unclosed string");
      return Val{};
    }
    p++;
    arena[used] = '\0';
    v.len = static_cast<uint16_t>(used - start);
    used++;
    return v;
  }

  // Kept out of primary() so its value buffer is off the stack while the
  // recursion continues.
  __attribute__((noinline)) Val variable(const char* name, const size_t nameLen) {
    SleepThemeVars::Id id;
    if (!SleepThemeVars::find(name, nameLen, id)) {
      fail("unknown value");
      return Val{};
    }
    switch (SleepThemeVars::info(id).kind) {
      case SleepThemeVars::Kind::Number: {
        int64_t n = 0;
        return SleepThemeVars::number(id, n) ? num(static_cast<double>(n)) : Val{};
      }
      case SleepThemeVars::Kind::Timestamp: {
        uint32_t t = 0;
        return SleepThemeVars::timestamp(id, t) ? num(static_cast<double>(t)) : Val{};
      }
      case SleepThemeVars::Kind::Text: {
        char value[SleepThemeVars::VALUE_LEN];
        SleepThemeVars::format(id, nullptr, 0, value, sizeof(value));
        return value[0] == '\0' ? Val{} : str(value, strlen(value));
      }
      case SleepThemeVars::Kind::Image:
        break;
    }
    return Val{};
  }

  __attribute__((noinline)) Val call(const char* name, const size_t nameLen) {
    p++;  // (
    Val args[3];
    int argc = 0;
    skip();
    if (!eat(")")) {
      do {
        if (argc == 3) {
          fail("too many arguments");
          return Val{};
        }
        args[argc++] = ternary();
      } while (ok && eat(","));
      if (!eat(")")) fail("expected ')'");
    }
    if (!ok) return Val{};
    auto is = [&](const char* fn) { return strlen(fn) == nameLen && strncmp(fn, name, nameLen) == 0; };
    double x = 0, y = 0;

    if (is("round") || is("floor") || is("ceil") || is("abs")) {
      if (!toNum(args[0], x)) return Val{};
      if (is("floor")) return num(std::floor(x));
      if (is("ceil")) return num(std::ceil(x));
      if (is("abs")) return num(std::fabs(x));
      // round(x[, digits]), halves away from zero.
      double scale = 1;
      if (argc > 1 && toNum(args[1], y)) scale = std::pow(10.0, std::fmax(0.0, std::fmin(6.0, std::floor(y))));
      const double r = std::floor(std::fabs(x) * scale + 0.5) / scale;
      return num(x < 0 ? -r : r);
    }
    if (is("min") || is("max")) {
      if (!toNum(args[0], x) || !toNum(args[1], y)) return Val{};
      return num(is("min") ? std::fmin(x, y) : std::fmax(x, y));
    }
    if (is("upper") || is("lower")) {
      if (args[0].type == Type::Empty) return Val{};
      const Val s = toStr(args[0]);
      if (!ok || used >= ARENA_SIZE) return Val{};
      Val v;
      v.type = Type::Str;
      v.off = static_cast<uint16_t>(used);
      SleepThemeText::applyTransform(
          is("upper") ? SleepThemeFormat::TextTransform::Upper : SleepThemeFormat::TextTransform::Lower, text(s),
          arena + used, ARENA_SIZE - used);
      v.len = static_cast<uint16_t>(strlen(arena + used));
      used += v.len + 1;
      return v;
    }
    if (is("pad")) {
      // pad(n, width): whole number, zero-padded.
      if (!toNum(args[0], x)) return Val{};
      const int width = toNum(args[1], y) ? static_cast<int>(std::fmin(12, std::fmax(0, y))) : 0;
      char buf[24];
      snprintf(buf, sizeof(buf), "%0*lld", width, static_cast<long long>(x));
      return str(buf, strlen(buf));
    }
    if (is("plural")) {
      // plural(n, "day", "days")
      if (!toNum(args[0], x)) return Val{};
      return x == 1 ? args[1] : args[2];
    }
    if (is("date")) {
      // date(time.now, "dd.MM.yyyy")
      if (!toNum(args[0], x) || args[1].type != Type::Str) return Val{};
      char buf[SleepThemeVars::VALUE_LEN];
      SleepThemeVars::formatTime(static_cast<uint32_t>(x), text(args[1]), args[1].len, buf, sizeof(buf));
      return buf[0] == '\0' ? Val{} : str(buf, strlen(buf));
    }
    fail("unknown function");
    return Val{};
  }

  const char* p;
  const char* end;
  char* arena;
  size_t used = 0;
  int depth = 0;
  bool ok = true;
  const char* err = nullptr;
  const char* errPos = nullptr;
};

}  // namespace

bool isPlainValue(const char* src, const size_t len) {
  size_t i = 0;
  while (i < len && src[i] == ' ') i++;
  if (i >= len || !isNameStart(src[i])) return false;
  while (i < len && isNameChar(src[i])) i++;
  while (i < len && src[i] == ' ') i++;
  return i == len || src[i] == ':';
}

bool evaluate(const char* src, const size_t len, char* out, const size_t outSize, bool& present) {
  out[0] = '\0';
  present = false;
  // Heap, not stack: ARENA_SIZE is over the 256-byte local limit and the
  // evaluator recurses. Freed on return.
  auto arena = makeUniqueNoThrow<char[]>(ARENA_SIZE);
  if (!arena) {
    LOG_ERR("STH", "malloc failed: %u bytes expression arena", static_cast<unsigned>(ARENA_SIZE));
    return false;
  }
  Evaluator ev(src, len, arena.get());
  if (!ev.run(out, outSize)) {
    LOG_ERR("STH", "Expression error (%s at %u) in {{%.*s}}", ev.error(), static_cast<unsigned>(ev.errorAt(src)),
            static_cast<int>(len), src);
    out[0] = '\0';
    return false;
  }
  present = out[0] != '\0';
  return true;
}

}  // namespace SleepThemeExpr
