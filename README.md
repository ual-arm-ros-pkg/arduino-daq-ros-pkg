arduino_daq
==================

This package contains a standalone C++ library and a ROS node for 
UAL eCAR's Arduino-based DAQ system, with 4 analog outputs (MAX5500), 
ADC inputs and GPIO support.

Used microcontroller is atmega328P, although it could be recompiled for other larger versions.

Examples of use & demos for ROS:

* Launch:

       roscore     # In one terminal
       rosrun arduino_daq arduino_daq_node    # In another terminal

* Setting a DAC analog value (volts):

        rostopic pub /arduino_daq_dac0 std_msgs/Float64 1.5

* Setting a GPIO digital output pin:

        rostopic pub /arduino_daq_GPIO_output7 std_msgs/Bool 1


Frame format:
=====================

Communication PC <-> arduino happens based on data frames as follow:

         START_FLAG   |  OPCODE  |  DATA_LEN   |   DATA      |    CHECKSUM    | END_FLAG |
           0x69          1 byte      1 byte       N bytes       =sum(data)       0x96

## Computer => controller
	* 0x10: Set DAC value. DATA_LEN = 3
		* DATA[0]   = DAC index
		* DATA[1:2] = DAC value (0x0000-0xffff)  (MSByte first!)
	* 0x11: Set GPIO pin. DATA_LEN = 2
		* DATA[0]   = Arduino-based pin number
		* DATA[1]   = 0/1
	* 0x12: Read GPIO pin. DATA_LEN = 1
		* DATA[0]   = Arduino-based pin number
	* 0x20: Start ADC continuous acquisition task
	* 0x21: Stop ADC task

## Controller => Computer
	* 0x80: Set DAC value ACK. DATA_LEN=0
	* 0x81: GPIO pin value ACK. DATA_LEN=0
	* 0x82: GPIO pin value read. DATA_LEN=1
	* 0x90: Start ADC ACK.
	* 0x91: Stop ADC ACK.
	* 0x92: ADC readings.
	* 0x93: Pulse counter readings.
	* 0xfe: Unknown command opcode. (Error)

ROS graph
=======================


<img width="300" src="https://raw.githubusercontent.com/ual-arm-ros-pkg/arduino-daq-ros-pkg/master/doc/rosgraph_arduino_daq.png" />
