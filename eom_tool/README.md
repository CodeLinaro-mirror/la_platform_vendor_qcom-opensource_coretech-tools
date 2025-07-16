# EOM Tool README
===============

## Introduction
---------------

The EOM (Eye Opening Monitor) tool is a C program designed to run EOM tests on multiple lanes of a PCIe or USB device, primarily for hardware validation, signal integrity, and link quality analysis. It interacts with eom driver via an IOCTL interface.

**License:**  
SPDX-License-Identifier: BSD-3-Clause-Clear  
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.


## Features
------------

*   Supports PCIe and USB devices
*   Allows running EOM tests on multiple lanes (max lanes: 16, max SBDFs: 8)
*   Configurable via command-line arguments for flexible control
*   Generates output in JSON format for integration and analysis
*   Includes a spinner animation and robust progress reporting
*   Linux and Android support (buildable with Android.mk)

## Command-Line Arguments
-------------------------

*   `-d <pcie|usb>`: Select the type of device (PCIe or USB)
*   `-s <sbdf list>`: List of SBDFs for PCIe devices (comma-separated)
*   `-m <lane mask>`: Specify the lane mask (default: 0xff)
*   `-a`: Run EOM on all lanes
*   `-f <output_file>`: Specify the path and output file name
*   `-t <dwell time in us>`: Specify the dwell time in microseconds
*   `-g`: Force selecting the device
*   `-r <RC index>`: Specify the RC index
*   `-l <Lane number>`: Specify the lane number

## Usage
-----

To use the EOM tool, run the executable with the desired command-line arguments. For example:

```bash
./eom_tool -d pcie -s 0:0:0.0,1:0:0.0 -m 0x3,0x3 -a -f ./eom_output
```

This will run the EOM test on all lanes of the specified PCIe devices and generate output in the `eom_output` file.

## More Usage Examples
-----------------

### Basic PCIe EOM Test
```bash
./eom_tool -d pcie -s 0:0:0.0 -a -f ./eom_results.json
```

### Multiple Devices with Custom Lane Masks
```bash
./eom_tool -d pcie -s 0:0:0.0,1:0:0.0 -m 0x3,0x7 -f ./multi_device_eom.json
```

### USB Device with Custom Dwell Time
```bash
./eom_tool -d usb -s 0:0:0.0 -a -t 200000 -f ./usb_eom_results.json
```

### Force Device Selection
```bash
./eom_tool -d pcie -s 0:0:0.0,1:0:0.0 -g -a -f ./forced_eom.json
```

## Output
--------

The output of the EOM tool is in JSON format and includes information about the test results, such as the interface, instance, time scale, time units, voltage scale, voltage units, and EOM data for each lane, including the lane number, and eye data.

## Output Format
---------------

The tool generates JSON output with the following structure:

```json
{
  "version": "1.0.0",
  "results": [
    {
      "chip_info": {
        "id": "chip_id_string",
        "family": 12345,
        "version": 1,
        "serial_num": "serial_number"
      },
      "interface": "pcie",
      "instance": 0,
      "time_scale": 1.95,
      "time_units": "ps",
      "voltage_scale": 1.5,
      "voltage_units": "mV",
      "note": "",
      "lanes": [
        {
          "lane_number": 0,
          "note": "",
          "eye": [
            [x_coordinate, y_coordinate, error_count],
            ...
          ]
        }
      ]
    }
  ]
}
```

## File Overview
----------------

- **eom_tool.c**: Main source code for the EOM tool; contains argument parsing, driver IOCTLs, lane/thread management, JSON output, etc.
- **Android.mk**: Android build integration; builds the tool as a native executable, installs to vendor partition, links kernel/user-space libraries.
- **vendor-product.mk**: Used by Android builds to include this tool as part of a larger product image (modify as needed for device integration).

## Requirements
--------------

### Software Requirements
- Linux operating system **OR** Android build environment
- EOM kernel driver loaded and `/dev/eom` available on target device
- `linux/eom_ioctl.h` header file (kernel driver interface)
- Android NDK (for Android)
- pthread library (POSIX threads)


### Hardware Requirements
- PCIe or USB devices with EOM capability
- Appropriate device drivers loaded
- Root access to device for driver control and device node access

## Build & Installation
-----------------------

### Android Build

Use the provided Android.mk for integration in Android products:

- Include `Android.mk` in your build tree.
- The tool will be built and installed to the vendor partition as `eom_tool` under `${TARGET_OUT_VENDOR}/bin`.
- Make sure kernel headers (for IOCTL) and device drivers are available.

### Dependencies

The tool relies on:

- Standard C/POSIX libraries
- pthread (multithreading)
- Kernel headers (device IOCTLs)

## Known Limitations
-------------------

- Maximum of 8 SBDF targets (`MAX_SBDFS`)
- Maximum of 16 lanes per device (`MAX_LANES`)
- Requires root privileges for device access
- Platform-specific sysfs paths for chip information

## Troubleshooting
-----------------

### Common Issues

1. **Device Not Found**
   ```
   Error: Failed to open EOM device
   Solution: Ensure EOM kernel driver is loaded and /dev/eom exists
   ```

2. **Permission Denied**
   ```
   Error: Failed to open lane device
   Solution: Run with appropriate privileges (usually root)
   ```

3. **Invalid SBDF Format**
   ```
   Error: Invalid SBDF format 'x:x:x.x'
   Solution: Use format S:B:D.F or S:B:D:F (e.g., 0:0:0.0)


## Known Issues
-------------

*   The tool assumes that the `eom_ioctl.h` header file is available and includes the necessary definitions for the EOM IOCTLs.

## Future Development
-------------------

*   Improve error handling and robustness
*   Add support for additional device types and interfaces
*   Enhance the output format to include link speed.
*   Add Test infrastructure for testing the EOM tool.

## License
----------
SPDX-License-Identifier: BSD-3-Clause-Clear  
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
