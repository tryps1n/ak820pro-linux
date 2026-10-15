#include <iostream>
#include <vector>
#include <cstdint>
#include <hidapi/hidapi.h>
#include <thread>
#include <chrono>
#include <ctime>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

const int WIDTH = 128, HEIGHT = 128;

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

    // 3. Transmit the 256-byte header in 60-byte chunks
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

void send_time_sync(hid_device* handle) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm* local_time = std::localtime(&now_c);

    int year = local_time->tm_year + 1900 - 2000; // e.g., 26 for 2026
    int month = local_time->tm_mon + 1;
    int day = local_time->tm_mday;
    int hour = local_time->tm_hour;
    int minute = local_time->tm_min;
    int second = local_time->tm_sec;

    std::vector<uint8_t> packet(64, 0);
    packet[0] = 0x00; // report ID
    packet[1] = 0x12; // command for time sync

    packet[2] = static_cast<uint8_t>(year);
    packet[3] = static_cast<uint8_t>(month);
    packet[4] = static_cast<uint8_t>(day);
    packet[5] = static_cast<uint8_t>(hour);
    packet[6] = static_cast<uint8_t>(minute);
    packet[7] = static_cast<uint8_t>(second);

    int result = hid_send_feature_report(handle, packet.data(), packet.size());
    
    if (result < 0) {
        std::cerr << "Failed to send data." << std::endl;
    }
    else {
        std::cout << "Time sync command sent successfully." << std::endl;
    }

}

std::vector<uint8_t> load_and_convert_image(const char* filepath) {
   int width, height, channels;
   unsigned char* img_data = stbi_load(filepath, &width, &height, &channels, 3);
    
    if (!img_data) {
        std::cerr << "Failed to load image: " << filepath << "\n";
        return {};
    }
    
    if (width != 128 || height != 128) {
        std::cout << "Warning: Image is " << width << "x" << height << ". Ideally it should be 128x128 pixels.\n";
    }

    std::vector<uint8_t> rgb565_bytes;
    rgb565_bytes.reserve(width * height * 2);

    for (int i = 0; i < width*height; ++i) {
        uint8_t  r = img_data[i*3 + 0];
        uint8_t  g = img_data[i*3 + 1];
        uint8_t  b = img_data[i*3 + 2];

        uint16_t conv_pixel = rgb888_to_rgb565(r, g, b);
    
        rgb565_bytes.push_back(conv_pixel >> 8);
        rgb565_bytes.push_back(conv_pixel & 0xFF);
    }

    stbi_image_free(img_data);

    return rgb565_bytes;
}