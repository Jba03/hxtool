/************************************************************
 # hxtool.cc: Modding tool for cpa .hx audio files
 * Copyright (c) 2024 Jba03 <jba03@jba03.xyz>
 ************************************************************/

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <map>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <SDL2/SDL.h>

#include <hx2.h>
#include <stream.h>

static bool Quit = false;
static bool WantsQuit = false;
static bool WantsSave = false;
static bool WantsLayout = true;

static const unsigned int W = 800;
static const unsigned int H = 500;

static SDL_Window *Window = nullptr;
static SDL_Renderer *Renderer = nullptr;
static ImGuiWindowFlags imgui_windowflags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
static ImVec4 ColorCoefficients = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

/**/
static hx_t *hx_ctx = nullptr;
static std::filesystem::path work_directory;
static std::filesystem::path current_file;
static std::filesystem::path DroppedFile = "";
static char* BasePath = nullptr;

static hx_entry* PlayingEvent = nullptr;
static hx_entry* SelectedEvent = nullptr;
static hx_entry* SelectedObject = nullptr;
static int SelectedEntryIndex = 0;

struct LogEntry {
  enum Type { Status, Info, Warning, Error} type;
  std::string text;
};

static struct std::vector<LogEntry> Log;

static std::map<std::string, std::fstream*> FileMap;

static bool IsResourceFile(std::filesystem::path p) {
  return p.extension() == ".hst" || p.extension() == ".HST" ||
  p.extension() == ".hos" || p.extension() == ".HOS";
}

static std::fstream* FindOrCreateFileStream(std::filesystem::path p) {
  std::string filename = work_directory.string() + p.filename().string();
  if (FileMap.find(filename) != FileMap.end()) return FileMap[filename];
  return FileMap[filename] = new std::fstream(filename, std::ios::binary | std::ios::in | std::ios::out);
}

static char* ReadCB(const char* fn, size_t pos, size_t *size, void* userdata) {
  std::string filename = fn;
  std::filesystem::path path(fn);
  
  std::fstream *fs = FindOrCreateFileStream(path);
    
  if (fs->is_open()) {
    fs->seekg(0, std::ios_base::end);
    size_t real_size = fs->tellg();
    if (*size > real_size) *size = real_size;
    fs->seekg(pos);
    
    char* data = (char*)malloc(*size);
    fs->read(data, *size);
    
    FileMap[filename] = fs;
    
    return data;
  }
  
  return nullptr;
}

static void WriteCB(const char* filename, void* data, size_t pos, size_t *size, void* userdata) {
  std::filesystem::path path(filename);
  std::fstream *fs = FindOrCreateFileStream(path);
  
  if (fs->is_open()) {
    fs->seekg(pos);
    fs->write((char*)data, *size);
    fs->close();
  } else {
    std::cerr << "err: " << strerror(errno) << "\n";
  }
}

static void ErrorCB(const char* str, void*) {
  Log.push_back({ LogEntry::Type::Warning, str });
}

static std::string ConfigFile() {
  std::filesystem::path path = BasePath;
  if (!std::filesystem::exists(path)) std::filesystem::create_directory(path);
  path += "/hxtool.cfg";
  return path.string();
}

static void SaveConfig() {
  FILE *fp = fopen(ConfigFile().c_str(), "w");
  fprintf(fp, "ThemeColor = %X\n", ImGui::ColorConvertFloat4ToU32(ColorCoefficients));
  fprintf(fp, "BorderLess = %d\n", SDL_GetWindowFlags(Window) & SDL_WINDOW_BORDERLESS);
  fclose(fp);
}

static void LoadConfig() {
  FILE *fp = fopen(ConfigFile().c_str(), "r");
  if (!fp) return;
  int color = 0xFFFFFFFF, borderless = 0;
  fscanf(fp, "ThemeColor = %X\n", &color);
  fscanf(fp, "BorderLess = %d\n", &borderless);
  ColorCoefficients = ImGui::ColorConvertU32ToFloat4(color);
  SDL_SetWindowBordered(Window, SDL_bool(!borderless));
  fclose(fp);
}

#pragma mark - Audio player

static int AudioLength = 0;
static int AudioPosition = 0;
static int AudioPositionTotal = 0;
static int AudioQueueIndex = 0;
static bool AudioRepeat = false;
static float AudioMixVolume = 0.5f;

static int AudioSampleRate = 0;
static int AudioChannelCount = 0;

static std::deque<hx_audio_stream*> AudioQueue;
static std::deque<hx_audio_stream*> AudioSwapQueue;

static void AudioClear() {
  for (auto& e : AudioQueue) {
    hx_audio_stream_dealloc(e);
    free(e);
  }
  
  for (auto& e : AudioSwapQueue) {
    hx_audio_stream_dealloc(e);
    free(e);
  }
  
  AudioLength = 0;
  AudioPositionTotal = 0;
  AudioQueueIndex = 0;
  AudioQueue.clear();
  AudioSwapQueue.clear();
}

