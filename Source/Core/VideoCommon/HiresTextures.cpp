// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/HiresTextures.h"

#include <algorithm>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <xxhash.h>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Orca/Profile.h"
#include "Core/System.h"
#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/VideoConfig.h"

constexpr std::string_view s_format_prefix{"tex1_"};

static std::unordered_map<std::string, std::shared_ptr<HiresTexture>> s_hires_texture_cache;
static std::unordered_map<std::string, bool> s_hires_texture_id_to_arbmipmap;

static auto s_file_library = std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();
static bool s_isolation_reported = false;

namespace
{
std::pair<std::string, bool> GetNameArbPair(const TextureInfo& texture_info)
{
  if (s_hires_texture_id_to_arbmipmap.empty())
    return {"", false};

  const auto texture_name_details = texture_info.CalculateTextureName();
  // look for an exact match first
  const std::string full_name = texture_name_details.GetFullName();
  if (auto iter = s_hires_texture_id_to_arbmipmap.find(full_name);
      iter != s_hires_texture_id_to_arbmipmap.end())
  {
    return {full_name, iter->second};
  }

  // Single wildcard ignoring the tlut hash
  const std::string texture_name_single_wildcard_tlut =
      fmt::format("{}_{}_$_{}", texture_name_details.base_name, texture_name_details.texture_name,
                  texture_name_details.format_name);
  if (auto iter = s_hires_texture_id_to_arbmipmap.find(texture_name_single_wildcard_tlut);
      iter != s_hires_texture_id_to_arbmipmap.end())
  {
    return {texture_name_single_wildcard_tlut, iter->second};
  }

  // Single wildcard ignoring the texture hash
  const std::string texture_name_single_wildcard_tex =
      fmt::format("{}_${}_{}", texture_name_details.base_name, texture_name_details.tlut_name,
                  texture_name_details.format_name);
  if (auto iter = s_hires_texture_id_to_arbmipmap.find(texture_name_single_wildcard_tex);
      iter != s_hires_texture_id_to_arbmipmap.end())
  {
    return {texture_name_single_wildcard_tex, iter->second};
  }

  return {"", false};
}
}  // namespace

