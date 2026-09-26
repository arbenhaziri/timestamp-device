#pragma once

// All sensor I/O runs on the main loop; ring changes are deferred until a
// capture completes, because any extra command would cancel that capture.
HardwareSerial fingerprintUart(1);
FingerprintProtocol::Transport<HardwareSerial> fingerLink(fingerprintUart);
const uint8_t SFM_RING_RED = 3, SFM_RING_GREEN = 5, SFM_RING_BLUE = 6;
const uint8_t SFM_RING_YELLOW = 1, SFM_RING_PURPLE = 2, SFM_RING_CYAN = 4;

bool enrollActive = false, enrollLift = false;
uint8_t enrollStep = 0;
uint32_t enrollSince = 0, fingerLowSince = 0, fingerHighSince = 0, fingerRetryAt = 0;
bool fingerArmed = false, fingerNeedsStop = true, fingerNeedsCount = true;
bool fingerModeReady = false;
int fingerCount = -1;
uint16_t lastFingerId = 0, enrolledFingerId = 0;
uint8_t desiredRing = SFM_RING_YELLOW, sentRing = 0xff;
String fingerMessage = "Connecting";

void fingerMessageSet(const String& text) {
  fingerMessage = text;
  logLine("finger: " + text);
}

void finishEnrollment(const String& message) {
  enrollActive = false; enrollStep = 0;
  fingerArmed = false; fingerLowSince = 0; fingerHighSince = 0;
  fingerMessageSet(message);
}

void fingerOffline() {
  if (enrollActive) finishEnrollment("Enrollment failed: sensor not responding");
  else if (haveFp) fingerMessageSet("Sensor not responding; no punch");
  haveFp = false; fingerCount = -1; lastFingerId = 0;
  fingerModeReady = false; fingerNeedsStop = true; fingerNeedsCount = true;
  fingerArmed = false; fingerRetryAt = millis() + 10000;
  fingerMessage = "Sensor not responding";
  sentRing = 0xff;
}

bool sendFingerCommand(uint8_t cmd, uint8_t p1 = 0, uint8_t p2 = 0,
                       uint8_t p3 = 0, uint32_t timeout = 1000) {
  if (fingerLink.start(millis(), cmd, p1, p2, p3, timeout)) return true;
  fingerOffline(); return false;
}

bool fingerprintBusy() {
  return enrollActive || fingerLink.busy() || digitalRead(PIN_FP_TOUCH) == HIGH;
}

void handleEnroll() {
  if (!requireMutation()) return;
  if (!haveFp || !fingerModeReady || fingerCount < 0) {
    server.send(503, "text/plain", "Fingerprint sensor not ready"); return;
  }
  if (enrollActive || (fingerLink.busy() && fingerLink.command() != 0xc3)) {
    server.send(409, "text/plain", "Sensor busy; lift your finger and try again"); return;
  }
  if (fingerCount >= SFM_CAPACITY) {
    server.send(409, "text/plain", "Fingerprint storage full"); return;
  }
  enrollActive = true; enrollStep = 1; enrollLift = true;
  enrollSince = millis(); fingerLowSince = 0; fingerArmed = false;
  enrolledFingerId = 0; lastFingerId = 0;
  pendingRevertAt = 0; desiredRing = SFM_RING_CYAN;
  fingerMessageSet("Lift finger, then touch (1/3)");
  server.send(202, "text/plain", "Enrollment started");
}

void handleEnrollCancel() {
  if (!requireMutation()) return;
  if (enrollActive) {
    fingerLink.abandon(); fingerNeedsStop = true;
    finishEnrollment("Enrollment cancelled; no punch");
    setIdle();
  }
  server.send(200, "text/plain", "ok");
}