static void AudioCallback(void*, Uint8 *stream, int len) {
  SDL_memset(stream, 0, len);
  
  unsigned int AudioRemaining = AudioLength - AudioPositionTotal;
  len = (len > AudioRemaining ? AudioRemaining : len);
  
  if (len == 0) {
    if (AudioRepeat) {
      printf("repeat!\n");
      AudioQueueIndex = 0;
      AudioPosition = 0;
      AudioPositionTotal = 0;
      AudioQueue.swap(AudioSwapQueue);
      return;
    } else {
      printf("pause!\n");
      AudioClear();
      SDL_PauseAudio(1);
      return;
    }
  }
  
  SDL_MixAudio(stream, (Uint8*)AudioQueue.front()->data + AudioPosition, len, std::clamp(AudioMixVolume, 0.0f, 1.0f) * SDL_MIX_MAXVOLUME);
  AudioPosition += len;
  AudioPositionTotal += len;
  
  if (AudioPosition >= AudioQueue.front()->size) {
    AudioSwapQueue.push_back(AudioQueue.front());
    AudioQueue.pop_front();
    AudioQueueIndex++;
    AudioPosition = 0;
  }
}

static int AudioLoad(hx_audio_stream_t *stream) {
  if (SDL_GetAudioStatus() != SDL_AUDIO_STOPPED) {
    AudioClear();
    SDL_CloseAudio();
  }
  
  if (!stream->data) {
    Log.push_back({ LogEntry::Type::Error, "failed to load audio stream: data not loaded!" });
    return -1;
  }
  
  AudioQueue.push_back(stream);
  return 1;
}

static void AudioPlay() {
  if (SDL_GetAudioStatus() == SDL_AUDIO_PLAYING)
    AudioClear();
  
  SDL_AudioSpec audio;
  SDL_memset(&audio, 0, sizeof(audio));
  audio.freq = 0;
  audio.format = AUDIO_S16;
  audio.channels = 0;
  audio.samples = 1; /* poll `sample_rate` times/sec */
  audio.callback = &AudioCallback;
  audio.userdata = &AudioQueue;
  
  for (auto enqueued : AudioQueue) {
    /* Decode the stream */
    hx_audio_stream* pcm = enqueued;
    if (pcm->info.fmt != HX_FORMAT_PCM) {
      pcm = (hx_audio_stream*)malloc(sizeof(*pcm));
      pcm->info.fmt = HX_FORMAT_PCM;
    }
    
    if (hx_audio_convert(enqueued, pcm) < 0) {
      Log.push_back({ LogEntry::Type::Error, "failed to load audio stream: unsupported codec " + std::string(hx_format_name(enqueued->info.fmt)) });
      return;
    }
//
//    switch (enqueued->info.codec) {
//      case HX_FORMAT_PCM: break;
//      case HX_FORMAT_DSP: dsp_decode(hx_ctx, enqueued, pcm); break;
//      default:
//        Log.push_back({ LogEntry::Type::Error, "failed to load audio stream: unsupported codec " + std::string(HX_FORMAT_name(enqueued->info.codec)) });
//        return;
//    }
//
    audio.channels = pcm->info.num_channels;
    audio.freq = pcm->info.sample_rate;
    AudioLength += pcm->size;
    
    /* Replace the stream */
    AudioQueue.pop_front();
    AudioQueue.push_back(pcm);
  }
  
  AudioSampleRate = audio.freq;
  AudioChannelCount = audio.channels;
  AudioPosition = 0;
  
  if (AudioQueue.size() > 0) {
    if (SDL_OpenAudio(&audio, NULL) < 0) {
      fprintf(stderr, "failed to open audio: %s\n", SDL_GetError());
      return;
    }
    
    SDL_PauseAudio(0);
  }
}

static void QueueAudioEntry(hx_entry_t* e) {
  if (e->i_class == HX_CLASS_EVENT_RESOURCE_DATA) {
    hx_event_resource_data_t *data = (hx_event_resource_data_t*)e->data;
    hx_entry_t *link = hx_context_find_entry(hx_ctx, data->link);
    if (link) {
      if (link->i_class == HX_CLASS_WAVE_RESOURCE_DATA) {
        hx_wav_resource_data_t *waveres = (hx_wav_resource_data_t*)link->data;
        link = hx_context_find_entry(hx_ctx, waveres->default_cuuid);
        if (link) {
          hx_wave_file_id_object_t *waveobj = (hx_wave_file_id_object_t*)link->data;
          if (AudioLoad(waveobj->audio_stream)) {
            PlayingEvent = e;
            AudioPlay();
          }
        }
      } else if (link->i_class == HX_CLASS_PROGRAM_RESOURCE_DATA) {
        hx_program_resource_data_t *progres = (hx_program_resource_data_t*)link->data;
        if (progres->num_links > 0) {
          bool success = false;
          for (int i = 0; i < progres->num_links; i++) {
            link = hx_context_find_entry(hx_ctx, progres->links[i]);
            if (link) {
              hx_wav_resource_data_t *waveres = (hx_wav_resource_data_t*)link->data;
              link = hx_context_find_entry(hx_ctx, waveres->default_cuuid);
              if (link) {
                hx_wave_file_id_object_t *waveobj = (hx_wave_file_id_object_t*)link->data;
                success |= AudioLoad(waveobj->audio_stream);
              }
            }
          }
          
          if (success) {
            PlayingEvent = e;
            AudioPlay();
          }
        }
      }
    }
  }
}

static ImColor Color(float r, float g, float b, float a) {
  return ImColor(r * ColorCoefficients.x, g * ColorCoefficients.y, b * ColorCoefficients.z, a);
}

