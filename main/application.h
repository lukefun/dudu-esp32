#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <list>
#include <vector>
#include <condition_variable>
#include <memory>

#include <opus_encoder.h>
#include <opus_decoder.h>
#include <opus_resampler.h>

#include "protocol.h"
#include "ota.h"
#include "background_task.h"
#include "audio_processor.h"

#include "ble_config/ble_config.h"  // [新增] 添加BLE配置头文件

#if CONFIG_USE_WAKE_WORD_DETECT
#include "wake_word_detect.h"
#endif

#define SCHEDULE_EVENT (1 << 0)
#define AUDIO_INPUT_READY_EVENT (1 << 1)
#define AUDIO_OUTPUT_READY_EVENT (1 << 2)
#define CHECK_NEW_VERSION_DONE_EVENT (1 << 3)

enum DeviceState {
    kDeviceStateUnknown,            // [新增] 设备状态：未知
    kDeviceStateStarting,           // [新增] 设备状态：启动中
    kDeviceStateWifiConfiguring,    // [新增] 设备状态：WiFi配置中
    kDeviceStateIdle,               // [新增] 设备状态：空闲
    kDeviceStateConnecting,         // [新增] 设备状态：连接中
    kDeviceStateListening,          // [新增] 设备状态：倾听中
    kDeviceStateSpeaking,           // [新增] 设备状态：说话中
    kDeviceStateUpgrading,          // [新增] 设备状态：升级中
    kDeviceStateActivating,         // [新增] 设备状态：激活中
    kDeviceStateFatalError          // [新增] 设备状态：致命错误
};

#define OPUS_FRAME_DURATION_MS 60

class Application {
public:
    // 单例模式获取实例
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return voice_detected_; }
    void Schedule(std::function<void()> callback);
    // 添加设置设备状态的函数
    void SetDeviceState(DeviceState state);  
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void UpdateIotStates();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    void PlaySound(const std::string_view& sound);
    bool CanEnterSleepMode();

private:
    Application();
    ~Application();

#if CONFIG_USE_WAKE_WORD_DETECT
    WakeWordDetect wake_word_detect_;
#endif
    std::unique_ptr<AudioProcessor> audio_processor_;
    Ota ota_;
    std::mutex mutex_;
    std::list<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
#if CONFIG_USE_DEVICE_AEC || CONFIG_USE_SERVER_AEC
    bool realtime_chat_enabled_ = true;
#else
    bool realtime_chat_enabled_ = false;
#endif
    bool aborted_ = false;
    bool voice_detected_ = false;
    bool busy_decoding_audio_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;

    // Audio encode / decode
    TaskHandle_t audio_loop_task_handle_ = nullptr;
    BackgroundTask* background_task_ = nullptr;
    std::chrono::steady_clock::time_point last_output_time_;
    std::list<AudioStreamPacket> audio_decode_queue_;
    std::condition_variable audio_decode_cv_;

    // 新增：用于维护音频包的timestamp队列
    std::list<uint32_t> timestamp_queue_;
    std::mutex timestamp_mutex_;
    std::atomic<uint32_t> last_output_timestamp_ = 0;

    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;

    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;

    void MainEventLoop();
    void OnAudioInput();
    void OnAudioOutput();
    void ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckNewVersion();
    void ShowActivationCode();
    void OnClockTimer();
    void SetListeningMode(ListeningMode mode);
    void AudioLoop();
};

#endif // _APPLICATION_H_