void updateFingerprint() {
  const uint32_t now = millis();
  const bool touched = digitalRead(PIN_FP_TOUCH) == HIGH;
  if (touched) { fingerLowSince = 0; if (!fingerHighSince) fingerHighSince = now; }
  else { fingerHighSince = 0; if (!fingerLowSince) fingerLowSince = now;
    if (now - fingerLowSince >= 300) fingerArmed = true; }

  if (enrollActive && (!serviceMode || bootButtonConfirmed || now - enrollSince >= 30000)) {
    fingerLink.abandon(); fingerNeedsStop = true;
    finishEnrollment("Enrollment stopped; try again"); setIdle();
  }

  uint8_t reply[8];
  const uint8_t command = fingerLink.command();
  const int received = fingerLink.poll(now, reply);
  if (received < 0) { fingerOffline(); return; }
  if (received > 0) {
    const uint8_t status = reply[4];
    if (command == 0xfe) {
      if (status != 0x18 && !(reply[1] == 0xfe && status == 1)) { fingerOffline(); return; }
      fingerNeedsStop = false; fingerNeedsCount = true;
    } else if (command == 0x09) {
      if (status != 0 || FingerprintProtocol::id(reply) > SFM_CAPACITY) { fingerOffline(); return; }
      fingerCount = FingerprintProtocol::id(reply); fingerNeedsCount = false;
      if (!haveFp) { haveFp = true; fingerMessageSet("Ready; " + String(fingerCount) + " enrolled"); }
    } else if (command == 0x2d) {
      if (status != 0 || reply[3] != 1) { fingerOffline(); return; }
      fingerModeReady = true;
    } else if (command >= 1 && command <= 3) {
      if (!enrollActive) return; // cancelled results can never become punches
      if (status != 0) {
        String reason = status == 7 ? "Finger already enrolled" : status == 8 ? "Capture timed out" : "Capture failed (" + String(status) + ")";
        finishEnrollment(reason + "; no punch"); flash(SFM_RING_RED, 800);
        fingerNeedsStop = true;
      } else if (reply[1] == 3) {
        const uint16_t id = FingerprintProtocol::enrolledId(reply);
        if (!id) { finishEnrollment("No valid fingerprint ID returned; no punch"); fingerNeedsStop = true; }
        else {
          enrolledFingerId = id;
          finishEnrollment("Enrolled ID " + String(id) + ". Lift finger; next touch punches.");
          fingerNeedsCount = true; flash(SFM_RING_BLUE, 1200);
        }
      } else if (command < 3) {
        ++enrollStep; enrollLift = true; enrollSince = now;
        fingerLowSince = 0; fingerArmed = false;
        fingerMessageSet("Lift finger (" + String(enrollStep - 1) + "/3 saved)");
      } else { finishEnrollment("Enrollment failed; no ID"); fingerNeedsStop = true; }
    } else if (command == 0x0c) {
      // Clear the previous ID before validating this response. Neither a
      // stale ID nor an error status may enter the durable punch queue.
      lastFingerId = FingerprintProtocol::matchedId(reply);
      fingerArmed = false; fingerLowSince = 0;
      if (!lastFingerId) { fingerMessageSet("Not recognized; no punch"); reject("no fingerprint ID"); }
      else if (!bootButtonTracking && !enrollActive) {
        fingerMessageSet("Matched ID " + String(lastFingerId));
        const String ref = String(lastFingerId);
        recordPunch(ref.c_str());
      }
    } else if (command == 0xc3 && status != 0) {
      logLine("finger: ring command failed; reader remains available");
    }
  }
  if (fingerLink.busy()) return;
  if (!haveFp && (int32_t)(now - fingerRetryAt) < 0) return;
  if (fingerNeedsStop) { sendFingerCommand(0xfe); return; }
  if (fingerNeedsCount) { sendFingerCommand(0x09); return; }
  if (!haveFp) return;
  if (!fingerModeReady) { sendFingerCommand(0x2d, 0, 1, 0); return; } // reject duplicate enrollments
  if (desiredRing != sentRing) {
    sentRing = desiredRing;
    sendFingerCommand(0xc3, sentRing, sentRing, 50); return;
  }
  if (enrollActive) {
    if (enrollLift) {
      if (!touched && fingerLowSince && now - fingerLowSince >= 300) {
        enrollLift = false; enrollSince = now;
        fingerMessageSet("Touch the same finger (" + String(enrollStep) + "/3)");
      }
    } else if (touched && fingerHighSince && now - fingerHighSince >= 50) {
      fingerArmed = false;
      sendFingerCommand(enrollStep, 0, 0, enrollStep == 1 ? 3 : 0, 12000);
    }
    return;
  }
  if (!bootButtonTracking && touched && fingerArmed && fingerHighSince && now - fingerHighSince >= 50) {
    // Set the visible reading color before issuing the capture command.
    // Ring commands sent during capture would abort it on this module.
    if (desiredRing != SFM_RING_CYAN || sentRing != SFM_RING_CYAN) {
      desiredRing = SFM_RING_CYAN;
      return;
    }
    fingerArmed = false; lastFingerId = 0;
    fingerMessage = "Reading finger";
    sendFingerCommand(0x0c, 0, 0, 0, 12000);
  }
}

// Used only by the already-authorized physical factory-reset sequence.
// Ordinary matching/enrollment never deletes existing templates.
bool eraseFingerprints() {
  fingerLink.abandon();
  for (uint8_t cmd : {uint8_t(0xfe), uint8_t(0x05)}) {
    if (!fingerLink.start(millis(), cmd, 0, 0, 0, 2000)) return false;
    uint8_t reply[8]; int result;
    do { result = fingerLink.poll(millis(), reply); delay(1); } while (!result);
    if (result < 0 || (cmd == 5 && reply[4] != 0)) return false;
  }
  return true;
}