static void Style() {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.AntiAliasedFill = true;
  style.FrameRounding = 5.0f;
  style.WindowPadding = ImVec2(4,4);
  
  
  style.Colors[ImGuiCol_Text] = Color(0.9f, 0.7f, 1.0f, 1.0f);
  style.Colors[ImGuiCol_TextDisabled] = Color(0.9f, 0.66f, 1.0f, 0.5f);
  style.Colors[ImGuiCol_Separator] = Color(0.9f, 0.6f, 1.0f, 0.1f);
  style.Colors[ImGuiCol_Button] = Color(0.9f, 0.6f, 1.0f, 0.25f);
  style.Colors[ImGuiCol_ButtonHovered] = Color(0.9f, 0.6f, 1.0f, 0.4f);
  style.Colors[ImGuiCol_ButtonActive] = Color(0.9f, 0.6f, 1.0f, 0.6f);
  
  style.Colors[ImGuiCol_TitleBg] = Color(0.1f, 0.1f, 0.1f, 1.0f);
  style.Colors[ImGuiCol_TitleBgActive] = Color(0.15f, 0.15f, 0.15f, 1.0f);
  style.Colors[ImGuiCol_Border] = Color(0.7f, 0.5f, 1.0f, 0.125f);
  style.Colors[ImGuiCol_Tab] = Color(1.0f, 1.0f, 1.0f, 0.1f);
  style.Colors[ImGuiCol_TabDimmed] = Color(1.0f, 1.0f, 1.0f, 0.1f);
  style.Colors[ImGuiCol_TabDimmedSelected] = Color(1.0f, 1.0f, 1.0f, 0.1f);
  style.Colors[ImGuiCol_TabHovered] = Color(1.0f, 1.0f, 1.0f, 0.25f);
  style.Colors[ImGuiCol_TabActive] = Color(1.0f, 1.0f, 1.0f, 0.1f);
  style.Colors[ImGuiCol_TabSelected] = Color(1.0f, 1.0f, 1.0f, 0.1f);
  style.Colors[ImGuiCol_TabSelectedOverline] = Color(1.0f, 1.0f, 1.0f, 0.0f);
  style.Colors[ImGuiCol_TabDimmedSelectedOverline] = Color(1.0f, 1.0f, 1.0f, 0.0f);
  style.Colors[ImGuiCol_TabUnfocused] = Color(1.0f, 1.0f, 1.0f, 0.1f);
  style.Colors[ImGuiCol_TextSelectedBg] = Color(1.0f, 1.0f, 1.0f, 0.1f);
  
  style.Colors[ImGuiCol_Header] = Color(0.6f, 0.5f, 1.0f, 0.25f);
  style.Colors[ImGuiCol_HeaderHovered] = Color(0.6f, 0.4f, 1.0f, 0.25f);
  style.Colors[ImGuiCol_HeaderActive] = Color(0.6f, 0.4f, 1.0f, 0.5f);
  
  style.Colors[ImGuiCol_MenuBarBg] = Color(0.15f, 0.1f, 0.2f, 1.0f);
  style.Colors[ImGuiCol_WindowBg] = Color(0.05f*1.3, 0.025f*1.3, 0.075f*1.3, 1.0f);
  
  style.Colors[ImGuiCol_SliderGrab] = Color(0.6f, 0.4f, 0.75f, 1.0f);
  style.Colors[ImGuiCol_SliderGrabActive] = Color(0.75f, 0.55f, 0.9f, 1.0f);
  
  style.Colors[ImGuiCol_FrameBg] = Color(0.15f, 0.1f, 0.2f, 0.75f);
  style.Colors[ImGuiCol_FrameBgHovered] = Color(0.6f, 0.4f, 0.75f, 0.5f);
  style.Colors[ImGuiCol_FrameBgActive] = Color(0.6f, 0.4f, 0.75f, 0.75);
  
  style.Colors[ImGuiCol_TableHeaderBg] = Color(0.8f, 0.5f, 1.0f, 0.25f);
  style.Colors[ImGuiCol_TableBorderLight] = Color(1.0f, 0.8f, 1.0f, 0.1f);
  style.Colors[ImGuiCol_TableBorderStrong] = Color(1.0f, 0.8f, 1.0f, 0.1f);
  style.Colors[ImGuiCol_TableRowBg] = Color(1.0f, 0.8f, 1.0f, 0.1f);
  style.Colors[ImGuiCol_TableRowBgAlt] = Color(1.0f, 0.8f, 1.0f, 0.2f);
  
  style.Colors[ImGuiCol_PopupBg] = Color(0.10f, 0.05f, 0.15f, 1.0f);
  style.Colors[ImGuiCol_ModalWindowDimBg] = Color(0.0f, 0.0f, 0.0f, 0.5f);
  
  style.Colors[ImGuiCol_CheckMark] = Color(0.0f, 1.0f, 0.5f, 0.75f);
}

