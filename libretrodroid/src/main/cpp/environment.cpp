/*
 *     Copyright (C) 2020  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define MODULE_NAME_CORE "Libretro Core"

#include <utility>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <EGL/egl.h>
#include <unordered_map>

#include "retro_netpacket_api.h"
#include "log.h"
#include "environment.h"
#include "vfs/vfs.h"
#include "microphone/microphoneinterface.h"
#include "vulkancommon/libretro_vulkan.h"
#include "netpacket.h"

void Environment::initialize(
    const std::string &requiredSystemDirectory,
    const std::string &requiredSavesDirectory,
    retro_hw_get_current_framebuffer_t required_callback_get_current_framebuffer
) {
    callback_get_current_framebuffer = required_callback_get_current_framebuffer;
    systemDirectory = requiredSystemDirectory;
    savesDirectory = requiredSavesDirectory;
}

void Environment::deinitialize() {
    callback_get_current_framebuffer = nullptr;
    hw_context_reset = nullptr;
    hw_context_destroy = nullptr;

    retro_disk_control_callback = nullptr;

    memoryDescriptors.clear();
    memoryMap = {};
    hasMemoryMap = false;

    savesDirectory = std::string();
    systemDirectory = std::string();
    language = RETRO_LANGUAGE_ENGLISH;

    pixelFormat = RETRO_PIXEL_FORMAT_RGB565;
    useHWAcceleration = false;
    useDepth = false;
    useStencil = false;
    bottomLeftOrigin = false;
    screenRotation = 0;

    gameGeometryUpdated = false;
    gameGeometryWidth = 0;
    gameGeometryHeight = 0;
    gameGeometryAspectRatio = -1.0f;

    rumbleStates.fill(libretrodroid::RumbleState {});

    vulkanNegotiationInterface = nullptr;
    hwRenderInterface = nullptr;
    hwRenderContextType = RETRO_HW_CONTEXT_NONE;
}

void Environment::updateVariable(const std::string& key, const std::string& value) {
    auto current = variables[key];
    current.key = key;

    if (value != current.value) {
        current.value = value;
        variables[key] = current;
        dirtyVariables = true;
    }
}

bool Environment::environment_handle_set_variables(const struct retro_variable* received) {
    unsigned count = 0;
    while (received[count].key != nullptr) {
        LOGD("Received variable %s: %s", received[count].key, received[count].value);

        std::string key(received[count].key);
        std::string description(received[count].value);
        std::string value(received[count].value);

        auto firstValueStart = value.find(';') + 2;
        auto firstValueEnd = value.find('|', firstValueStart);
        value = value.substr(firstValueStart, firstValueEnd - firstValueStart);

        auto currentVariable = variables[key];
        currentVariable.key = key;
        currentVariable.description = description;

        if (currentVariable.value.empty()) {
            currentVariable.value = value;
        }

        variables[key] = currentVariable;
        LOGD("Assigning variable %s: %s", currentVariable.key.c_str(), currentVariable.value.c_str());

        count++;
    }

    return true;
}

bool Environment::environment_handle_get_variable(struct retro_variable* requested) {
    auto it = variables.find(requested->key);
    if (it != variables.end()) {
        requested->value = it->second.value.c_str();
        return true;
    }
    return false;
}

bool Environment::environment_handle_set_controller_info(const struct retro_controller_info* received) {
    unsigned player = 0;
    while (received[player].types != nullptr) {
        controllers.emplace_back();

        unsigned controller = 0;
        while (received[player].types[controller].desc != nullptr) {
            auto currentController = received[player].types[controller];
            controllers[player].push_back(Controller { currentController.id, currentController.desc });
            controller++;
        }

        player++;
    }

    return true;
}

bool Environment::environment_handle_set_hw_render(struct retro_hw_render_callback* hw_render_callback) {
    useHWAcceleration = true;

    hwRenderContextType = hw_render_callback->context_type;

    if (hw_render_callback->context_type == RETRO_HW_CONTEXT_VULKAN) {
        LOGD("Core requested Vulkan HW rendering context");
        useDepth = hw_render_callback->depth;
        useStencil = hw_render_callback->stencil;
        bottomLeftOrigin = hw_render_callback->bottom_left_origin;
        hw_context_reset = hw_render_callback->context_reset;
        hw_context_destroy = hw_render_callback->context_destroy;
        return true;
    }

    useDepth = hw_render_callback->depth;
    useStencil = hw_render_callback->stencil;
    bottomLeftOrigin = hw_render_callback->bottom_left_origin;

    hw_context_destroy = hw_render_callback->context_destroy;
    hw_context_reset = hw_render_callback->context_reset;
    hw_render_callback->get_current_framebuffer = callback_get_current_framebuffer;
    hw_render_callback->get_proc_address = &eglGetProcAddress;

    return true;
}

bool Environment::environment_handle_get_vfs_interface(struct retro_vfs_interface_info* vfsInterfaceInfo) {
    if (!useVirtualFileSystem) {
        return false;
    }

    vfsInterfaceInfo->required_interface_version = libretrodroid::VFS::SUPPORTED_VERSION;
    vfsInterfaceInfo->iface = libretrodroid::VFS::getInterface();
    return true;
}

bool Environment::environment_handle_get_microphone_interface(struct retro_microphone_interface* microphone_interface) {
    if (!enableMicrophone) {
        return false;
    }

    *microphone_interface = *libretrodroid::MicrophoneInterface::getInterface();
    return true;
}

bool Environment::handle_callback_environment(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_OVERSCAN: {
            LOGD("Called RETRO_ENVIRONMENT_GET_OVERSCAN");
            *((bool*) data) = false;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
            LOGD("Called RETRO_ENVIRONMENT_GET_CAN_DUPE");
            *((bool*) data) = true;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            LOGD("Called RETRO_ENVIRONMENT_GET_VARIABLE");
            return environment_handle_get_variable(static_cast<struct retro_variable*>(data));
        }

        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
            LOGD("Called RETRO_ENVIRONMENT_SET_CONTROLLER_INFO");
            return environment_handle_set_controller_info(static_cast<const struct retro_controller_info*>(data));

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            LOGD("Called RETRO_ENVIRONMENT_SET_PIXEL_FORMAT");
            pixelFormat = (*(unsigned*) data);
            return true;

        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            LOGD("Called RETRO_ENVIRONMENT_GET_LANGUAGE");
            *((unsigned*) data) = language;
            return true;

        case RETRO_ENVIRONMENT_SET_VARIABLES:
            LOGD("Called RETRO_ENVIRONMENT_SET_VARIABLES");
            return environment_handle_set_variables(static_cast<const struct retro_variable*>(data));

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            LOGD("Called RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE. Is dirty?: %d", dirtyVariables);
            *((bool*) data) = dirtyVariables;
            dirtyVariables = false;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: {
            LOGD("Called RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER");
            *((unsigned*) data) = RETRO_HW_CONTEXT_VULKAN;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            LOGD("Called RETRO_ENVIRONMENT_SET_HW_RENDER");
            return environment_handle_set_hw_render(static_cast<struct retro_hw_render_callback*>(data));

        case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE: {
            LOGD("Called RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE");
            auto* iface = (retro_hw_render_context_negotiation_interface_vulkan*) data;
            setVulkanNegotiationInterface(iface);
            return true;
        }

        case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE: {
            LOGD("Called RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE");
            if (hwRenderInterface) {
                *(const retro_hw_render_interface**) data = hwRenderInterface;
                return true;
            }
            return false;
        }

        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
            LOGD("Called RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE");
            ((struct retro_rumble_interface*) data)->set_rumble_state = &callback_set_rumble_state;
            return true;

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            LOGD("Called RETRO_ENVIRONMENT_GET_LOG_INTERFACE");
            ((struct retro_log_callback*) data)->log = &callback_retro_log;
            return true;

        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            LOGD("Called RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY");
            *(const char**) data = savesDirectory.c_str();
            return !savesDirectory.empty();

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            LOGD("Called RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY");
            *(const char**) data = systemDirectory.c_str();
            return !systemDirectory.empty();

        case RETRO_ENVIRONMENT_SET_ROTATION: {
            LOGD("Called RETRO_ENVIRONMENT_SET_ROTATION");
            unsigned screenRotationIndex = (*static_cast<unsigned*>(data));
            screenRotation = screenRotationIndex * (float) (-M_PI / 2.0);
            screenRotationUpdated = true;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
            LOGD("Called RETRO_ENVIRONMENT_SET_ROTATION");
            retro_disk_control_callback = static_cast<struct retro_disk_control_callback*>(data);
            return true;
        }

        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
            LOGD("Called RETRO_ENVIRONMENT_GET_PERF_INTERFACE");
            return false;

        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
            auto *received = static_cast<const struct retro_memory_map *>(data);
            memoryDescriptors.assign(received->descriptors, received->descriptors + received->num_descriptors);
            memoryMap.descriptors = memoryDescriptors.data();
            memoryMap.num_descriptors = static_cast<unsigned>(memoryDescriptors.size());
            hasMemoryMap = !memoryDescriptors.empty();
            return true;
        }

            // TODO... RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO can also change frame-rate
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            LOGD("Called RETRO_ENVIRONMENT_SET_GEOMETRY");
            auto* geo = (struct retro_game_geometry*) data;
            if (geo) {
                gameGeometryUpdated = true;
                gameGeometryWidth = geo->base_width;
                gameGeometryHeight = geo->base_height;
                gameGeometryAspectRatio = geo->aspect_ratio;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
            LOGD("Called RETRO_ENVIRONMENT_GET_VFS_INTERFACE");
            return environment_handle_get_vfs_interface((struct retro_vfs_interface_info*) data);

        case RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE:
            LOGD("Called RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE");
            return environment_handle_get_microphone_interface((struct retro_microphone_interface*) data);

        case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
            LOGI("Called RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE");
            libretrodroid::NetpacketBridge::getInstance().setCoreInterface(
                static_cast<const wn_netpacket_callback*>(data)
            );
            return true;

        default:
            LOGD("Unhandled environ %d", cmd);
            return false;
    }
}

retro_hw_context_reset_t Environment::getHwContextReset() const {
    return hw_context_reset;
}

retro_hw_context_reset_t Environment::getHwContextDestroy() const {
    return hw_context_destroy;
}

void Environment::clearHwContextDestroy() {
    hw_context_destroy = nullptr;
}

struct retro_disk_control_callback* Environment::getRetroDiskControlCallback() const {
    return retro_disk_control_callback;
}

const struct retro_memory_map* Environment::getMemoryMap() const {
    return hasMemoryMap ? &memoryMap : nullptr;
}

int Environment::getPixelFormat() const {
    return pixelFormat;
}

bool Environment::isUseHwAcceleration() const {
    return useHWAcceleration;
}

bool Environment::isUseDepth() const {
    return useDepth;
}

bool Environment::isUseStencil() const {
    return useStencil;
}

bool Environment::isBottomLeftOrigin() const {
    return bottomLeftOrigin;
}

float Environment::getScreenRotation() const {
    return screenRotation;
}

bool Environment::isScreenRotationUpdated() const {
    return screenRotationUpdated;
}

void Environment::clearScreenRotationUpdated() {
    screenRotationUpdated = false;
}

unsigned int Environment::getGameGeometryWidth() const {
    return gameGeometryWidth;
}

unsigned int Environment::getGameGeometryHeight() const {
    return gameGeometryHeight;
}

float Environment::getGameGeometryAspectRatio() const {
    return gameGeometryAspectRatio;
}

bool Environment::isGameGeometryUpdated() const {
    return gameGeometryUpdated;
}

void Environment::clearGameGeometryUpdated() {
    gameGeometryUpdated = false;
}

void Environment::setLanguage(const std::string& androidLanguage) {
    if (androidLanguage == "ja") {
        language = RETRO_LANGUAGE_JAPANESE;
    } else if (androidLanguage == "ko") {
        language = RETRO_LANGUAGE_KOREAN;
    } else if (androidLanguage == "zh_CN") {
        language = RETRO_LANGUAGE_CHINESE_SIMPLIFIED;
    } else if (androidLanguage == "zh_TW") {
        language = RETRO_LANGUAGE_CHINESE_TRADITIONAL;
    } else {
        language = RETRO_LANGUAGE_ENGLISH;
    }
}

void Environment::setEnableVirtualFileSystem(bool value) {
    useVirtualFileSystem = value;
}

void Environment::setEnableMicrophone(bool value) {
    enableMicrophone = value;
}

std::array<libretrodroid::RumbleState, 4> & Environment::getLastRumbleStates() {
    return rumbleStates;
}

const std::vector<struct Variable> Environment::getVariables() const {
    std::vector<struct Variable> result;
    result.reserve(variables.size());

    std::for_each(variables.begin(), variables.end(), [&](auto &it) {
        result.push_back(it.second);
    });

    return result;
}

const std::vector<std::vector<struct Controller>> &Environment::getControllers() const {
    return controllers;
}

retro_hw_render_context_negotiation_interface_vulkan* Environment::getVulkanNegotiationInterface() const {
    return vulkanNegotiationInterface;
}

void Environment::setVulkanNegotiationInterface(retro_hw_render_context_negotiation_interface_vulkan* iface) {
    vulkanNegotiationInterface = iface;
}

retro_hw_render_interface* Environment::getHwRenderInterface() const {
    return hwRenderInterface;
}

void Environment::setHwRenderInterface(retro_hw_render_interface* iface) {
    hwRenderInterface = iface;
}

bool Environment::isUseVulkan() const {
    return hwRenderContextType == RETRO_HW_CONTEXT_VULKAN;
}

retro_hw_context_type Environment::getHwRenderContextType() const {
    return hwRenderContextType;
}

float Environment::retrieveGameSpecificAspectRatio() {
    return gameGeometryAspectRatio;
}

bool Environment::handle_callback_set_rumble_state(
    unsigned port,
    enum retro_rumble_effect effect,
    uint16_t strength
) {
    if (port < 4) {
        auto& state = rumbleStates[port];
        state.port = port;
        state.effect = effect;
        state.strength = strength;
    }
    return true;
}

void Environment::callback_retro_log(enum retro_log_level level, const char *fmt, ...) {
    char buffer[4096] = {0};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    switch(level) {
        case RETRO_LOG_DEBUG: LOGE("debug: %s", buffer); break;
        case RETRO_LOG_INFO: LOGE("info: %s", buffer); break;
        case RETRO_LOG_WARN: LOGE("warn: %s", buffer); break;
        case RETRO_LOG_ERROR: LOGE("error: %s", buffer); break;
        default: LOGE("%s", buffer); break;
    }
}

bool Environment::callback_set_rumble_state(
    unsigned port,
    enum retro_rumble_effect effect,
    uint16_t strength
) {
    return Environment::getInstance().handle_callback_set_rumble_state(port, effect, strength);
}

bool Environment::callback_environment(unsigned cmd, void *data) {
    return Environment::getInstance().handle_callback_environment(cmd, data);
}