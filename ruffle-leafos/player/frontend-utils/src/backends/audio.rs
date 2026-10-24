use cpal::{Device, SampleFormat, StreamConfig, ErrorKind, Error, Sample, FromSample};
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use ruffle_core::backend::audio::{
    AudioBackend, AudioMixer, DecodeError, RegisterError, SoundHandle, SoundInstanceHandle,
    SoundStreamInfo, SoundTransform, swf,
};
use ruffle_core::impl_audio_mixer_backend;

pub struct CpalAudioBackend {
    #[expect(dead_code)]
    device: cpal::Device,
    #[expect(dead_code)]
    config: cpal::StreamConfig,
    stream: cpal::Stream,
    mixer: AudioMixer,
}

impl CpalAudioBackend {
    pub fn new(preferred_device_name: Option<&str>) -> Result<Self, Error> {
        // Create CPAL audio device.
        let host = cpal::default_host();
        let device = host.default_output_device().unwrap();

        // Create audio stream for device.
        let config = device.default_output_config().unwrap();
        let sample_format = config.sample_format();
        println!(
            "device id:{:?}, device copnfig: {:?}, sample_format:{:?}",
            device.id(),
            config,
            sample_format
        );


        let config : StreamConfig = config.into();
        //config.buffer_size = cpal::BufferSize::Fixed(8192);
        let mixer = AudioMixer::new(config.channels as u8, config.sample_rate);

        
        
        // Start the audio stream.
        let stream = {
            let mixer = mixer.proxy();
            let err_fn = |err: Error| match err.kind() {
                ErrorKind::DeviceChanged | ErrorKind::Xrun | ErrorKind::RealtimeDenied => {
                    log::error!("{err}")
                }
                _ => log::error!("Stream error: {err}"),
            };

            let stream_result = match sample_format {
                SampleFormat::F32 => device.build_output_stream(
                    config,
                    move |buffer, _| mixer.mix::<f32>(buffer),
                    err_fn,
                    None,
                ),
                SampleFormat::F64 => device.build_output_stream(
                    config,
                    move |buffer, _| mixer.mix::<f64>(buffer),
                    err_fn,
                    None,
                ),
                SampleFormat::I16 => device.build_output_stream(
                    config,
                    move |buffer, _| mixer.mix::<i16>(buffer),
                    err_fn,
                    None,
                ),
                SampleFormat::U16 => device.build_output_stream(
                    config,
                    move |buffer: &mut [u16], _| {
                        mixer.mix::<i16>(bytemuck::cast_slice_mut(buffer));
                        for s in buffer.iter_mut() {
                            *s = (*s).wrapping_add(32768);
                        }
                    },
                    err_fn,
                    None,
                ),
                _ => return Err(ErrorKind::HostUnavailable.into()),
            };
            stream_result.map_err(|err| {
                log_build_stream_error(
                    &device,
                    sample_format,
                    &config,
                    &err,
                );
                err
            })?
        };

        stream.play()?;

        Ok(Self {
            device,
            config,
            stream,
            mixer,
        })
    }
    // pub fn new(preferred_device_name: Option<&str>) -> Result<Self, CpalError> {
    //     // 1. 遍历所有可用的音频主机
    //     for host_id in cpal::available_hosts() {
    //         let host: Host = cpal::host_from_id(host_id).map_err(|_| CpalError::NoDevices)?; // 注意：这里需要你的枚举里有对应项

    //         log::info!("🔍 正在扫描 Host: {:?}", host_id);

    //         // 将迭代器收集到 Vec 中
    //         let devices: Vec<Device> = host.output_devices().expect("无法获取设备列表").collect();

    //         log::info!("当前 Host device数量 {:?}", devices.len());

    //         for device in devices {
    //             // 修复点 2: 显式类型注解
    //             let device: Device = device;

    //             // 获取设备名称
    //             let device_name = device
    //                 .name()
    //                 .unwrap_or_else(|_| "Unknown Device".to_string());

    //             // 尝试获取配置
    //             let config_result = device.default_output_config();

    //             match config_result {
    //                 Ok(config) => {
    //                     log::info!("✅ 发现有效设备: {} (Host: {:?})", device_name, host_id);

    //                     // 检查名称匹配
    //                     let is_name_match = match preferred_device_name {
    //                         Some(name) => device_name.contains(name),
    //                         None => true,
    //                     };

    //                     if is_name_match {
    //                         // --- 找到目标设备，开始构建流 ---

    //                         // 修复点 3: 显式类型注解
    //                         let mut sample_format: SampleFormat = config.sample_format();
    //                         let mut stream_config: StreamConfig = config.into();

    //                         log::info!(
    //                             "默认device配置: {:?} SampleFormat:{:?}",
    //                             stream_config,
    //                             sample_format
    //                         );

    //                         sample_format = cpal::SampleFormat::I16;
    //                         stream_config.sample_rate.0= 22050;
    //                         stream_config.buffer_size = cpal::BufferSize::Fixed(8192);