static void Layout(ImGuiID dockspaceId) {
  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoDockingOverCentralNode);
  ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);
  
  ImGuiID dock_main_id = dockspaceId;
  ImGuiID left3 = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.25f, nullptr, &dock_main_id);
  ImGuiID middle1 = dock_main_id;
  ImGuiID middle2 = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Down, 0.45f, nullptr, &middle1);
  ImGuiID middle3 = ImGui::DockBuilderSplitNode(middle2, ImGuiDir_Down, 0.66f, nullptr, &middle2);
  ImGuiID right1 = ImGui::DockBuilderSplitNode(middle1, ImGuiDir_Right, 0.33f, nullptr, &middle1);
  
  ImGui::DockBuilderDockWindow("Events", left3);
  ImGui::DockBuilderDockWindow("Info", middle1);
  ImGui::DockBuilderDockWindow("Audio Player", middle2);
  ImGui::DockBuilderDockWindow("Log Window", middle3);
  ImGui::DockBuilderDockWindow("Object Window", right1);
  
  ImGui::DockBuilderFinish(dockspaceId);
  Style();
}

static ImVec4 EntryColor(hx_entry_t *e) {
  switch (e->i_class) {
    case HX_CLASS_EVENT_RESOURCE_DATA: {
      hx_event_resource_data_t *data = static_cast<hx_event_resource_data_t*>(e->data);
      float y = 2.2f * (uint8_t(data->name[6] *-2) / 255.0f);
      float w = 6.2f * (uint8_t(data->name[7] *-1) / 255.0f);
      return ImVec4(0.6f*y, 0.5f, 1.0f*w, 1.0f);
    }
      
    case HX_CLASS_WAVE_RESOURCE_DATA: {
      return ImVec4(1.0f, 0.7f, 0.1f, 0.5f);
    }
      
    case HX_CLASS_WAVE_FILE_ID_OBJECT: {
      return ImVec4(1.0f, 0.7f, 0.1f, 0.9f);
    }
      
    default: return ImVec4(1.0f, 1.0f, 1.0f, 0.6f);
  }
}

static bool DrawPlayButton(int id, bool paused = false, bool s = true) {
  ImVec2 size = s ? ImVec2(15,15) : ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
  ImVec4 text = paused ? ImVec4(1.0f, 0.75f, 0.25f, 0.75f) : ImVec4(0.25f, 1.0f, 0.43f, 0.75f);
  ImVec4 background = ImVec4(text.x, text.y, text.z, 0.15f);
  ImGui::PushStyleColor(ImGuiCol_Text, paused ? ImVec4(0,0,0,0) : text);
  ImGui::PushStyleColor(ImGuiCol_Button, background);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
  
  ImVec2 cursor = ImGui::GetCursorScreenPos();
  s = ImGui::ArrowButtonEx(std::to_string(id).c_str(), ImGuiDir_Right, size);
  
  if (paused) {
    ImDrawList *drawlist = ImGui::GetWindowDrawList();
    ImVec2 p0 = cursor;
    p0.x += size.x / 4.0f+1;
    p0.y += size.y / 4.0f;
    ImVec2 p1 = ImVec2(p0.x, p0.y + size.y/2.0f);

    drawlist->AddLine(p0, p1, ImGui::GetColorU32(text), 2);
    p0.x = cursor.x + size.x - size.x / 3.0-1;
    p1.x = cursor.x + size.x - size.x / 3.0-1;
    drawlist->AddLine(p0, p1, ImGui::GetColorU32(text), 2);
  }
  
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
  return s;
}

static void DrawAudioPlayer() {
  ImGui::Begin("Audio Player", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 0.75f, 1.0f, 0.025f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
  ImGui::BeginChild("AudioPlayerGroup", ImVec2(ImGui::GetContentRegionAvail().x / 1.5f, 0.0f), ImGuiChildFlags_Border);
    if (PlayingEvent) {
      ImGui::Text("%s", PlayingEvent ? ((hx_event_resource_data*)PlayingEvent->data)->name : "");
      ImGui::SameLine();
      ImGui::TextDisabled("B:%d/%d Q:%d/%d\n", AudioPositionTotal, AudioLength, AudioQueueIndex+1, AudioQueue.size()+AudioSwapQueue.size());
      
      
      ImGui::SameLine();
      ImVec2 p = ImGui::GetCursorPos();
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetTextLineHeightWithSpacing() / 2.0f - 1);
      
      ImDrawList *drawlist =  ImGui::GetWindowDrawList();
      drawlist->AddCircle(ImGui::GetCursorScreenPos(), 5.0f, ImColor(1.0f, 0.5f, 0.2f, 0.25f));
      if (AudioQueue.size()>0) drawlist->PathArcTo(ImGui::GetCursorScreenPos(), 5.0f, -M_PI_2, float(AudioPosition) / float(AudioQueue.front()->size) * M_PI * 2 - M_PI_2);
      drawlist->PathStroke(ImColor(1.0f,0.8f,0.3f,1.0f), 0, 2.0f);
      
      ImGui::SetCursorPos(p);
      ImGui::NewLine();
      
      
      unsigned int bytes_per_sec = AudioChannelCount * AudioSampleRate * 2;
      float sec = float(AudioLength - AudioPositionTotal) / bytes_per_sec;
      int min = int(sec / 60.0f) % 60;
      
      char buf[HX_STRING_MAX_LENGTH];
      snprintf(buf, HX_STRING_MAX_LENGTH, "%02d:%02d:%06.3f", 0, min, fmod(sec, 60.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0,2));
      ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 5.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 1.0f, 1.0f));
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::SliderInt("##Duration", &AudioPositionTotal, 0, AudioLength, buf, ImGuiSliderFlags_ReadOnly | ImGuiSliderFlags_NoInput);
      ImGui::PopStyleColor();
      ImGui::PopStyleVar(2);
      
      SDL_AudioStatus status = SDL_GetAudioStatus();
      if (DrawPlayButton(0, status != SDL_AUDIO_PAUSED, false)) {
        if (AudioQueue.size() > 0) {
          SDL_PauseAudio(status != SDL_AUDIO_PAUSED);
        } else {
          /* Enqueue the last played event */
          QueueAudioEntry(PlayingEvent);
        }
      }
      
      ImGui::SameLine();
    } else {
      ImGui::TextDisabled("The audio queue is empty.");
    }
  
  ImGui::Checkbox("Repeat", &AudioRepeat);
  ImGui::SameLine();
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 5.0f);
  ImGui::SetNextItemWidth(100.0f);
  ImGui::SliderFloat("Volume", &AudioMixVolume, 0.0f, 1.0f);
  ImGui::PopStyleVar();
  ImGui::SameLine();
  ImGui::EndChild();
  
  ImGui::SameLine();
  ImGui::BeginChild("AudioQueueGroup", ImVec2(0,0), ImGuiChildFlags_Border);
  SDL_LockAudio();
  for (auto e : AudioQueue) (e==AudioQueue.front()?ImGui::Text:ImGui::TextDisabled)("%016llX\n", e->wavefile_cuuid);
  SDL_UnlockAudio();
  ImGui::EndChild();
  
  
  
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
    
  ImGui::End();
}

