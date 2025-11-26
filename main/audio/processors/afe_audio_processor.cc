#include "afe_audio_processor.h"
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

/**
 * @brief 初始化 AFE 音频处理器
 * 
 * 此函数用于初始化音频处理模块，包括配置音频编解码器、设置帧长度、加载模型，
 * 并根据配置创建 AFE（Audio Front End）接口实例。同时启动一个音频处理任务。
 *
 * @param codec 指向 AudioCodec 实例的指针，用于获取音频输入通道等信息
 * @param frame_duration_ms 每帧音频数据的时间长度（单位：毫秒）
 * @param models_list 指向语音识别模型列表的指针；若为空则从默认路径加载模型
 */
void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    ESP_LOGI(TAG, "Initializing  AfeAudioProcessor");
    codec_ = codec;
    // 计算每帧采样点数（假设采样率为 16kHz）
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // 预分配输出缓冲区容量以提升性能
    output_buffer_.reserve(frame_samples_);

    // 判断是否存在参考信号输入通道
    int ref_num = codec_->input_reference() ? 1 : 0;

    // 构建输入格式字符串，'M' 表示主麦克风，'R' 表示参考信号
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    // 加载语音处理相关模型（如 VAD 和 NS）
    srmodel_list_t *models;
    if (models_list == nullptr) {
        ESP_LOGI(TAG, "Loading models from model directory");
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    // 过滤出噪声抑制和语音活动检测模型名称
    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    // 初始化 AFE 配置结构体，并设置各项参数
    ESP_LOGI(TAG, "AFE config, input format: %s",input_format);
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    // 根据是否找到噪声抑制模型决定是否启用 NS 功能
    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    // 根据宏定义控制是否启用设备端回声消除与语音活动检测功能
#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = false;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    // 创建 AFE 接口实例并初始化内部资源
    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    
    // 启动音频处理线程任务
    xTaskCreate([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", 4096, this, 3, NULL);
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

/**
 * @brief 音频处理任务主循环函数，负责从AFE接口获取音频数据并进行处理。
 *
 * 此函数运行在一个独立的任务中，持续监听事件组信号以决定是否继续执行音频处理流程。
 * 它会调用AFE接口的fetch_with_delay方法来获取音频数据，并根据VAD状态判断语音活动情况，
 * 同时将处理后的音频数据通过回调函数输出。
 *
 * 该函数没有参数，也不返回任何值。
 */
void AfeAudioProcessor::AudioProcessorTask() {
    // 获取AFE接口定义的数据读取和写入块大小
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        // 等待PROCESSOR_RUNNING事件标志被设置，表示允许开始或继续处理音频数据
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        // 从AFE模块获取带延迟的音频数据
        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        
        // 检查当前任务是否仍处于运行状态，若不是则跳过本次循环
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }

        // 判断获取到的数据是否有效，无效则记录日志并跳过后续处理
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }

        // 处理VAD（Voice Activity Detection）状态变化逻辑
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        // 如果设置了输出回调函数，则对音频数据进行缓冲与分帧处理
        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            
            // 将新获取的数据追加至输出缓冲区
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            
            // 当缓冲区中的样本数达到一帧所需的数量时，触发一次输出操作
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // 缓冲区刚好等于一帧长度时，移动整个缓冲区作为一帧输出
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // 缓冲区超过一帧长度时，复制前一帧数据并移除已使用的部分
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        }
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}
