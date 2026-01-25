#include "engine/sync/midiclockout.h"

#include <QtDebug>
#include <cmath>

#include <QChronoTimer>

#include "control/controlobject.h"
#include "engine/sync/enginesync.h"
#include "moc_midiclockout.cpp"
#include "preferences/usersettings.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("MidiClockOut");
constexpr mixxx::Bpm kDefaultBpm(9999.9);
} // namespace

MidiClockOut::MidiClockOut(const QString& group, EngineSync* pEngineSync)
        : m_group(group),
          m_pEngineSync(pEngineSync),
          m_syncMode(SyncMode::None),
          m_oldTempo(kDefaultBpm),
          m_absTimeWhenPrevOutputBufferReachesDac(0),
          enabled(false),
          tickCount(0),
          tickError(0),
          sixteenths(1),
          beats(1),
          bars(1),
          skipNextTick(false),
          currentBpm(120.0),
          currentTickLength(20833),
          maximumNextTickCutoffTime(0),
          tickCutOff(300),
          ticknsTimerID(Qt::TimerId::Invalid),
          m_pMidiClockEnableButton(std::make_unique<ControlPushButton>(ConfigKey(group, "out_enabled"))),
          m_pMidiClockTickButton(std::make_unique<ControlPushButton>(ConfigKey(group, "tick"))),
          m_pMidiClockNudgeFwdButton(std::make_unique<ControlPushButton>(ConfigKey(group, "nudge_fwd"))),
          m_pMidiClockNudgeBackButton(std::make_unique<ControlPushButton>(ConfigKey(group, "nudge_back"))),
          m_pMidiClockPosSixteenths(std::make_unique<ControlObject>(ConfigKey(group, "num_sixteenths"))),
          m_pMidiClockPosBeats(std::make_unique<ControlObject>(ConfigKey(group, "num_beats"))), 
          m_pMidiClockPosBars(std::make_unique<ControlObject>(ConfigKey(group, "num_bars"))) 
{
    // Setup GUI
    m_pMidiClockEnableButton->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pMidiClockEnableButton->setStates(2);
    QObject::connect(m_pMidiClockEnableButton.get(), 
        &ControlObject::valueChanged, 
        this, 
        &MidiClockOut::slotControlOutEnabled);

    m_pMidiClockTickButton->setButtonMode(mixxx::control::ButtonMode::Trigger);
    m_pMidiClockTickButton->setStates(1);
    QObject::connect(m_pMidiClockTickButton.get(),
        &ControlObject::valueChanged,
        this,
        &MidiClockOut::slotControlTick);

    m_pMidiClockNudgeFwdButton->setButtonMode(mixxx::control::ButtonMode::Trigger);
    m_pMidiClockNudgeFwdButton->setStates(1);
    QObject::connect(m_pMidiClockNudgeFwdButton.get(),
        &ControlObject::valueChanged,
        this,
        &MidiClockOut::slotControlNudgeFwd);

    m_pMidiClockNudgeBackButton->setButtonMode(mixxx::control::ButtonMode::Trigger);
    m_pMidiClockNudgeBackButton->setStates(1);
    QObject::connect(m_pMidiClockNudgeBackButton.get(),
        &ControlObject::valueChanged,
        this,
        &MidiClockOut::slotControlNudgeBack);

    m_pMidiClockPosSixteenths->setReadOnly();
    m_pMidiClockPosSixteenths->forceSet(sixteenths);

    m_pMidiClockPosBeats->setReadOnly();
    m_pMidiClockPosBeats->forceSet(beats);

    m_pMidiClockPosBars->setReadOnly();
    m_pMidiClockPosBars->forceSet(bars);

    //Initialize tick timer    
    //pticknsTimer->setParent(this);   
    //pticknsTimer->callOnTimeout(this, &MidiClockOut::tick);

    ticknsTimer.setParent(this);
    ticknsTimer.callOnTimeout(this, &MidiClockOut::tick);
    //audioThreadDebugOutput();
}

MidiClockOut::~MidiClockOut() {
    // TODO: Setup a SYSEX command to optionally be sent on exit, that would tell
    // external followers to switch their clocks to internal mode, and set their BPMs
    // with midi.setTempo()
    // (not all sequencers work like LP Pro firmware and treat external 0xF8 as tap tempo..)

    enabled = false;

    // Disconnect the connection from the GUI buttons
    if (m_pMidiClockEnableButton) {
        QObject::disconnect(m_pMidiClockEnableButton.get(),
            &ControlObject::valueChanged,
            this,
            &MidiClockOut::slotControlOutEnabled);
    }

    if (m_pMidiClockTickButton) {
        QObject::disconnect(m_pMidiClockTickButton.get(),
            &ControlObject::valueChanged,
            this,
            &MidiClockOut::slotControlTick);
    }

    if (m_pMidiClockNudgeFwdButton) {
        QObject::disconnect(m_pMidiClockNudgeFwdButton.get(),
            &ControlObject::valueChanged,
            this,
            &MidiClockOut::slotControlNudgeFwd);
    }

    if (m_pMidiClockNudgeBackButton) {
        QObject::disconnect(m_pMidiClockNudgeBackButton.get(),
            &ControlObject::valueChanged,
            this,
            &MidiClockOut::slotControlNudgeBack);
    }

    // Destroy control objects before releasing Link.
    m_pMidiClockEnableButton.reset();
    m_pMidiClockTickButton.reset();
    m_pMidiClockNudgeFwdButton.reset();
    m_pMidiClockNudgeBackButton.reset();
   
    m_pMidiClockPosSixteenths.reset();
    m_pMidiClockPosBeats.reset();
    m_pMidiClockPosBars.reset();
}