static void EntryTableTree(hx_entry_t* root, int depth, std::string info = "--") {
  if (!root) return;
 
  ImGui::TableNextColumn();
  
  char out[HX_STRING_MAX_LENGTH];
  
  if (root->i_class == HX_CLASS_EVENT_RESOURCE_DATA) {
    hx_event_resource_data_t *data = static_cast<hx_event_resource_data_t*>(root->data);
    snprintf(out, HX_STRING_MAX_LENGTH, "%s\n", data->name);
  } else {
    snprintf(out, HX_STRING_MAX_LENGTH, "%016llX\n", root->cuuid);
  }
    
  
  
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + depth * 10);
  
  ImGuiTableFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanAllColumns;
  
  std::vector<hx_entry_t*> next;
  std::vector<std::string> info_v;
  
  ImGui::PushStyleColor(ImGuiCol_Text, EntryColor(root));
  if (ImGui::TreeNodeEx(out, flags)) {
    
    if (root->i_class == HX_CLASS_EVENT_RESOURCE_DATA) {
      hx_event_resource_data_t *data = static_cast<hx_event_resource_data_t*>(root->data);
      next.push_back(hx_context_find_entry(hx_ctx, data->link));
    }
    
    if (root->i_class == HX_CLASS_WAVE_RESOURCE_DATA) {
      hx_wav_resource_data_t *data = static_cast<hx_wav_resource_data_t*>(root->data);
      if (data->default_cuuid) next.push_back(hx_context_find_entry(hx_ctx, data->default_cuuid));
      for (unsigned int i = 0; i < data->num_links; i++) {
        next.push_back(hx_context_find_entry(hx_ctx, data->links[i].cuuid));
        switch(HX_BYTESWAP32(data->links[i].language)) {
          case HX_LANGUAGE_DE: info_v.push_back("DE"); break;
          case HX_LANGUAGE_EN: info_v.push_back("EN"); break;
          case HX_LANGUAGE_ES: info_v.push_back("ES"); break;
          case HX_LANGUAGE_FR: info_v.push_back("FR"); break;
          case HX_LANGUAGE_IT: info_v.push_back("IT"); break;
          default: break;
        }
      }
    }
    
    if (root->i_class == HX_CLASS_PROGRAM_RESOURCE_DATA) {
      hx_program_resource_data_t *data = static_cast<hx_program_resource_data_t*>(root->data);
      for (unsigned int i = 0; i < data->num_links; i++) {
        next.push_back(hx_context_find_entry(hx_ctx, data->links[i]));
      }
    }
    
    ImGui::TreePop();
  }
  
  if (ImGui::IsItemClicked()) {
    SelectedObject = root;
  }
  
  ImGui::PopStyleColor();
  
  ImGui::TableNextColumn();
  ImGui::TextDisabled(info.c_str());
  
  ImGui::TableNextColumn();
  hx_class_name(root->i_class, hx_context_version(hx_ctx), out, HX_STRING_MAX_LENGTH);
  
  ImGui::TextDisabled(out);
  
  for (unsigned int i = 0; i < next.size(); i++) {
    hx_entry_t *e = next.at(i);
    ImGui::TableNextRow();
    EntryTableTree(e, depth + 1, info_v.size() > 0 ? info_v[i] : "--");
  }
}

static void DrawInfo() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
  ImGui::Begin("Info", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  if (SelectedEvent) {
    const ImGuiTableFlags flags =
    ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBody;

    if (ImGui::BeginTable("Entries", 3, flags)) {
      ImGui::TableSetupColumn("Name/uuid", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthFixed, 50);
      ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 120);
      ImGui::TableHeadersRow();
      EntryTableTree(SelectedEvent, 0);
      ImGui::EndTable();
    }
  }
  
  ImGui::End();
  ImGui::PopStyleVar();
}

