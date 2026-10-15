BUILD_DIR = build/
SRC_DIR = src/
TARGET = $(BUILD_DIR)/main
CC = g++
SRC = $(SRC_DIR)/main.cpp

.PHONY: all clean

all: $(TARGET) 

$(TARGET): $(SRC_DIR)/main.cpp clean
	mkdir build
	$(CC) $(SRC) -lhidapi-libusb -o $(BUILD_DIR)/main 
	@echo "Finished."
# 	./$(BUILD_DIR)/main

clean: $(TARGET) 
	rm -rf build/