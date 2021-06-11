// Copyright 2020 F1TENTH Foundation
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//
//   * Neither the name of the {copyright_holder} nor the names of its
//     contributors may be used to endorse or promote products derived from
//     this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// -*- mode:c++; fill-column: 100; -*-

#include "vesc_driver/vesc_interface.hpp"
#include "vesc_driver/vesc_packet_factory.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace vesc_driver
{

class VescInterface::Impl
{
public:
  Impl()
  : io_service_(),
    serial_port_(io_service_)
  {}

  void rxThread();

  std::unique_ptr<std::thread> rx_thread_;
  bool rx_thread_run_;
  PacketHandlerFunction packet_handler_;
  ErrorHandlerFunction error_handler_;
  boost::asio::io_service io_service_;
  boost::asio::serial_port serial_port_;
  std::mutex serial_mutex_;
};

void VescInterface::Impl::rxThread()
{
  // the size varies dynamically and start from 0. capacity is 4096 fixed.
  Buffer buffer;
  buffer.reserve(4096);

  // buffer with fixed size used to read from serial
  Buffer bufferRx(4096);

  while (rx_thread_run_) {
    int bytes_needed = VescFrame::VESC_MIN_FRAME_SIZE;
    if (!buffer.empty()) {
      // search buffer for valid packet(s)
      Buffer::iterator iter(buffer.begin());
      Buffer::iterator iter_begin(buffer.begin());
      while (iter != buffer.end()) {
        // check if valid start-of-frame character
        if (VescFrame::VESC_SOF_VAL_SMALL_FRAME == *iter ||
          VescFrame::VESC_SOF_VAL_LARGE_FRAME == *iter)
        {
          // good start, now attempt to create packet
          std::string error;
          VescPacketConstPtr packet =
            VescPacketFactory::createPacket(iter, buffer.end(), &bytes_needed, &error);
          if (packet) {
            // good packet, check if we skipped any data
            if (std::distance(iter_begin, iter) > 0) {
              std::ostringstream ss;
              ss << "Out-of-sync with VESC, unknown data leading valid frame. Discarding " <<
                std::distance(iter_begin, iter) << " bytes.";
              error_handler_(ss.str());
            }
            // call packet handler
            packet_handler_(packet);
            // update state
            iter = iter + packet->frame().size();
            iter_begin = iter;
            // continue to look for another frame in buffer
            continue;
          } else if (bytes_needed > 0) {
            // need more data, break out of while loop
            break;  // for (iter_sof...
          } else {
            // else, this was not a packet, move on to next byte
            error_handler_(error);
          }
        }

        iter++;
      }

      // if iter is at the end of the buffer, more bytes are needed
      if (iter == buffer.end()) {
        bytes_needed = VescFrame::VESC_MIN_FRAME_SIZE;
      }

      // erase "used" buffer
      if (std::distance(iter_begin, iter) > 0) {
        std::ostringstream ss;
        ss << "Out-of-sync with VESC, discarding " << std::distance(iter_begin, iter) << " bytes.";
        error_handler_(ss.str());
      }
      buffer.erase(buffer.begin(), iter);
    }

    // attempt to read at least bytes_needed bytes from the serial port
    int bytes_to_read = std::min(bytes_needed, 4096);

    {
      boost::system::error_code ec;

      const size_t bytes_read = boost::asio::read(
        serial_port_,
        boost::asio::buffer(bufferRx, bufferRx.size()),
        boost::asio::transfer_exactly(bytes_to_read),
        ec
      );

      if (ec.value() > 0) {
        std::ostringstream ss;

        ss << "Serial port comunication error " << std::endl;
        ss << "failed " << ec.failed() << std::endl;
        ss << ec.value() << std::endl;
        ss << ec.category().name() << std::endl;
        ss << "try to read the bytes received " << std::endl;

        error_handler_(ss.str());
      }

      std::copy(bufferRx.begin(), bufferRx.begin() + bytes_read, std::back_inserter(buffer));

      if (bytes_needed > 0 && 0 == bytes_read && !buffer.empty()) {
        error_handler_("Possibly out-of-sync with VESC, read timout in the middle of a frame.");
      }
    }
    // Only attempt to read every 10 ms
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

VescInterface::VescInterface(
  const std::string & port,
  const PacketHandlerFunction & packet_handler,
  const ErrorHandlerFunction & error_handler)
: impl_(new Impl())
{
  setPacketHandler(packet_handler);
  setErrorHandler(error_handler);
  // attempt to conect if the port is specified
  if (!port.empty()) {
    connect(port);
  }
}

VescInterface::~VescInterface()
{
  disconnect();
}

void VescInterface::setPacketHandler(const PacketHandlerFunction & handler)
{
  // todo - definately need mutex
  impl_->packet_handler_ = handler;
}

void VescInterface::setErrorHandler(const ErrorHandlerFunction & handler)
{
  // todo - definately need mutex
  impl_->error_handler_ = handler;
}

void VescInterface::connect(const std::string & port)
{
  // todo - mutex?

  if (isConnected()) {
    throw SerialException("Already connected to serial port.");
  }

  // connect to serial port
  try {
    impl_->serial_port_.open(port);
    impl_->serial_port_.set_option(boost::asio::serial_port_base::baud_rate(115200));
    impl_->serial_port_.set_option(
      boost::asio::serial_port::flow_control(
        boost::asio::serial_port::flow_control::none));
    impl_->serial_port_.set_option(
      boost::asio::serial_port::parity(
        boost::asio::serial_port::parity::none));
    impl_->serial_port_.set_option(
      boost::asio::serial_port::stop_bits(
        boost::asio::serial_port::stop_bits::one));
  } catch (const std::exception & e) {
    std::stringstream ss;
    ss << "Failed to open the serial port " << port << " to the VESC. " << e.what();
    throw SerialException(ss.str().c_str());
  }

  // start up a monitoring thread
  impl_->rx_thread_run_ = true;
  impl_->rx_thread_ = std::unique_ptr<std::thread>(
    new std::thread(
      &VescInterface::Impl::rxThread, impl_.get()));
}

void VescInterface::disconnect()
{
  // todo - mutex?

  if (isConnected()) {
    // bring down read thread
    impl_->rx_thread_run_ = false;
    requestFWVersion();
    impl_->rx_thread_->join();
    impl_->serial_port_.close();
  }
}

bool VescInterface::isConnected() const
{
  // std::lock_guard<std::mutex> lock(impl_->serial_mutex_);
  return impl_->serial_port_.is_open();
}

void VescInterface::send(const VescPacket & packet)
{
  try {
    // std::lock_guard<std::mutex> lock(impl_->serial_mutex_);
    size_t written = impl_->serial_port_.write_some(
      boost::asio::buffer(packet.frame()));
    if (written != packet.frame().size()) {
      std::stringstream ss;
      ss << "Wrote " << written << " bytes, expected " << packet.frame().size() << ".";
      throw SerialException(ss.str().c_str());
    }
  } catch (const std::exception & e) {
    std::stringstream ss;
    ss << "Failed to open the serial port to the VESC. " << e.what();
    throw SerialException(ss.str().c_str());
  }
}

void VescInterface::requestFWVersion()
{
  send(VescPacketRequestFWVersion());
}

void VescInterface::requestState()
{
  send(VescPacketRequestValues());
}

void VescInterface::setDutyCycle(double duty_cycle)
{
  send(VescPacketSetDuty(duty_cycle));
}

void VescInterface::setCurrent(double current)
{
  send(VescPacketSetCurrent(current));
}

void VescInterface::setBrake(double brake)
{
  send(VescPacketSetCurrentBrake(brake));
}

void VescInterface::setSpeed(double speed)
{
  send(VescPacketSetRPM(speed));
}

void VescInterface::setPosition(double position)
{
  send(VescPacketSetPos(position));
}

void VescInterface::setServo(double servo)
{
  send(VescPacketSetServoPos(servo));
}

}  // namespace vesc_driver
