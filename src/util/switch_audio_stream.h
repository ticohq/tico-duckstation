// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "util/audio_stream.h"

#include <atomic>
#include <switch.h>

/// Audio through the Switch's audio renderer service: a thread refills two wave buffers from the stream.
class SwitchAudioStream final : public AudioStream
{
public:
  SwitchAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
  ~SwitchAudioStream();

  void SetPaused(bool paused) override;

  bool Initialize(Error* error);

private:
  void DestroyContextAndStream();

  static void AudioThread(void* userdata);

  AudioDriver m_audio_driver;
  u8* m_mem_pool = nullptr;
  Thread m_audio_thread;
  u32 m_audio_thread_buffer_size = 0;

  enum class State
  {
    Paused,
    Playing,
    Stop
  };

  std::atomic<State> m_state = State::Playing;
};