static int ReplaceWaveFile(hx_wave_file_id_object_t *data, std::filesystem::path file) {
  if (file.extension() != ".wav") return -1;
  
  Uint8* buf = nullptr;
  Uint32 sz = 0;
  
  SDL_AudioSpec spec;
  if (!SDL_LoadWAV(file.c_str(), &spec, &buf, &sz)) {
    Log.push_back({ LogEntry::Type::Error, "Failed to load .wav file " + file.string() + ": " + SDL_GetError() });
    return -1;
  }
  
  enum hx_format wanted_format = data->audio_stream->info.fmt;
  
  
  if (SDL_AUDIO_BITSIZE(spec.format) != 16) {
    /* Conversion needed */
    SDL_AudioCVT cvt;
    SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, spec.channels, spec.freq);
    
    cvt.len = sz;
    cvt.buf = buf;
    SDL_ConvertAudio(&cvt);
    
    return -1;
  }
  
  hx_audio_stream_t pcm;
  pcm.size = sz;
  pcm.data = (short*)buf;
  pcm.info.fmt = HX_FORMAT_PCM;
  pcm.info.sample_rate = spec.freq;
  pcm.info.num_channels = spec.channels;
  pcm.info.endianness = spec.format & SDL_AUDIO_MASK_ENDIAN;
  pcm.info.num_samples = (sz / (pcm.info.num_channels * sizeof(short)));  //spec.samples * spec.freq; //(pcm.size / data->wave_header.block_alignment) / pcm.info.num_channels;;
  
  Log.push_back({ LogEntry::Type::Info, "Encoding " + file.filename().string() + " (" + hx_format_name(pcm.info.fmt) + " -> " + hx_format_name(wanted_format) + ")" });
  
//  switch (wanted_codec) {
//    case HX_FORMAT_PCM:
//      *data->audio_stream = pcm;
//      break;
//    case HX_FORMAT_DSP:
//      dsp_encode(hx_ctx, &pcm, data->audio_stream);
//      break;
//    default:
//      break;
//  }
  
  if (hx_audio_convert(&pcm, data->audio_stream) < 0) {
    Log.push_back({ LogEntry::Type::Error, "Failed to convert audio stream: unsupported formats" });
    return;
  }
  
  SDL_free(buf);
}

static void DrawObjectWindow() {
  ImGui::Begin("Object Window");
  if (SelectedObject) {
    char name[HX_STRING_MAX_LENGTH];
    snprintf(name, HX_STRING_MAX_LENGTH, "%016llX\n", SelectedObject->cuuid);
    ImGui::Text("%s", name);
    
    hx_class_name(SelectedObject->i_class, hx_context_version(hx_ctx), name, HX_STRING_MAX_LENGTH);
    ImGui::TextDisabled("%s @ %X\n", name, SelectedObject->file_offset);
    
    ImGui::Separator();
    ImGui::Spacing();
    
    if (SelectedObject->i_class == HX_CLASS_EVENT_RESOURCE_DATA) {
      hx_event_resource_data_t *data = static_cast<hx_event_resource_data_t*>(SelectedObject->data);
      ImGui::InputText("Name", data->name, HX_STRING_MAX_LENGTH);
      ImGui::InputFloat("C0", &data->c[0]);
      ImGui::InputFloat("C1", &data->c[1]);
      ImGui::InputFloat("C2", &data->c[2]);
      ImGui::InputFloat("C3", &data->c[3]);
    } else if (SelectedObject->i_class == HX_CLASS_WAVE_RESOURCE_DATA) {
      hx_wav_resource_data_t *data = static_cast<hx_wav_resource_data_t*>(SelectedObject->data);
      ImGui::InputScalar("Flags", ImGuiDataType_S8, &data->res_data.flags);
      ImGui::InputFloat("C0", &data->res_data.c[0]);
      ImGui::InputFloat("C1", &data->res_data.c[1]);
      ImGui::InputFloat("C2", &data->res_data.c[2]);
    } else if (SelectedObject->i_class == HX_CLASS_WAVE_FILE_ID_OBJECT) {
      hx_wave_file_id_object_t *data = static_cast<hx_wave_file_id_object_t*>(SelectedObject->data);
      ImGui::TextDisabled("%s, (%d) ch %s", data->ext_stream_size==0 ? "Internal" : "External", data->audio_stream->info.num_channels, hx_format_name(data->audio_stream->info.fmt));
      ImGui::TextDisabled("Size: %d bytes", hx_audio_stream_size(data->audio_stream));
      
      if (data->ext_stream_size>0) {
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("Ext. File", data->ext_stream_filename, HX_STRING_MAX_LENGTH);
        ImGui::TextDisabled("(offset 0x%X)", data->ext_stream_offset);
      }
      
      ImGui::SetNextItemWidth(100);
      if (ImGui::InputScalar("Sample rate", ImGuiDataType_U32, &data->audio_stream->info.sample_rate, nullptr, nullptr, "%d Hz")) {
        data->audio_stream->info.sample_rate = std::clamp((int)data->audio_stream->info.sample_rate, 1, 88200);
      }
      
      
      ImGui::Spacing();
      
//      static bool hovered = false;
//      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 0.75f, 1.0f, hovered ? 0.1f : 0.025f));
//      ImGui::BeginChild("##WavDragDrop", ImVec2(0,-1), ImGuiChildFlags_Border);
//      ImGui::TextWrapped("Drop a .wav file here to replace this stream");
//      ImGui::EndChild();
//      ImGui::PopStyleColor();
//      hovered = ImGui::IsItemHovered();
//
//      if (DroppedFile.string().length()>0 && ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) {
//        ReplaceWaveFile(data, DroppedFile);
//      }
    }
  }
  ImGui::End();
}