namespace
{
bool IsModSession()
{
  return Orca::SessionActive() && (Orca::Mod::Requested() || Orca::Mod::ActiveMod());
}

// True for a mip level file (<name>_mip<N>). It loads together with <name>, so it is not a
// texture of its own.
bool IsMipLevelFile(std::string_view filename)
{
  const size_t at = filename.rfind("_mip");
  return at != std::string_view::npos && at + 4 < filename.size() &&
         std::all_of(filename.begin() + at + 4, filename.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}
}  // namespace

std::vector<HiresTextureFile>
CollectHiresTextureFiles(const std::vector<std::string>& player_directories,
                         const std::vector<std::string>& orca_directories)
{
  constexpr auto extensions = std::to_array<std::string_view>({".png", ".dds"});
  std::vector<HiresTextureFile> files;
  // Texture names seen so far, and whether each came from Orca's pack.
  std::unordered_map<std::string, bool> taken;
  const auto collect = [&](const std::string& texture_directory, bool orca_pack) {
    bool failed_insert = false;
    int kept_by_orca = 0;
    for (const std::string& path :
         Common::DoFileSearch(texture_directory, extensions, /*recursive*/ true))
    {
      std::string filename;
      SplitPath(path, nullptr, &filename, nullptr);
      if (filename.substr(0, s_format_prefix.length()) != s_format_prefix ||
          IsMipLevelFile(filename))
      {
        continue;
      }
      const size_t arb_index = filename.rfind("_arb");
      const bool has_arbitrary_mipmaps = arb_index != std::string::npos;
      if (has_arbitrary_mipmaps)
        filename.erase(arb_index, 4);
      const auto [it, inserted] = taken.try_emplace(filename, orca_pack);
      if (!inserted)
      {
        if (it->second && !orca_pack)
          ++kept_by_orca;
        else
          failed_insert = true;
        continue;
      }
      files.push_back({std::move(filename), path, has_arbitrary_mipmaps, orca_pack});
    }
    if (failed_insert)
    {
      ERROR_LOG_FMT(VIDEO, "One or more textures at path '{}' were already inserted",
                    texture_directory);
    }
    if (kept_by_orca > 0)
    {
      INFO_LOG_FMT(VIDEO,
                   "Orca: {} texture(s) at path '{}' replace the same textures as Orca's "
                   "own pack, whose are used in a session",
                   kept_by_orca, texture_directory);
    }
  };
  for (const std::string& texture_directory : orca_directories)
    collect(texture_directory, true);
  if (!IsModSession())
  {
    for (const std::string& texture_directory : player_directories)
      collect(texture_directory, false);
  }
  return files;
}

void HiresTexture::Shutdown()
{
  Clear();
  s_isolation_reported = false;
}

void HiresTexture::Update()
{
  const bool mod_session = IsModSession();
  if (mod_session && Orca::Mod::ActiveMod() && !s_isolation_reported)
  {
    // Count candidate files without loading or watching the player's packs. Log once even if
    // replacements are disabled; Clear() on a config change must not repeat the session report.
    const auto directories = GetTextureDirectoriesForFirstMatchingGameId(
        File::GetUserPath(D_HIRESTEXTURES_IDX), SConfig::GetInstance().GetGameIDsForTextures());
    constexpr auto extensions = std::to_array<std::string_view>({".png", ".dds"});
    size_t ignored = 0;
    for (const std::string& directory : directories)
    {
      for (const std::string& path : Common::DoFileSearch(directory, extensions, true))
      {
        std::string filename;
        SplitPath(path, nullptr, &filename, nullptr);
        if (filename.starts_with(s_format_prefix) && !IsMipLevelFile(filename))
          ++ignored;
      }
    }
    INFO_LOG_FMT(VIDEO,
                 "orca isolation save=clean textures=bundle-only ignored-user-textures={}", ignored);
    s_isolation_reported = true;
  }

  if (!g_ActiveConfig.bHiresTextures)
  {
    Clear();
    return;
  }

  const std::vector<std::string> game_ids = SConfig::GetInstance().GetGameIDsForTextures();
  const std::set<std::string> user_directories =
      mod_session ? std::set<std::string>{} :
                    GetTextureDirectoriesForFirstMatchingGameId(
                        File::GetUserPath(D_HIRESTEXTURES_IDX), game_ids);
  // Orca: in a session, also load Orca's own pack (Data/Sys/Orca/Textures/<game ID>), which
  // replaces the Online menu's labels.
  std::set<std::string> orca_directories;
  if (const Orca::Profile* profile = Orca::ActiveProfile();
      profile && profile->mod && !profile->mod->EngineTextures())
  {
    // Mod mode: only the bundle's orca/textures/. A flat bundle uses the engine's own
    // pack below. Player directories are neither registered nor watched in a mod session.
    if (!profile->mod->tree.Under("orca/textures/").empty())
      orca_directories.insert(profile->mod->dir + "orca/textures/");
  }
  else if (Orca::SessionActive() && (!mod_session || Orca::Mod::ActiveMod()))
  {
    // A requested mod waits for its profile before choosing any texture source.
    orca_directories = GetTextureDirectoriesForFirstMatchingGameId(
        File::GetSysDirectory() + "Orca/Textures/", game_ids);
  }

  // Watch these directories for any texture reloads
  for (const auto& texture_directory : orca_directories)
    s_file_library->Watch(texture_directory);
  for (const auto& texture_directory : user_directories)
    s_file_library->Watch(texture_directory);

  for (HiresTextureFile& file :
       CollectHiresTextureFiles({user_directories.begin(), user_directories.end()},
                                {orca_directories.begin(), orca_directories.end()}))
  {
    // As in Dolphin, a name registered by an earlier Update keeps its file, except that Orca's
    // pack always wins.
    if (file.orca_pack)
      s_hires_texture_id_to_arbmipmap.insert_or_assign(file.id, file.has_arbitrary_mipmaps);
    else if (!s_hires_texture_id_to_arbmipmap.emplace(file.id, file.has_arbitrary_mipmaps).second)
      continue;
    // Since this is just a texture (single file) the mapper doesn't really matter
    // just provide a string
    s_file_library->SetAssetIDMapData(file.id, std::map<std::string, std::filesystem::path>{
                                                   {"texture", StringToPath(file.path)}});

    // Orca's pack (a few small labels) is always preloaded so the game's own label never flashes
    // while ours loads. Player packs follow the cache setting.
    if (g_ActiveConfig.bCacheHiresTextures || file.orca_pack)
    {
      auto hires_texture =
          std::make_shared<HiresTexture>(file.has_arbitrary_mipmaps, std::move(file.id));
      static_cast<void>(hires_texture->LoadTexture());
      if (file.orca_pack)
        s_hires_texture_cache.insert_or_assign(hires_texture->GetId(), hires_texture);
      else
        s_hires_texture_cache.try_emplace(hires_texture->GetId(), hires_texture);
    }
  }

  const std::vector<std::string> game_ids_for_textures =
      SConfig::GetInstance().GetGameIDsForTextures();
  const std::string game_id_display = fmt::format("{}", fmt::join(game_ids_for_textures, "' or '"));

  std::string message;
  if (g_ActiveConfig.bCacheHiresTextures)
  {
    message = fmt::format("Preloading '{}' custom textures for '{}'", s_hires_texture_cache.size(),
                          game_id_display);
  }
  else
  {
    message = fmt::format("Found '{}' custom textures for '{}'",
                          s_hires_texture_id_to_arbmipmap.size(), game_id_display);
  }
  // Orca: in a session the top left belongs to the online overlay, and Orca's pack is always
  // loaded, so only log the count.
  if (Orca::SessionActive())
    INFO_LOG_FMT(VIDEO, "{}", message);
  else
    OSD::AddMessage(message, 10000);
}

void HiresTexture::Clear()
{
  s_hires_texture_cache.clear();
  s_hires_texture_id_to_arbmipmap.clear();
  s_file_library = std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();
}

std::shared_ptr<HiresTexture> HiresTexture::Search(const TextureInfo& texture_info)
{
  auto [base_filename, has_arb_mipmaps] = GetNameArbPair(texture_info);
  if (base_filename == "")
    return nullptr;

  if (auto iter = s_hires_texture_cache.find(base_filename); iter != s_hires_texture_cache.end())
  {
    return iter->second;
  }
  else
  {
    auto hires_texture = std::make_shared<HiresTexture>(has_arb_mipmaps, std::move(base_filename));
    if (g_ActiveConfig.bCacheHiresTextures)
    {
      s_hires_texture_cache.try_emplace(hires_texture->GetId(), hires_texture);
    }
    return hires_texture;
  }
}

HiresTexture::HiresTexture(bool has_arbitrary_mipmaps, std::string id)
    : m_has_arbitrary_mipmaps(has_arbitrary_mipmaps), m_id(std::move(id))
{
}

VideoCommon::TextureDataResource* HiresTexture::LoadTexture() const
{
  auto& system = Core::System::GetInstance();
  auto& custom_resource_manager = system.GetCustomResourceManager();
  return custom_resource_manager.GetTextureDataFromAsset(m_id, s_file_library);
}

std::set<std::string> GetTextureDirectoriesWithGameId(const std::string& root_directory,
                                                      const std::string& game_id)
{
  std::set<std::string> result;
  const std::string texture_directory = root_directory + game_id;

  if (File::Exists(texture_directory))
  {
    result.insert(texture_directory);
  }
  else
  {
    // If there's no directory with the region-specific ID, look for a 3-character region-free one
    const std::string region_free_directory = root_directory + game_id.substr(0, 3);

    if (File::Exists(region_free_directory))
    {
      result.insert(region_free_directory);
    }
  }

  const auto match_gameid_or_all = [game_id](const std::string& filename) {
    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    return basename == game_id || basename == game_id.substr(0, 3) || basename == "all";
  };

  // Look for any other directories that might be specific to the given gameid
  const auto files = Common::DoFileSearch(root_directory, ".txt", true);
  for (const auto& file : files)
  {
    if (match_gameid_or_all(file))
    {
      // The following code is used to calculate the top directory
      // of a found gameid.txt file
      // ex:  <root directory>/My folder/gameids/<gameid>.txt
      // would insert "<root directory>/My folder"
      const auto directory_path = file.substr(root_directory.size());
      const std::size_t first_path_separator_position = directory_path.find_first_of(DIR_SEP_CHR);
      result.insert(root_directory + directory_path.substr(0, first_path_separator_position));
    }
  }

  return result;
}

std::set<std::string>
GetTextureDirectoriesForFirstMatchingGameId(const std::string& root_directory,
                                            const std::vector<std::string>& game_ids)
{
  for (const auto& game_id : game_ids)
  {
    auto directories = GetTextureDirectoriesWithGameId(root_directory, game_id);
    if (!directories.empty())
      return directories;
  }
  return {};
}
