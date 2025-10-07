/*
 * Copyright (c) 2024 Kuba Birecki
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_rgb_fx_heatmap

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/rgb_fx.h>

#include <zmk/rgb_fx.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct fx_heatmap_config {
    size_t *pixel_map;
    size_t pixel_map_size;
    uint8_t blending_mode;
    uint16_t color_cold_hue;
    uint16_t color_hot_hue;
    uint8_t saturation;
    uint8_t lightness;
};

struct fx_heatmap_data {
    uint32_t *key_counts;
    bool is_active;
};

static int fx_heatmap_on_key_press(const struct device *dev, const zmk_event_t *event) {
    const struct fx_heatmap_config *config = dev->config;
    struct fx_heatmap_data *data = dev->data;

    const struct zmk_position_state_changed *pos_event;

    if (!data->is_active) {
        return 0;
    }

    if ((pos_event = as_zmk_position_state_changed(event)) == NULL) {
        // Event not supported.
        return -ENOTSUP;
    }

    if (!pos_event->state) {
        // Don't track key releases.
        return 0;
    }

    size_t pixel_idx = zmk_rgb_fx_get_pixel_by_key_position(pos_event->position);

    if (pixel_idx >= config->pixel_map_size) {
        // Invalid pixel index
        return -EINVAL;
    }

    data->key_counts[pixel_idx]++;

    zmk_rgb_fx_request_frames(1);

    return 0;
}

static void fx_heatmap_render_frame(const struct device *dev, struct rgb_fx_pixel *pixels,
                                    size_t num_pixels) {
    const struct fx_heatmap_config *config = dev->config;
    struct fx_heatmap_data *data = dev->data;

    size_t *pixel_map = config->pixel_map;

    // Find the maximum key press count
    uint32_t max_count = 1; // Start at 1 to avoid divide-by-zero
    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        if (data->key_counts[i] > max_count) {
            max_count = data->key_counts[i];
        }
    }

    // Render each pixel based on its usage
    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        // Calculate usage ratio (0.0 = not used, 1.0 = most used)
        float usage = (float)data->key_counts[i] / (float)max_count;

        // Interpolate hue from cold to hot based on usage
        // Note: HSL hue wraps at 360, so we need to handle interpolation carefully
        int16_t hue_delta = config->color_hot_hue - config->color_cold_hue;
        
        // Handle hue wrapping for smooth interpolation
        if (hue_delta > 180) {
            hue_delta -= 360;
        } else if (hue_delta < -180) {
            hue_delta += 360;
        }
        
        uint16_t hue = config->color_cold_hue + (int16_t)(hue_delta * usage);
        
        // Ensure hue stays in valid range [0, 359]
        while (hue >= 360) hue -= 360;
        while (hue < 0) hue += 360;

        struct zmk_color_hsl color_hsl = {
            .h = hue,
            .s = config->saturation,
            .l = config->lightness,
        };

        struct zmk_color_rgb color_rgb;
        zmk_hsl_to_rgb(&color_hsl, &color_rgb);

        pixels[pixel_map[i]].value = zmk_apply_blending_mode(pixels[pixel_map[i]].value,
                                                              color_rgb, config->blending_mode);
    }
}

static void fx_heatmap_start(const struct device *dev) {
    struct fx_heatmap_data *data = dev->data;

    data->is_active = true;
    zmk_rgb_fx_request_frames(1);
}

static void fx_heatmap_stop(const struct device *dev) {
    struct fx_heatmap_data *data = dev->data;

    data->is_active = false;
}

static int fx_heatmap_init(const struct device *dev) {
    const struct fx_heatmap_config *config = dev->config;
    struct fx_heatmap_data *data = dev->data;

    // Initialize all key counts to zero
    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        data->key_counts[i] = 0;
    }

    return 0;
}

static const struct rgb_fx_api fx_heatmap_api = {
    .on_start = fx_heatmap_start,
    .on_stop = fx_heatmap_stop,
    .render_frame = fx_heatmap_render_frame,
};

#define FX_HEATMAP_DEVICE(idx)                                                                     \
                                                                                                   \
    static size_t fx_heatmap_##idx##_pixel_map[] = DT_INST_PROP(idx, pixels);                     \
                                                                                                   \
    static uint32_t fx_heatmap_##idx##_key_counts[DT_INST_PROP_LEN(idx, pixels)];                 \
                                                                                                   \
    static struct fx_heatmap_data fx_heatmap_##idx##_data = {                                     \
        .key_counts = fx_heatmap_##idx##_key_counts,                                              \
        .is_active = false,                                                                       \
    };                                                                                            \
                                                                                                   \
    static struct fx_heatmap_config fx_heatmap_##idx##_config = {                                 \
        .pixel_map = &fx_heatmap_##idx##_pixel_map[0],                                            \
        .pixel_map_size = DT_INST_PROP_LEN(idx, pixels),                                          \
        .blending_mode = DT_INST_ENUM_IDX(idx, blending_mode),                                    \
        .color_cold_hue = DT_INST_PROP(idx, color_cold_hue),                                      \
        .color_hot_hue = DT_INST_PROP(idx, color_hot_hue),                                        \
        .saturation = DT_INST_PROP(idx, saturation),                                              \
        .lightness = DT_INST_PROP(idx, lightness),                                                \
    };                                                                                            \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(idx, &fx_heatmap_init, NULL, &fx_heatmap_##idx##_data,                  \
                          &fx_heatmap_##idx##_config, POST_KERNEL,                                \
                          CONFIG_APPLICATION_INIT_PRIORITY, &fx_heatmap_api);                     \
                                                                                                   \
    static int fx_heatmap_##idx##_event_handler(const zmk_event_t *event) {                       \
        const struct device *dev = DEVICE_DT_GET(DT_DRV_INST(idx));                               \
                                                                                                   \
        return fx_heatmap_on_key_press(dev, event);                                               \
    }                                                                                             \
                                                                                                   \
    ZMK_LISTENER(fx_heatmap_##idx, fx_heatmap_##idx##_event_handler);                             \
    ZMK_SUBSCRIPTION(fx_heatmap_##idx, zmk_position_state_changed);

DT_INST_FOREACH_STATUS_OKAY(FX_HEATMAP_DEVICE);