static void DrawEntries() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4,4));
  ImGui::Begin("Events", NULL, ImGuiWindowFlags_NoDecoration & ~ImGuiWindowFlags_NoScrollbar);
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2,1));
  
  if (hx_ctx) {
    if (ImGui::BeginTable("table", 2, ImGuiTableFlags_SizingFixedFit)) {
      for (hx_size_t i = 0; i < hx_context_num_entries(hx_ctx); i++) {
        hx_entry_t *entry = hx_context_get_entry(hx_ctx, i);
        if (entry->i_class == HX_CLASS_EVENT_RESOURCE_DATA) {
          hx_event_resource_data_t *data = (hx_event_resource_data_t*)entry->data;
          
          ImVec4 color = (i == SelectedEntryIndex) ? ImVec4(1.0f, 0.7f, 0.4f, 1.0f) : EntryColor(entry);
          ImGui::PushStyleColor(ImGuiCol_Text, color);
          ImGui::TableNextColumn();
          
          ImGui::SetCursorPosY(ImGui::GetCursorPosY()-1);
          
          if (DrawPlayButton(i, AudioPositionTotal != 0 && PlayingEvent == entry && SDL_GetAudioStatus() == SDL_AUDIO_PLAYING)) {
            if (PlayingEvent && PlayingEvent == entry && SDL_GetAudioStatus() == SDL_AUDIO_PLAYING) {
              AudioClear();
              PlayingEvent = nullptr;
              SDL_CloseAudio();
            } else {
              QueueAudioEntry(entry);
            }
          }
          
          ImGui::TableNextColumn();
          
          if (ImGui::Selectable(data->name, SelectedEntryIndex == i)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
              printf("left\n");
            }
            
            
            SelectedEvent = entry;
            SelectedEntryIndex = i;
          }
          
          
          ImGui::PopStyleColor();
        }
      }
      
      ImGui::EndTable();
    }
  }
  
  ImGui::PopStyleVar(2);
  ImGui::End();
}

static size_t LastLogNumEntries = 0;

static void DrawLog() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3,2));
  ImGui::Begin("Log Window");
  
  if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Right)) {
    Log.clear();
  }
  
  for (auto& line : Log) {
    ImVec4 color;
    std::string type;
    if (line.type == LogEntry::Status) color = ImVec4(0.3f, 1.0f, 0.6f, 1.0);
    if (line.type == LogEntry::Info) color = ImVec4(0.2f, 0.5f, 1.0f, 1.0);
    if (line.type == LogEntry::Warning) color = ImVec4(1.0f, 0.6f, 0.0f, 1.0);
    if (line.type == LogEntry::Error) color = ImVec4(1.0f, 0.3f, 0.4f, 1.0);
    if (line.type == LogEntry::Status) type = "status";
    if (line.type == LogEntry::Info) type = "info";
    if (line.type == LogEntry::Warning) type = "warning";
    if (line.type == LogEntry::Error) type = "error";
    
    ImVec4 color2 = color;
    color2.w = 0.75f;
    ImGui::TextColored(color, "[%s]", type.c_str());
    ImGui::SameLine();
    
    ImGui::PushStyleColor(ImGuiCol_Text, color2);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0,1));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputText(("##" + std::to_string(&line - Log.data())).c_str(), (char*)line.text.c_str(), line.text.length(), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
  }
  
  if (LastLogNumEntries != Log.size()) ImGui::SetScrollHereY();
    
  ImGui::End();
  ImGui::PopStyleVar();
  
  LastLogNumEntries = Log.size();
}

static void Save() {
  hx_context_write(hx_ctx, "out.hxc", HX_VERSION_HXC);
  Log.push_back({ LogEntry::Type::Status, "Successfully saved out.hxc" });
}

static void DrawMainMenuBar() {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
     // if (ImGui::MenuItem("Save")) Save();
      ImGui::Separator();
      if (ImGui::MenuItem("Exit")) WantsQuit = true;
      ImGui::EndMenu();
    }
    
    if (ImGui::BeginMenu("Options")) {
//      if (ImGui::BeginMenu("Default audio language")) {
//        ImGui::MenuItem("English", nullptr, true);
//        ImGui::MenuItem("German", nullptr, false);
//        ImGui::MenuItem("Spanish", nullptr, false);
//        ImGui::MenuItem("French", nullptr, false);
//        ImGui::MenuItem("Italian", nullptr, false);
//        ImGui::EndMenu();
//      }
      
      if (ImGui::BeginMenu("Style")) {
        SDL_bool borderless = SDL_bool(SDL_GetWindowFlags(Window) & SDL_WINDOW_BORDERLESS);
        if (ImGui::MenuItem("Borderless window", nullptr, borderless)) {
          SDL_SetWindowBordered(Window, borderless);
        }
        
        ImGui::SetColorEditOptions(ImGuiColorEditFlags_NoInputs);
        if (ImGui::ColorEdit4("Theme color", &ColorCoefficients.x)) Style();
        
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          SaveConfig();
        }
        
        ImGui::EndMenu();
      }
    
      ImGui::EndMenu();
    }
    
    if (SDL_GetWindowFlags(Window) & SDL_WINDOW_BORDERLESS) {
      const char* txt = current_file.filename().c_str();
      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x / 2.0f - ImGui::CalcTextSize(txt).x / 2.0f);
      ImGui::TextDisabled("%s", txt);
    }
      
    ImGui::EndMainMenuBar();
  }
}

