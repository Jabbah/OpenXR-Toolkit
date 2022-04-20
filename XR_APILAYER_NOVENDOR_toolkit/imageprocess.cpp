// MIT License
//
// Copyright(c) 2021 Matthieu Bucchianeri
// Copyright(c) 2021-2022 Jean-Luc Dupiot - Reality XP
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "shader_utilities.h"
#include "factories.h"
#include "interfaces.h"
#include "layer.h"
#include "log.h"

namespace {

    using namespace toolkit;
    using namespace toolkit::config;
    using namespace toolkit::graphics;
    using namespace toolkit::log;

    struct alignas(16) ImageProcessorConfig {
        XrVector4f Params1; // Contrast, Brightness, Exposure, Saturation (-1..+1 params)
        XrVector4f Params2; // ColorGainR, ColorGainG, ColorGainB (-1..+1 params)
        XrVector4f Params3; // Highlights, Shadows, Vibrance (0..1 params)
    };

    class ImageProcessor : public IImageProcessor {
      public:
        ImageProcessor(std::shared_ptr<IConfigManager> configManager, std::shared_ptr<IDevice> graphicsDevice)
            : m_configManager(configManager), m_device(graphicsDevice),
              m_userParams(GetUserParams(configManager.get(), 1)) {
            createRenderResources();
        }

        void reload() override {
            createRenderResources();
        }

        void update() override {
            // Generic implementation to support more than just Off/On modes in the future.
            const auto mode = m_configManager->getEnumValue<PostProcessType>(config::SettingPostProcess);
            const auto hasModeChanged = mode != m_mode;

            if (hasModeChanged)
                m_mode = mode;

            if (mode != PostProcessType::Off) {
                if (hasModeChanged || checkUpdateConfig(mode)) {
                    updateConfig();
                }
            }
        }

        void process(std::shared_ptr<ITexture> input, std::shared_ptr<ITexture> output, int32_t slice) override {
            const auto shader = m_mode != PostProcessType::Off ? 1 + input->isArray() : 0;
            m_device->setShader(m_shaders[shader], SamplerType::LinearClamp);
            m_device->setShaderInput(0, m_cbParams);
            m_device->setShaderInput(0, input, slice);
            m_device->setShaderOutput(0, output, slice);
            m_device->dispatchShader();
        }

      private:
        void createRenderResources() {
            const auto shadersDir = dllHome / "shaders";
            const auto shaderFile = shadersDir / "postprocess.hlsl";

            utilities::shader::Defines defines;
            // defines.add("POST_PROCESS_SRC_SRGB", true);
            // defines.add("POST_PROCESS_DST_SRGB", true);

            m_shaders[0] = m_device->createQuadShader(
                shaderFile, "mainPassThrough", "Postprocess PS (none)", defines.get() /*,  shadersDir*/);

            m_shaders[1] = m_device->createQuadShader(
                shaderFile, "mainPostProcess", "Postprocess PS", defines.get() /*,  shadersDir*/);

            defines.add("VPRT", true);
            m_shaders[2] = m_device->createQuadShader(
                shaderFile, "mainPostProcess", "Postprocess PS (VPRT)", defines.get() /*,  shadersDir*/);

            // TODO: For now, we're going to require that all image processing shaders share the same configuration
            // structure.
            m_cbParams = m_device->createBuffer(sizeof(ImageProcessorConfig), "Postprocess CB");

            updateConfig();
        }

        bool checkUpdateConfig(PostProcessType mode) const {
            if (mode != PostProcessType::Off) {
                return m_configManager->hasChanged(SettingPostSunGlasses) ||
                       m_configManager->hasChanged(SettingPostContrast) ||
                       m_configManager->hasChanged(SettingPostBrightness) ||
                       m_configManager->hasChanged(SettingPostExposure) ||
                       m_configManager->hasChanged(SettingPostSaturation) ||
                       m_configManager->hasChanged(SettingPostVibrance) ||
                       m_configManager->hasChanged(SettingPostHighlights) ||
                       m_configManager->hasChanged(SettingPostShadows) ||
                       m_configManager->hasChanged(SettingPostColorGainR) ||
                       m_configManager->hasChanged(SettingPostColorGainG) ||
                       m_configManager->hasChanged(SettingPostColorGainB);
            }
            return false;
        }

