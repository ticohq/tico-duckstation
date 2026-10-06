#include "switch_audio_stream.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"

#include <cstdlib>
#include <cstring>
#include <switch.h>
#include "fmt/printf.h"
Log_SetChannel(SwitchAudioStream);

// the audio renderer voice is stereo
static constexpr u32 kOutputChannels = 2;

SwitchAudioStream::SwitchAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
  : AudioStream(sample_rate, parameters)
{
}

SwitchAudioStream::~SwitchAudioStream()
{
  DestroyContextAndStream();
}

void SwitchAudioStream::SetPaused(bool paused)
{
  m_paused = paused;
  m_state = paused ? State::Paused : State::Playing;
}

bool SwitchAudioStream::Initialize(Error* error)
{
  static const AudioRendererConfig ar_config = {
    .output_rate = AudioRendererOutputRate_48kHz,
    .num_voices = 4,
    .num_effects = 0,
    .num_sinks = 1,
    .num_mix_objs = 1,
    .num_mix_buffers = 2,
  };
  Result r = audrenInitialize(&ar_config);
  if (R_FAILED(r))
  {
    Error::SetStringFmt(error, "audrenInitialize() failed: 0x{:08X}", r);
    return false;
  }

  r = audrvCreate(&m_audio_driver, &ar_config, 2);
  if (R_FAILED(r))
  {
    audrenExit();
    Error::SetStringFmt(error, "audrvCreate() failed: 0x{:08X}", r);
    return false;
  }

  const u32 num_frames = GetBufferSizeForMS(
    m_sample_rate, (m_parameters.output_latency_ms == 0) ? m_parameters.buffer_ms : m_parameters.output_latency_ms);
  u32 pool_size = num_frames * kOutputChannels * sizeof(int16_t) * 2;
  pool_size = (pool_size + AUDREN_MEMPOOL_ALIGNMENT - 1) & ~(AUDREN_MEMPOOL_ALIGNMENT - 1);
  m_mem_pool = reinterpret_cast<u8*>(aligned_alloc(AUDREN_MEMPOOL_ALIGNMENT, pool_size));
  int mpid = audrvMemPoolAdd(&m_audio_driver, m_mem_pool, pool_size);
  audrvMemPoolAttach(&m_audio_driver, mpid);

  m_audio_thread_buffer_size = num_frames;

  static const u8 channel_ids[] = {0, 1};
  audrvDeviceSinkAdd(&m_audio_driver, AUDREN_DEFAULT_DEVICE_NAME, 2, channel_ids);

  audrvUpdate(&m_audio_driver);
  audrenStartAudioRenderer();

  audrvVoiceInit(&m_audio_driver, 0, 2, PcmFormat_Int16, m_sample_rate);
  audrvVoiceSetDestinationMix(&m_audio_driver, 0, AUDREN_FINAL_MIX_ID);
  audrvVoiceSetMixFactor(&m_audio_driver, 0, 1.f, 0, 0);
  audrvVoiceSetMixFactor(&m_audio_driver, 0, 1.f, 1, 1);
  audrvVoiceStart(&m_audio_driver, 0);

  threadCreate(&m_audio_thread, SwitchAudioStream::AudioThread, this, nullptr, 1024 * 128, 0x20, 0);
  threadStart(&m_audio_thread);

  // stereo out; the volume is applied as the frames are read
  BaseInitialize(&StereoSampleReaderImpl);

  return true;
}

void SwitchAudioStream::DestroyContextAndStream()
{
  m_state = State::Stop;

  if (m_mem_pool)
  {
    threadWaitForExit(&m_audio_thread);
    threadClose(&m_audio_thread);

    audrvClose(&m_audio_driver);
    audrenExit();

    free(m_mem_pool);
    m_mem_pool = nullptr;
  }
}

void SwitchAudioStream::AudioThread(void* userdata)
{
  SwitchAudioStream* const this_ptr = static_cast<SwitchAudioStream*>(userdata);

  AudioDriverWaveBuf buffers[2];
  memset(&buffers[0], 0, sizeof(AudioDriverWaveBuf) * 2);
  for (int i = 0; i < 2; i++)
  {
    buffers[i].data_pcm16 = (s16*)this_ptr->m_mem_pool;
    buffers[i].size = this_ptr->m_audio_thread_buffer_size * kOutputChannels * sizeof(int16_t);
    buffers[i].start_sample_offset = i * this_ptr->m_audio_thread_buffer_size;
    buffers[i].end_sample_offset = buffers[i].start_sample_offset + this_ptr->m_audio_thread_buffer_size;
  }

  while (this_ptr->m_state != State::Stop)
  {
    AudioDriverWaveBuf* refill_buffer = nullptr;
    for (int i = 0; i < 2; i++)
    {
      if (buffers[i].state == AudioDriverWaveBufState_Free || buffers[i].state == AudioDriverWaveBufState_Done)
      {
        refill_buffer = &buffers[i];
        break;
      }
    }

    if (refill_buffer)
    {
      int16_t* data = reinterpret_cast<s16*>(this_ptr->m_mem_pool) +
                      refill_buffer->start_sample_offset * kOutputChannels;

      if (this_ptr->m_state == State::Paused)
      {
        memset(data, 0, this_ptr->m_audio_thread_buffer_size * kOutputChannels * sizeof(int16_t));
      }
      else
      {
        this_ptr->ReadFrames(data, this_ptr->m_audio_thread_buffer_size);
      }

      armDCacheFlush(data,
                     this_ptr->m_audio_thread_buffer_size * kOutputChannels * sizeof(int16_t));

      audrvVoiceAddWaveBuf(&this_ptr->m_audio_driver, 0, refill_buffer);
      audrvVoiceStart(&this_ptr->m_audio_driver, 0);
    }

    audrvUpdate(&this_ptr->m_audio_driver);
    audrenWaitFrame();
  }
}

std::unique_ptr<AudioStream> AudioStream::CreateSwitchAudioStream(u32 sample_rate,
                                                                  const AudioStreamParameters& parameters, Error* error)
{
  std::unique_ptr<SwitchAudioStream> stream = std::make_unique<SwitchAudioStream>(sample_rate, parameters);
  if (!stream->Initialize(error))
    stream.reset();
  return stream;
}