    //                         let mixer = AudioMixer::new(
    //                             stream_config.channels as u8,
    //                             stream_config.sample_rate.0,
    //                         );

    //                         // 启动流
    //                         let stream = {
    //                             let mixer = mixer.proxy();
    //                             let error_handler =
    //                                 move |err| log::error!("Audio stream error: {}", err);

    //                             // 这里不需要改动，因为 sample_format 已经有了具体类型
    //                             let stream_result = match sample_format {
    //                                 cpal::SampleFormat::F32 => device.build_output_stream(
    //                                     &stream_config,
    //                                     move |buffer, _| mixer.mix::<f32>(buffer),
    //                                     error_handler,
    //                                     None,
    //                                 ),
    //                                 cpal::SampleFormat::I16 => device.build_output_stream(
    //                                     &stream_config,
    //                                     move |buffer, _| mixer.mix::<i16>(buffer),
    //                                     error_handler,
    //                                     None,
    //                                 ),
    //                                 cpal::SampleFormat::U16 => device.build_output_stream(
    //                                     &stream_config,
    //                                     move |buffer: &mut [u16], _| {
    //                                         mixer.mix::<i16>(bytemuck::cast_slice_mut(buffer));
    //                                         for s in buffer.iter_mut() {
    //                                             *s = (*s).wrapping_add(32768);
    //                                         }
    //                                     },
    //                                     error_handler,
    //                                     None,
    //                                 ),
    //                                 _ => {
    //                                     return Err(CpalError::UnsupportedSampleFormat(
    //                                         sample_format,
    //                                     ));
    //                                 }
    //                             };

    //                             stream_result.map_err(|err| {
    //                                 log_build_stream_error(
    //                                     host_id,
    //                                     &device_name,
    //                                     sample_format,
    //                                     &stream_config,
    //                                     &err,
    //                                 );
    //                                 err
    //                             })?
    //                         };

    //                         stream.play().map_err(CpalError::Play)?;

    //                         return Ok(Self {
    //                             device,
    //                             config: stream_config,
    //                             stream,
    //                             mixer,
    //                         });
    //                     }
    //                 }
    //                 Err(e) => {
    //                     log::debug!("⚠️ 跳过设备 (配置无效): {} - 错误: {}", device_name, e);
    //                 }
    //             }
    //         }
    //     }

    //     Err(CpalError::NoDevices)
    // }
}

fn write_data<T>(output: &mut [T], channels: usize, next_sample: &mut dyn FnMut() -> f32)
where
    T: Sample + FromSample<f32>,
{
    for frame in output.chunks_mut(channels) {
        let value: T = T::from_sample(next_sample());
        for sample in frame.iter_mut() {
            *sample = value;
        }
    }
}


fn log_build_stream_error(
    device: &Device,
    sample_format: cpal::SampleFormat,
    stream_config: &cpal::StreamConfig,
    err: &cpal::Error,
) {
    log::error!(
        "Failed to construct CPAL audio stream. device=\"{:?}\", sample_format={:?}, config={:?}, error={}, error_debug={:?}",
        device.id(),
        sample_format,
        stream_config,
        err,
        err,
    );

}


// fn log_build_stream_error(
//     host_id: cpal::HostId,
//     device_name: &str,
//     sample_format: cpal::SampleFormat,
//     stream_config: &cpal::StreamConfig,
//     err: &cpal::BuildStreamError,
// ) {
//     log::error!(
//         "Failed to construct CPAL audio stream. host={:?}, device=\"{}\", sample_format={:?}, config={:?}, error={}, error_debug={:?}",
//         host_id,
//         device_name,
//         sample_format,
//         stream_config,
//         err,
//         err,
//     );

//     if let cpal::BuildStreamError::BackendSpecific { err } = err {
//         log::error!(
//             "Backend-specific audio stream error. This is the ALSA error reported through CPAL: {}",
//             err.description
//         );
//     }
// }

impl AudioBackend for CpalAudioBackend {
    impl_audio_mixer_backend!(mixer);

    fn play(&mut self) {
        self.stream.play().expect("Error trying to resume CPAL audio stream. This feature may not be supported by your audio device.");
    }

    fn pause(&mut self) {
        self.stream.pause().expect("Error trying to pause CPAL audio stream. This feature may not be supported by your audio device.");
    }
}

// #[expect(dead_code)]
// fn get_suitable_output_device(
//     preferred_device_name: Option<&str>,
//     host: &cpal::Host,
// ) -> Option<cpal::Device> {
//     // First let's check for any user preference...
//     if let Some(preferred_device_name) = preferred_device_name
//         && let Ok(mut devices) = host.output_devices()
//         && let Some(device) =
//             devices.find(|device| device.name.ok().as_deref() == Some(preferred_device_name))
//     {
//         return Some(device);
//     }

//     // Then let's fall back to the device default
//     host.default_output_device()
// }