static void DrawCloseDialog() {
  if (ImGui::BeginPopupModal("##CloseDialog", nullptr, ImGuiWindowFlags_NoDecoration)) {
    ImGui::Text("There are unsaved changes. Exit anyway?");
    if (ImGui::Button("Yes")) Quit = true;
    ImGui::SameLine();
    if (ImGui::Button("No")) {
      ImGui::CloseCurrentPopup();
      WantsQuit = false;
    }
    ImGui::EndPopup();
  }
}

static void Draw() {
  DrawMainMenuBar();
  
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::SetNextWindowViewport(viewport->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  
  ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking;
  window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
  window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDecoration;
  
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin("DockSpace Demo", nullptr, window_flags);
  
  ImGui::PopStyleVar(3);
  
  ImGuiID dockspaceId = ImGui::GetID("DockSpace");
  ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoWindowMenuButton | ImGuiDockNodeFlags_NoTabBar);
  
  if (WantsLayout) {
    Layout(dockspaceId);
    WantsLayout = false;
  }
    
  DrawEntries();
  DrawInfo();
  DrawAudioPlayer();
  DrawLog();
  DrawObjectWindow();
  DrawCloseDialog();
  
  if (WantsQuit) ImGui::OpenPopup("##CloseDialog");
  
  ImGui::End();
}


static void LoadHXFile(std::filesystem::path path) {
  std::string extension = path.extension().string();
  if (extension.starts_with(".hx") || extension.starts_with(".HX")) {
    if (hx_ctx) {
      SDL_CloseAudio();
      AudioClear();
      hx_context_free(&hx_ctx);
      hx_ctx = nullptr;
      PlayingEvent = nullptr;
      SelectedObject = nullptr;
    }
    
    current_file = path.filename();
    work_directory = path;
    work_directory.remove_filename();
    
    hx_ctx = hx_context_alloc();
    hx_context_callback(hx_ctx, &ReadCB, &WriteCB, &ErrorCB, NULL);
    
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    if (hx_context_open(hx_ctx, path.string().c_str()) < 0) {
      Log.push_back({ LogEntry::Type::Error, "Failed to load file" + path.string() });
      return;
    }
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    Log.push_back({ LogEntry::Type::Status, "Loaded " + path.filename().string() + " in " +
      std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count() / 1'000'000'000.0f) + " seconds." });
    
    SelectedEvent = hx_context_get_entry(hx_ctx, 0);
    
    SDL_SetWindowTitle(Window, ("hxtool - " + current_file.string()).c_str());
    
  }
}

static int DrawUI(void* = nullptr, SDL_Event *event = nullptr) {
  if (event) {
    if (event->window.event != SDL_WINDOWEVENT_EXPOSED) return 0;
  }
  
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  
  Draw();
  
  ImGui::Render();
  
  ImGuiIO& io = ImGui::GetIO();
  SDL_RenderSetScale(Renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
  SDL_SetRenderDrawColor(Renderer, 10, 10, 10, 255);
  SDL_RenderClear(Renderer);
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), Renderer);
  SDL_RenderPresent(Renderer);
  
  return 1;
}

static void HandleEvent(SDL_Event *e) {
  ImGui_ImplSDL2_ProcessEvent(e);
  switch (e->type) {
    case SDL_DROPFILE:
      DroppedFile = e->drop.file;
      break;
    case SDL_QUIT:
      WantsQuit = true;
    default:
      break;
  }
}

int main(int argc, char** argv) {
  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
  Window = SDL_CreateWindow("hxtool", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  Renderer = SDL_CreateRenderer(Window, -1, SDL_RENDERER_PRESENTVSYNC);
  SDL_SetWindowMinimumSize(Window, W, H);
  
  BasePath = SDL_GetBasePath();
  
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.IniFilename = nullptr;
  
  ImGui_ImplSDL2_InitForSDLRenderer(Window, Renderer);
  ImGui_ImplSDLRenderer2_Init(Renderer);
  
  Log.push_back({ LogEntry::Type::Info, "Drag and drop: .hxc, .hx2, .hxg" });
  
  Style();
  LoadConfig();
  
  SDL_AddEventWatch(DrawUI, nullptr);
  
  while (!Quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) HandleEvent(&e);
    
    DrawUI();
    
    if (WantsQuit) Quit = true;
    if (!DroppedFile.empty()) LoadHXFile(DroppedFile);
    DroppedFile.clear();
  }
  
  SaveConfig();
  SDL_free(BasePath);
  
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_DestroyRenderer(Renderer);
  SDL_DestroyWindow(Window);
  SDL_Quit();
  
  return 0;
}
