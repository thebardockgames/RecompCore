// RecompCore: StaticRecomp CPU core - state serializer (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The serializer the native modules use for their savestate sections. The
// byte layout is the one the chassis's PointerWrap produced for the same
// calls (raw little-endian copies of trivially copyable values, a u32 count
// before each container, bool as one byte), so the sections of every state
// written before this class loads unchanged, and a state written now loads
// in the chassis's reader.
//
// Three modes, as before: Read, Write and Measure (a bail-out: once set,
// nothing more is read or written; a module absent from a state switches to
// it so the rest of the section is skipped). BufferStream reads from or
// appends to a byte vector; StaticRecompCore adapts the chassis's PointerWrap
// to the same interface for its savestates.

#pragma once

#include <cstddef>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompState
{
class StateStream
{
public:
  enum class Mode
  {
    Read,
    Write,
    Measure,
  };
  virtual ~StateStream() = default;

  Mode GetMode() const { return m_mode; }
  bool IsReadMode() const { return m_mode == Mode::Read; }
  bool IsWriteMode() const { return m_mode == Mode::Write; }
  bool IsMeasureMode() const { return m_mode == Mode::Measure; }
  // Bail-out: nothing more is read or written (an implementation may tell its
  // backing store as well).
  virtual void SetMeasureMode() { m_mode = Mode::Measure; }

  // Raw bytes in the current mode (the only primitive the implementation
  // provides; it switches to Measure when the data runs out).
  virtual void Bytes(void* data, size_t size) = 0;

  template <typename T>
  requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> && !std::is_same_v<T, bool>)
  void Do(T& x)
  {
    Bytes(&x, sizeof(x));
  }
  void Do(bool& x)
  {
    u8 stable = static_cast<u8>(x);
    Bytes(&stable, 1);
    if (IsReadMode())
      x = stable != 0;
  }
  template <typename T>
  requires(std::is_trivially_copyable_v<T>)
  void DoArray(T* x, u32 count)
  {
    Bytes(x, size_t(count) * sizeof(T));
  }
  template <typename T>
  requires(!std::is_trivially_copyable_v<T>)
  void DoArray(T* x, u32 count)
  {
    for (u32 i = 0; i < count; ++i)
      Do(x[i]);
  }
  template <typename T>
  void Do(std::vector<T>& x)
  {
    DoContiguous(x);
  }
  template <typename T>
  void Do(std::basic_string<T>& x)
  {
    DoContiguous(x);
  }
  template <typename T>
  void Do(std::deque<T>& x)
  {
    u32 count = static_cast<u32>(x.size());
    Do(count);
    if (IsReadMode())
      x.resize(count);
    for (auto& elem : x)
      Do(elem);
  }
  template <typename T, typename U>
  void Do(std::pair<T, U>& x)
  {
    Do(x.first);
    Do(x.second);
  }

protected:
  explicit StateStream(Mode mode) : m_mode(mode) {}

private:
  template <typename C>
  void DoContiguous(C& container)
  {
    u32 size = static_cast<u32>(container.size());
    Do(size);
    if (IsReadMode())
      container.resize(size);
    if (size > 0)
      DoArray(&container[0], size);
  }
  Mode m_mode;
};

// A byte vector: appends when writing, consumes when reading.
class BufferStream final : public StateStream
{
public:
  // Writer.
  BufferStream() : StateStream(Mode::Write) {}
  // Reader over `data` (kept alive by the caller).
  explicit BufferStream(const std::vector<u8>& data)
      : StateStream(Mode::Read), m_read(data.data()), m_read_end(data.data() + data.size())
  {
  }

  void Bytes(void* data, size_t size) override
  {
    switch (GetMode())
    {
    case Mode::Write:
      m_buffer.insert(m_buffer.end(), static_cast<const u8*>(data),
                      static_cast<const u8*>(data) + size);
      break;
    case Mode::Read:
      if (m_read + size > m_read_end)
      {
        SetMeasureMode();  // ran out of data: the rest of the section is skipped
        m_overrun = true;
        break;
      }
      std::memcpy(data, m_read, size);
      m_read += size;
      break;
    case Mode::Measure:
      break;
    }
  }
  const std::vector<u8>& Buffer() const { return m_buffer; }
  std::vector<u8>&& Take() { return std::move(m_buffer); }
  size_t Remaining() const { return m_read_end > m_read ? size_t(m_read_end - m_read) : 0; }
  bool Overrun() const { return m_overrun; }

private:
  std::vector<u8> m_buffer;
  const u8* m_read = nullptr;
  const u8* m_read_end = nullptr;
  bool m_overrun = false;
};
}  // namespace StaticRecompState
