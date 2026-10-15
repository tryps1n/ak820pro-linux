#include <iostream>
#include <hidapi/hidapi.h>
#include "stb_image.h"
#include "lib.cpp"

// 0c45:800a
constexpr unsigned short VENDOR_ID = 0x0c45;
constexpr unsigned short PRODUCT_ID = 0x800a;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cout << "Format:" 
                  << "  ./main <image_path>" << std::endl;
        return 0;
    }

    if (hid_init() != 0) {
        std::cerr << "Failed to initialise hidapi" << std::endl;
        return 1;
    }

    struct hid_device_info *devs, *cur_dev;
    devs = hid_enumerate(VENDOR_ID, PRODUCT_ID);

    if (!devs) {
        std::cerr << "No device matching with ids found." << std::endl;
        hid_exit();
        return 1;
    }

    std::cout << "Found device successfully." << std::endl;

    cur_dev = devs;
    hid_device_info *hid_device_iface2, *hid_device_iface3;
    while (cur_dev != nullptr) {
        if (cur_dev->interface_number == 2) hid_device_iface2 = cur_dev;
        if (cur_dev->interface_number == 3) hid_device_iface3 = cur_dev;
        cur_dev = cur_dev->next;
    }

    hid_device *devIface2 = hid_open_path(hid_device_iface2->path);
    if (!devIface2) {
        std::cerr << "Error, couldn't open path to dev interface 2." << std::endl;
        return 2;
    }
    hid_device *devIface3 = hid_open_path(hid_device_iface3->path);
    if (!devIface3) {
        std::cerr << "Error, couldn't open path to dev interface 3." << std::endl;
        return 3;
    }

    const char* filepath = argv[1];
    std::vector<uint8_t> pixels = load_and_convert_image(filepath);
    
    upload_image_data(devIface3, devIface2, pixels); 

    hid_free_enumeration(devs);
    hid_close(devIface2);
    hid_close(devIface3);
    hid_exit();

    return 0;
}