#include <iostream>
#include <vector>
#include <cstdint>
#include <hidapi/hidapi.h>
#include <thread>
#include <chrono>
#include <ctime>
#define STB_IMAGE_IMPLEMENTATION
#include "lib/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "lib/stb_image_resize2.h"
#include <gif_lib.h>

const int WIDTH = 128, HEIGHT = 128;
const int OUTPUT_W = 128, OUTPUT_H = 128, NUM_CHANNELS = 3;

struct GifAnimation {
    uint8_t frame_count = 0;
    uint8_t delay_cs = 1; // delay in centiseconds (10 = 100ms)
    std::vector<uint8_t> concatenated_rgb565;
};

uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t r5 = (r >> 3) & 0x1F;
    uint16_t g6 = (g >> 2) & 0x3F;
    uint16_t b5 = (b >> 3) & 0x1F;
    uint16_t rgb565 = (r5 << 11) | (g6 << 5) | b5;

    return (rgb565 << 8) | (rgb565 >> 8);
}

static bool send_cmd(hid_device* ctrl, uint8_t cmd, uint8_t sub, uint8_t b8, uint8_t b9 = 0) {
    uint8_t buf[65] = {0};            // buf[0] = report ID 0, buf[1..64] = payload
    buf[1] = 0x04; buf[2] = cmd; buf[3] = sub;
    buf[9] = b8;  buf[10] = b9;       // payload bytes 8 and 9
    if (hid_send_feature_report(ctrl, buf, 65) < 0) {
        std::wcerr << L"feature: " << hid_error(ctrl) << L"\n";
        return false;
    }
    uint8_t rd[65] = {0};
    hid_get_feature_report(ctrl, rd, 65);   // best-effort handshake, ignore result
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    return true;
}

