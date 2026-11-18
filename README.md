# Ajazz AK820 Pro Linux C Driver

A simple C file for controlling and customizing the built-in TFT screen on the Ajazz AK820 Pro mechanical keyboard on Linux

---

## Features

* **Static Images:** Upload custom static images to the keyboard's TFT display.
* **Animated GIFs:** Upload animated GIFs to the display (laggy).

---

## Building from Source

just clone and run `make`. run the main file with the flags.

---

## Usage

Make sure your keyboard is connected via **USB wired mode**, then run the compiled binary from the root of the repository:

```bash
./build/main <path-to-media> <flag>
```

### Flags

`i` - Image - Upload a static image to the TFT screen   
`g` - GIF - Upload an animated GIF to the TFT screen
