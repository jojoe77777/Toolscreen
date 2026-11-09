#pragma once

#include <cstdint>
#include <string>

namespace ResizePerf {

int64_t NowUs();
bool Watching();
void Trigger(const char* source);
int64_t CurrentWatchStart();
void EnterModalLoop();
void ExitModalLoop();
void RecordLoadLibrary(int64_t microseconds, bool alreadyLoaded, const std::string& name);
void RecordWindowMessage(unsigned message, int64_t microseconds, int64_t watchStart);
void OnFrameEnd() noexcept;

class ScopedWindowMessage {
  public:
    explicit ScopedWindowMessage(unsigned message)
        : m_message(message), m_active(Watching()), m_watchStart(m_active ? CurrentWatchStart() : 0), m_start(m_active ? NowUs() : 0) {}
    ~ScopedWindowMessage() {
        if (!m_active) { return; }
        try {
            RecordWindowMessage(m_message, NowUs() - m_start, m_watchStart);
        } catch (...) {
        }
    }
    ScopedWindowMessage(const ScopedWindowMessage&) = delete;
    ScopedWindowMessage& operator=(const ScopedWindowMessage&) = delete;

  private:
    unsigned m_message;
    bool m_active;
    int64_t m_watchStart;
    int64_t m_start;
};

}
