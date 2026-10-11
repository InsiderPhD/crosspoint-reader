#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  // Blocking waits for the render task to finish. Try makes one attempt and
  // returns at once; the caller must check ownsLock() before touching anything
  // the lock protects.
  enum class Mode { Blocking, Try };
  explicit RenderLock(Mode mode = Mode::Blocking);
  explicit RenderLock(Activity&);  // Activity argument retained for compatibility.
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  bool ownsLock() const { return isLocked; }
  void unlock();
  static bool peek();
};
