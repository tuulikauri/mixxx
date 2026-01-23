#include "engine/sync/midiclockout.h"

#include <QtDebug>
#include <cmath>

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
          m_enabled(false),
          tickCount(0),
          sixteenths(1),
          beats(1),
          bars(1),
          //m_pLink(std::make_unique<ableton::BasicLink<MixxxClockRef>>(120.0)),
          m_pMidiClockEnableButton(std::make_unique<ControlPushButton>(ConfigKey(group, "out_enabled"))),
          m_pMidiClockTickButton(std::make_unique<ControlPushButton>(ConfigKey(group, "tick"))),
          m_pMidiClockNudgeFwdButton(std::make_unique<ControlPushButton>(ConfigKey(group, "nudge_fwd"))),
          m_pMidiClockNudgeBackButton(std::make_unique<ControlPushButton>(ConfigKey(group, "nudge_back"))),
          m_pMidiClockPosSixteenths(std::make_unique<ControlObject>(ConfigKey(group, "num_sixteenths"))),
          m_pMidiClockPosBeats(std::make_unique<ControlObject>(ConfigKey(group, "num_beats"))), 
          m_pMidiClockPosBars(std::make_unique<ControlObject>(ConfigKey(group, "num_bars"))) 
{

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
    m_pMidiClockPosSixteenths->forceSet(0);

    m_pMidiClockPosBeats->setReadOnly();
    m_pMidiClockPosBeats->forceSet(0);

    m_pMidiClockPosBars->setReadOnly();
    m_pMidiClockPosBars->forceSet(0);

    //audioThreadDebugOutput();
}

MidiClockOut::~MidiClockOut() {    
    m_enabled = false;

    // Disconnect the connection from the Link GUI button
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

    // Destroy control objects before releasing Link.
    m_pMidiClockEnableButton.reset();

    // Finally release Link
    //m_pLink.reset();
}

void MidiClockOut::slotControlOutEnabled(double controlButtonValue) {
    m_enabled = (controlButtonValue > 0);    
}

void MidiClockOut::slotControlTick(double controlButtonValue) {    
    tick();
}

void MidiClockOut::slotControlNudgeFwd(double controlButtonValue) {
    fwdSixteenth();
}

void MidiClockOut::slotControlNudgeBack(double controlButtonValue) {
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
}

/// Notify a Syncable that they should sync phase.
void MidiClockOut::requestSync() {
    qDebug() << "MidiClockOut::requestSync()";
}

/// Must NEVER return a mode that was not set directly via
/// notifySyncModeChanged.
SyncMode MidiClockOut::getSyncMode() const {
    return m_syncMode;
}

bool MidiClockOut::isPlaying() const {
    return m_enabled;
}

bool MidiClockOut::isAudible() const {
    return m_enabled;
}

bool MidiClockOut::isQuantized() const {
    return m_enabled;
}

mixxx::Bpm MidiClockOut::getBpm() const {
    return mixxx::Bpm(120.0); // TODO: dummy bpm
}


double MidiClockOut::getBeatDistance() const {    
    return std::fmod(0.5, 1.0); // TODO: dummy
}

mixxx::Bpm MidiClockOut::getBaseBpm() const {    
    return mixxx::Bpm(120.0);
}

void MidiClockOut::updateLeaderBeatDistance(double beatDistance) {
}

void MidiClockOut::forceUpdateLeaderBeatDistance(double beatDistance) {
}

void MidiClockOut::updateLeaderBpm(mixxx::Bpm bpm) {
}

void MidiClockOut::notifyLeaderParamSource() {
}

void MidiClockOut::reinitLeaderParams(double beatDistance, mixxx::Bpm, mixxx::Bpm bpm) {
}

void MidiClockOut::updateInstantaneousBpm(mixxx::Bpm) {
}


/// Class functions


void MidiClockOut::testMessage() {
    qDebug() << "MidiClockOut::testMessage()";
}

void MidiClockOut::tick() {
    tickCount++;

    if ((tickCount % 6) == 0) {
        sixteenths++;
        if (((sixteenths-1) % 4) == 0) {
            sixteenths = 1;
            beats++;
            if (((beats-1) % 4) == 0) {
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
    // TODO: how to handle backwards in beat-counting?
    
    /* if (sixteenths > 1) {
        sixteenths--;
        if (((sixteenths - 1) % 4) == 0) {
            sixteenths = 1;
            if (beats > 1) {
                beats--;
                if (((beats - 1) % 4) == 0) {
                    beats = 1;
                    if (bars > 1) {
                        bars--;
                    }
                }
            }
        }

        m_pMidiClockPosSixteenths->forceSet(sixteenths);
        m_pMidiClockPosBeats->forceSet(beats);
        m_pMidiClockPosBars->forceSet(bars);
    }
    */
    qDebug() << "MidiClockOut::backSixteenth()";
}

std::chrono::microseconds MidiClockOut::getHostTime() const {
    std::chrono::microseconds time = std::chrono::microseconds(1);
    return time;
}

std::chrono::microseconds MidiClockOut::getHostTimeAtSpeaker(std::chrono::microseconds hostTime) const {
    std::chrono::microseconds time = std::chrono::microseconds(1);
    return time;
}
