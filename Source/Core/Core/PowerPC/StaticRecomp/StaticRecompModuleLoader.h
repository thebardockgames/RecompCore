// RecompCore: StaticRecomp CPU core - loading a recompiled module (phase L2).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The module DLL (or shared object) and the checks its descriptor must pass
// before the core runs it: ABI versions, CPUState size, sorted code / SMC /
// chunk ranges that tile the code, chunk hashes, entry point and game id.

#pragma once

#include <optional>
#include <string>

#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"

namespace StaticRecompModuleLoader
{
class ModuleLibrary
{
public:
  ModuleLibrary() = default;
  ~ModuleLibrary();
  ModuleLibrary(const ModuleLibrary&) = delete;
  ModuleLibrary& operator=(const ModuleLibrary&) = delete;

  bool Open(const std::string& path);
  void Close();
  bool IsOpen() const { return m_handle != nullptr; }
  void* Symbol(const char* name) const;

private:
  void* m_handle = nullptr;
};

// nullopt when the module may run; the reason otherwise.
std::optional<std::string> ValidateModule(const StaticRecompModuleDesc& desc, const std::string& game_id);
}  // namespace StaticRecompModuleLoader
