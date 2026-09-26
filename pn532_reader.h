#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "pn532_protocol.h"

// Polls the I2C PN532 one short transaction at a time. No IRQ or reset pin is
// wired. A failed transaction backs off instead of blocking fingerprint work.
class Pn532Reader {
 public:
  enum class Event : uint8_t { None, Ready, Unavailable, Lost, NoCard, Card };
  Pn532Reader(uint8_t sda, uint8_t scl) : sda_(sda), scl_(scl) {}

  Event tick(uint8_t uid[10], uint8_t& uidLength, bool startAllowed) {
    const uint32_t now = millis();
    if (stage_ == Stage::Idle) {
      if (!startAllowed || !reached(now, nextAt_)) return Event::None;
      if (!busFree()) return fail(now, "I2C line low");
      if (command_ == Command::Sam) sendHostAck(); // abandon a command left by an ESP-only reset
      if (!startCommand(now)) return fail(now, "command write");
      return Event::None;
    }

    if (reached(now, deadline_)) return fail(now, stage_ == Stage::WaitAck ? "ACK timeout" : "response timeout");
    // The PN532 may briefly stretch SCL while waking. Only call it stuck
    // after the deadline, not on the first LOW sample.
    if (!busFree()) return Event::None;
    if (!reached(now, nextAt_)) return Event::None;
    nextAt_ = now + 10;
    if (Wire.requestFrom(Pn532Protocol::kAddress, uint8_t(1)) != 1) return fail(now, "status read");
    int status = Wire.read();
    if (status < 0) return fail(now, "status empty");
    if (!(status & 1)) return Event::None;

    if (stage_ == Stage::WaitAck) {
      uint8_t ack[7];
      if (!readFrame(ack, sizeof(ack))) return fail(now, "ACK read");
      if (!Pn532Protocol::isAck(ack, sizeof(ack))) return fail(now, "bad ACK");
      stage_ = Stage::WaitResponse;
      deadline_ = now + (command_ == Command::Poll ? 500 : 700);
      nextAt_ = now + 10;
      return Event::None;
    }

    uint8_t wire[32], payload[24];
    if (!readFrame(wire, sizeof(wire))) return fail(now, "response read");
    const int length = Pn532Protocol::responsePayload(wire, sizeof(wire), opcode(), payload, sizeof(payload));
    if (length < 0) return fail(now, "bad response");
    stage_ = Stage::Idle;
    nextAt_ = now + 10;
    switch (command_) {
      case Command::Sam:
        if (length != 0) return fail(now, "SAM response");
        command_ = Command::Firmware;
        return Event::None;
      case Command::Firmware:
        if (length != 4 || payload[0] != 0x32) return fail(now, "firmware response");
        command_ = Command::Retries;
        return Event::None;
      case Command::Retries:
        if (length != 0) return fail(now, "retry config");
        command_ = Command::Poll;
        ready_ = true;
        failureReported_ = false;
        lastError_ = "none";
        return Event::Ready;
      case Command::Poll: {
        nextAt_ = now + 120;
        const int found = Pn532Protocol::cardUid(payload, length, uid, uidLength);
        if (found < 0) return fail(now, "card response");
        return found ? Event::Card : Event::NoCard;
      }
    }
    return Event::None;
  }

  bool ready() const { return ready_; }
  const char* lastError() const { return lastError_; }

 private:
  enum class Command : uint8_t { Firmware, Sam, Retries, Poll };
  enum class Stage : uint8_t { Idle, WaitAck, WaitResponse };
  Command command_ = Command::Sam;
  Stage stage_ = Stage::Idle;
  bool ready_ = false;
  bool failureReported_ = false;
  const char* lastError_ = "none";
  uint32_t nextAt_ = 0, deadline_ = 0;
  const uint8_t sda_, scl_;

  static bool reached(uint32_t now, uint32_t due) { return int32_t(now - due) >= 0; }
  bool busFree() const { return digitalRead(sda_) == HIGH && digitalRead(scl_) == HIGH; }

  uint8_t opcode() const {
    switch (command_) {
      case Command::Firmware: return 0x02;
      case Command::Sam: return 0x14;
      case Command::Retries: return 0x32;
      case Command::Poll: return 0x4a;
    }
    return 0;
  }

  bool startCommand(uint32_t now) {
    const uint8_t firmware[] = {0x02};
    const uint8_t sam[] = {0x14, 0x01, 0x14, 0x00}; // normal mode, no IRQ
    const uint8_t retries[] = {0x32, 0x05, 0xff, 0x01, 0x00}; // one finite card search
    const uint8_t poll[] = {0x4a, 0x01, 0x00}; // one ISO14443A card
    const uint8_t* data = firmware;
    size_t size = sizeof(firmware);
    if (command_ == Command::Sam) { data = sam; size = sizeof(sam); }
    else if (command_ == Command::Retries) { data = retries; size = sizeof(retries); }
    else if (command_ == Command::Poll) { data = poll; size = sizeof(poll); }
    uint8_t frame[16];
    size_t frameSize = Pn532Protocol::commandFrame(data, size, frame, sizeof(frame));
    if (!frameSize) return false;
    Wire.beginTransmission(Pn532Protocol::kAddress);
    bool allWritten = Wire.write(frame, frameSize) == frameSize;
    if (Wire.endTransmission() != 0 || !allWritten) return false;
    stage_ = Stage::WaitAck;
    deadline_ = now + 700;
    nextAt_ = now + 10;
    return true;
  }

  static bool readFrame(uint8_t* out, size_t size) {
    if (Wire.requestFrom(Pn532Protocol::kAddress, uint8_t(size)) != size) return false;
    for (size_t i = 0; i < size; ++i) {
      int b = Wire.read();
      if (b < 0) return false;
      out[i] = uint8_t(b);
    }
    return true;
  }

  static void sendHostAck() {
    Wire.beginTransmission(Pn532Protocol::kAddress);
    Wire.write(Pn532Protocol::kAck, sizeof(Pn532Protocol::kAck));
    Wire.endTransmission();
  }

  Event fail(uint32_t now, const char* reason) {
    if (busFree()) sendHostAck();
    const bool wasReady = ready_;
    lastError_ = reason;
    ready_ = false;
    stage_ = Stage::Idle;
    command_ = Command::Sam;
    nextAt_ = now + 10000;
    if (wasReady) { failureReported_ = true; return Event::Lost; }
    if (!failureReported_) { failureReported_ = true; return Event::Unavailable; }
    return Event::None;
  }
};
