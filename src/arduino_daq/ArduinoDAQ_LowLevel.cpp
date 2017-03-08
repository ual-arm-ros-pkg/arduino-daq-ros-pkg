/* +---------------------------------------------------------------------------+
   |                  University of Almeria ARM-eCar module                    |
   |                                                                           |
   |   Copyright (C) 2014-16  University of Almeria                            |
   +---------------------------------------------------------------------------+ */

#include <arduino_daq/ArduinoDAQ_LowLevel.h>
#include <mrpt/system/threads.h> // for sleep()
#include <arduino_daq/ArduinoDAQ_LowLevel.h>
#include <functional>
#include <cstring>

#ifdef HAVE_ROS
#include <ros/console.h>
#endif

#include <iostream>

using namespace std;
using namespace mrpt;
using namespace mrpt::utils;

//#define DEBUG_TRACES

#ifdef HAVE_ROS
void log_callback(const std::string &msg, const mrpt::utils::VerbosityLevel level, const std::string &loggerName, const mrpt::system::TTimeStamp timestamp, void *userParam)
{
	ROS_INFO("%s",msg.c_str());
}
#endif

ArduinoDAQ_LowLevel::ArduinoDAQ_LowLevel() :
	mrpt::utils::COutputLogger("ArduinoDAQ_LowLevel"),
#ifdef HAVE_ROS
	m_nh_params("~"),
#endif
#ifndef _WIN32
	m_serial_port_name("/dev/serial/by-id/usb-FTDI_TTL232R-3V3_FT9J3NTD-if00-port0"),
#else
	m_serial_port_name("COM3"),
#endif
	m_serial_port_baudrate(9600)
{
#ifdef HAVE_ROS
	this->logRegisterCallback(&log_callback, this);
#endif
}

ArduinoDAQ_LowLevel::~ArduinoDAQ_LowLevel()
{
}

bool ArduinoDAQ_LowLevel::initialize()
{
#ifdef HAVE_ROS
	m_nh_params.getParam("SERIAL_PORT",m_serial_port_name);
	m_nh_params.getParam("SERIAL_PORT_BAUDRATE",m_serial_port_baudrate);
#endif

	// Try to connect...
	if (this->AttemptConnection())
	{
		MRPT_LOG_INFO("Connection OK to ArduinoDAQ.");
	}
	else return false;


#ifdef HAVE_ROS
	// Subscribers: GPIO outputs
	m_sub_auto_pos.resize(13);
	for (int i=0;i<13;i++) {
		auto fn = boost::bind(&ArduinoDAQ_LowLevel::daqSetDigitalPinCallback, this, i, _1);
		m_sub_auto_pos[i] = m_nh.subscribe<std_msgs::Bool>( mrpt::format("arduino_daq_GPIO_output%i",i), 10, fn);
	}

	// Subscribers: DAC outputs
	m_sub_dac.resize(4);
	for (int i=0;i<4;i++) {
		auto fn = boost::bind(&ArduinoDAQ_LowLevel::daqSetDACCallback, this, i, _1);
		m_sub_auto_pos[i] = m_nh.subscribe<std_msgs::Float64>( mrpt::format("arduino_daq_dac%i",i), 10, fn);
	}
#endif
	return true;
}

bool ArduinoDAQ_LowLevel::iterate()
{
	// Main module loop code.
	const size_t MAX_FRAMES_PER_ITERATE = 20;
	size_t nFrames = 0;

	if (!m_serial.isOpen())
		return false;

	std::vector<uint8_t> rxFrame;
	while (++nFrames<MAX_FRAMES_PER_ITERATE && ReceiveFrameFromController(rxFrame))
	{
		// Process them:
		//MRPT_LOG_INFO_STREAM  << "Rx frame, len=" << rxFrame.size();
		if (rxFrame.size() >= 5)
		{
			switch (rxFrame[1])
			{
				case 0x92:
				{
					TFrame_ADC_readings rx;
					::memcpy((uint8_t*)&rx, &rxFrame[0], sizeof(rx));

					if (m_adc_callback)
						m_adc_callback(rx.payload);
					break;
				}
			};
		}
	}

	return true;
}

#ifdef HAVE_ROS
void ArduinoDAQ_LowLevel::daqSetDigitalPinCallback(int pin, const std_msgs::Bool::ConstPtr& msg)
{
    ROS_INFO("GPIO: output[%i]=%s", pin, msg->data ? "true":"false" );

    if (!CMD_GPIO_output(pin,msg->data)) {
        ROS_ERROR("*** Error sending CMD_GPIO_output!!! ***");
    }
}

void ArduinoDAQ_LowLevel::daqSetDACCallback(int dac_index, const std_msgs::Float64::ConstPtr& msg)
{
    ROS_INFO("DAC: channel[%i]=%f V", dac_index, msg->data);

    if (!CMD_DAC(dac_index,msg->data)) {
        ROS_ERROR("*** Error sending CMD_DAC!!! ***");
    }

}
#endif

bool ArduinoDAQ_LowLevel::AttemptConnection()
{
	if (m_serial.isOpen()) return true; // Already open.

	try {
		m_serial.open(m_serial_port_name);

		// Set basic params:
		m_serial.setConfig(m_serial_port_baudrate);
		m_serial.setTimeouts(100,0,10,0,50);

		return true;
	}
	catch (std::exception &e)
	{
		MRPT_LOG_DEBUG_FMT("[ArduinoDAQ_LowLevel::AttemptConnection] COMMS error: %s", e.what() );
		return false;
	}
}