// GUI Controls

void MidiClockOut::slotControlOutEnabled(double controlButtonValue) {
    enabled = (controlButtonValue > 0);
    if (enabled) {        
        ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(
            tickLengthFromBpm(currentBpm.value())));
        ticknsTimer.start();
        ticknsTimerID = ticknsTimer.id();
    } else {
        ticknsTimer.stop();
    }
}

void MidiClockOut::slotControlTick(double controlButtonValue) {  
    Q_UNUSED(controlButtonValue)
    tick();
}

void MidiClockOut::slotControlNudgeFwd(double controlButtonValue) {
    Q_UNUSED(controlButtonValue)
    fwdSixteenth();
}

void MidiClockOut::slotControlNudgeBack(double controlButtonValue) {
    Q_UNUSED(controlButtonValue)
    backSixteenth();    
}

/// Syncable overrides

/// Notify a Syncable that their mode has changed. The Syncable must record
/// this mode and return the latest mode in response to getMode().
void MidiClockOut::setSyncMode(SyncMode syncMode) {
    m_syncMode = syncMode;
}

/// Notify a Syncable that it is now the only currently-playing syncable.
void MidiClockOut::notifyUniquePlaying() {
    // The clock should keep playing at the previous tempo; 
    // BUT... how to change tempo now? To drive external synths?
    
    // Could have a deck's tempo-fader mapped, and grab the last clock leaders
    // fader to now control the MIDI Clock.
     
    // We could have a pre-programmed switch-over to internal clock (SYSEX)
    // and midi.setTempo() to the current tempo as a fall-back, so that 
    // external synths can use their own clocks if nothing is playing on Mixxx.
    // Not all clock followers will support a SYSEX command to switch their clocks.
    // But its probably the best we can do...
}

/// Notify a Syncable that they should sync phase.
void MidiClockOut::requestSync() {
    // TODO: Is this the correct way to get the leaders phase?
    EngineChannel* pLeaderChannel = m_pEngineSync->getLeaderChannel();
    beatDistance = pLeaderChannel->getEngineBuffer()->getExactPlayPos();
    uint32_t newTickCount = tickCount - (tickCount % 24) + (beatDistance.value() * 24);
    tickError += newTickCount - tickCount;

    qDebug() << "MidiClockOut::requestSync(), error (ticks):" << tickError;
}

/// Must NEVER return a mode that was not set directly via
/// notifySyncModeChanged.
SyncMode MidiClockOut::getSyncMode() const {
    return m_syncMode;
}

bool MidiClockOut::isPlaying() const {
    return enabled;
}

bool MidiClockOut::isAudible() const {
    // TODO: Should this be marked audible? Potentially external drum machines are.
    return enabled;
}

bool MidiClockOut::isQuantized() const {
    return enabled;
}

mixxx::Bpm MidiClockOut::getBpm() const {
    return mixxx::Bpm(currentBpm);
}

double MidiClockOut::getBeatDistance() const {    
    return std::fmod(tickCount, 24.0);
}

mixxx::Bpm MidiClockOut::getBaseBpm() const {    
    return mixxx::Bpm(120.0); //TODO: whats this for?
}

void MidiClockOut::updateLeaderBeatDistance(double beatDistance) {    
    tickError = (tickCount % 24) + (beatDistance * 24); //TODO: do we want sequencers to keep playing?    
}

void MidiClockOut::forceUpdateLeaderBeatDistance(double beatDistance) {
    tickCount = tickCount - (tickCount % 24) + (beatDistance * 24); //TODO: Or jump?
}

void MidiClockOut::updateLeaderBpm(mixxx::Bpm bpm) {
    
    dnewBpm = bpm.reasonableValueOr(currentBpm.value()); // dont follow ultra fast or slow BPMs
    newTickLength = tickLengthFromBpm(dnewBpm);
    differenceTickLength = newTickLength - currentTickLength;
    //m_tempoUpdateError = m_differenceTickLength; // TODO: scaled by the position; but probably not achievable?

    newNextTickTime = plannedNextTickTime + differenceTickLength;
    
    if (newNextTickTime > maximumNextTickCutoffTime) {
        //we set a flag instead of changing anything; this flag might be set
        //many times before the cutoff time
        flag_useNewInsteadOfPlannedTickTime = true;
        flag_plannedTickWillBeLate = false;
    } else if (flag_plannedTickWillBeLate == false) {
        //the next tick is too soon at the new BPM; the old tick will trigger
        //but too late for the new BPM. So catch up on the tick afterwards.
        //Should this timestamp be overwritten if theres another tempo request...
        //or maybe only the first should be saved, since it has the longest time at the
        //old bpm?
        flag_plannedTickWillBeLate = true;
        timeReceivedNewLeaderBpmLate = 
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
    }
}

