#pragma once
#include <functional>
#include <string>

// Channel - how the harness reaches the owner. Subsumes the old SendSink /
// SpeakSink function pointers and the loops subsystem's chat-allowlist hooks.
// The device wires Telegram + TTS; host tests wire recorders.
namespace agent {

struct Channel {
  // Deliver text to a chat. Returns false if the send failed (harness treats
  // delivery as best-effort - a failed send is logged, never fatal to a turn).
  std::function<bool(const std::string& chatId, const std::string& text)> send;

  // Speak text on the device speaker (TTS). Optional; may be null.
  std::function<void(const std::string& text)> speak;

  // Owner-allowlist re-check at fire time (loops security rail).
  std::function<bool(const std::string& chatId)> isAllowed;

  // The first allowlisted chat (loops' default alert target).
  std::function<std::string()> firstAllowedChat;
};

}  // namespace agent
