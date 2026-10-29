#include "net/message.h"

namespace moho::net {

bool AppendFramed(const Message& m, std::string* out) {
  const size_t size = kMessageHeader + m.payload.size();
  if (size > kMessageMax) return false;
  out->push_back(static_cast<char>(m.type));
  out->push_back(static_cast<char>(size & 0xff));
  out->push_back(static_cast<char>((size >> 8) & 0xff));
  out->append(m.payload);
  return true;
}

MessageWriter& MessageWriter::U8(uint8_t v) {
  buf_.push_back(static_cast<char>(v));
  return *this;
}
MessageWriter& MessageWriter::U16(uint16_t v) {
  for (int i = 0; i < 2; ++i) buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  return *this;
}
MessageWriter& MessageWriter::U32(uint32_t v) {
  for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  return *this;
}
MessageWriter& MessageWriter::Str(const std::string& s) {
  buf_.append(s.c_str());  // (up to a 0 in it: the reader stops there)
  buf_.push_back('\0');
  return *this;
}
MessageWriter& MessageWriter::Bytes(const std::string& b) {
  buf_.append(b);
  return *this;
}

uint8_t MessageReader::U8() {
  if (at_ + 1 > p_.size()) return ok_ = false, 0;
  return static_cast<uint8_t>(p_[at_++]);
}
uint16_t MessageReader::U16() {
  if (at_ + 2 > p_.size()) return ok_ = false, 0;
  uint16_t v = static_cast<uint16_t>(static_cast<uint8_t>(p_[at_]) | (static_cast<uint8_t>(p_[at_ + 1]) << 8));
  at_ += 2;
  return v;
}
uint32_t MessageReader::U32() {
  if (at_ + 4 > p_.size()) return ok_ = false, 0;
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(p_[at_ + i])) << (8 * i);
  at_ += 4;
  return v;
}
std::string MessageReader::Str() {
  const size_t end = p_.find('\0', at_);
  if (end == std::string::npos) {  // (the original's reader stops at the stream's end too; a string must end in 0)
    ok_ = false;
    std::string s = p_.substr(at_);
    at_ = p_.size();
    return s;
  }
  std::string s = p_.substr(at_, end - at_);
  at_ = end + 1;
  return s;
}
std::string MessageReader::Rest() {
  std::string s = at_ < p_.size() ? p_.substr(at_) : std::string();
  at_ = p_.size();
  return s;
}

bool MessageSplitter::Next(Message* out) {
  if (broken_ || buf_.size() - at_ < kMessageHeader) return false;
  const size_t size = static_cast<uint8_t>(buf_[at_ + 1]) | (static_cast<size_t>(static_cast<uint8_t>(buf_[at_ + 2])) << 8);
  if (size < kMessageHeader) return broken_ = true, false;
  if (buf_.size() - at_ < size) return false;
  out->type = static_cast<uint8_t>(buf_[at_]);
  out->payload.assign(buf_, at_ + kMessageHeader, size - kMessageHeader);
  at_ += size;
  if (at_ > 4096 && at_ * 2 > buf_.size()) buf_.erase(0, at_), at_ = 0;  // (taken bytes dropped now and then)
  return true;
}

}  // namespace moho::net
