// The message unit everywhere in the original's networking: the lobby, the in-game client manager and the replay
// stream (CMessage 0x483490: `[u8 type][u16 size, little-endian, the 3-byte header included][payload]`, at most
// 0xFFFF bytes, "Message too large"). Payload fields as its BinaryReader / Stream read and write them: integers
// little-endian, a string zero-terminated (BinaryReader::ReadString 0x4ccdd0 reads up to a 0 byte).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace moho::net {

struct Message {
  uint8_t type = 0;
  std::string payload;
};

constexpr size_t kMessageHeader = 3;
constexpr size_t kMessageMax = 0xFFFF;

// A message's bytes as they go on a stream: header and payload. False (nothing appended) when it is too large.
bool AppendFramed(const Message& m, std::string* out);

class MessageWriter {
 public:
  MessageWriter& U8(uint8_t v);
  MessageWriter& U16(uint16_t v);
  MessageWriter& U32(uint32_t v);
  MessageWriter& I32(int32_t v) { return U32(static_cast<uint32_t>(v)); }
  MessageWriter& Str(const std::string& s);  // zero-terminated
  MessageWriter& Bytes(const std::string& b);  // as they are (the rest of the message)
  Message Done(uint8_t type) const { return Message{type, buf_}; }

 private:
  std::string buf_;
};

class MessageReader {
 public:
  explicit MessageReader(const std::string& payload) : p_(payload) {}
  uint8_t U8();
  uint16_t U16();
  uint32_t U32();
  int32_t I32() { return static_cast<int32_t>(U32()); }
  std::string Str();
  std::string Rest();  // everything not read yet
  bool Ok() const { return ok_; }  // false once a read ran past the end
  bool AtEnd() const { return at_ >= p_.size(); }

 private:
  const std::string& p_;
  size_t at_ = 0;
  bool ok_ = true;
};

// The messages in a stream of bytes as they arrive (a TCP stream): Feed the bytes, then take whole messages.
class MessageSplitter {
 public:
  void Feed(const char* data, size_t size) { buf_.append(data, size); }
  bool Next(Message* out);  // a whole message taken, or false (wait for more bytes)
  bool Broken() const { return broken_; }  // a size field that cannot be (shorter than the header)

 private:
  std::string buf_;
  size_t at_ = 0;
  bool broken_ = false;
};

}  // namespace moho::net