/** Sends a binary packet (returns false on COMMS error) */
bool ArduinoDAQ_LowLevel::WriteBinaryFrame(const uint8_t *full_frame, const size_t full_frame_len)
{
	if (!AttemptConnection()) return false;

	ASSERT_(full_frame!=NULL);

	try
	{
#ifdef DEBUG_TRACES
		{
			std::string s;
			s+=mrpt::format("TX frame (%u bytes): ", (unsigned int) full_frame_len);
			for (size_t i=0;i< full_frame_len;i++)
				s+=mrpt::format("%02X ", full_frame[i]);
			ROS_INFO("Tx frame: %s", s.c_str());
		}
#endif

		m_serial.WriteBuffer(full_frame,full_frame_len);
		return true;
	}
	catch (std::exception &)
	{
		return false;
	}
}

bool ArduinoDAQ_LowLevel::ReceiveFrameFromController(std::vector<uint8_t> &rxFrame)
{
	rxFrame.clear();
	size_t	nFrameBytes = 0;
	std::vector<uint8_t> buf;
	buf.resize(0x10000);
	buf[0] = buf[1] = 0;

	size_t	lengthField;

	/*
	START_FLAG   |  OPCODE  |  DATA_LEN   |   DATA      |    CHECKSUM    | END_FLAG |
	  0x69          1 byte      1 byte       N bytes       =sum(data)       0x96
	*/

	//                                   START_FLAG     OPCODE + LEN       DATA      CHECKSUM +  END_FLAG
	while ( nFrameBytes < (lengthField=(    1        +     1   +  1    +  buf[2]  +     1     +     1      )  ) )
	{
		if (lengthField>200)
		{
			nFrameBytes = 0;	// No es cabecera de trama correcta
			buf[1]=buf[2]=0;
			MRPT_LOG_INFO("[rx] Reset frame (invalid len)");
		}

		size_t nBytesToRead;
		if (nFrameBytes<3)
			nBytesToRead = 1;
		else
			nBytesToRead = (lengthField) - nFrameBytes;

		size_t 	nRead;
		try
		{
			nRead = m_serial.Read( &buf[0] + nFrameBytes, nBytesToRead );
		}
		catch (std::exception &e)
		{
			// Disconnected?
			std::cerr << "[ArduinoDAQ_LowLevel::ReceiveFrameFromController] Comms error: " << e.what() << std::endl;
			return false;
		}

		if ( !nRead && !nFrameBytes )
		{
			//cout << "[rx] No frame (buffer empty)\n";
			return false;
		}

		if (nRead<nBytesToRead)
			mrpt::system::sleep(1);

		// Lectura OK:
		// Check start flag:
		bool is_ok = true;

		if (!nFrameBytes && buf[0]!= FRAME_START_FLAG )
		{
			is_ok = false;
			//cout << "[rx] Reset frame (start flag)\n";
		}

		if (nFrameBytes>2 && nFrameBytes+nRead==lengthField)
		{
			if (buf[nFrameBytes+nRead-1]!=FRAME_END_FLAG)
			{
				is_ok= false;
				//cout << "[rx] Reset frame (end flag)\n";
			}
			//else { cout << "[rx] Frame OK\n"; }
		}

		MRPT_TODO("Checksum");

		if (is_ok)
		{
			nFrameBytes+=nRead;
		}
		else
		{
			nFrameBytes = 0;	// No es cabecera de trama correcta
			buf[1]=buf[2]=0;
		}
	}

	// Frame received
	lengthField= buf[2]+5;
	rxFrame.resize(lengthField);
	::memcpy( &rxFrame[0], &buf[0], lengthField);

#ifdef DEBUG_TRACES
		{
			std::string s;
			s+=mrpt::format("RX frame (%u bytes): ", (unsigned int) lengthField);
			for (size_t i=0;i< lengthField;i++)
				s+=mrpt::format("%02X ", rxFrame[i]);
			ROS_INFO("%s", s.c_str());
		}
#endif

	// All OK
	return true;
}


bool ArduinoDAQ_LowLevel::CMD_GPIO_output(int pin, bool pinState)
{
    TFrameCMD_GPIO_output cmd;
    cmd.payload.pin_index = pin;
    cmd.payload.pin_value = pinState ? 1:0;

    cmd.calc_and_update_checksum();

    return WriteBinaryFrame(reinterpret_cast<uint8_t*>(&cmd),sizeof(cmd));
}

//!< Sets the clutch
bool ArduinoDAQ_LowLevel::CMD_DAC(int dac_index,double dac_value_volts)
{
    uint16_t dac_counts = 4096 * dac_value_volts / 5.0;
    mrpt::utils::saturate(dac_counts, uint16_t(0), uint16_t(4095));

    TFrameCMD_SetDAC cmd;
    cmd.payload.dac_index = dac_index;
    cmd.payload.dac_value_HI = dac_counts >> 8;
    cmd.payload.dac_value_LO = dac_counts & 0x00ff;

    cmd.calc_and_update_checksum();

    return WriteBinaryFrame(reinterpret_cast<uint8_t*>(&cmd),sizeof(cmd));
}

bool ArduinoDAQ_LowLevel::IsConnected() const
{
	return m_serial.isOpen();
}

bool ArduinoDAQ_LowLevel::CMD_ADC_START(const TFrameCMD_ADC_start_payload_t &adc_config)
{
	TFrameCMD_ADC_start cmd;
	cmd.payload = adc_config;
	cmd.calc_and_update_checksum();

	return WriteBinaryFrame(reinterpret_cast<uint8_t*>(&cmd), sizeof(cmd));
}
bool ArduinoDAQ_LowLevel::CMD_ADC_STOP()
{
	TFrameCMD_ADC_stop cmd;
	cmd.calc_and_update_checksum();

	return WriteBinaryFrame(reinterpret_cast<uint8_t*>(&cmd), sizeof(cmd));
}