void MidiClockOut::notifyLeaderParamSource() {
}

void MidiClockOut::reinitLeaderParams(double beatDistance, mixxx::Bpm, mixxx::Bpm bpm) {
    updateLeaderBeatDistance(beatDistance);
    updateLeaderBpm(bpm);
}

void MidiClockOut::updateInstantaneousBpm(mixxx::Bpm) {
}


/// Class functions

// 24 ppqn tick length in microseconds; mixxx::bpm supports 0 to 500 tempo range
std::chrono::microseconds MidiClockOut::tickLengthFromBpm(double bpm) {
    //[us/t] = 1000000 [us/s] * 60 [s/min] / (24 [t/b] * bpm [b/min]) =
    // 2500000 / bpm   
    std::chrono::microseconds conversion{(int)(2500000 / bpm)};
    return conversion;
}

void MidiClockOut::testMessage() {
    qDebug() << "MidiClockOut::testMessage()";
}

void MidiClockOut::skipTick() {
    skipNextTick = true;
}

void MidiClockOut::tick() {
    if (tickError > 1) {
        // TODO: Do something to catchup with the timer for each tick until its fixed; shorter timers so the 0xF8s
        // are still sent, but quicker for a few pulses until caught up
        // Or... just send them all at once and let the serial buffer / MIDI buffer handle it? Brrrrrrrrrrr
    } else if (tickError < 0) {
        skipNextTick = true;
        // TODO: Check on positives/ negatives... so that the error pushes the sync in the right direction
    }
    if (!newBpm.compareEq(currentBpm)) {
        currentTickLength = tickLengthFromBpm(newBpm.value());
        ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(currentTickLength));
        ticknsTimer.start();        
        ticknsTimerID = ticknsTimer.id();
        currentBpm = newBpm;        
    }
    plannedNextTickTime = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch() + currentTickLength);
    maximumNextTickCutoffTime = plannedNextTickTime - tickCutOff;

    if (skipNextTick) {
        skipNextTick = false;
        return;
    }

    tickCount++;

    if ((tickCount % 6) == 0) {
        sixteenths++;
        if (((sixteenths - 1) % 4) == 0) {
            sixteenths = 1;
            beats++;
            if (((beats - 1) % 4) == 0) {
                beats = 1;
                bars++;
            }
        }
 
    m_pMidiClockPosSixteenths->forceSet(sixteenths);
    m_pMidiClockPosBeats->forceSet(beats);
    m_pMidiClockPosBars->forceSet(bars);
    }

    qDebug() << "MidiClockOut::tick()";
}

void MidiClockOut::fwdSixteenth() {
    tickCount+=6;

    sixteenths++;
    if (((sixteenths - 1) % 4) == 0) {
        sixteenths = 1;
        beats++;
        if (((beats - 1) % 4) == 0) {
            beats = 1;
            bars++;
        }
    }

    m_pMidiClockPosSixteenths->forceSet(sixteenths);
    m_pMidiClockPosBeats->forceSet(beats);
    m_pMidiClockPosBars->forceSet(bars);
    

    qDebug() << "MidiClockOut::fwdSixteenth()";
}

void MidiClockOut::backSixteenth() {
    if (tickCount >= 6) {
        tickCount -= 6;
        sixteenths = ((tickCount / 6) % 4) + 1;
        beats = ((tickCount / 24) % 4) + 1;
        bars = (tickCount / (24 * 4)) + 1;
    } else {
        tickCount = 0;
        sixteenths = 1;
        beats = 1;
        bars = 1;
    }

    m_pMidiClockPosSixteenths->forceSet(sixteenths);
    m_pMidiClockPosBeats->forceSet(beats);
    m_pMidiClockPosBars->forceSet(bars);

    qDebug() << "MidiClockOut::backSixteenth()";
}

std::chrono::microseconds MidiClockOut::getHostTime() const {
    std::chrono::microseconds time = 
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
    return time;
}

std::chrono::microseconds MidiClockOut::getHostTimeAtSpeaker(std::chrono::microseconds hostTime) const {
    std::chrono::microseconds time = 
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
    return time;
}

/// This method is called at the start of the audio callback.
/// It captures the current time and updates the audio buffer time.
/// If Ableton Link is enabled, it captures the session state and notifies
/// the engine sync about any changes in tempo and beat distance.
void MidiClockOut::onCallbackStart(std::chrono::microseconds absTimeWhenPrevOutputBufferReachesDac) {
    m_absTimeWhenPrevOutputBufferReachesDac = absTimeWhenPrevOutputBufferReachesDac;

    if (!enabled) {
        return;
    }
    
}

void MidiClockOut::onCallbackEnd(int sampleRate, size_t bufferSize) {
    Q_UNUSED(sampleRate)
    Q_UNUSED(bufferSize)
}