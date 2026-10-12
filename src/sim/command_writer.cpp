#include "sim/command_writer.h"

#include <cstring>

#include "core/log.h"
#include "sim/command_stream.h"

namespace moho {
namespace {

// Payload writer: little-endian, NUL-terminated strings (the reader is command_stream.cpp's In).
struct Out {
  std::string d;
  void U8(int v) { d.push_back(static_cast<char>(v)); }
  void U16(uint32_t v) {
    U8(v & 0xff);
    U8((v >> 8) & 0xff);
  }
  void U32(uint32_t v) {
    U16(v & 0xffff);
    U16(v >> 16);
  }
  void F32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    U32(u);
  }
  void Str(const std::string& s) {
    d.append(s.c_str());  // to the first NUL
    d.push_back(0);
  }
  void IdSet(const std::vector<uint32_t>& ids) {
    U32(static_cast<uint32_t>(ids.size()));
    for (uint32_t id : ids) U32(id);
  }
  void Target(const StreamTarget& t) {
    U8(t.type);
    if (t.type == 1) {
      U32(t.entity);
    } else if (t.type == 2) {
      F32(t.pos.x);
      F32(t.pos.y);
      F32(t.pos.z);
    }
  }
  void Cells(const std::vector<std::pair<int16_t, int16_t>>& cells) {
    U32(static_cast<uint32_t>(cells.size()));
    for (const auto& c : cells) {
      U16(static_cast<uint16_t>(c.first));
      U16(static_cast<uint16_t>(c.second));
    }
  }
  void Lua(const std::string& bytes) {
    if (bytes.empty()) U8(2);  // nil
    else d.append(bytes);
  }
};

std::string Frame(int type, const std::string& payload) {
  std::string m;
  const size_t len = payload.size() + 3;
  m.push_back(static_cast<char>(type));
  m.push_back(static_cast<char>(len & 0xff));
  m.push_back(static_cast<char>((len >> 8) & 0xff));
  m.append(payload);
  return m;
}

}  // namespace

void CommandWriter::Message(int type, const std::string& payload) {
  if (payload.size() + 3 > 0xffff) {
    Logf(LogLevel::Error, "moho64: a command message of type %d is too long (%zu bytes) and was dropped", type,
         payload.size());
    return;
  }
  body_ += Frame(type, payload);
}

uint32_t CommandWriter::NewCommandId() {
  if (nextId_ > 0xffffffu) nextId_ = 1;  // (the original's client reuses ids the simulation reported gone)
  return (static_cast<uint32_t>(source_) << 24) | nextId_++;
}

void CommandWriter::RequestPause() { Message(4, {}); }
void CommandWriter::Resume() { Message(5, {}); }
void CommandWriter::SingleStep() { Message(6, {}); }

void CommandWriter::CreateUnit(int army0, const std::string& blueprint, float x, float z, float heading) {
  Out o;
  o.U8(army0);
  o.Str(blueprint);
  o.F32(x);
  o.F32(z);
  o.F32(heading);
  Message(7, o.d);
}

void CommandWriter::CreateProp(const std::string& blueprint, float x, float y, float z) {
  Out o;
  o.Str(blueprint);
  o.F32(x);
  o.F32(y);
  o.F32(z);
  Message(8, o.d);
}

void CommandWriter::DestroyEntity(uint32_t entity) {
  Out o;
  o.U32(entity);
  Message(9, o.d);
}

void CommandWriter::ProcessInfoPair(uint32_t entity, const std::string& key, const std::string& value) {
  Out o;
  o.U32(entity);
  o.Str(key);
  o.Str(value);
  Message(11, o.d);
}

uint32_t CommandWriter::IssueCommand(const std::vector<uint32_t>& units, StreamIssueData c, bool clear) {
  return Issue(12, units, std::move(c), clear);
}

uint32_t CommandWriter::IssueFactoryCommand(const std::vector<uint32_t>& units, StreamIssueData c, bool clear) {
  return Issue(13, units, std::move(c), clear);
}

uint32_t CommandWriter::Issue(int message, const std::vector<uint32_t>& units, StreamIssueData c, bool clear) {
  if (c.id == 0xffffffffu) c.id = NewCommandId();
  Out o;
  o.IdSet(units);
  o.U32(c.id);
  o.U32(static_cast<uint32_t>(c.coordWith));
  o.U8(c.type);
  o.U32(static_cast<uint32_t>(c.insertBefore));
  o.Target(c.target);
  o.Target(c.target2);
  o.U32(static_cast<uint32_t>(c.formIndex));
  if (c.formIndex != -1) {
    o.F32(c.qw);
    o.F32(c.qx);
    o.F32(c.qy);
    o.F32(c.qz);
    o.F32(c.scale);
  }
  o.Str(c.blueprint);
  o.Cells(c.cells);
  o.U32(static_cast<uint32_t>(c.maxCount));
  o.U32(static_cast<uint32_t>(c.count));
  o.Lua(c.lua);
  o.U8(clear ? 1 : 0);
  Message(message, o.d);
  return c.id;
}

void CommandWriter::IncreaseCommandCount(uint32_t command, int32_t delta) {
  Out o;
  o.U32(command);
  o.U32(static_cast<uint32_t>(delta));
  Message(14, o.d);
}

void CommandWriter::DecreaseCommandCount(uint32_t command, int32_t delta) {
  Out o;
  o.U32(command);
  o.U32(static_cast<uint32_t>(delta));
  Message(15, o.d);
}

void CommandWriter::SetCommandTarget(uint32_t command, const StreamTarget& target) {
  Out o;
  o.U32(command);
  o.Target(target);
  Message(16, o.d);
}

void CommandWriter::SetCommandType(uint32_t command, int type) {
  Out o;
  o.U32(command);
  o.U32(static_cast<uint32_t>(type));
  Message(17, o.d);
}

void CommandWriter::SetCommandCells(uint32_t command, const std::vector<std::pair<int16_t, int16_t>>& cells, float x,
                                    float y, float z) {
  Out o;
  o.U32(command);
  o.Cells(cells);
  o.F32(x);
  o.F32(y);
  o.F32(z);
  Message(18, o.d);
}

void CommandWriter::RemoveCommandFromQueue(uint32_t command, uint32_t unit) {
  Out o;
  o.U32(command);
  o.U32(unit);
  Message(19, o.d);
}

void CommandWriter::ExecuteLuaInSim(const std::string& function, const std::string& luaValue) {
  Out o;
  o.Str(function);
  o.Lua(luaValue);
  Message(21, o.d);
}

void CommandWriter::LuaSimCallback(const std::string& name, const std::string& luaValue,
                                   const std::vector<uint32_t>& units) {
  Out o;
  o.Str(name);
  o.Lua(luaValue);
  o.IdSet(units);
  Message(22, o.d);
}

void CommandWriter::EndGame() { Message(23, {}); }

std::string CommandWriter::Take() {
  if (body_.empty()) return {};
  std::string out = Frame(1, std::string(1, static_cast<char>(source_)));
  out += body_;
  body_.clear();
  return out;
}

std::string CommandWriter::AdvanceMessage(uint32_t beats) {
  Out o;
  o.U32(beats);
  return Frame(0, o.d);
}

bool ApplyCommands(Sim& sim, const std::string& bytes, std::map<std::string, int>* unhandled) {
  CommandStream stream(bytes, 0);
  while (!stream.AtEnd()) stream.ApplyUntilAdvance(sim);
  if (unhandled)
    for (const auto& [what, n] : stream.unhandled()) (*unhandled)[what] += n;
  return !stream.hadError();
}

}  // namespace moho