void upload_image_header(hid_device *handle, std::vector<uint8_t> frame_bytes) {
    std::vector<uint8_t> header(256, 0);
    header[0] = 0xAA; // Protocol start magic marker
    header[1] = 0x05; // Screen data command type

    uint32_t payload_size = frame_bytes.size();
    header[4] = static_cast<uint8_t>(payload_size & 0xFF);
    header[5] = static_cast<uint8_t>((payload_size >> 8) & 0xFF);
    header[6] = static_cast<uint8_t>((payload_size >> 16) & 0xFF);
    header[7] = static_cast<uint8_t>((payload_size >> 24) & 0xFF);

    header[8]  = WIDTH & 0xFF;
    header[9]  = (WIDTH >> 8) & 0xFF;
    header[10] = HEIGHT & 0xFF;
    header[11] = (HEIGHT >> 8) & 0xFF;
    header[16] = 0x01; // Single frame count

    for (size_t i = 0; i < header.size(); i += 60) {
        uint8_t packet[65] = {0};
        packet[0] = 0x00; // Report ID
        packet[1] = 0x00; // Report ID
        packet[2] = 0x05; // Command ID
        
        size_t chunk = std::min<size_t>(61, header.size() - i);
        std::copy(header.begin() + i, header.begin() + i + chunk, packet + 4);
        
        if (hid_write(handle, packet, 65) < 0) {
            std::cerr << "Error writing header chunk to device.\n";
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

bool upload_image_data(hid_device* ctrl, hid_device* data, const std::vector<uint8_t>& px) {
    constexpr size_t CHUNK = 4096;
    std::vector<uint8_t> payload(256, 0xFF);
    payload[0] = 1;  payload[1] = 0;                       // 1 frame, delay 0
    payload.insert(payload.end(), px.begin(), px.end());   // 32768 bytes, LE RGB565
    size_t chunks = (payload.size() + CHUNK - 1) / CHUNK;  // 9
    payload.resize(chunks * CHUNK, 0xFF);

    if (!send_cmd(ctrl, 0x18, 0x00, 0x00)) return false;                  // START
    if (!send_cmd(ctrl, 0x72, 0x03, chunks & 0xFF, chunks >> 8)) return false;

    std::vector<uint8_t> out(CHUNK + 1);
    for (size_t i = 0; i < chunks; ++i) {
        out[0] = 0x00;
        std::copy_n(payload.begin() + i * CHUNK, CHUNK, out.begin() + 1);
        if (hid_write(data, out.data(), out.size()) < 0) {
            std::wcerr << L"write: " << hid_error(data) << L"\n";
            return false;
        }
        uint8_t ack[64] = {0};
        hid_read_timeout(data, ack, 64, 300);   // expect 01 5A 02 00…
    }
    return send_cmd(ctrl, 0x02, 0x00, 0x00);                              // SAVE
}

bool upload_gif_data(hid_device* ctrl, hid_device* data,
                       const std::vector<uint8_t>& px,
                       uint8_t frame_count = 1, uint8_t delay = 0) {
    constexpr size_t CHUNK = 4096;
    std::vector<uint8_t> payload(256, 0xFF);
    payload[0] = frame_count;
    payload[1] = delay;
    payload.insert(payload.end(), px.begin(), px.end());

    size_t chunks = (payload.size() + CHUNK - 1) / CHUNK;
    payload.resize(chunks * CHUNK, 0xFF);

    if (!send_cmd(ctrl, 0x18, 0x00, 0x00)) return false; // START
    if (!send_cmd(ctrl, 0x72, 0x03, chunks & 0xFF, (chunks >> 8) & 0xFF)) return false;

    std::vector<uint8_t> out(CHUNK + 1);
    for (size_t i = 0; i < chunks; ++i) {
        out[0] = 0x00;
        std::copy_n(payload.begin() + i * CHUNK, CHUNK, out.begin() + 1);
        if (hid_write(data, out.data(), out.size()) < 0) {
            std::wcerr << L"write: " << hid_error(data) << L"\n";
            return false;
        }
        uint8_t ack[64] = {0};
        hid_read_timeout(data, ack, 64, 300);
    }
    return send_cmd(ctrl, 0x02, 0x00, 0x00); // SAVE
}

std::vector<uint8_t> load_and_convert_image(const char* filepath) {
    int input_width, input_height, input_channels;

    std::vector<unsigned char> img_data(OUTPUT_H * OUTPUT_W * NUM_CHANNELS);

    unsigned char* raw_img = stbi_load(filepath, &input_width, &input_height, &input_channels, 3);
    unsigned char* res = stbir_resize_uint8_linear(
            raw_img,  input_width,  input_height,  0, // 0 defaults input stride to input_w * num_channels
            img_data.data(), OUTPUT_W, OUTPUT_H, 0, // 0 defaults output stride to output_w * num_channels // Tells the pixel layout helper it has an alpha channel to weight properly
            STBIR_RGB
        );

    stbi_image_free((void*)raw_img);

    if (!res) {
        std::cerr << "Failed to load image: " << filepath << "\n";
        return {};
    }
    
    std::vector<uint8_t> rgb565_bytes;
    rgb565_bytes.reserve(OUTPUT_W * OUTPUT_H * 2);

    for (int i = 0; i < OUTPUT_H*OUTPUT_W; ++i) {
        uint8_t  r = img_data[i*3 + 0];
        uint8_t  g = img_data[i*3 + 1];
        uint8_t  b = img_data[i*3 + 2];

        uint16_t conv_pixel = rgb888_to_rgb565(r, g, b);
    
        rgb565_bytes.push_back(conv_pixel >> 8);
        rgb565_bytes.push_back(conv_pixel & 0xFF);
    }

    // stbi_image_free(img_data);

    return rgb565_bytes;
}

GifAnimation load_gif(const char* filepath) {
    int error = 0;
    GifFileType* gif = DGifOpenFileName(filepath, &error);
    if (!gif) {
        std::cerr << "Failed to load gif." << std::endl;
        return {};
    }

    if (DGifSlurp(gif) == GIF_ERROR) {
        std::cerr << "Error reading gif data." << std::endl;
        DGifCloseFile(gif, &error);
        return {};
    }

    int rawW = gif->SWidth; 
    int rawH = gif->SHeight;
    int frameCount = std::min(gif->ImageCount, 255);

    GifAnimation anim;
    anim.frame_count = static_cast<uint8_t>(frameCount);
    anim.concatenated_rgb565.reserve(frameCount * OUTPUT_W * OUTPUT_H * 2);

    std::vector<uint8_t> canvas(rawW * rawH * 3, 0);
    std::vector<uint8_t> resized(OUTPUT_W * OUTPUT_H * 3);

uint32_t total_delay_cs = 0;
    int valid_delay_frames = 0;

    for (int f = 0; f < frameCount; ++f) {
        SavedImage* frame = &gif->SavedImages[f];
        GifImageDesc& desc = frame->ImageDesc;
        ColorMapObject* cmap = desc.ColorMap ? desc.ColorMap : gif->SColorMap;

        int transparent_idx = -1;
        int disposal_mode = 0;

        for (int e = 0; e < frame->ExtensionBlockCount; ++e) {
            ExtensionBlock& ext = frame->ExtensionBlocks[e];
            if (ext.Function == GRAPHICS_EXT_FUNC_CODE && ext.ByteCount >= 4) {
                uint8_t packed = ext.Bytes[0];
                disposal_mode = (packed >> 2) & 0x07;
                if (packed & 0x01) {
                    transparent_idx = static_cast<uint8_t>(ext.Bytes[3]);
                }

                uint16_t frame_delay_cs = static_cast<uint8_t>(ext.Bytes[1]) |
                                          (static_cast<uint8_t>(ext.Bytes[2]) << 8);

                if (frame_delay_cs > 0) {
                    total_delay_cs += frame_delay_cs;
                    valid_delay_frames++;
                }
            }
        }

        std::vector<uint8_t> prev_canvas;
        if (disposal_mode == 3) {
            prev_canvas = canvas;
        }

        if (cmap) {
            for (int y = 0; y < desc.Height; ++y) {
                int dst_y = desc.Top + y;
                if (dst_y < 0 || dst_y >= rawH) continue;

                for (int x = 0; x < desc.Width; ++x) {
                    int dst_x = desc.Left + x;
                    if (dst_x < 0 || dst_x >= rawW) continue;

                    uint8_t color_idx = frame->RasterBits[y * desc.Width + x];
                    if (color_idx == transparent_idx || color_idx >= cmap->ColorCount) {
                        continue;
                    }

                    GifColorType rgb = cmap->Colors[color_idx];
                    size_t dst_offset = (dst_y * rawW + dst_x) * 3;
                    canvas[dst_offset + 0] = rgb.Red;
                    canvas[dst_offset + 1] = rgb.Green;
                    canvas[dst_offset + 2] = rgb.Blue;
                }
            }
        }

        stbir_resize_uint8_linear(
            canvas.data(), rawW, rawH, 0,
            resized.data(), OUTPUT_W, OUTPUT_H, 0,
            STBIR_RGB
        );

        for (int p = 0; p < OUTPUT_W * OUTPUT_H; ++p) {
            uint8_t r = resized[p * 3 + 0];
            uint8_t g = resized[p * 3 + 1];
            uint8_t b = resized[p * 3 + 2];

            uint16_t conv_pixel = rgb888_to_rgb565(r, g, b);
            anim.concatenated_rgb565.push_back(conv_pixel >> 8);
            anim.concatenated_rgb565.push_back(conv_pixel & 0xFF);
        }

        if (disposal_mode == 2) {
            for (int y = 0; y < desc.Height; ++y) {
                int dst_y = desc.Top + y;
                if (dst_y < 0 || dst_y >= rawH) continue;
                for (int x = 0; x < desc.Width; ++x) {
                    int dst_x = desc.Left + x;
                    if (dst_x < 0 || dst_x >= rawW) continue;
                    size_t dst_offset = (dst_y * rawW + dst_x) * 3;
                    canvas[dst_offset + 0] = 0;
                    canvas[dst_offset + 1] = 0;
                    canvas[dst_offset + 2] = 0;
                }
            }
        } else if (disposal_mode == 3 && !prev_canvas.empty()) {
            canvas = std::move(prev_canvas);
        }
    }

    int avg_delay_cs = (valid_delay_frames > 0)
                     ? static_cast<int>(total_delay_cs / valid_delay_frames)
                     : 10;

    int device_delay = avg_delay_cs * 10;

    anim.delay_cs = static_cast<uint8_t>(std::clamp(device_delay, 1, 255));

    std::cout << "Detected GIF frame delay: " << avg_delay_cs << " cs ("
              << (avg_delay_cs * 10) << " ms)\n";

    DGifCloseFile(gif, &error);
    return anim;
}