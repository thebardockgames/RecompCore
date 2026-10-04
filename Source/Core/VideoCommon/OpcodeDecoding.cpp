// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DL facts:
//  Ikaruga uses (nearly) NO display lists!
//  Zelda WW uses TONS of display lists
//  Zelda TP uses almost 100% display lists except menus (we like this!)
//  Super Mario Galaxy has nearly all geometry and more than half of the state in DLs (great!)

// Note that it IS NOT GENERALLY POSSIBLE to precompile display lists! You can compile them as they
// are while interpreting them, and hope that the vertex format doesn't change, though, if you do
// it right when they are called. The reason is that the vertex format affects the sizes of the
// vertices.

#include "VideoCommon/OpcodeDecoding.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FrameInterpolation.h"
#include "VideoCommon/NativeGXBridge.h"
#include "VideoCommon/NativeGXVerify.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

namespace OpcodeDecoder
{
bool g_record_fifo_data = false;
// Opt-in state audit against the synchronous native FIFO observer.
static FILE* s_native_audit = [] {
  const char* path = std::getenv("MODERNGEKKO_GX_STATE_AUDIT");
  return path ? std::fopen((std::string(path) + ".gpu.csv").c_str(), "wb") : nullptr;
}();
static u64 s_audit_frame = 0, s_audit_draw = 0;

// Diagnostic GX command profile for native-renderer design (off unless
// MODERNGEKKO_GX_PROFILE is set). Per XFB frame: display lists, matrix loads
// (immediate XF writes vs. indexed loads from guest RAM) and primitives.
// Every 30th frame also lists each display list call with a content hash, to
// tell static model lists from per-frame generated ones.
namespace GXProfile
{
struct Frame
{
  u32 dlists = 0, prims = 0, vertices = 0;
  u32 xf_pos = 0, xf_nrm = 0, xf_post = 0, xf_light = 0, xf_reg = 0, xf_words = 0;
  u32 idx_pos = 0, idx_nrm = 0, idx_tex = 0, idx_light = 0;
};
static FILE* s_frames = [] {
  const char* path = std::getenv("MODERNGEKKO_GX_PROFILE");
  FILE* file = path ? std::fopen((std::string(path) + ".frames.csv").c_str(), "wb") : nullptr;
  if (file)
    std::fputs("frame,dlists,prims,vertices,xf_pos_mtx,xf_nrm_mtx,xf_post_mtx,xf_light,xf_regs,"
               "xf_words,idx_pos_mtx,idx_nrm_mtx,idx_tex_mtx,idx_light\n",
               file);
  return file;
}();
static FILE* s_dlists = [] {
  const char* path = std::getenv("MODERNGEKKO_GX_PROFILE");
  FILE* file = path ? std::fopen((std::string(path) + ".dlists.csv").c_str(), "wb") : nullptr;
  if (file)
    std::fputs("frame,order,address,size,hash\n", file);
  return file;
}();
static Frame s_frame;
static u64 s_frame_index = 0;
static u32 s_dl_order = 0;

// Inventory for the native renderer (phase 0): which vertex formats, texture
// formats, TEV configurations, pixel pipeline states and EFB copies the game
// actually uses. Keys are raw register words; counts are draws (or copies).
struct Inventory
{
  std::map<u64, u64> vertex_formats;   // VCD lo/hi + VAT group words
  std::map<u64, u64> texture_formats;  // format << 8 | TLUT format (CI only)
  std::map<u64, u64> tev_configs;      // hash of TEV/indirect/alpha/fog registers
  std::map<u64, u64> tev_stage_counts; // TEV stages << 8 | indirect stages
  std::map<u64, u64> pixel_states;     // zmode, blend, dst alpha, z control, cull
  std::map<u64, u64> efb_copies;       // copy format, flags, EFB pixel format
};
static Inventory s_inventory;

static u32 BPWord(u32 reg)
{
  return reinterpret_cast<const u32*>(&bpmem)[reg];
}

static u64 HashWords(u64 hash, u32 value)
{
  hash ^= value;
  return hash * 1099511628211ULL;
}

static void InventoryDraw(u8 vat)
{
  const auto& cp = g_main_cp_state;
  const auto& attr = cp.vtx_attr[vat & 7];
  u64 vertex = 14695981039346656037ULL;
  for (u32 word : {cp.vtx_desc.low.Hex, cp.vtx_desc.high.Hex, attr.g0.Hex, attr.g1.Hex, attr.g2.Hex})
    vertex = HashWords(vertex, word);
  ++s_inventory.vertex_formats[vertex];

  const u32 stages = bpmem.genMode.numtevstages + 1;
  const u32 indirect = bpmem.genMode.numindstages;
  ++s_inventory.tev_stage_counts[(u64(stages) << 8) | indirect];

  u64 tev = 14695981039346656037ULL;
  tev = HashWords(tev, BPWord(0x00) & 0x000F3C70u);  // TEV/indirect stage counts, cull excluded
  for (u32 reg = 0x10; reg < 0x10 + 16; ++reg)
    tev = HashWords(tev, BPWord(reg));                // indirect stages
  tev = HashWords(tev, BPWord(0x27));                 // indirect order
  for (u32 reg = 0x28; reg < 0x30; ++reg)
    tev = HashWords(tev, BPWord(reg));                // TEV orders
  for (u32 reg = 0xC0; reg < 0xC0 + stages * 2; ++reg)
    tev = HashWords(tev, BPWord(reg));                // combiners of the active stages
  for (u32 reg = 0xF3; reg <= 0xFD; ++reg)
    tev = HashWords(tev, BPWord(reg));                // alpha test, z textures, konst selection
  tev = HashWords(tev, BPWord(0xF1) >> 21);           // fog type (fsel)
  ++s_inventory.tev_configs[tev];

  for (u32 stage = 0; stage < stages; ++stage)
  {
    const auto& order = bpmem.tevorders[stage / 2];
    if (!order.getEnable(stage & 1))
      continue;
    const auto& unit = bpmem.tex.GetUnit(order.getTexMap(stage & 1));
    const u32 format = static_cast<u32>(unit.texImage0.format.Value());
    const bool palette = format == 8 || format == 9 || format == 10;
    const u32 tlut = palette ? static_cast<u32>(unit.texTlut.tlut_format.Value()) : 0xFF;
    ++s_inventory.texture_formats[(u64(format) << 8) | tlut];
  }

  const u64 pixel = (u64(BPWord(0x40)) << 40) ^ (u64(BPWord(0x41)) << 16) ^
                    (u64(BPWord(0x42) & 0x1FF) << 56) ^ (u64(BPWord(0x43) & 0xFF) << 8) ^
                    ((BPWord(0x00) >> 14) & 3);
  ++s_inventory.pixel_states[pixel];
}

static void InventoryEfbCopy()
{
  const u32 copy = BPWord(0x52);
  ++s_inventory.efb_copies[(u64(copy) << 8) | (BPWord(0x43) & 0x3F)];
}

static void WriteInventory()
{
  const char* path = std::getenv("MODERNGEKKO_GX_PROFILE");
  if (!path)
    return;
  FILE* file = std::fopen((std::string(path) + ".inventory.csv").c_str(), "wb");
  if (!file)
    return;
  std::fputs("category,key,count\n", file);
  const auto dump = [&](const char* name, const std::map<u64, u64>& values) {
    for (const auto& [key, count] : values)
      std::fprintf(file, "%s,%016llX,%llu\n", name, static_cast<unsigned long long>(key),
                   static_cast<unsigned long long>(count));
  };
  dump("vertex_format", s_inventory.vertex_formats);
  dump("texture_format", s_inventory.texture_formats);
  dump("tev_stages", s_inventory.tev_stage_counts);
  dump("tev_config", s_inventory.tev_configs);
  dump("pixel_state", s_inventory.pixel_states);
  dump("efb_copy", s_inventory.efb_copies);
  std::fclose(file);
}

static void OnXFLoad(u16 address, u8 count)
{
  // XF memory map: 0x000-0x0FF pos/tex matrices, 0x400-0x45F normal
  // matrices, 0x500-0x5FF post-transform matrices, 0x600-0x67F lights,
  // 0x1000+ registers.
  if (address < 0x100)
    ++s_frame.xf_pos;
  else if (address >= 0x400 && address < 0x460)
    ++s_frame.xf_nrm;
  else if (address >= 0x500 && address < 0x600)
    ++s_frame.xf_post;
  else if (address >= 0x600 && address < 0x680)
    ++s_frame.xf_light;
  else
    ++s_frame.xf_reg;
  s_frame.xf_words += count;
}

static void OnIndexed(CPArray array)
{
  switch (array)
  {
  case CPArray::XF_A: ++s_frame.idx_pos; break;
  case CPArray::XF_B: ++s_frame.idx_nrm; break;
  case CPArray::XF_C: ++s_frame.idx_tex; break;
  default: ++s_frame.idx_light; break;
  }
}

static void OnDisplayList(u32 address, u32 size, const u8* data)
{
  ++s_frame.dlists;
  if (!s_dlists || s_frame_index % 30 != 0 || !data)
    return;
  u64 hash = 14695981039346656037ULL;
  for (u32 i = 0; i < size; ++i)
  {
    hash ^= data[i];
    hash *= 1099511628211ULL;
  }
  std::fprintf(s_dlists, "%llu,%u,%08X,%u,%016llX\n", static_cast<unsigned long long>(s_frame_index),
               s_dl_order++, address, size, static_cast<unsigned long long>(hash));
}

static void OnXfbCopy()
{
  const Frame& f = s_frame;
  std::fprintf(s_frames, "%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
               static_cast<unsigned long long>(s_frame_index), f.dlists, f.prims, f.vertices,
               f.xf_pos, f.xf_nrm, f.xf_post, f.xf_light, f.xf_reg, f.xf_words, f.idx_pos,
               f.idx_nrm, f.idx_tex, f.idx_light);
  if (s_frame_index % 60 == 0)
  {
    std::fflush(s_frames);
    if (s_dlists)
      std::fflush(s_dlists);
  }
  if (s_frame_index % 300 == 0)
    WriteInventory();
  s_frame = {};
  s_dl_order = 0;
  ++s_frame_index;
}
}  // namespace GXProfile

template <bool is_preprocess>
class RunCallback final : public Callback
{
public:
  OPCODE_CALLBACK(void OnXF(u16 address, u8 count, const u8* data))
  {
    m_cycles += 18 + 6 * count;

    if constexpr (!is_preprocess)
    {
      if (NativeGXVerify::IsEnabled())
        NativeGXVerify::RefXf(address, count, data);
      if (FrameInterpolation::IsActive())
        data = FrameInterpolation::FilterXF(address, count, data);
      LoadXFReg(address, count, data);
      if (GXProfile::s_frames)
        GXProfile::OnXFLoad(address, count);

      INCSTAT(g_stats.this_frame.num_xf_loads);
    }
  }
  OPCODE_CALLBACK(void OnCP(u8 command, u32 value))
  {
    m_cycles += 12;
    const u8 sub_command = command & CP_COMMAND_MASK;
    if constexpr (!is_preprocess)
    {
      if (NativeGXVerify::IsEnabled())
        NativeGXVerify::RefCp(command, value);
      if (sub_command == MATINDEX_A)
      {
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
        auto& system = Core::System::GetInstance();
        system.GetXFStateManager().SetTexMatrixChangedA(value);
      }
      else if (sub_command == MATINDEX_B)
      {
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
        auto& system = Core::System::GetInstance();
        system.GetXFStateManager().SetTexMatrixChangedB(value);
      }
      else if (sub_command == VCD_LO || sub_command == VCD_HI)
      {
        VertexLoaderManager::g_main_vat_dirty = BitSet8::AllTrue(CP_NUM_VAT_REG);
        VertexLoaderManager::g_bases_dirty = true;
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
      }
      else if (sub_command == CP_VAT_REG_A || sub_command == CP_VAT_REG_B ||
               sub_command == CP_VAT_REG_C)
      {
        VertexLoaderManager::g_main_vat_dirty[command & CP_VAT_MASK] = true;
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
      }
      else if (sub_command == ARRAY_BASE)
      {
        VertexLoaderManager::g_bases_dirty = true;
      }

      INCSTAT(g_stats.this_frame.num_cp_loads);
    }
    else if constexpr (is_preprocess)
    {
      if (sub_command == VCD_LO || sub_command == VCD_HI)
      {
        VertexLoaderManager::g_preprocess_vat_dirty = BitSet8::AllTrue(CP_NUM_VAT_REG);
      }
      else if (sub_command == CP_VAT_REG_A || sub_command == CP_VAT_REG_B ||
               sub_command == CP_VAT_REG_C)
      {
        VertexLoaderManager::g_preprocess_vat_dirty[command & CP_VAT_MASK] = true;
      }
    }
    GetCPState().LoadCPReg(command, value);
  }
  OPCODE_CALLBACK(void OnBP(u8 command, u32 value))
  {
    m_cycles += 12;

    if constexpr (is_preprocess)
    {
      LoadBPRegPreprocess(command, value, m_cycles);
    }
    else
    {
      if (NativeGXVerify::IsEnabled())
        NativeGXVerify::RefBp(command, value);
      if (FrameInterpolation::SuppressBPWrite(command))
        return;
      LoadBPReg(command, value, m_cycles);
      if (command == BPMEM_TRIGGER_EFB_COPY && bpmem.triggerEFBCopy.copy_to_xfb &&
          FrameInterpolation::IsActive() && !FrameInterpolation::IsReplaying())
      {
        const UPE_Copy copy = bpmem.triggerEFBCopy;
        const float y_scale = copy.scale_invert ?
                                  256.0f / static_cast<float>(bpmem.dispcopyyscale) :
                                  static_cast<float>(bpmem.dispcopyyscale) / 256.0f;
        const u32 height = static_cast<u32>(1.0f + bpmem.copyTexSrcWH.y * y_scale);
        FrameInterpolation::OnXfbCopied(command, value, bpmem.copyTexDest << 5,
                                        bpmem.copyDestStride << 5, height, m_in_display_list);
      }
      if (s_native_audit && command == 0x52 && bpmem.triggerEFBCopy.copy_to_xfb)
      { ++s_audit_frame; s_audit_draw = 0; std::fflush(s_native_audit); }
      if (GXProfile::s_frames && !FrameInterpolation::IsReplaying() && command == 0x52 &&
          bpmem.triggerEFBCopy.copy_to_xfb)
        GXProfile::OnXfbCopy();
      if (NativeGXVerify::IsEnabled() && command == 0x52 && bpmem.triggerEFBCopy.copy_to_xfb)
        NativeGXVerify::OnXfbCopy();
      if (GXProfile::s_frames && !FrameInterpolation::IsReplaying() && command == 0x52 &&
          !bpmem.triggerEFBCopy.copy_to_xfb)
        GXProfile::InventoryEfbCopy();
      INCSTAT(g_stats.this_frame.num_bp_loads);
    }
  }
  OPCODE_CALLBACK(void OnIndexedLoad(CPArray array, u32 index, u16 address, u8 size))
  {
    m_cycles += 6;

    if constexpr (is_preprocess)
      PreprocessIndexedXF(array, index, address, size);
    else
    {
      if (NativeGXVerify::IsEnabled())
        NativeGXVerify::RefIndexed(static_cast<u8>(array), index, address, size);
      LoadIndexedXF(array, index, address, size);
      if (GXProfile::s_frames)
        GXProfile::OnIndexed(array);
    }
  }
  OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat,
                                          u32 vertex_size, u16 num_vertices, const u8* vertex_data))
  {
    // load vertices
    const u32 size = vertex_size * num_vertices;
    if constexpr (!is_preprocess)
    {
      if (NativeGXVerify::IsEnabled())
      {
        NativeGXVerify::RefPrimitive(static_cast<u8>(primitive), vat, vertex_size, num_vertices,
                                     vertex_data);
        NativeGXVerify::RefPrimitiveVertices(vat, num_vertices, vertex_data);
      }
    }
    if constexpr (!is_preprocess) if (s_native_audit)
    {
      if (s_audit_frame >= 5900 && s_audit_frame <= 6700)
      {
        u64 hash = 14695981039346656037ULL;
        for (u32 i = 0; i < size; ++i) { hash ^= vertex_data[i]; hash *= 1099511628211ULL; }
        std::fprintf(s_native_audit,"%llu,%llu,%u,%u,%u,%u,%u,%llu\n",
          static_cast<unsigned long long>(s_audit_frame),static_cast<unsigned long long>(s_audit_draw),
          num_vertices,vertex_size,bpmem.genMode.hex,bpmem.zmode.hex,bpmem.blendmode.hex,
          static_cast<unsigned long long>(hash));
      }
      ++s_audit_draw;
    }

    if constexpr (!is_preprocess)
    {
      if (GXProfile::s_frames && !FrameInterpolation::IsReplaying())
      {
        ++GXProfile::s_frame.prims;
        GXProfile::s_frame.vertices += num_vertices;
        GXProfile::InventoryDraw(vat);
      }
    }
    if constexpr (!is_preprocess)
    {
      if (FrameInterpolation::IsActive())
        vertex_data = FrameInterpolation::FilterVertices(vat, vertex_size, num_vertices, vertex_data);
    }
    const u32 bytes =
        VertexLoaderManager::RunVertices<is_preprocess>(vat, primitive, num_vertices, vertex_data);
    if constexpr (!is_preprocess)
    {
      if (FrameInterpolation::IsReplaying())
        FrameInterpolation::AfterVertices();
    }

    ASSERT(bytes == size);

    // 4 GPU ticks per vertex, 3 CPU ticks per GPU tick
    m_cycles += num_vertices * 4 * 3 + 6;
  }
  // This can't be inlined since it calls Run, which makes it recursive
  // m_in_display_list prevents it from actually recursing infinitely, but there's no real benefit
  // to inlining Run for the display list directly.
  OPCODE_CALLBACK_NOINLINE(void OnDisplayList(u32 address, u32 size))
  {
    m_cycles += 6;

    if constexpr (!is_preprocess)
    {
      if (NativeGXVerify::IsEnabled())
        NativeGXVerify::RefDisplayList(address, size, m_in_display_list);
    }
    if (m_in_display_list)
    {
      WARN_LOG_FMT(VIDEO, "recursive display list detected");
    }
    else
    {
      m_in_display_list = true;

      auto& system = Core::System::GetInstance();

      if constexpr (is_preprocess)
      {
        auto& memory = system.GetMemory();
        const u8* const start_address = memory.GetPointerForRange(address, size);

        system.GetFifo().PushFifoAuxBuffer(start_address, size);

        if (start_address != nullptr)
        {
          Run(start_address, size, *this);
        }
      }
      else
      {
        const u8* start_address;

        auto& fifo = system.GetFifo();
        if (fifo.UseDeterministicGPUThread())
        {
          start_address = static_cast<u8*>(fifo.PopFifoAuxBuffer(size));
        }
        else
        {
          auto& memory = system.GetMemory();
          start_address = memory.GetPointerForRange(address, size);
        }
        // Frame interpolation records each list (or substitutes the recorded
        // copy while replaying).
        if (FrameInterpolation::IsActive())
          start_address = FrameInterpolation::OnDisplayListCall(start_address, address, size);

        // Avoid the crash if memory.GetPointerForRange failed ..
        if (start_address != nullptr)
        {
          if (GXProfile::s_frames && !FrameInterpolation::IsReplaying())
            GXProfile::OnDisplayList(address, size, start_address);
          // temporarily swap dl and non-dl (small "hack" for the stats)
          g_stats.SwapDL();

          Run(start_address, size, *this);
          if (FrameInterpolation::IsActive())
            FrameInterpolation::OnDisplayListEnd();
          INCSTAT(g_stats.this_frame.num_dlists_called);

          // un-swap
          g_stats.SwapDL();
        }
      }

      m_in_display_list = false;
    }
  }
  OPCODE_CALLBACK(void OnNop(u32 count))
  {
    if constexpr (!is_preprocess)
    {
      if (NativeGXVerify::IsEnabled())
        NativeGXVerify::RefNop(count);
    }
    m_cycles += 6 * count;  // Hm, this means that we scan over nop streams pretty slowly...
  }
  OPCODE_CALLBACK(void OnUnknown(u8 opcode, const u8* data))
  {
    if constexpr (!is_preprocess)
    {
      if (NativeGXVerify::IsEnabled())
        NativeGXVerify::RefUnknown(opcode);
    }
    if (static_cast<Opcode>(opcode) == Opcode::GX_CMD_UNKNOWN_METRICS)
    {
      // 'Zelda Four Swords' calls it and checks the metrics registers after that
      m_cycles += 6;
      DEBUG_LOG_FMT(VIDEO, "GX 0x44");
    }
    else if (static_cast<Opcode>(opcode) == Opcode::GX_CMD_INVL_VC)
    {
      // Invalidate Vertex Cache
      m_cycles += 6;
      DEBUG_LOG_FMT(VIDEO, "Invalidate (vertex cache?)");
    }
    else
    {
      auto& system = Core::System::GetInstance();
      system.GetCommandProcessor().HandleUnknownOpcode(opcode, data, is_preprocess);
      m_cycles += 1;
    }
  }

  OPCODE_CALLBACK(void OnCommand(const u8* data, u32 size))
  {
    ASSERT(size >= 1);
    if constexpr (!is_preprocess)
    {
      // Display lists get added directly into the FIFO stream since this same callback is used to
      // process them.
      if (g_record_fifo_data && static_cast<Opcode>(data[0]) != Opcode::GX_CMD_CALL_DL)
      {
        Core::System::GetInstance().GetFifoRecorder().WriteGPCommand(data, size);
      }
      if (!m_in_display_list && FrameInterpolation::IsActive())
        FrameInterpolation::RecordCommand(data, size);
      if (!m_in_display_list && NativeGXVerify::IsEnabled())
        NativeGXVerify::OnTopLevelCommand(data, size);
      if (!m_in_display_list && NativeGXBridge::IsEnabled())
        NativeGXBridge::OnTopLevelCommand(data, size);
    }
  }

  OPCODE_CALLBACK(CPState& GetCPState())
  {
    if constexpr (is_preprocess)
      return g_preprocess_cp_state;
    else
      return g_main_cp_state;
  }

  OPCODE_CALLBACK(u32 GetVertexSize(u8 vat))
  {
    VertexLoaderBase* loader = VertexLoaderManager::RefreshLoader<is_preprocess>(vat);
    return loader->m_vertex_size;
  }

  u32 m_cycles = 0;
  bool m_in_display_list = false;
};

template <bool is_preprocess>
u8* RunFifo(DataReader src, u32* cycles)
{
  using CallbackT = RunCallback<is_preprocess>;
  auto callback = CallbackT{};
  u32 size = Run(src.GetPointer(), static_cast<u32>(src.size()), callback);

  if (cycles != nullptr)
    *cycles = callback.m_cycles;

  src.Skip(size);
  return src.GetPointer();
}

template u8* RunFifo<true>(DataReader src, u32* cycles);
template u8* RunFifo<false>(DataReader src, u32* cycles);

void RunReplay(const u8* data, u32 size)
{
  RunCallback<false> callback;
  Run(data, size, callback);
}

}  // namespace OpcodeDecoder