        void updateConfig() {
            using namespace DirectX;
            using namespace xr::math;
            using namespace utilities;

            // standard gains:
            // - reduce contrast and brighness ranges
            // - increase exposure and vibrance effect
            // - limit RGB gains
            // - limit shadows range
            static constexpr XMVECTORF32 kGain[1][3] = {
                {{{{1.f, .8f, 3.f, 1.f}}}, {{{1.f, 1.f, 1.f, 1.f}}}, {{{1.f, .5f, 1.f, 1.f}}}},
            };

            // standard presets
            static constexpr XMINT4 kBias[to_integral(PostSunGlassesType::MaxValue)][3] = {
                // none
                {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},

                // sunglasses light: +2.5 contrast, -5 bright, -5 expo, -20 highlights
                {{25, -50, -50, 0}, {0, 0, 0, 0}, {20, 0, 0, 0}},

                // sunglasses dark: +2.5 contrast, -10 bright, -10 expo, -40 highlights, +5 shad
                {{25, -100, -100, 0}, {0, 0, 0, 0}, {400, 50, 0, 0}},

                // deep night: +0.5 contrast, -40 bright, +20 expo, -15 sat, +2.5 vib, -75 high, +15 shad
                {{5, -400, 200, -150}, {0, 0, 0, 0}, {750, 150, 25, 0}},
            };

            const auto userParams = GetUserParams(m_configManager.get(), 0);
            const auto bias = to_integral(m_configManager->getEnumValue<PostSunGlassesType>(SettingPostSunGlasses));

            // [0..1000] -> [-1..+1]
            const auto params1 =
                XMVectorSaturate((XMLoadSInt4(&userParams[0]) + XMLoadSInt4(&kBias[bias][0])) * 0.001f);
            StoreXrVector4(&m_config.Params1, (params1 * 2.0 - XMVectorSplatOne()) * kGain[0][0]);

            // [0..1000] -> [-1..+1]
            const auto params2 =
                XMVectorSaturate((XMLoadSInt4(&userParams[1]) + XMLoadSInt4(&kBias[bias][1])) * 0.001f);
            StoreXrVector4(&m_config.Params2, (params2 * 2.0 - XMVectorSplatOne()) * kGain[0][1]);

            // [0..1000] -> [0..1]
            const auto params3 =
                XMVectorSaturate((XMLoadSInt4(&userParams[2]) + XMLoadSInt4(&kBias[bias][2])) * 0.001f);
            StoreXrVector4(&m_config.Params3, params3 * kGain[0][2]);

            m_cbParams->uploadData(&m_config, sizeof(m_config));
        }

        static std::array<DirectX::XMINT4, 3> GetUserParams(const IConfigManager* configManager, size_t index) {
            using namespace DirectX;
            if (configManager) {
                static const char* lut[] = {"", "_u1", "_u2", "_u3", "_u4"}; // placeholder up to 4
                const auto suffix = lut[std::min(index, std::size(lut))];

                return {XMINT4(configManager->getValue(SettingPostContrast + suffix),
                               configManager->getValue(SettingPostBrightness + suffix),
                               configManager->getValue(SettingPostExposure + suffix),
                               configManager->getValue(SettingPostSaturation + suffix)),

                        XMINT4(configManager->getValue(SettingPostColorGainR + suffix),
                               configManager->getValue(SettingPostColorGainG + suffix),
                               configManager->getValue(SettingPostColorGainB + suffix),
                               0),

                        XMINT4(configManager->getValue(SettingPostHighlights + suffix),
                               configManager->getValue(SettingPostShadows + suffix),
                               configManager->getValue(SettingPostVibrance + suffix),
                               0)};
            }
            return {XMINT4(500, 500, 500, 500), XMINT4(500, 500, 500, 0), XMINT4(0, 0, 0, 0)};
        }

        const std::shared_ptr<IConfigManager> m_configManager;
        const std::shared_ptr<IDevice> m_device;
        const std::array<DirectX::XMINT4, 3> m_userParams;

        std::shared_ptr<IQuadShader> m_shaders[3]; // off, on, vprt
        std::shared_ptr<IShaderBuffer> m_cbParams;

        PostProcessType m_mode{PostProcessType::Off};
        ImageProcessorConfig m_config{};
    };

} // namespace

namespace toolkit::graphics {

    bool IsDeviceSupportingFP16(std::shared_ptr<IDevice> device) {
        if (device) {
            if (auto device11 = device->getAs<D3D11>()) {
                D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT feature = {};
                device11->CheckFeatureSupport(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, &feature, sizeof(feature));
                return (feature.PixelShaderMinPrecision & D3D11_SHADER_MIN_PRECISION_16_BIT) != 0;
            }
            if (auto device12 = device->getAs<D3D12>()) {
                D3D12_FEATURE_DATA_D3D12_OPTIONS feature = {};
                device12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &feature, sizeof(feature));
                return (feature.MinPrecisionSupport & D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT) != 0;
            }
        }

        return false;
    }

    GpuArchitecture GetGpuArchitecture(UINT VendorId) {
        // Known PCI vendor IDs
        constexpr uint32_t kVendorID_AMD = 0x1002;
        constexpr uint32_t kVendorID_Intel = 0x8086;
        constexpr uint32_t kVendorID_NVIDIA = 0x10DE;

        return VendorId == kVendorID_AMD      ? GpuArchitecture::AMD
               : VendorId == kVendorID_Intel  ? GpuArchitecture::Intel
               : VendorId == kVendorID_NVIDIA ? GpuArchitecture::NVidia
                                              : GpuArchitecture::Unknown;
    }

    GpuArchitecture GetGpuArchitecture(std::shared_ptr<IDevice> device) {
        if (device) {
            std::string name = device->getDeviceName();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });

            if (name.find("nvidia") != std::string::npos)
                return GpuArchitecture::NVidia;

            if (name.find("intel") != std::string::npos)
                return GpuArchitecture::Intel;

            // put last in case other vendor names have these 3 letters in their device name.
            if (name.find("amd") != std::string::npos)
                return GpuArchitecture::AMD;
        }

        return GpuArchitecture::Unknown;
    }

    std::shared_ptr<IImageProcessor> CreateImageProcessor(std::shared_ptr<IConfigManager> configManager,
                                                          std::shared_ptr<IDevice> graphicsDevice) {
        return std::make_shared<ImageProcessor>(configManager, graphicsDevice);
    }

} // namespace toolkit::graphics
